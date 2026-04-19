# Arbitrage Strategy Design - 2026-04-19

## Summary

This note records the design and implementation plan for adding arbitrage strategies to `optionMM` without impacting the `option_mm_core` market-making hot path.

The final design keeps OMM on the existing product strategy thread as the top-priority quote owner, and moves arbitrage to separate lower-priority sidecar thread(s). The first implemented arbitrage strategy is Put-Call Parity (`PCP`).

## Core Design

### Priority and Threading

- `OMM strategy thread`
  - Remains the highest-priority per-product thread.
  - Owns quote lifecycle, quote replacement, MM hedging, and MM gateway callback handling.
  - Does not run arbitrage decision logic.

- `Arbitrage sidecar thread`
  - Runs separately from the OMM strategy thread.
  - Reads immutable engine snapshots only:
    - `tick_snapshot_`
    - `greeks_snapshot_`
    - instrument registry
    - atomic arbitrage params
  - Produces bounded `ArbIntent` messages.

- `Gateway dispatcher`
  - Converts accepted `ArbIntent` messages into normal live orders.
  - Routes arbitrage order callbacks into a separate arb event queue.
  - Keeps MM-originated queue/order/quote work ahead of arbitrage intent draining.

This preserves a strict rule: market making remains the only quote owner, and arbitrage is execution-only.

### Ownership Rules

- MM owns all quote state.
- Arbitrage may not place or manage resting quotes in v1.
- Arbitrage submits orders only.
- If MM and arbitrage compete for budgets or dispatch bandwidth, MM wins and arbitrage is clipped or suppressed first.

## Implemented Runtime Model

### Shared Types and Config

Added runtime/config support for per-product arbitrage strategies:

- `ArbitrageStrategyType`
  - `None`
  - `PCP`
- `ArbIntent`
  - carries strategy metadata plus the normal `Order`
- `ArbParamsConfig`
  - `enabled`
  - `min_edge_ticks`
  - `cooldown_ms`
  - `scan_interval_ms`
  - `cleanup_timeout_ms`
  - `max_order_volume`
  - `max_live_orders`
  - `cleanup_on_partial`
- `products[].arbitrage_core`
- `products[].arbitrage_strategies[]`

### Engine Integration

The engine now includes:

- per-product `arb_threads_`
- per-product `arb_intent_buf_`
- per-product `arb_event_buf_`
- per-product `AtomicArbParams`
- per-product arbitrage strategy slots

The startup sequence now:

1. Builds the normal MM strategies.
2. Builds configured arbitrage strategies.
3. Starts the gateway dispatcher and pricer.
4. Starts the OMM strategy thread(s).
5. Starts the arb sidecar thread(s) for products that have arbitrage configured.

### Dispatch Priority

The gateway dispatcher keeps MM first:

1. gateway callbacks
2. MM orders
3. MM quotes
4. arbitrage intents

Arbitrage intent draining is burst-capped so it cannot starve the dispatch loop.

## PCP Strategy Design

### Strategy Scope

`PCPArbitrageStrategy` is the first implementation of `IArbitrageStrategy`.

It is:

- per-product
- snapshot-driven
- order-only
- IOC/taker-style in spirit
- cleanup-capable after partial basket execution

### Pair Registry

At initialization, PCP builds a per-product registry of valid parity sets:

- `(expiry_date, strike)` -> `{call_id, put_id, future_id}`

The registry uses the existing instrument metadata and does not require locks or heap work on the OMM thread.

### Opportunity Logic

PCP evaluates two directions:

1. long synthetic / short future
2. short synthetic / long future

It triggers only when:

- all three legs have valid top-of-book data
- market data is fresh
- executable size is positive on all legs
- edge exceeds `min_edge_ticks`
- local per-strategy live-order and risk checks pass

### Execution and Cleanup

When PCP fires:

- it submits three orders, one per leg
- order ids are tagged as arbitrage-owned
- fills, cancels, rejects, and acks route back to the arb thread

If the basket fills fully, the attempt completes with no cleanup.

If the basket fills partially and `cleanup_on_partial` is enabled:

- PCP calculates the residual filled inventory by leg
- PCP submits cleanup orders to flatten that residual
- cleanup stays isolated from MM quote ownership

## Control Surface

### gRPC

Added independent arbitrage control RPCs:

- `SetArbStrategyParams`
- `StartArbStrategy`
- `StopArbStrategy`

Added snapshot payloads:

- `arb_params`
- `arb_strategy_states`

Existing MM control RPCs remain MM-only and backward-compatible.

### GUI

The trader GUI now includes:

- an arbitrage strategy selector for the selected product
- independent `Start Arb` / `Stop Arb` controls
- live arbitrage state display:
  - enabled/running/gated
  - pair count
  - live orders
  - last edge
  - last trigger edge
  - last evaluation timestamp
  - suppress reason

MM controls remain separate and unchanged in intent.

## Latency Protections

The design explicitly protects OMM latency:

- arbitrage does not run on the OMM strategy thread
- arbitrage does not own quotes
- arbitrage dispatch is burst-limited
- arb event handling is isolated from MM callback handling
- the arb loop now yields when idle instead of busy-spinning

During implementation, the signal queue depth metric was also tightened so backpressure tests remain stable without changing MM scheduling semantics.

## Test Plan

Implemented validation covers:

- PCP pair construction and execution behavior
- full basket fill with no cleanup
- partial basket fill with cleanup
- MM engine regressions
- signal backpressure behavior

Key tests:

- `test_pcp_arbitrage`
- `test_option_mm_core`
- `test_simple_mm`

## Validation Results

Validated in `build-wsl` on 2026-04-19:

```bash
./test_simple_mm
./test_pcp_arbitrage
./test_option_mm_core
```

Results:

- `test_simple_mm`: passed
- `test_pcp_arbitrage`: passed
- `test_option_mm_core`: passed

## Remaining Notes

- PCP is the only arbitrage strategy implemented in v1.
- Cross-product arbitrage is not included.
- Arbitrage remains execution-only; no resting arb quote model is implemented.
- The Windows GUI build environment in this workspace still needs a valid MSVC standard-library setup before GUI compilation can be fully verified there.
