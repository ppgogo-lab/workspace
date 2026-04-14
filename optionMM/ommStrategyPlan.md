# optionMM Strategy Plan

## Goal

Build a production-grade core option market making strategy for `optionMM` that is:

- ultra-low latency on the hot path
- safe and predictable in real trading
- maintainable without recreating the old `optionMMStrategy` class complexity

This is a behavioral rewrite informed by `optionMMStrategy`, not a class-by-class port.

## Scope

V1 includes:

- theo-driven option quoting
- market-follow adjustment
- inventory-aware skew and one-sided quoting
- quote widening and suppression under unstable conditions
- quote lifecycle management
- product-level supervisory risk gating

V1 excludes:

- PCP arbitrage
- vega-order workflow
- RFQ flow
- strategy-owned vol fitting
- strategy-owned base-offset learning
- dedicated hedge-order subsystem

Risk reduction in V1 is done through quote shaping, reducing-side-only quoting, and suppression.

## Implemented Design Changes

The current `optionMM` code now reflects these major design changes:

- added `option_mm_core` as the main low-latency per-product strategy
- moved gateway and timer callbacks onto the owning strategy thread through per-product SPSC queues
- reduced hot-path `PricingSignal` to a compact 64-byte contract
- kept full `Greeks` snapshots only for supervisory risk and monitoring
- made local strategy state the owner of quote lifecycle and immediate inventory reaction
- reduced `PreTradeRisk::check_quote()` to hard validation instead of quote lifecycle ownership
- added direct tests for `option_mm_core`, compact-signal handling, and the revised risk boundary

## Current Design Decisions

The next implementation pass should follow these decisions:

1. pricer must generate real asymmetric `theo_bid` and `theo_ask`
2. product-level aggregate `delta + vega` breach must fully suppress the whole product
3. product-level risk gating should use `delta + vega`, not gamma
4. quote lifecycle should use stricter state handling with explicit replace/cancel intent
5. requote policy should prioritize minimum churn
6. rapid underlying move should suppress quoting briefly
7. `hedge_delta_threshold` should trigger immediate hedge orders
8. large-product repricing should rotate fairly across products

## Non-Negotiable Design Corrections

The original engine had three issues that needed correction:

1. Single-thread strategy ownership is currently violated.
   `IMarketMaker` says all `on_*` methods run on the strategy thread, but the gateway dispatcher and timer threads call strategy methods directly today.
   Status: fixed. Gateway and timer events are now routed into per-product strategy queues.

2. Quote lifecycle is not represented in pre-trade risk state.
   `PreTradeRisk::check_quote()` validates quotes, but quote acknowledgements and quote cancellations are not fed back into `PreTradeRisk`.
   That meant quote lifecycle and hard-risk state were mixed.
   Status: partially corrected. Quote lifecycle now lives in strategy state, while `PreTradeRisk` remains a hard-check module.

3. The pricer silently limits repricing to 128 options per product.
   That is not acceptable for a production option book.
   Status: fixed. Repricing now iterates the full product option set in bounded batches.

## Core Architecture

Keep the existing high-level engine:

- feed thread
- pricer thread
- one strategy thread per product
- gateway dispatcher thread
- vol fitter thread
- risk monitor thread
- timer thread

Do not port the old `optionMakerMaster -> UnderlyingExecutor -> OptionExecutor -> Calculator` object graph.
Instead, use one flat per-product strategy with fixed-size state arrays.

### Strategy Ownership Model

Each product strategy thread is the sole owner of:

- per-option quote state
- per-option local position state
- product-level local aggregates used for immediate quote decisions
- pending quote/order lifecycle state

The risk monitor remains a separate supervisory component.
Its outputs are treated as kill-switch style soft-risk flags, not as the strategy's primary source of inventory truth.

## Event Model

Do not use a single MPSC queue into the strategy thread.
For latency and maintainability, use separate SPSC queues per producer:

- `signal_buf_[product]` for pricer -> strategy
- `gateway_event_buf_[product]` for gateway dispatcher -> strategy
- `timer_buf_[product]` for timer -> strategy

The strategy loop polls these queues in a fixed priority order:

1. gateway events
2. timer events
3. pricing signals

This preserves single-writer queues, keeps ownership clear, and avoids cross-thread strategy mutation.

### Required Event Handling Rules

- fills, acks, cancels, rejects, and timer events must be routed into the owning strategy thread, never invoked directly from other threads
- pricing signals may be dropped only under explicit backpressure policy
- gateway events may not be dropped
- timer events should be coalescible, not queued unboundedly

## Hot-Path Data Model

Use fixed-size arrays indexed by `instrument_id`.
No maps, no heap allocation, no `shared_ptr`, no string-based lookups on the hot path.

### ProductMMState

Per product:

- underlying instrument id
- dense list of option ids
- active option count
- session state
- quote-enable flag
- local aggregate delta, vega, net position
- supervisory risk flags snapshot
- refresh timestamps

### OptionMMState

Per option:

- `instrument_id`
- `underlying_id`
- last theo bid/ask and core Greeks used by strategy
- latest known option market top-of-book snapshot
- local net position and traded volume counters
- live bid/ask quote price and volume
- quote state
- last quote timestamp
- suppress flags
- stale/freshness timestamps

### QuoteDecision

Stack-only temporary object:

- action
- bid price
- ask price
- bid volume
- ask volume
- reason flags

### QuoteState

- `Idle`
- `Live`
- `ReplacePending`
- `CancelPending`
- `Suppressed`

### SuppressFlags

Bitmask:

- stale theo
- stale option market
- invalid market
- crossed market
- no edge
- throttled
- position breach
- product risk breach
- session closed

## Signal Contract

`PricingSignal` is now a compact strategy-facing payload rather than an embedded full `Greeks` or `MarketTick` object.

Current signal contract:

- `instrument_id`
- `underlying_id`
- `flags`
- `sequence_no`
- `calc_ts_ns`
- `theo_bid`
- `theo_ask`
- `delta`
- `vega`
- `underlying_ref_bid`
- `underlying_ref_ask`

Current design rules:

- full `Greeks` remain in `greeks_snapshot_` for `PostTradeRisk` and monitoring only
- option top-of-book is read from shared `tick_snapshot_` on the strategy side
- no full market tick is copied into the strategy queue
- the current implementation uses equal `theo_bid` and `theo_ask` from the pricer, leaving room for later separate bid/ask theo logic without changing the queue contract

## Quote Decision Logic

### Quote Center

Quote center is derived from:

1. theo from pricer
2. market-follow adjustment from current option top of book
3. instrument-level inventory skew
4. product-level suppression gate, not quote-center skew

The strategy must not recompute pricing models or vol fits.

### Quote Width

Width starts from configured base spread and is widened by:

- market width
- stale or unstable option market
- near-limit inventory

Width is clamped between configured min and max.

Product-level aggregate `delta + vega` breach should not just widen quotes. It should suppress the whole product and cancel live quotes.

### Quote Eligibility

Suppress or reject quoting when:

- theo is invalid
- option market is stale or unusable
- session is closed
- position limit is breached
- product-level aggregate `delta + vega` breach is active

On aggregate product `delta + vega` breach, the strategy should cancel live quotes and suppress the whole product until recovery conditions are met.

Near limits, switch to reducing-side-only quoting before full suppression.

### Underlying Shock Handling

Rapid underlying movement should not just widen quotes in v1.
It should suppress quoting briefly for the affected product, then allow quoting to resume after a short cooldown if the market stabilizes.

### Requote Policy

Avoid quote churn aggressively.
Only update if one of these is true:

- quote price moved more than `requote_price_epsilon_ticks`
- quoted volume changed materially
- quote state changed
- quote refresh timer fired and policy requires a refresh

Also enforce `min_quote_interval_ns` or equivalent.

## Risk Model

Split risk into two layers.

### Local Strategy Risk

Used for immediate quote decisions:

- per-option local position
- product-local aggregate delta + vega exposure
- quote suppression thresholds

This state is updated immediately on the owning strategy thread from routed fills.

### Supervisory Risk

Handled by `PostTradeRisk`:

- independent portfolio check
- breach flags for product/global suppression
- product-level `delta + vega` breach should suppress the whole product, not just widen quotes
- operator and monitoring visibility

Supervisory risk is not the source of truth for the next quote on the hot path.
It is an asynchronous guardrail.

## Order And Quote Lifecycle

Do not keep quote lifecycle hidden inside `PreTradeRisk`.
Split responsibilities cleanly:

- `HardRiskChecker`
  stateless or near-stateless checks such as max order size and price validity
- `LiveOrderBookState`
  strategy-owned lifecycle state for active quotes and orders

This avoids coupling production quote semantics to the current order-only `PreTradeRisk` implementation.

Current implementation status:

- strategy state owns live quote id, live prices, live sizes, suppress state, and requote timing
- `PreTradeRisk::check_quote()` now performs hard validation only
- `PreTradeRisk` still tracks open orders for order-side checks
- a later cleanup can rename or split `PreTradeRisk` more explicitly, but the ownership boundary is already improved

The strategy must explicitly track:

- live quote ids
- pending replace/cancel state
- last acknowledged quote
- fill-driven quote invalidation

## Latency Rules

The hot path must obey these rules:

- no heap allocation
- no locks
- no logging except counters
- no dynamic polymorphism inside the quote loop
- no full-book scans per signal unless bounded and justified
- no string work
- no blocking calls

Precompute at startup:

- option metadata
- option-to-product mapping
- option-to-underlying mapping
- tick size
- strike
- option type
- any static quoting coefficients

Keep hot and cold fields separated in memory if profiling shows cache pressure.

## Maintainability Rules

Maintainability matters because low-latency code rots quickly if all logic is fused together.

Use three clear layers:

1. engine transport layer
   queues, threads, routing, dispatcher
2. strategy state layer
   product/option state, lifecycle, suppress flags
3. quote policy layer
   pure functions that transform state snapshots into `QuoteDecision`

Do not mix exchange callbacks, config parsing, monitoring, and quote math in one class.

Prefer:

- POD structs for state
- small pure helper functions for quote math
- explicit ownership boundaries
- one authoritative place for lifecycle state

Avoid:

- deep inheritance
- executor trees
- duplicated state across modules
- hidden state transitions

## Parameter Model

Replace the current simple parameter set with MM-specific controls:

- `base_half_spread_ticks`
- `min_half_spread_ticks`
- `max_half_spread_ticks`
- `quote_volume`
- `warning_position`
- `max_position`
- `inventory_skew_per_lot_ticks`
- `follow_weight`
- `requote_price_epsilon_ticks`
- `min_quote_interval_ms`
- `market_width_widen_threshold_ticks`
- `underlying_move_widen_threshold_ticks`
- `use_one_sided_at_limits`
- `enabled`

These remain atomically readable by the strategy thread.

## Engine Changes Required

Completed:

1. Added `gateway_event_buf_[MAX_PRODUCTS]`
2. Added `timer_buf_[MAX_PRODUCTS]`
3. Routed gateway callbacks into product queues instead of direct strategy calls
4. Routed timer events into product queues instead of direct strategy calls
5. Chunked pricer batch output across all product options
6. Shrunk `PricingSignal`
7. Added strategy type `option_mm_core`

Next engine changes:

1. derive separate `theo_bid` / `theo_ask` instead of using a single pricer mid
2. reduce strategy-loop wakeup overhead further under idle conditions
3. add explicit quote-ack / cancel-ack event types for stricter lifecycle modeling
4. implement product-level `delta + vega` whole-product suppression
5. add brief underlying-shock suppression windows
6. add immediate hedge behavior on threshold breach
7. rotate repricing fairly across products

## Test And Benchmark Plan

### Functional

- valid theo generates two-sided quote
- invalid or stale state suppresses quote
- inventory skew changes quote center correctly
- reducing-side-only quoting activates near limits
- product-level delta + vega breach suppresses the whole product
- quote lifecycle transitions are correct on ack, fill, cancel, reject

### Concurrency

- strategy state is only mutated on the strategy thread
- no direct cross-thread `on_*` calls remain
- gateway and timer routing preserve order within each producer stream

### Performance

- no allocations on hot path
- repricing covers full option set for products larger than 128 options
- measure:
  - tick to pricing signal
  - signal to strategy decision
  - strategy decision to gateway send
  - fill to local position update
- track p50, p99, and p99.9

### Regression

- quote churn stays below configured thresholds in stable markets
- session open/close behavior is deterministic
- supervisory risk flags suppress without corrupting local strategy state

## Rollout Order

Completed:

1. fixed event ownership and queue topology
2. shrank pricing signal and cleaned state boundaries
3. implemented `option_mm_core` with flat state arrays
4. split hard-risk checks from quote lifecycle state
5. added product-level quote shaping and suppression

Next:

1. implement asymmetric `theo_bid` / `theo_ask` generation in the pricer
2. tighten lifecycle modeling for quote replace and cancel acknowledgements
3. implement product-level `delta + vega` whole-product suppression
4. add brief underlying-shock suppression windows
5. add immediate hedge behavior on threshold breach
6. rotate repricing fairly across products
7. benchmark tail latency under full-product repricing bursts
8. only after that, consider adding new strategy features

## Final Principle

For this project, the best production design is not the most feature-rich and not the most abstract.
It is a flat, single-owner, array-based strategy with explicit state transitions, minimal hot-path data movement, and slow-path controls kept outside the quoting loop.

