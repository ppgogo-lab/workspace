# Market-Making Hot-Path Latency Plan

## Summary

The hot path should use `TopOfBookTick` as a true 64-byte best-bid-offer tick. Current code carries 5 levels in `TopOfBookTick`, making tick rings/snapshots move 192 bytes. Move depth into a cold/public `DepthTopOfBookTick`, then reduce dispatch and parameter-load overhead around the pricing-to-quote path.

## Key Changes

- Establish a baseline first:
  - Build/run non-ASAN latency presets: `latency-release`, `latency-release-monitoring-off`, and native variants when CPU-compatible.
  - Record stage p50/p95/p99 for `tick->signal_emit`, `signal_emit->strategy`, `strategy->quote_send`, and `quote_send->gateway`.
- Redefine hot/cold tick types:
  - Make `TopOfBookTick` exactly 64 bytes and `alignas(64)`, containing only timestamp, instrument id, last price, best bid/ask, best bid/ask volume, and sequence.
  - Add `DepthTopOfBookTick` for 5-level/public/cold depth data.
  - Update hot rings, snapshots, pricer, strategy, arbitrage triggers, and latency tests to use the 64-byte `TopOfBookTick`.
  - Update feed decoders to push `TopOfBookTick`; optionally publish `DepthTopOfBookTick` only to cold monitoring if needed.
- Batch strategy evaluation:
  - Add a batch signal path to `IMarketMaker`, with a default loop fallback for existing strategies.
  - In `strategy_loop`, pass popped `PricingSignal` batches directly to the strategy.
  - In `OptionMMCoreStrategy`, compute shared batch state once: `now_ns`, product regime, cached params, product pressure, thresholds, and quote policy.
- Cache runtime MM params:
  - Add a version counter to `AtomicMMParams::apply`.
  - Strategy thread keeps a plain cached `MMParamsConfig`, refreshed only when the version changes.
  - Use cached params in quote decisions, hedge checks, and quote material-change checks to remove repeated atomic loads.
- Reduce hot diagnostics:
  - In `low_latency_mode`, skip per-signal diagnostic timestamp calls unless latency diagnostics are explicitly enabled.
  - Update monitor atomics only when quote lifecycle/suppression state changes, not on every quote evaluation.
- Batch dispatcher drains:
  - Change gateway dispatcher order/quote drains to `try_pop_batch`.
  - Preserve existing callback interleaving so acks/fills still get prompt routing.

## Public Interfaces / Types

- `TopOfBookTick`: hot 64-byte BBO type, `static_assert(sizeof(TopOfBookTick) == 64)` and `alignof(TopOfBookTick) == 64`.
- `DepthTopOfBookTick`: cold/public 5-level market depth type.
- `IMarketMaker::on_signals(...)`: optional batch interface with default compatibility behavior.
- `AtomicMMParams::version`: monotonically increasing config version for strategy-side caching.

## Test Plan

- Correctness: `test_ring_buffer`, `test_latest_snapshot`, `test_option_mm_core`, `test_simple_mm`, `test_pcp_arbitrage`, `test_config`.
- Add tests for:
  - `TopOfBookTick` size/alignment.
  - `DepthTopOfBookTick` depth preservation.
  - Batch strategy path matching single-signal quote behavior.
  - Param cache refresh after `AtomicMMParams::apply`.
- Latency gates:
  - Run `ctest --preset latency-release-monitoring-off`.
  - Run `ctest --preset latency-release`.
  - Compare each phase against the captured baseline before moving to the next phase.

## Assumptions

- `TopOfBookTick` remains the canonical hot-path tick name.
- 5-level depth is not required for current hot market-making decisions.
- Production target is `low_latency_mode: true` with monitoring `off` or `deferred`.
