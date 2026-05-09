# Chi Design Review Findings

This file records research notes for the cross-repo option market making design review.

## Refactor Applied

- Added lightweight quote lifecycle hooks to `BaseQuotingStrategy` instead of adding a service layer. Strategies can update monitor mirrors, publish alerts, or immediately reevaluate after a deferred replace/cancel lifecycle transition.
- Moved OptionMMCore cancel give-up alerting behind the base lifecycle path. This keeps stale-theo cancels, risk/session cancels, cancel retries, and give-up alerts consistent.
- Implemented base order cancel intent using the existing `Order` ring buffer plus an `is_cancel` flag. The gateway dispatcher translates that intent into `IGateway::cancel_order()` directly.
- Kept lifecycle objects fixed-size and stack/ring-buffer based; no new heap allocation or cross-thread service dependency was added to the hot path.

## Chi Production Design

- `public/inc/OptionQuoteAlgo.h` is a broad shared strategy interface: strategy callbacks receive market data, RFQ, greeks, params, orders, quotes, trades, maker-quote-instrument operations, autoquote operations, hedge PnL, and can call execution/subscription/timer helpers on the engine.
- `marketmakerservice/src/OptionMakerService.cpp` owns service integration: dynamic strategy loading through `StrategyLoader`, wiring memory DB triggers, registering engines, high-priority execution messages, and routing events to `OptionQuoteEngine`.
- `engine/src/OptionQuoteEngine.cpp` owns the option MM lifecycle: global autoquote start/stop, per-instrument active state, RFQ timers, maker quote statics, cancel-all, maker quote instrument status notification, quote/order execution, and callbacks into `OptionQuoteAlgo`.
- `engine/src/QuoteEngine.cpp` owns shared quote/order mechanics: calls strategy callbacks, executes returned quote vectors, handles pending replace/cancel behavior, and retries cancel-all through `setCancelAllOrderQuoteMap`.
- `tradeservice/src/TradeService.cpp` is independent gateway/order lifecycle infrastructure: subscribes to order/quote/trade request and API callback messages, initializes gateway manager and trade engine, persists memory DB state, and sends order/trade/quote callbacks back to other services.
- `optionMMStrategy/src/optionMaker/optionMakerMaster.cpp` uses `OptionQuoteAlgo` as a dispatcher. It maps instruments to product ids, creates one `OmmWorker` per product, subscribes market data, builds option-series/underlying/related-option maps, and forwards market/order/quote/trade/RFQ/greeks events to workers.

## Current optionMM Design

- `include/strategy/mm_framework.h` exposes a narrower per-product `IMarketMaker`: pricing signal, fill, quote/order ack/cancel/reject, timer, monitor snapshots.
- `include/strategy/base_quoting_strategy.h` and `src/strategy/base_quoting_strategy.cpp` already centralize quote lifecycle and order lifecycle tracking through final callbacks and subclass hooks.
- `include/strategy/quote_lifecycle.h` defines a reusable quote finite state machine with direct replace vs cancel-first policy, pending/live/cancel states, cancel retry, material-change gating, and fill integration.
- `include/strategy/order_lifecycle.h` tracks order pending/live/cancel/fill/reject states, but base `cancel_order()` is currently a stub.
- `src/engine/strategy_worker.cpp` serializes gateway callbacks, timers, and pricing signals onto the product strategy thread.
- `src/engine/gateway_dispatcher_worker.cpp` is the central execution service: drains strategy order/quote buffers, calls gateway send/cancel, tracks live state, publishes monitor/persistence events, and routes gateway callbacks back to strategy threads.
- `src/strategy/option_mm_core.cpp` implements actual product MM logic: option selection, quoting decisions, suppress reasons, hedging, product risk gates, monitor mirrors. It relies on `BaseQuotingStrategy` for quote/order lifecycle.

## Main Comparison

- Current optionMM has a cleaner low-latency architecture than chi for hot-path threading: per-product strategy threads, ring buffers, no broad memory DB dependency in strategy callbacks.
- Chi has a more complete trader-facing lifecycle model: dynamic strategy loading, explicit strategy setting events, per-instrument maker quote activation, autoquote lifecycle callbacks, subscription helpers, RFQ lifecycle, and cancel-all/cancel-again semantics are first-class.
- Current optionMM lifecycle is cleaner internally but less complete at the extension boundary. A new trader strategy must understand engine buffers and protected base helpers, and there is no chi-like strategy context/facade that exposes only safe operations.
- Current optionMM quote lifecycle is stronger than chi in encapsulation, but follow-up actions from lifecycle events are incomplete: `BaseQuotingStrategy` computes `request_requote` but does not trigger a strategy hook or schedule a reevaluation.
- Current optionMM order lifecycle is weaker than quote lifecycle: hedge/arb/manual cancels go around strategy base logic, and `BaseQuotingStrategy::cancel_order()` is not implemented.
- Chi’s production strategy decomposition is useful: `optionMakerMaster` is a dispatcher, `OmmWorker` owns product state, and lower executors own underlying/series logic. optionMM currently has most product logic concentrated in `OptionMMCoreStrategy`.
