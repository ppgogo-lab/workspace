# Architecture Latency Implementation - 2026-05-10

## Plan
Implement the lowest-risk, highest-value items from `docs/architecture_latency_review_20260510.md` first:

1. Fix pre-trade risk fill bookkeeping.
2. Reduce pre-trade risk check cost without adding locks or allocation.
3. Add explicit latency regression thresholds around the existing benchmark.

The FEMAS send-path lock removal and `TradingEngine` state split are intentionally left as follow-up patches because they touch exchange callback semantics and broad ownership boundaries.

## Implementation

### Pre-Trade Risk
- Replaced order-id lookup scans with a fixed `FixedHashTable<OrderId, uint16_t, MAX_OPEN_ORDERS * 2>`.
- Added a fixed free-slot stack so open order slot allocation is O(1).
- Added per-instrument best own bid/ask summaries so self-trade checks avoid scanning all open orders.
- Kept a bounded repair path that recomputes one instrument side only when the removed order was the best price for that side.
- Added fill cleanup from `BaseQuotingStrategy::on_fill` for tracked orders.
- Added hedge-specific fill cleanup in `OptionMMCoreStrategy::on_fill_impl`, because hedge orders are submitted directly by the core strategy and are not tracked by the base order lifecycle array.

### Latency Thresholds
- Extended `tests/test_latency.cpp` with configurable budgets:
  - `OMM_LATENCY_BUDGET_TICK_TO_GATEWAY_NS`
  - `OMM_LATENCY_BUDGET_TICK_SIGNAL_NS`
  - `OMM_LATENCY_BUDGET_SIGNAL_STRATEGY_NS`
  - `OMM_LATENCY_BUDGET_STRATEGY_SEND_NS`
  - `OMM_LATENCY_BUDGET_SEND_GATEWAY_NS`
  - `OMM_LATENCY_BUDGET_CALLBACK_ROUTE_NS`
  - `OMM_LATENCY_MIN_CAPTURE_RATIO_PCT`
- Added p99 budget logging for each checked series.
- Added end-to-end tick-to-gateway p99 checks and capture-ratio checks in both direct-replace and cancel-first latency scenarios.

## Tests
- Passed: WSL build and run of `test_pre_trade_risk`.
- Passed: WSL build and run of `test_option_mm_core`.
- Passed: WSL build of `test_latency`.
- Expected skip: `build-wsl/test_latency` skips latency tests under ASAN.
- Passed: release latency preset via `scripts/run_latency_release_wsl.sh` with `OMM_LATENCY_MONITORING=off`.

## Notes
- The release latency build emitted existing compiler warnings in unrelated areas, including `thread_utils.cpp`, `sim_instruments.cpp`, `network_transport.cpp`, `ring_buffer.h`, `numa_utils.h`, and `test_latency.cpp`.
- The WSL launcher emitted its existing localhost/NAT text noise, but commands completed successfully.
