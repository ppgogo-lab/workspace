# Findings

## Review Scope
- User requested an architecture review for lower latency and better maintainability.
- No code changes are planned in this pass unless needed for documentation artifacts.

## Findings Log
- Root-level `README.md` does not exist; project knowledge is spread across `SPEC.md`, `LLD.md`, `docs/`, and plan files.
- `CMakeLists.txt` separates Windows GUI builds from Linux backend builds and already has switches for AVX-512 Black76, native release tuning, IPO/LTO, DPDK, and latency CTest registration.
- The repository contains many prior optimization documents under `docs/`, including gateway contention, batching, NUMA, atomics, virtual dispatch, and monitoring deferral notes; the review should compare implementation against those intended directions.
- Initial filesystem inventory shows distinct modules for `engine`, `feed`, `pricing`, `strategy`, `risk`, `gateway`, `common`, `monitoring`, `persistence`, `gui`, and tests.
- Error: attempted to read missing `README.md`; will use `SPEC.md`, `LLD.md`, and existing docs instead.
- `SPSCRingBuffer` is fixed-size, trivially-copyable only, cache-line separated, and supports batch push/pop. It uses acquire/release cursor synchronization and a power-of-two capacity.
- `TopOfBookTick` is exactly 64 bytes and is used by the engine tick buffer, matching prior optimization notes.
- `TradingEngine` owns most runtime state directly and exposes many friend worker classes. This supports cache/local ownership but creates a very wide facade and makes ownership boundaries harder to audit.
- The pricer loop batches up to 128 options per product, uses cached time/discount/log-strike data, supports latest-only coalesced mailboxes when `signal_buf_` is full, and refreshes cold full Greeks separately.
- Strategy loop drains gateway events, timer events, ring-buffer signals, and coalesced signals with bounded budgets. This is good for fairness but means strategy latency depends on configured burst constants.
- Gateway dispatcher interleaves callbacks with send bursts and can skip monitoring/persistence side effects in `execution.low_latency_mode`.
- `low_latency_femas.yaml` is already tuned toward latency: monitoring off, persistence disabled, realtime enabled, low-latency spinning enabled.
- `FEMASGateway::send_order` and `send_quote` use lock-free slot allocation but still take `state_rw_lock_` to index state before every send.
- `PreTradeRisk` uses linear scans across `MAX_OPEN_ORDERS` for allocation, order lookup, and self-trade checks; this is cache-predictable but O(4096) on the strategy hot path.
- `BaseQuotingStrategy::on_fill` updates order/quote lifecycle and local position state, but does not call `pre_risk_->on_order_fill(...)`; only `on_order_ack` and `on_order_cancel` update pre-trade risk state.
- `TradingEngine::book_portfolios_snapshot` builds a `std::unordered_map` while holding `book_state_mutex_`. This is monitor-side, not the trading hot path, but it increases UI/gRPC jitter and maintenance risk.
- The active plan should include a recommendation to make prior optimization docs measurable through CTest/presets so regressions are visible.

## Final Review Output
- Created `docs/architecture_latency_review_20260510.md`.
- Top recommendations: remove FEMAS send-path shared lock, make pre-trade risk O(1)/instrument-local and fix fill bookkeeping, split `TradingEngine` state boundaries, move monitor aggregation out of request-time locks, and add latency thresholds.

## Implementation Pass
- User asked to proceed with code following `docs/architecture_latency_review_20260510.md`.
- Implementation should start with the suggested execution plan item 1: pre-trade risk fill bookkeeping and pre-trade risk hot-path data structure.
- `OptionMMCoreStrategy::maybe_trigger_hedge` pushes hedge orders directly, so hedge fills are not covered by the base order lifecycle tracker; hedge fill cleanup must happen in the strategy-specific fill branch.
- Existing latency tests already had stage p99 checks. Implementation extended them with configurable budgets and end-to-end/capture checks instead of duplicating benchmark parsing elsewhere.
- Final changed code scope: `include/risk/pre_trade_risk.h`, `src/risk/pre_trade_risk.cpp`, `src/strategy/base_quoting_strategy.cpp`, `src/strategy/option_mm_core.cpp`, `tests/test_pre_trade_risk.cpp`, `tests/test_option_mm_core.cpp`, and `tests/test_latency.cpp`.
