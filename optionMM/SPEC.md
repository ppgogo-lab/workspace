# OptionMM — System Specification

## 1. Overview

OptionMM is an ultra-low latency, high-frequency trading system targeting market makers in Chinese commodity and equity index options markets. The system receives market data, computes theoretical option prices and Greeks via Black-76, generates quotes and orders through a market making framework, and submits them to exchange gateways. A gRPC-based monitoring interface allows remote clients to observe system state and control strategy behavior in real time.

---

## 2. Target Users

- **Role**: Market makers (proprietary trading desks)
- **Markets**: Chinese futures and options exchanges
  - SHFE (Shanghai Futures Exchange) — commodity futures/options
  - DCE (Dalian Commodity Exchange) — commodity futures/options
  - CZCE (Zhengzhou Commodity Exchange) — commodity futures/options
  - CFFEX (China Financial Futures Exchange) — equity index futures/options
- **Products**: Commodity options, equity index options (and their underlying futures for hedging)

---

## 3. Deployment Model

- **One instance per market**: Each deployed instance is configured for a single exchange/gateway combination before startup.
- **Multi-product simultaneous market making**: A single instance supports market making across multiple option products concurrently. Each product runs its strategy on a **dedicated thread pinned to a separate CPU core**, providing full performance isolation between products.
- **Max products per instance**: `MAX_PRODUCTS = 32` (compile-time constant). Products are declared in the static config.
- **No runtime switching** of feed type or gateway — all connectivity decisions are made via static config at launch.
- **Configuration**: Static YAML file per instance. No central configuration service.
- **Local development rule**: Always build, test, and run this project in **WSL Ubuntu** by default. Do not use the Windows shell for project execution unless the task is explicitly Windows-specific.
- **Local utility rule**: Use WSL-hosted helper scripts under `scripts/` for repeatable local tasks such as PDF text extraction.

---

## 4. Latency Requirements

| Metric                        | Target         |
|-------------------------------|----------------|
| Tick-to-trade (end-to-end)    | < 2 microseconds |
| Vol surface refresh interval  | ~1 minute (off critical path) |
| Greeks/position update        | Off critical path (side thread) |

### Low-Latency Design Constraints
- Solarflare NIC with **OpenOnload** kernel bypass
- **Zero dynamic memory allocation** on the critical path
- **Lock-free ring buffers** (SPSC) between pipeline stages
- **Core pinning** for all hot threads
- **SIMD-optimized** Black-76 math (AVX2/AVX-512)
- Pre-allocated order, quote, and tick object pools
- No exceptions, no RTTI on the critical path

---

## 5. System Architecture

### 5.1 Critical Path (< 2μs budget)

```
[Feed Thread] ──tick_buf──► [Pricer Thread] ──signal_buf[0]──► [Strategy Thread 0, core N]
                                            ──signal_buf[1]──► [Strategy Thread 1, core N+1]
                                            ──signal_buf[i]──► [Strategy Thread i, core N+i]
                                                    │
                           [Gateway Dispatcher Thread] ◄── order_buf[i] / quote_buf[i]
                                        │
                                  [IGateway API]
```

- All SPSC ring buffers between stages; no locks on critical path.
- Each stage pinned to a dedicated CPU core.
- **Pricer routes signals** by `instrument_id → product_index` (O(1) array lookup) to the correct strategy thread's ring buffer.
- **Gateway dispatcher** centralizes all sends on one thread; strategy threads push to their own SPSC output buffer (lock-free from strategy thread's perspective).
- **One `PreTradeRisk` per strategy thread** — fully isolated, no cross-product contention.

### 5.2 Side Path (separate threads)

| Thread                   | Responsibility                                           |
|--------------------------|----------------------------------------------------------|
| Vol Surface Fitter       | Periodic IV surface fit (every ~1 min)                   |
| Risk Monitor             | Portfolio position limits, Greeks limits checks          |
| gRPC Monitor Server      | Pushes state to remote client, handles control RPCs      |
| Logger                   | Async structured logging (never on critical path)        |

---

## 6. Market Data Feed

### 6.1 Feed Types (configured per instance)

| Type       | Description                                         |
|------------|-----------------------------------------------------|
| FPGA       | Hardware-timestamped, lowest latency, direct FPGA feed |
| Multicast  | UDP multicast via Solarflare OpenOnload             |

- Feed type is known at startup from config; no runtime switching.
- Feed handler exposes a unified `IFeedHandler` interface to the rest of the system.

### 6.2 Market Data Content

- Full 5-level order book (bid/ask price + volume)
- Last price, open interest, volume
- OHLC, pre-settlement, pre-close
- Hardware or software receive timestamp (nanoseconds)

---

## 7. Pricing Engine

### 7.1 Black-76 Model

Used for real-time theoretical price and Greeks calculation on the critical path.

**Inputs per contract**:
- Forward price `F` (from futures market data)
- Strike `K`
- Time to expiry `T` (years)
- Risk-free rate `r` (from config)
- Implied volatility `σ` (from vol surface, pre-computed)

**Outputs**:
- Theoretical price
- Delta (∂V/∂F)
- Gamma (∂²V/∂F²)
- Vega (∂V/∂σ)
- Theta (∂V/∂T)
- Rho (∂V/∂r)

### 7.2 Implied Volatility Surface

- Recomputed periodically (approximately every 1 minute) on a dedicated side thread.
- Result is atomically published to the critical path (read-only snapshot pointer swap).
- Three fitting methods supported (selected via config at startup):

| Method        | Description                                  |
|---------------|----------------------------------------------|
| SVI           | Stochastic Volatility Inspired parametric fit |
| SABR          | Stochastic Alpha Beta Rho model               |
| Cubic Spline  | Strike-space cubic spline interpolation       |

- Vol surface is indexed by (expiry, strike) and supports interpolation for non-traded strikes.

---

## 8. Market Making Framework

### 8.1 Framework Responsibilities

- Maintain two-sided quotes (bid/ask) per option instrument
- Spread management: configurable min/max spread, target mid
- Inventory skew: adjust bid/ask prices based on net position
- Position limits enforcement (strategy-level soft limits)
- Delta hedging triggers: fire hedge orders when delta exposure exceeds threshold
- Quote update throttling: avoid quote flooding (configurable rate)
- Start/stop per instrument or globally

### 8.2 Simple MM Strategy (initial pipeline verification)

A minimal market making strategy implementing the framework interface:
- Fixed spread around theoretical price
- No inventory skew
- Simple delta hedge trigger at configurable delta threshold
- Used to validate the full pipeline before deploying complex strategies

### 8.3 Strategy Parameters (runtime-adjustable via gRPC)

| Parameter              | Description                              |
|------------------------|------------------------------------------|
| `bid_spread`           | Distance of bid from theo price          |
| `ask_spread`           | Distance of ask from theo price          |
| `max_position`         | Max net position per instrument          |
| `hedge_delta_threshold`| Delta exposure to trigger hedge          |
| `quote_volume`         | Volume per quote side                    |
| `enabled`              | Start/stop quoting                       |

---

## 9. Risk Management

### 9.1 Pre-Trade Risk (Hard Limits — Critical Path)

Checked synchronously before every order/quote submission. Order is **blocked** if any check fails.

| Check              | Description                                      |
|--------------------|--------------------------------------------------|
| Self-trade check   | Reject order that would match own resting order  |
| Max volume per order | Reject if order volume exceeds configured max  |

### 9.2 Post-Trade Risk (Soft Limits — Side Path)

Monitored asynchronously. Triggers alerts and can signal strategy to stop quoting.

| Check              | Description                                      |
|--------------------|--------------------------------------------------|
| Position limits    | Net/long/short position per instrument, per account |
| Delta limit        | Aggregate portfolio delta                        |
| Gamma limit        | Aggregate portfolio gamma                        |
| Vega limit         | Aggregate portfolio vega                         |

- Risk thresholds are **runtime-adjustable** via gRPC control interface.

---

## 10. Gateway Interface

### 10.1 Abstract Interface (`IGateway`)

All gateway implementations expose a unified interface:
- `SendOrder(Order&)` → OrderId
- `SendQuote(Quote&)` → QuoteId
- `CancelOrder(OrderId)`
- `CancelQuote(QuoteId)`
- Callbacks: `OnOrderAck`, `OnOrderFill`, `OnQuoteAck`, `OnQuoteFill`, `OnOrderReject`

### 10.2 Supported Gateways

| Gateway | Exchange(s)        | Protocol      |
|---------|--------------------|---------------|
| CTP     | SHFE, DCE, CZCE    | CTP TraderAPI |
| FEMAS   | CFFEX              | FEMAS API     |

- Gateway is selected at startup from config; no runtime switching.

---

## 11. Monitoring & Control Interface (gRPC)

### 11.1 Design Principles

- Protocol: **gRPC + Protobuf**
- Client runs remotely (separate machine)
- Server runs as a side-path thread inside the trading engine
- Real-time data is pushed via **server-streaming RPCs**
- Control commands use **unary RPCs**
- No gRPC traffic on the critical path

### 11.2 Streaming Data (Server → Client)

| Stream              | Content                                          | 
|---------------------|--------------------------------------------------|
| `MarketDataStream`  | Live ticks: prices, volumes, order book          |
| `OrderStream`       | Order status updates, fills                      |
| `QuoteStream`       | Quote status updates, fills                      |
| `TradeStream`       | Fill reports                                     |
| `GreeksStream`      | Per-instrument theo price + Greeks               |
| `PositionStream`    | Per-instrument position snapshots                |
| `PnlStream`         | Realized/unrealized P&L, portfolio Greeks        |
| `VolSurfaceStream`  | Vol surface snapshot after each fit              |
| `RiskAlertStream`   | Soft risk limit breach notifications             |

### 11.3 Control Commands (Client → Server, Unary RPC)

All per-product commands include `instrument_id` to target a specific product's strategy thread.

| RPC                       | Description                                                    |
|---------------------------|----------------------------------------------------------------|
| `SetStrategyParams`       | Update MM params for a specific product (`instrument_id`)      |
| `StartStrategy`           | Enable quoting for a specific product or all products          |
| `StopStrategy`            | Disable quoting for a specific product or all products         |
| `SetRiskThreshold`        | Update portfolio-level soft risk limit values                  |
| `SendManualOrder`         | Submit a manual order bypassing strategy                       |
| `CancelOrder`             | Cancel an active order                                         |
| `CancelQuote`             | Cancel an active quote                                         |
| `GetSnapshot`             | Request current state (all products, positions, P&L, etc.)    |

### 11.4 Interface-Only Scope (Phase 1)

- Proto definitions and gRPC service contracts will be designed now.
- Client-side UI implementation is **out of scope** for Phase 1.
- Server-side gRPC stub implementation will be included.

---

## 12. Configuration (YAML)

Top-level sections:

```yaml
instance:
  exchange: SHFE              # SHFE | DCE | CZCE | CFFEX
  account_id: "ACC001"

feed:
  type: multicast             # fpga | multicast
  multicast:
    interface: "eth0"
    groups: []
  fpga:
    device: "/dev/fpga0"

gateway:
  type: ctp                   # ctp | femas
  ctp:
    front_addr: "tcp://..."
    broker_id: ""
    user_id: ""
    password: ""
  femas:
    front_addr: "tcp://..."
    broker_id: ""
    user_id: ""
    password: ""

pricing:
  risk_free_rate: 0.025
  vol_surface:
    method: svi               # svi | sabr | cubic_spline
    fit_interval_seconds: 60

risk:
  hard:
    max_volume_per_order: 100
  soft:
    max_net_position: 500
    max_delta: 1000.0
    max_gamma: 500.0
    max_vega: 10000.0

# Multi-product: one entry per product, each gets its own strategy thread + CPU core
products:
  - instrument_id: "cu2501-C-75000"
    underlying_id: "cu2501"
    strategy_core: 4                  # dedicated CPU core for this product's strategy thread
    strategy_type: simple_mm
    params:
      bid_spread: 0.5
      ask_spread: 0.5
      quote_volume: 10
      hedge_delta_threshold: 50.0
      enabled: true
  - instrument_id: "cu2501-P-75000"
    underlying_id: "cu2501"
    strategy_core: 5
    strategy_type: simple_mm
    params:
      bid_spread: 0.5
      ask_spread: 0.5
      quote_volume: 10
      hedge_delta_threshold: 50.0
      enabled: true

monitoring:
  grpc_listen_addr: "0.0.0.0:50051"

thread_affinity:
  feed_core: 2
  pricer_core: 3
  # strategy cores are declared per-product above
  gateway_dispatcher_core: 6        # dedicated thread to serialize all gateway sends
  vol_fitter_core: 7
  risk_monitor_core: 8
  grpc_server_core: 9
```

---

## 13. Hardware & OS Platform

### 13.1 Target Hardware

| Component | Specification |
|-----------|--------------|
| CPU | Intel Xeon Gold 6544Y (Emerald Rapids, 5th Gen Xeon Scalable) |
| Cores | 16 physical cores, 32 logical threads (HyperThreading) |
| Base / Turbo | 3.6 GHz / 4.1 GHz |
| L1D / L2 / L3 | 48 KB / 2 MB / 45 MB (per core / per core / shared) |
| Memory | DDR5 5200 MT/s, 8 channels |
| PCIe | PCIe 5.0, up to 80 lanes |
| SIMD | AVX-512 (VNNI, BF16, AMX) |
| NUMA | 1 domain (HEX mode, default); optional SNC2 = 2 domains per socket |
| OS | Linux (RHEL 8/9 or Ubuntu 22.04 LTS recommended) |
| NIC | Solarflare SFN8522 or X2 series with OpenOnload |

### 13.2 Core Layout (16-core system)

```
Core 0–1   : OS, kernel, system IRQs       [non-isolated]
Core 1      : Solarflare NIC IRQ affinity   [pinned via /proc/irq]
Core 2      : Feed handler thread
Core 3      : Pricer thread
Core 4–11   : Strategy threads (up to 8 products, one core per product)
Core 12     : Gateway dispatcher thread
Core 13     : Vol surface fitter thread
Core 14     : Risk monitor thread
Core 15     : gRPC server thread
```

**Rules:**
- Cores 2–15 are isolated from the kernel scheduler (`isolcpus`, `nohz_full`, `rcu_nocbs`)
- Only one latency-critical thread per physical core (disable HT sibling or leave unused)
- If SNC2 is enabled in BIOS, all trading threads should be on the same NUMA domain as the NIC's PCIe slot

### 13.3 Linux Kernel Boot Parameters

```
isolcpus=2-15 nohz_full=2-15 rcu_nocbs=2-15
intel_idle.max_cstate=1 processor.max_cstate=1
hugepagesz=2M hugepages=512 hugepagesz=1G hugepages=4
transparent_hugepage=never
```

### 13.4 Key Runtime Tuning

| Tuning | Setting | Reason |
|--------|---------|--------|
| CPU governor | `performance` | Prevent frequency scaling on critical cores |
| Turbo boost | Enabled (keep `no_turbo=0`) | Max single-core throughput |
| IRQ balance | Disabled (`systemctl disable irqbalance`) | Prevent IRQ migration to isolated cores |
| NIC IRQ affinity | Core 1 only | Isolate network interrupts from trading cores |
| NUMA balancing | Disabled (`kernel.numa_balancing=0`) | Prevent OS from migrating memory pages |
| Transparent Huge Pages | Never | Prevents unpredictable latency from THP collapse |
| Swappiness | 0 | Prevent swap-induced latency spikes |
| `kernel.randomize_va_space` | 0 | Deterministic address layout |
| `kernel.sched_rt_runtime_us` | -1 | Allow RT threads unlimited CPU on isolated cores |
| OpenOnload | `EF_POLL_USEC=100 EF_SPIN_USEC=100 EF_INT_DRIVEN=0` | Busy-poll instead of interrupt-driven receive |

All tuning commands and verification scripts are captured in `Shell.md`.

---

## 15. Technology Stack

| Component         | Choice                                |
|-------------------|---------------------------------------|
| Language          | C++17                                 |
| Build system      | CMake 3.20+                           |
| Networking        | Solarflare / OpenOnload               |
| Serialization     | Protobuf 3                            |
| RPC               | gRPC                                  |
| Config parsing    | yaml-cpp                              |
| Math              | Eigen or manual SIMD (AVX2/AVX-512)   |
| Logging           | spdlog (async, off critical path)     |
| Testing           | Google Test                           |
| Gateway SDKs      | CTP TraderAPI/MdAPI, FEMAS API        |

---

## 16. Project Directory Structure

```
optionMM/
├── CMakeLists.txt
├── SPEC.md
├── Shell.md                            # Linux/CPU tuning commands and verification scripts
├── config.yaml                         # Example instance config
├── proto/
│   └── trading.proto                   # gRPC service + message definitions
├── include/
│   ├── common/
│   │   ├── types.h                     # Core types: Instrument, Order, Quote, Trade, Greeks
│   │   ├── ring_buffer.h               # Lock-free SPSC ring buffer
│   │   ├── thread_utils.h              # Core pinning, CPU affinity helpers
│   │   └── config.h                    # Config struct + YAML loader
│   ├── feed/
│   │   ├── feed_handler.h              # IFeedHandler abstract interface
│   │   ├── fpga_feed.h                 # FPGA feed implementation
│   │   └── multicast_feed.h            # Multicast/OpenOnload feed implementation
│   ├── pricing/
│   │   ├── black76.h                   # Black-76 pricer (SIMD optimized)
│   │   ├── vol_surface.h               # IVolSurface interface + VolSurfaceManager
│   │   ├── svi.h                       # SVI fitting
│   │   ├── sabr.h                      # SABR fitting
│   │   └── cubic_spline.h              # Cubic spline fitting
│   ├── strategy/
│   │   ├── mm_framework.h              # IMarketMaker abstract framework
│   │   ├── mm_params.h                 # Strategy parameter structs
│   │   └── simple_mm.h                 # Simple MM strategy implementation
│   ├── risk/
│   │   ├── pre_trade_risk.h            # Hard limits: self-trade, max volume
│   │   └── post_trade_risk.h           # Soft limits: position, Greeks
│   ├── gateway/
│   │   ├── gateway.h                   # IGateway abstract interface
│   │   ├── ctp_gateway.h               # CTP implementation
│   │   └── femas_gateway.h             # FEMAS implementation
│   ├── engine/
│   │   └── trading_engine.h            # Main engine: wires all components, owns threads
│   └── monitoring/
│       └── grpc_server.h               # gRPC monitoring/control server
├── src/
│   ├── main.cpp
│   ├── feed/
│   │   ├── fpga_feed.cpp
│   │   └── multicast_feed.cpp
│   ├── pricing/
│   │   ├── black76.cpp
│   │   ├── vol_surface.cpp
│   │   ├── svi.cpp
│   │   ├── sabr.cpp
│   │   └── cubic_spline.cpp
│   ├── strategy/
│   │   ├── mm_framework.cpp
│   │   └── simple_mm.cpp
│   ├── risk/
│   │   ├── pre_trade_risk.cpp
│   │   └── post_trade_risk.cpp
│   ├── gateway/
│   │   ├── ctp_gateway.cpp
│   │   └── femas_gateway.cpp
│   ├── engine/
│   │   └── trading_engine.cpp
│   └── monitoring/
│       └── grpc_server.cpp
└── tests/
    ├── test_black76.cpp
    ├── test_vol_surface.cpp
    ├── test_pre_trade_risk.cpp
    └── test_ring_buffer.cpp
```

---

## 17. Out of Scope (Phase 1)

- Remote client UI implementation
- Backtesting / simulation mode
- Multi-instance coordination / distributed risk
- Automatic parameter optimization
- FIX protocol support
- Persistence / trade database
