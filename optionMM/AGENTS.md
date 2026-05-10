# agent.md

## 1. Project Overview

**Low Latency design is high priority**
OptionMM is an ultra-low latency (<2μs tick-to-trade), high-frequency trading system for market making in Chinese commodity and equity index options markets (SHFE, DCE, CZCE, CFFEX). The system uses Black-76 pricing, lock-free ring buffers, SIMD-optimized math (AVX2/AVX-512), and core pinning to achieve sub-microsecond latency on the critical path.

**Key constraints:**
- Zero dynamic memory allocation on critical path
- No locks on critical path (SPSC ring buffers only)
- No exceptions or RTTI on critical path
- All hot threads pinned to dedicated CPU cores
- Target hardware: Intel Xeon Gold 6544Y with Solarflare NIC + OpenOnload

## 2. Code and build

- **Code** add detail comments for every task, especially special design for low latency.
- **Plan** finish a plan, document the plan, implementation, test result into a .md file in /docs
- **Comment** add **Doxygen style** comment for every public method, containing @brief, @param, @return. If the implementation is complex, or special design for lower latency, add @note to introduce the idea.
- **Build** when in dev stage, build backend in WSL + Ubuntu, while building ui in windows.
- **Test** Run backend in WSL, using scripts/run_sim_demo.sh; Run GUI in windows, using scripts/run_windows_gui.cmd


## 3. Architecture

### Thread Pipeline (Critical Path)

```
[Feed Thread] ──tick_buf──► [Pricer Thread] ──signal_buf[i]──► [Strategy Thread i]
                                                                        │
                           [Gateway Dispatcher] ◄── order_buf[i] / quote_buf[i]
```

All stages use lock-free SPSC ring buffers

### 4. Key Components

**Pricing (`src/pricing/`, `include/pricing/`):**
- `black76.cpp`: SIMD-optimized Black-76 pricer (AVX2/AVX-512)
- `vol_surface.cpp`: Volatility surface manager, supports 5 models
- `orc_wing.cpp`: Core Vol surface fitting model used in production

**Strategy (`src/strategy/`, `include/strategy/`):**
- `mm_framework.cpp`: Base market making framework
- `simple_mm.cpp`: Simple two-sided quoting strategy
- `option_mm_core.cpp`: core option market making strategy used in production
- `pcp_arbitrage.cpp`: Put-Call Parity (PCP) Arbitrage Strategy implementation

**Risk (`src/risk/`, `include/risk/`):**
- `pre_trade_risk.cpp`: Per-strategy-thread hard limits (max order volume)
- `post_trade_risk.cpp`: Portfolio-level soft limits (delta/gamma/vega)

**Gateway (`src/gateway/`, `include/gateway/`):**
- `ctp_gateway.cpp`: CTP (SimNow) gateway implementation
- `femas_gateway.cpp`: FEMAS gateway implementation
- `sim_gateway.cpp`: Simulated gateway for testing (no external SDK)

**Feed (`src/feed/`, `include/feed/`):**
- `multicast_feed.cpp`: UDP multicast market data receiver
- `fpga_feed.cpp`: FPGA-accelerated feed (placeholder)
- `femas_feed.cpp`: FEMAS gateway feed 

**Engine (`src/engine/`, `include/engine/`):**
- `trading_engine.cpp`: Main orchestrator, owns all threads and ring buffers

**Common (`src/common/`, `include/common/`):**
- `types.h`: Core types (MarketTick, Order, Quote, Greeks, Position, etc.)
- `ring_buffer.h`: Lock-free SPSC ring buffer template
- `config.h`: YAML config parser
- `thread_utils.cpp`: Core pinning utilities
- `instrument_lookup.h`: Fixed-capacity code -> instrument_id lookup


## 5. Think Before Coding

**Don't assume. Don't hide confusion. Surface tradeoffs.**

Before implementing:
- State your assumptions explicitly. If uncertain, ask.
- If multiple interpretations exist, present them - don't pick silently.
- If something is unclear, stop. Name what's confusing. Ask.


## 6. Goal-Driven Execution

**Define success criteria. Loop until verified.**

Transform tasks into verifiable goals:
- "Add validation" → "Write tests for invalid inputs, then make them pass"
- "Fix the bug" → "Write a test that reproduces it, then make it pass"
- "Refactor X" → "Ensure tests pass before and after"

---

