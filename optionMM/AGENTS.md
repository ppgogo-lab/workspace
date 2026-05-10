# AGENTS.md

## 1. Project Overview

**Low latency design is high priority.**
OptionMM is an ultra-low latency, high-frequency trading system for market making in Chinese commodity and equity index options markets (SHFE, DCE, CZCE, CFFEX). The system uses Black-76 pricing, lock-free ring buffers, SIMD-optimized math (AVX2/AVX-512), and core pinning to keep the critical path sub-microsecond.

**Key constraints:**
- Zero dynamic memory allocation on the critical path.
- No locks on the critical path; use SPSC ring buffers only.
- No exceptions or RTTI on the critical path.
- All hot threads pinned to dedicated CPU cores.
- Target hardware: Intel Xeon Gold 6544Y with Solarflare NIC + OpenOnload.

## 2. Code and Build

- **Code:** Add detailed comments for every task, especially special low-latency design choices.
- **Plan:** Finish a plan, then document the plan, implementation, and test result in a `.md` file under `docs/`.
- **Comment:** Add **Doxygen style** comments for every public method, containing `@brief`, `@param`, and `@return`. If the implementation is complex or has special lower-latency design, add `@note`.
- **Build:** During development, build the backend in WSL + Ubuntu and build the UI in Windows.

## 3. Running The Backend

When the user asks to "start backend", run the backend in WSL using the Windows `Start-Process` wrapper below. Do not run `wsl.exe bash -lc "./scripts/run_sim_demo.sh"` directly in the foreground unless the user explicitly wants a blocking foreground process.

**Correct detached backend startup from PowerShell:**

```powershell
Start-Process -FilePath 'wsl.exe' -ArgumentList @('bash','-lc','cd /mnt/d/workspace/optionMM && mkdir -p logs && ./scripts/run_sim_demo.sh > logs/backend.log 2>&1') -WindowStyle Hidden
```

**Verify the backend is running:**

```powershell
wsl.exe bash -lc 'pgrep -a optionmm || true'
wsl.exe bash -lc 'cd /mnt/d/workspace/optionMM && tail -40 logs/backend.log || true'
```

Expected successful startup log:

```text
gRPC server listening on 0.0.0.0:50061
```

Market making must be stopped by default after backend startup. The trader starts quoting manually from the Windows GUI with the `Start MM` button, which calls the backend `StartStrategy` RPC.

**Stop the backend if needed:**

```powershell
wsl.exe bash -lc 'pkill -f /mnt/d/workspace/optionMM/build-wsl/optionmm || true'
```

**Important notes for agents:**
- WSL distributions can be invisible from the sandbox user. If `wsl.exe --list` claims no distro is installed, retry the backend command with escalated permissions.
- Avoid `nohup ... & echo $!` through the normal shell tool. The command splitter can mangle `&`, `$!`, or pipes before the command reaches WSL.
- Use `pgrep` instead of `ps | grep` for verification to avoid shell pipe parsing issues.
- Backend log path from Windows: `D:\workspace\optionMM\logs\backend.log`.
- Backend log path from WSL: `/mnt/d/workspace/optionMM/logs/backend.log`.

## 4. Testing

- Run backend in WSL with `scripts/run_sim_demo.sh`, using the detached PowerShell startup command above for normal "start backend" requests.
- Run GUI in Windows with `scripts/run_windows_gui.cmd`.

## 5. Architecture

### Thread Pipeline (Critical Path)

```text
[Feed Thread] --tick_buf--> [Pricer Thread] --signal_buf[i]--> [Strategy Thread i]
                                                                        |
                           [Gateway Dispatcher] <-- order_buf[i] / quote_buf[i]
```

All stages use lock-free SPSC ring buffers.

## 6. Key Components

**Pricing (`src/pricing/`, `include/pricing/`):**
- `black76.cpp`: SIMD-optimized Black-76 pricer (AVX2/AVX-512).
- `vol_surface.cpp`: Volatility surface manager, supports 5 models.
- `orc_wing.cpp`: Core vol surface fitting model used in production.

**Strategy (`src/strategy/`, `include/strategy/`):**
- `mm_framework.cpp`: Base market making framework.
- `simple_mm.cpp`: Simple two-sided quoting strategy.
- `option_mm_core.cpp`: Core option market making strategy used in production.
- `pcp_arbitrage.cpp`: Put-Call Parity (PCP) arbitrage strategy implementation.

**Risk (`src/risk/`, `include/risk/`):**
- `pre_trade_risk.cpp`: Per-strategy-thread hard limits, such as max order volume.
- `post_trade_risk.cpp`: Portfolio-level soft limits, such as delta/gamma/vega.

**Gateway (`src/gateway/`, `include/gateway/`):**
- `ctp_gateway.cpp`: CTP (SimNow) gateway implementation.
- `femas_gateway.cpp`: FEMAS gateway implementation.
- `sim_gateway.cpp`: Simulated gateway for testing with no external SDK.

**Feed (`src/feed/`, `include/feed/`):**
- `multicast_feed.cpp`: UDP multicast market data receiver.
- `fpga_feed.cpp`: FPGA-accelerated feed placeholder.
- `femas_feed.cpp`: FEMAS gateway feed.

**Engine (`src/engine/`, `include/engine/`):**
- `trading_engine.cpp`: Main orchestrator, owns all threads and ring buffers.

**Common (`src/common/`, `include/common/`):**
- `types.h`: Core types such as `MarketTick`, `Order`, `Quote`, `Greeks`, and `Position`.
- `ring_buffer.h`: Lock-free SPSC ring buffer template.
- `config.h`: YAML config parser.
- `thread_utils.cpp`: Core pinning utilities.
- `instrument_lookup.h`: Fixed-capacity code to `instrument_id` lookup.

## 7. Think Before Coding

**Do not assume. Do not hide confusion. Surface tradeoffs.**

Before implementing:
- State assumptions explicitly. If uncertain, ask.
- If multiple interpretations exist, present them instead of picking silently.
- If something is unclear, stop, name what is confusing, and ask.

## 8. Goal-Driven Execution

**Define success criteria. Loop until verified.**

Transform tasks into verifiable goals:
- "Add validation" -> "Write tests for invalid inputs, then make them pass."
- "Fix the bug" -> "Write a test that reproduces it, then make it pass."
- "Refactor X" -> "Ensure tests pass before and after."
