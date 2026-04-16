# Latency Optimization Plan

## Goal

Lower end-to-end option market making latency, especially p99 and p99.9, without destabilizing quote lifecycle, risk behavior, or startup flow.

## Baseline

Already completed:

- Startup-built code-to-id lookup removed the feed-side linear scan.

Main remaining latency costs:

- Full-product reevaluation in `src/strategy/option_mm_core.cpp`
- Unfair event draining in `src/engine/trading_engine.cpp`
- Monitoring and logging on hot threads
- Gateway callback handling order in the dispatcher thread
- Spin-on-full queue backpressure for signals and timers
- Cancel-before-replace cost on quote updates

## Execution Order

Implement in this order:

1. Fair strategy-loop scheduling
2. Eliminate most full-book repricing
3. Prioritize gateway callback drain
4. Move monitoring off the hot path
5. Replace spin-on-full with coalescing where safe
6. Optimize quote replace path

## Phase 1: Fair Strategy-Loop Scheduling

Purpose:

- Stop gateway and timer bursts from starving fresh pricing signals.

TODO:

- [ ] Add hard burst caps for gateway events per outer loop iteration in `src/engine/trading_engine.cpp`
- [ ] Add hard burst caps for timer events per outer loop iteration in `src/engine/trading_engine.cpp`
- [ ] Always reserve budget for `signal_buf_[idx]` processing every loop
- [ ] Document the fairness policy in code comments
- [ ] Re-run the latency benchmark and compare p50, p99, and p99.9

Expected impact:

- Lower signal starvation under active markets
- Better p99 without changing quote logic

## Phase 2: Eliminate Most Full-Book Repricing

Purpose:

- Remove the biggest strategy-thread tail-latency source.

TODO:

- [ ] Replace unconditional `reevaluate_all()` on fills with targeted re-evaluation
- [ ] Requote only the touched instrument on local fills unless product-wide regime changes
- [ ] Track explicit regime transitions:
- [ ] Product suppression on or off
- [ ] Product exposure breach on or off
- [ ] Underlying shock suppression on or off
- [ ] Keep full sweeps only for quote refresh, session open, and true product-wide state transitions
- [ ] Add tests proving targeted reevaluation does not miss required cancels or requotes

Expected impact:

- Major p99 and p99.9 improvement
- Less strategy-thread work during fill bursts

## Phase 3: Prioritize Gateway Callback Drain

Purpose:

- Reduce ack and fill feedback delay into strategy state.

TODO:

- [ ] In the dispatcher, drain `gateway_->callback_buf` before or between send bursts
- [ ] Keep callback routing bounded per loop so send path is not starved in the other direction
- [ ] Verify no ordering assumptions break for synthetic quote handling
- [ ] Re-measure callback-to-strategy latency after the change

Expected impact:

- Faster quote state convergence
- Less stale `Live` and `CancelPending` state

## Phase 4: Move Monitoring Off The Hot Path

Purpose:

- Remove non-trading work from pricer and dispatcher loops.

TODO:

- [ ] Make `monitor_ticks_.publish()` optional or deferred
- [ ] Make `monitor_quotes_.publish()` optional or deferred
- [ ] Make `monitor_orders_` and `monitor_trades_` optional or deferred where practical
- [ ] Add a config flag such as `monitoring.hot_path_publish_enabled`
- [ ] Default the flag to enabled in sim and debug, disabled in production latency mode

Expected impact:

- Small p50 improvement
- Better tail latency under high message rate

## Phase 5: Replace Spin-On-Full With Coalescing Where Safe

Purpose:

- Avoid wasting CPU pushing stale work.

TODO:

- [ ] Keep gateway events lossless
- [ ] For pricing signals, replace indefinite spin with latest-only coalescing where safe
- [ ] For timer events, coalesce duplicate `QuoteRefresh` and `HedgeCheck`
- [ ] Add counters for dropped or coalesced signals and timers
- [ ] Re-measure overload behavior and queue depth

Expected impact:

- Lower queue pressure during bursts
- Better stability under overload

## Phase 6: Optimize Quote Replace Path

Purpose:

- Reduce exchange round-trip cost for repricing.

TODO:

- [ ] Check whether each gateway or exchange API supports real amend or replace
- [ ] If supported, add an atomic replace path
- [ ] If not supported, reduce unnecessary replacement intent churn while cancel is already pending
- [ ] Keep existing cancel timeout and retry safety intact
- [ ] Re-measure quote update latency and update rate

Expected impact:

- Best structural improvement on quote update rate
- Highest implementation risk, so implement last

## Cross-Cutting Measurement TODO

Measure after every phase, not only at the end.

TODO:

- [ ] Keep using `tests/test_latency.cpp`
- [ ] Add timestamps for:
- [ ] Feed tick received
- [ ] Pricer signal emitted
- [ ] Strategy signal dequeued
- [ ] Quote enqueued
- [ ] Gateway send entered
- [ ] Ack or fill callback routed back
- [ ] Track p50, p95, p99, and p99.9
- [ ] Track queue depths
- [ ] Track full sweep count versus targeted reevaluation count
- [ ] Track dropped and coalesced signal counters

## Recommendation

Start with Phase 1 and Phase 2 first. They are the highest-signal, lowest-regret changes and do not depend on exchange API changes.
