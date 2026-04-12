# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

OptionMM is an ultra-low latency (<2μs tick-to-trade), high-frequency trading system for market making in Chinese commodity and equity index options markets (SHFE, DCE, CZCE, CFFEX). The system uses Black-76 pricing, lock-free ring buffers, SIMD-optimized math (AVX2/AVX-512), and core pinning to achieve sub-microsecond latency on the critical path.

**Key constraints:**
- Zero dynamic memory allocation on critical path
- No locks on critical path (SPSC ring buffers only)
- No exceptions or RTTI on critical path
- All hot threads pinned to dedicated CPU cores
- Target hardware: Intel Xeon Gold 6544Y with Solarflare NIC + OpenOnload

## Build System

**Build commands:**
```bash
# Configure (first time or after CMakeLists.txt changes)
cmake -B build -DCMAKE_BUILD_TYPE=Release

# Build all targets
cmake --build build -j$(nproc)

# Build specific target
cmake --build build --target optionMM
cmake --build build --target test_black76

# Run all tests
cd build && ctest --output-on-failure

# Run specific test
./build/test_black76
./build/test_vol_surface

# Latency benchmark (NOT in ctest, run manually with RelWithDebInfo)
cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --target test_latency
./build/test_latency --gtest_filter="LatencyTest.*"

# Vol calibration test (reads MarketTick.csv, NOT in ctest)
./build/test_vol_calibration
```

**Build types:**
- `Release`: `-O3 -march=native -mavx2` (production)
- `Debug`: `-O0 -g3 -fsanitize=address,undefined` (development)
- `RelWithDebInfo`: `-O2 -g -fno-omit-frame-pointer` (profiling)

**Note:** Target CPU is Intel i7-9700 (dev) with AVX2. Production Xeon Gold 6544Y supports AVX-512; replace `-mavx2` with `-mavx512f -mavx512dq` in CMakeLists.txt for production builds.

## Architecture

### Thread Pipeline (Critical Path)

```
[Feed Thread] ──tick_buf──► [Pricer Thread] ──signal_buf[i]──► [Strategy Thread i]
                                                                        │
                           [Gateway Dispatcher] ◄── order_buf[i] / quote_buf[i]
```

All stages use lock-free SPSC ring buffers. Each thread pinned to dedicated core.

**Critical path threads:**
- Feed thread: Receives market data (multicast or FPGA), pushes to `tick_buf`
- Pricer thread: Computes Black-76 prices/Greeks, routes signals to strategy threads by product
- Strategy threads (1 per product, max 32): Market making logic, generates quotes/orders
- Gateway dispatcher: Centralizes all order/quote submissions to exchange gateway

**Side path threads (off critical path):**
- Vol surface fitter: Periodic IV surface calibration (~1 min interval)
- Risk monitor: Portfolio Greeks/position limit checks
- Timer: Triggers hedge checks and quote refreshes
- gRPC server: Remote monitoring and control

### Key Components

**Pricing (`src/pricing/`, `include/pricing/`):**
- `black76.cpp`: SIMD-optimized Black-76 pricer (AVX2/AVX-512)
- `vol_surface.cpp`: Volatility surface manager, supports 4 models
- `svi.cpp`, `sabr.cpp`, `wing.cpp`, `cubic_spline.cpp`: Vol surface fitting models

**Strategy (`src/strategy/`, `include/strategy/`):**
- `mm_framework.cpp`: Base market making framework
- `simple_mm.cpp`: Simple two-sided quoting strategy
- `mm_params.h`: Lock-free atomic MM parameters (updated via gRPC)

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

**Engine (`src/engine/`, `include/engine/`):**
- `trading_engine.cpp`: Main orchestrator, owns all threads and ring buffers

**Common (`src/common/`, `include/common/`):**
- `types.h`: Core types (MarketTick, Order, Quote, Greeks, Position, etc.)
- `ring_buffer.h`: Lock-free SPSC ring buffer template
- `config.h`: YAML config parser
- `thread_utils.cpp`: Core pinning utilities

### Important Constants (include/common/types.h)

- `MAX_INSTRUMENTS = 1024`: Max instruments per instance
- `MAX_PRODUCTS = 32`: Max products (each gets dedicated strategy thread)
- `MAX_OPEN_ORDERS = 4096`: Max open orders tracked

### Configuration

Config file: `config/example.yaml` (copy to `config/config.yaml` for runtime)

**Key sections:**
- `instance`: Exchange, account ID
- `feed`: Multicast or FPGA feed config
- `gateway`: CTP or FEMAS gateway config
- `pricing`: Risk-free rate, vol surface method (svi/sabr/wing/cubic)
- `risk`: Hard limits (per-order) and soft limits (portfolio Greeks)
- `products`: List of underlyings to trade, each with strategy params and core assignment
- `thread_affinity`: Core pinning for all threads
- `timer`: Hedge check and quote refresh intervals

## Development Workflow

**Adding a new volatility model:**
1. Create header in `include/pricing/` and implementation in `src/pricing/`
2. Add to `pricing_lib` in CMakeLists.txt
3. Wire into `TradingEngine::init_vol_surfaces()` in `src/engine/trading_engine.cpp`
4. Add enum value to `VolSurfaceMethod` in `include/common/config.h`
5. Update config parser in `src/common/config.cpp`

**Adding a new strategy:**
1. Inherit from `MMFramework` in `include/strategy/mm_framework.h`
2. Implement in `src/strategy/`, add to `strategy_lib` in CMakeLists.txt
3. Wire into `TradingEngine::init_strategies()` in `src/engine/trading_engine.cpp`
4. Add enum value to config if needed

**Adding a new gateway:**
1. Implement `IGateway` interface from `include/gateway/gateway.h`
2. Add to `gateway_lib` in CMakeLists.txt (may need external SDK linking)
3. Wire into main.cpp gateway factory

**Testing:**
- Unit tests use GoogleTest, located in `tests/`
- `test_simple_mm.cpp` uses `SimGateway` (no external SDK required)
- `test_latency.cpp` is a benchmark, not added to ctest (run manually)
- `test_vol_calibration.cpp` reads external CSV, not added to ctest

## Performance Notes

**Critical path optimizations already in place:**
- FP environment setup: FTZ/DAZ flags set in main to avoid denormal slowdown (see `types.h:setup_fp_environment()`)
- Pre-computed values cached in `TradingEngine`:
  - `option_log_K_[product][option_idx]`: log(strike) cached at startup
  - `option_T_[product][option_idx]`: time-to-expiry cached, refreshed every second by timer thread
  - `option_sqrt_T_[product][option_idx]`: sqrt(T) cached
  - `option_disc_[product][option_idx]`: exp(-r*T) cached
- Pricer thread batch-pushes all signals for a product in one call (reduces ring buffer overhead)
- All hot structs cache-line aligned (64 bytes)

**When modifying critical path code:**
- Never allocate memory (use pre-allocated pools or stack)
- Never use locks (use SPSC ring buffers or atomics)
- Avoid branches in tight loops (use branchless techniques)
- Keep hot data structures cache-line aligned
- Profile with `perf` before/after changes

## System Tuning

See `Shell.md` for detailed Linux kernel tuning (CPU isolation, IRQ affinity, huge pages, etc.)

Scripts in `scripts/`:
- `tune_system.sh`: Apply all kernel tuning (run as root before starting optionMM)
- `verify_tuning.sh`: Verify tuning is applied correctly
- `99-optionmm.conf`: sysctl config for kernel parameters

## gRPC Monitoring

Proto definition: `proto/trading.proto`

The gRPC server runs on port 50051 (configurable in YAML). Remote clients can:
- Subscribe to real-time state updates (positions, Greeks, orders)
- Update MM parameters dynamically (spreads, volumes, enabled flag)
- Submit manual orders
- Cancel orders

## Documentation

- `SPEC.md`: Full system specification (read this first for architecture deep-dive)
- `Shell.md`: Linux kernel tuning guide for production deployment
- `config/example.yaml`: Annotated config example
