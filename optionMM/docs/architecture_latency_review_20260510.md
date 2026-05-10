# Architecture and Latency Review - 2026-05-10

## Goal
Review the current OptionMM architecture and identify concrete changes that can reduce latency or make the codebase easier to maintain without weakening the low-latency constraints.

## Assumptions
- This pass is review-only. No runtime behavior was changed.
- Production latency mode means `config/low_latency_femas.yaml`: monitoring off, persistence disabled, `execution.low_latency_mode: true`, realtime scheduling enabled, and low-latency spinning enabled.
- Recommendations prioritize the trading hot path first, then maintainability of code that protects or explains the hot path.

## What Is Already Strong
- The pipeline follows the intended owner-per-stage model. `TradingEngine` owns fixed SPSC buffers for feed, pricing, strategy, gateway, arbitrage, monitoring, and persistence paths (`include/engine/trading_engine.h:706`).
- The tick hot path uses 64-byte `TopOfBookTick` instead of full book `MarketTick` (`include/common/types.h:180`), reducing cache footprint.
- `SPSCRingBuffer` is fixed-size, cache-line separated, trivially-copyable only, and supports batch push/pop (`include/common/ring_buffer.h:37`, `include/common/ring_buffer.h:117`, `include/common/ring_buffer.h:176`).
- The pricer batches up to 128 options per product and uses fused bid/mid/ask Black-76 quote pricing (`src/engine/pricer_worker.cpp:305`, `src/engine/pricer_worker.cpp:377`).
- Signal backpressure has a latest-only fallback mailbox instead of spinning the pricer indefinitely on a full strategy queue (`src/engine/pricer_worker.cpp:445`, `src/engine/pricer_worker.cpp:454`).
- The strategy loop uses bounded budgets for gateway callbacks, timers, ring-buffer signals, and coalesced signals (`src/engine/strategy_worker.cpp:56`, `src/engine/strategy_worker.cpp:123`, `src/engine/strategy_worker.cpp:137`).
- Gateway dispatcher can skip optional monitoring and persistence work in low-latency mode (`src/engine/gateway_dispatcher_worker.cpp:43`, `src/engine/gateway_dispatcher_worker.cpp:46`).
- The build already has release tuning switches, ISA-specific Black76 files, DPDK opt-in, test-only engine linkage, and latency CTest registration (`CMakeLists.txt:9`, `CMakeLists.txt:245`, `CMakeLists.txt:375`, `CMakeLists.txt:514`).

## Priority Recommendations

### 1. Remove the FEMAS send-path shared mutex from normal order and quote submission
`FEMASGateway::send_order` and `send_quote` allocate slots with atomics, but still take `state_rw_lock_` to index state before every send (`src/gateway/femas_gateway.cpp:586`, `src/gateway/femas_gateway.cpp:643`, `src/gateway/femas_gateway.cpp:665`, `src/gateway/femas_gateway.cpp:747`). That lock is now the main gateway-side serialization point.

Recommended direction:
- Replace the shared `state_rw_lock_` index update with single-owner gateway-dispatcher state where possible.
- Use fixed-size per-id arrays or atomic published slots for exchange local IDs, with a version/seqlock pattern similar to `LatestSnapshot`.
- Keep the FEMAS callback thread minimal: normalize callback data and push it to `callback_buf`; do more routing on the gateway dispatcher thread where ownership is already centralized.

Expected impact: lower p99 send latency and less callback/send interference under quote bursts.

### 2. Make pre-trade risk O(1) or instrument-local, and fix fill bookkeeping
`PreTradeRisk` scans `MAX_OPEN_ORDERS` for slot allocation, lookup, and self-trade checks (`src/risk/pre_trade_risk.cpp:22`, `src/risk/pre_trade_risk.cpp:33`, `src/risk/pre_trade_risk.cpp:47`). It is predictable but still O(4096) in the strategy path.

More importantly, `BaseQuotingStrategy::on_fill` updates quote/order lifecycle and position state but does not call `pre_risk_->on_order_fill(...)` (`src/strategy/base_quoting_strategy.cpp:208`, `src/strategy/base_quoting_strategy.cpp:225`). `pre_risk_` is updated on order ack and cancel (`src/strategy/base_quoting_strategy.cpp:308`, `src/strategy/base_quoting_strategy.cpp:327`), so filled strategy orders can remain in pre-risk state until cancel/reject paths remove them.

Recommended direction:
- Add the missing fill update for tracked non-quote orders.
- Replace global linear scans with fixed per-instrument bid/ask open-order slots or a fixed hash index keyed by `OrderId`.
- For quote self-trade prevention, decide explicitly whether exchange quote APIs provide sufficient protection; if not, model live quote sides in pre-trade risk as fixed per-instrument state.

Expected impact: fewer latency spikes in order-heavy products and more reliable hard-risk state.

### 3. Split `TradingEngine` into explicit hot-state, side-state, and facade modules
`TradingEngine` is the central state owner, which is good for performance, but it has a wide friend-class surface (`include/engine/trading_engine.h:58`) and mixes hot buffers, strategy state, live gateway state, book state, persistence, monitoring, and control methods in one class.

Recommended direction:
- Keep memory ownership centralized, but split layout into explicit structs: `HotPipelineState`, `StrategyProductState`, `GatewayLiveState`, `SidePathState`, and `ControlFacade`.
- Give workers typed references to only the state they need instead of full `TradingEngine` friendship.
- Preserve allocation-free static arrays; this is a structural refactor, not a move toward heap-owned services.

Expected impact: easier latency audits, fewer accidental side-path dependencies in hot code, and safer future changes.

### 4. Move monitor-side dynamic aggregation away from request-time locks
`book_portfolios_snapshot` builds a `std::unordered_map` while holding `book_state_mutex_` (`src/engine/trading_engine.cpp:1633`, `src/engine/trading_engine.cpp:1636`). This is not in the trading hot path, but it increases gRPC/UI jitter and makes monitor behavior less predictable during fills.

Recommended direction:
- Let `RiskMonitorWorker` maintain a fixed-capacity book/product portfolio snapshot table as fills arrive.
- Let gRPC read a `SnapshotArray` or fixed array copy without heap allocation.
- Keep expensive aggregation in the side thread, not the request handler.

Expected impact: smoother monitoring under load and simpler separation between risk ownership and UI reads.

### 5. Make latency regressions first-class CI artifacts
There is a latency preset and benchmark (`CMakePresets.json:17`, `CMakeLists.txt:517`), but the review did not find a threshold-based pass/fail policy for p50/p99 tick-to-gateway or callback-route latency.

Recommended direction:
- Add benchmark thresholds to `tests/test_latency.cpp` or a wrapper that parses its output.
- Track at least: tick-to-signal, signal-to-strategy, strategy-to-gateway, quote ack route, cancel route, signal coalescing count, monitor/persistence drops.
- Run both `OMM_LATENCY_MONITORING=off` and `deferred` modes.

Expected impact: architectural improvements become measurable, and regressions stop hiding inside functional test passes.

## Secondary Recommendations
- Keep `low_latency_femas.yaml` as the production baseline and add comments or a doc pointing out why `hot_path_publish_mode: off`, `low_latency_mode: true`, and `low_latency_spin: true` matter (`config/low_latency_femas.yaml:93`, `config/low_latency_femas.yaml:100`, `config/low_latency_femas.yaml:112`).
- Audit Doxygen comments generated in prior work. Many comments are mechanically correct but low signal, such as `@param Parameter supplied by the caller`; they increase noise around important low-latency notes.
- Consider replacing `std::snprintf` local ID formatting in hot gateway send with a fixed-width decimal encoder if profiling shows it on the p99 path.
- Keep DPDK/OpenOnload build and runtime settings documented as deployment presets, not just CMake toggles.

## Suggested Execution Plan
1. Fix and test pre-trade risk fill bookkeeping first; this is both correctness and latency hygiene.
2. Add latency threshold reporting around the current state before changing gateway internals.
3. Prototype lock-free or dispatcher-owned FEMAS state indexing behind the existing gateway API.
4. Refactor `TradingEngine` state into grouped structs without changing runtime behavior.
5. Move monitor portfolio aggregation into `RiskMonitorWorker` fixed snapshots.

## Verification
- Review-only pass; no production code was modified.
- Inspected core headers, engine workers, strategy base/core, FEMAS gateway, risk, monitoring, config, CMake, and latency test setup.
- Did not run the backend build or latency benchmark during this review.
