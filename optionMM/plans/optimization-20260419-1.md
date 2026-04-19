# OptionMM Latency Optimization - 2026-04-19

## Summary

This note records the implemented latency plan and the final validation results for the `OptionMMCoreStrategy` hot path.

The main goal was to make the latency work production-oriented instead of keeping the benchmark tied to the older small synthetic path.

## Implemented Plan

1. Upgrade the latency benchmark to exercise `option_mm_core` directly.
   - Replaced the old benchmark setup that effectively measured the simpler path.
   - Added separate benchmark scenarios for:
     - direct-replace venues
     - cancel-first venues
   - Added stage-level measurements:
     - tick -> signal emit
     - signal emit -> strategy
     - strategy -> quote send
     - quote send -> gateway
     - callback routing latency

2. Add production-oriented engine instrumentation.
   - Added counters for:
     - emitted pricing signals
     - suppressed pricing signals
     - pending future-tick overwrites
     - full-book vs single-instrument reevaluations
   - Added per-instrument timestamps for:
     - last signal emit
     - last strategy signal consume
     - last QuoteAck route
     - last QuoteCancel route

3. Add engine-side signal suppression controls.
   - Added new pricing config knobs:
     - `signal_emit_price_epsilon_ticks`
     - `signal_emit_underlying_epsilon_ticks`
     - `signal_emit_delta_epsilon`
     - `signal_emit_vega_epsilon`
   - The pricer now suppresses sub-threshold option signal updates instead of always emitting every recomputed signal.

4. Improve pricer fairness under repeated future ticks.
   - Preserved progress through the option chain instead of always restarting from offset 0 whenever a new future tick arrives.
   - Counted pending future-tick overwrites so overload behavior is visible.

5. Move tick monitoring under the same hot-path policy as other monitoring streams.
   - Tick publication now follows `full / deferred / off` rather than always publishing in the pricer loop.

6. Expose runtime stats from `OptionMMCoreStrategy`.
   - Added runtime stats for:
     - full-book reevaluations
     - single-instrument reevaluations

## Config Changes

Added to `config/config.yaml` and `config/example.yaml`:

```yaml
pricing:
  signal_emit_price_epsilon_ticks: 0.25
  signal_emit_underlying_epsilon_ticks: 0.5
  signal_emit_delta_epsilon: 0.01
  signal_emit_vega_epsilon: 0.02
```

## Validation Commands

The following validation was run in `build-latency-release` on 2026-04-19:

```bash
./build-latency-release/test_simple_mm
./build-latency-release/test_option_mm_core
./build-latency-release/test_latency --gtest_filter='LatencyTest.TickToQuoteLatency'
./build-latency-release/test_latency --gtest_filter='LatencyTest.TickToQuoteLatencyCancelFirst'
./build-latency-release/test_latency --gtest_filter='LatencyTest.*'
```

## Final Test Results

### Unit Tests

- `test_simple_mm`: passed
- `test_option_mm_core`: passed

### LatencyTest.TickToQuoteLatency

Scenario:
- `option_mm_core`
- exchange: `SHFE`
- replace policy: direct-replace
- products: `1`
- options per product: `160`
- iterations: `320`
- cancel latency: `0 ms`
- monitoring mode: `deferred`

Counts:
- captured quotes: `51200 / 51200` (`100%`)
- product 0 quotes: `51200`
- single-instrument reevaluations: `52503`
- full-book reevaluations: `0`

Latency:
- tick -> gateway: `p50 52.1 us`, `p95 153.4 us`, `p99 300.9 us`, `p99.9 2658.1 us`, `max 2705.1 us`
- tick -> signal emit: `p50 1.4 us`, `p95 59.4 us`, `p99 129.1 us`, `p99.9 266.1 us`
- signal emit -> strategy: `p50 37.0 us`, `p95 91.7 us`, `p99 290.4 us`, `p99.9 1096.4 us`
- strategy -> quote send: `p50 0.0 us`, `p95 0.1 us`, `p99 0.1 us`, `p99.9 1508.2 us`
- quote send -> gateway: `p50 0.2 us`, `p95 48.9 us`, `p99 81.4 us`, `p99.9 430.9 us`

Callback routing:
- QuoteAck route: `p50 16.1 us`, `p95 66.6 us`, `p99 177.6 us`, `p99.9 2807.3 us`

Signal / queue counters:
- emitted signals: `52000`
- suppressed signals: `0`
- pending future overwrites: `4`
- coalesced signal writes: `0`
- coalesced timer writes: `0`
- max signal ring depth: `192`
- max signal mailbox depth: `0`
- max timer ring depth: `0`

### LatencyTest.TickToQuoteLatencyCancelFirst

Scenario:
- `option_mm_core`
- exchange: `GFEX`
- replace policy: cancel-first
- products: `1`
- options per product: `16`
- iterations: `120`
- cancel latency: `1 ms`
- monitoring mode: `deferred`

Counts:
- captured quotes: `1920 / 1920` (`100%`)
- product 0 quotes: `1920`
- single-instrument reevaluations: `4000`
- full-book reevaluations: `0`

Latency:
- tick -> gateway: `p50 1115.8 us`, `p95 1212.3 us`, `p99 1281.5 us`, `p99.9 1642.3 us`, `max 1642.5 us`
- tick -> signal emit: `p50 2.7 us`, `p95 19.6 us`, `p99 70.9 us`, `p99.9 492.7 us`
- signal emit -> strategy: `p50 8.3 us`, `p95 28.2 us`, `p99 58.7 us`, `p99.9 88.0 us`
- strategy -> quote send: `p50 1086.3 us`, `p95 1177.3 us`, `p99 1226.5 us`, `p99.9 1248.5 us`
- quote send -> gateway: `p50 2.5 us`, `p95 29.9 us`, `p99 66.4 us`, `p99.9 116.3 us`

Callback routing:
- QuoteAck route: `p50 8.7 us`, `p95 25.8 us`, `p99 70.3 us`, `p99.9 76.5 us`
- QuoteCancel route: `p50 10.0 us`, `p95 38.5 us`, `p99 84.6 us`, `p99.9 109.0 us`

Signal / queue counters:
- emitted signals: `2064`
- suppressed signals: `0`
- pending future overwrites: `0`
- coalesced signal writes: `0`
- coalesced timer writes: `0`
- max signal ring depth: `0`
- max signal mailbox depth: `0`
- max timer ring depth: `0`

## Interpretation

1. Direct-replace production latency is now measured on a much more realistic `option_mm_core` workload.
   - These numbers are not directly comparable to the older tiny synthetic benchmark that reported single-digit microsecond p50.

2. Cancel-first latency is dominated by the venue policy.
   - In the cancel-first case, the largest component is `strategy -> quote send`, which reflects the cancel-wait-requote cycle rather than CPU-only engine overhead.

3. The current direct-replace production path is not queue-saturated in this benchmark.
   - Signal coalescing stayed at zero in the main run.
   - Pending future overwrite protection did engage.

4. The new instrumentation makes the next optimization pass clearer.
   - If further reductions are needed, the main next targets are:
     - reducing signal emit -> strategy delay under load
     - reducing tail spikes in QuoteAck callback routing
     - venue-specific optimization for cancel-first quote replacement
