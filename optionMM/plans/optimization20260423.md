# OptionMM Latency Optimization - 2026-04-23

## Summary

This pass focused on lowering software-side latency in the `direct-replace` path without a major architecture rewrite. The main priorities were to shorten the gateway callback hot path, reduce scheduling jitter, remove avoidable shared-state overhead, and keep monitoring and persistence from interfering with quote routing.

## Implemented Plan

### 1. Thread scheduling and core layout

- Fixed the default affinity collision between `risk_monitor_core` and `timer_core` by moving `timer_core` away from core `14`.
- Added optional best-effort real-time scheduling support for latency-critical engine threads.
- Applied RT scheduling hooks to pricer, strategy, arbitrage, gateway dispatcher, timer, risk, and vol fitter threads when enabled by config.

### 2. Faster gateway callback routing

- Reworked the gateway dispatcher so callback events are normalized and routed to strategy and arbitrage before deferred side effects.
- Moved monitoring publication, persistence enqueue, and recovery refresh work behind the fast route path.
- Removed repeated synchronous recovery-handle fetches from the callback hot path.
- Removed synchronous fill logging from the dispatcher fast path.

### 3. Lower shared-state overhead in live tracking

- Cached live order recovery state inside the engine rather than repeatedly fetching recovery handles during callbacks.
- Added explicit live recovery refresh helpers for orders and quotes.
- Improved `QuoteFill` handling so direct quote-id fills can be resolved from live quote state without depending on leg remapping first.

### 4. Lower timer and risk jitter

- Replaced coarse `100ms` polling loops in timer and risk processing with deadline-based sleeps and short capped idle waits.
- Allowed the timer and risk loops to react more quickly to pending work while still avoiding busy-spin behavior.

### 5. Keep non-critical work off the hot path

- Preserved deferred monitoring behavior for latency-sensitive runs.
- Interleaved callback draining with send bursts to reduce queueing spikes without pushing persistence or monitoring ahead of routing.

## Config Changes

Default core assignment change in `config/config.yaml`:

```yaml
risk_monitor_core: 14
timer_core: 11
```

New optional scheduling section:

```yaml
thread_scheduling:
  enable_realtime: false
  critical_priority: 80
  background_priority: 20
```

## Validation Commands

```bash
./build-latency-release/test_simple_mm --gtest_filter='TradingEngineIntegration.*'
./build-latency-release/test_latency --gtest_filter='LatencyTest.TickToQuoteLatency:LatencyTest.TickToQuoteLatencyCancelFirst'
```

## Final Test Results

### Direct-replace

Scenario:

- strategy: `option_mm_core`
- exchange: `SHFE`
- replace policy: `direct-replace`
- products: `1`
- options per product: `160`
- iterations: `320`
- cancel latency: `0 ms`
- monitoring mode: `deferred`

Counts:

- captured quotes: `51095 / 51200` (`99.7949%`)
- product 0 quotes: `51095`
- single-instrument reevaluations: `52405`
- full-book reevaluations: `0`

Latency:

- tick -> gateway: `p50 191.9 us`, `p95 510.3 us`, `p99 921.8 us`, `p99.9 3056.5 us`, `max 3180.6 us`
- tick -> signal emit: `p50 1.0 us`, `p95 57.9 us`, `p99 161.7 us`, `p99.9 225.0 us`
- signal emit -> strategy: `p50 34.4 us`, `p95 79.7 us`, `p99 127.3 us`, `p99.9 189.9 us`
- strategy -> quote send: `p50 0.0 us`, `p95 0.1 us`, `p99 0.1 us`, `p99.9 792.1 us`, `max 1245.0 us`
- quote send -> gateway: `p50 141.0 us`, `p95 422.6 us`, `p99 840.1 us`, `p99.9 7251.4 us`, `max 7350.5 us`

Callback routing:

- QuoteAck route: `p50 85.0 us`, `p95 246.8 us`, `p99 458.3 us`, `p99.9 6725.4 us`, `max 7150.3 us`

Signal and queue counters:

- emitted signals: `52000`
- suppressed signals: `0`
- pending future overwrites: `4`
- coalesced signal writes: `0`
- coalesced signal overwrites: `0`
- coalesced timer writes: `0`
- coalesced timer overwrites: `0`
- max signal ring depth: `160`
- max signal mailbox depth: `0`
- max timer ring depth: `0`

### Cancel-first

Scenario:

- strategy: `option_mm_core`
- exchange: `GFEX`
- replace policy: `cancel-first`
- products: `1`
- options per product: `16`
- iterations: `120`
- cancel latency: `1 ms`
- monitoring mode: `deferred`

Counts:

- captured quotes: `1920 / 1920` (`100%`)
- product 0 quotes: `1920`
- single-instrument reevaluations: `4016`
- full-book reevaluations: `0`

Latency:

- tick -> gateway: `p50 1098.4 us`, `p95 1222.0 us`, `p99 1336.8 us`, `p99.9 2159.8 us`, `max 2160.7 us`
- tick -> signal emit: `p50 2.6 us`, `p95 8.6 us`, `p99 72.4 us`, `p99.9 90.3 us`, `max 90.3 us`
- signal emit -> strategy: `p50 7.6 us`, `p95 28.9 us`, `p99 68.6 us`, `p99.9 118.4 us`, `max 118.6 us`
- strategy -> quote send: `p50 1076.3 us`, `p95 1175.6 us`, `p99 1250.6 us`, `p99.9 2123.8 us`, `max 2123.8 us`
- quote send -> gateway: `p50 6.8 us`, `p95 15.6 us`, `p99 33.4 us`, `p99.9 222.0 us`, `max 222.5 us`

Callback routing:

- QuoteAck route: `p50 9.5 us`, `p95 45.4 us`, `p99 174.6 us`, `p99.9 367.9 us`, `max 368.2 us`
- QuoteCancel route: `p50 11.0 us`, `p95 45.4 us`, `p99 91.1 us`, `p99.9 1068.8 us`, `max 1069.0 us`

Signal and queue counters:

- emitted signals: `2064`
- suppressed signals: `0`
- pending future overwrites: `0`
- coalesced signal writes: `0`
- coalesced signal overwrites: `0`
- coalesced timer writes: `0`
- coalesced timer overwrites: `0`
- max signal ring depth: `19`
- max signal mailbox depth: `0`
- max timer ring depth: `0`

## Interpretation

- The callback fast-path changes reduced the amount of non-routing work done before strategy and arbitrage receive updates.
- The timer and risk loops are now materially tighter than the previous `100ms` polling design.
- The `cancel-first` path remains dominated by venue workflow and configured cancel delay rather than internal software cost.
- The `direct-replace` benchmark on this WSL host is still noisier than the older checked-in baseline, so these numbers should not be treated as the final microsecond claim.
- The next measurement step should be to rerun the same release latency tests on the isolated pinned-core target host, ideally with `thread_scheduling.enable_realtime: true` if host permissions allow it.
