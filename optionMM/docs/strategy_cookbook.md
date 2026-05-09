# Strategy Cookbook

This cookbook is for traders implementing a new option market making strategy in
`optionMM`. The design target is low latency first, with simple ownership rules:
one strategy instance per product, one strategy thread per product, fixed-size
state, ring-buffer outputs, and lifecycle management handled by the base class.

## Mental Model

The hot path is:

1. Market data reaches the pricer.
2. The pricer emits `PricingSignal` messages for option instruments.
3. `TradingEngine::strategy_loop()` calls the product strategy on its dedicated
   strategy thread.
4. The strategy decides quote/order intent and calls protected helpers on
   `BaseQuotingStrategy`.
5. The gateway dispatcher drains quote/order ring buffers, talks to the gateway,
   and routes acks, cancels, rejects, and fills back to the same strategy thread.

Trader strategy code should only own pricing, sizing, suppression, and hedge
policy. Do not reimplement quote ack/cancel/reject state transitions in the
strategy. Use `BaseQuotingStrategy`.

## Files To Start From

- `include/strategy/base_quoting_strategy.h`: base class and lifecycle hooks.
- `src/strategy/base_quoting_strategy.cpp`: shared quote/order lifecycle.
- `include/strategy/simple_mm.h` and `src/strategy/simple_mm.cpp`: minimal
  strategy example.
- `include/strategy/option_mm_core.h` and `src/strategy/option_mm_core.cpp`:
  production-style option MM example with suppression, monitor state, and hedge
  logic.
- `include/strategy/template_mm_strategy.h`: older template. It is useful as a
  skeleton, but prefer the lifecycle contract documented here.

## Strategy Contract

Derive from `BaseQuotingStrategy`, not directly from `IMarketMaker`, unless you
are intentionally replacing the lifecycle model.

Implement these required hooks:

```cpp
void on_signal_impl(const PricingSignal& signal) noexcept override;
void on_fill_impl(const Trade& trade) noexcept override;
void on_timer_impl(const TimerEvent& event) noexcept override;
```

Optionally implement these lifecycle hooks:

```cpp
void on_quote_lifecycle_update(uint16_t instrument_id,
                               int64_t now_ns,
                               bool reevaluate) noexcept override;

void on_quote_cancel_give_up(uint16_t instrument_id,
                             const QuoteLifecycleState& state,
                             int64_t now_ns) noexcept override;
```

The public `on_signal`, `on_fill`, `on_quote_ack`, `on_quote_cancel`,
`on_quote_reject`, `on_order_ack`, `on_order_cancel`, `on_order_reject`, and
`on_timer` callbacks are final in the base class. This is deliberate: the base
class updates lifecycle state first, then calls trader hooks.

## Initialization

Your strategy needs an `init()` method that wires engine-owned dependencies and
marks active instruments for this product.

```cpp
void MyStrategy::init(uint8_t product_idx,
                      SPSCRingBuffer<Quote, 512>* quote_buf,
                      SPSCRingBuffer<Order, 512>* order_buf,
                      PreTradeRisk* pre_risk,
                      AtomicMMParams* params,
                      const Instrument* instruments) noexcept {
    product_idx_ = product_idx;
    quote_buf_ = quote_buf;
    order_buf_ = order_buf;
    pre_risk_ = pre_risk;
    params_ = params;
    instruments_ = instruments;

    for (uint16_t id = 0; id < MAX_INSTRUMENTS; ++id) {
        const Instrument& instr = instruments_[id];
        if (instr.instrument_id == INVALID_INSTRUMENT_ID) continue;
        if (instr.product_index != product_idx_) continue;
        if (instr.kind != InstrumentKind::Option) continue;

        instrument_state_[id].active = true;
        instrument_state_[id].instrument_id = id;
        instrument_state_[id].quote_lifecycle.replace_policy =
            QuoteLifecycleController::policy_for_exchange(instr.exchange);
    }
}
```

Keep all per-instrument state in fixed arrays indexed by `instrument_id`.
Avoid maps, heap allocation, strings, logging, and locks in the hot path.

## Quoting

Use `request_quote()` for all quote sends and replaces:

```cpp
void MyStrategy::on_signal_impl(const PricingSignal& signal) noexcept {
    if (!params_ || !params_->enabled.load(std::memory_order_relaxed)) return;

    const uint16_t id = signal.instrument_id;
    if (id >= MAX_INSTRUMENTS || !instrument_state_[id].active) return;

    const int64_t now_ns = get_monotonic_ns();
    if (now_ns - signal.calc_ts_ns > 100'000'000LL) {
        request_cancel(id, now_ns);
        return;
    }

    const Instrument& instr = instruments_[id];
    const double tick = instr.tick_size > 0.0 ? instr.tick_size : 0.01;
    const double mid = 0.5 * (signal.theo_bid + signal.theo_ask);
    if (mid <= 0.0 || signal.theo_bid <= 0.0 || signal.theo_ask < signal.theo_bid) {
        request_cancel(id, now_ns);
        return;
    }

    const double half_spread =
        params_->bid_spread.load(std::memory_order_relaxed) * 0.5;
    const double bid = std::floor((mid - half_spread) / tick) * tick;
    const double ask = std::ceil((mid + half_spread) / tick) * tick;
    if (bid <= 0.0 || ask <= bid) {
        request_cancel(id, now_ns);
        return;
    }

    const Volume size = params_->quote_volume.load(std::memory_order_relaxed);
    request_quote(id, bid, ask, size, size, now_ns);
}
```

`request_quote()` handles:

- pre-trade risk check,
- material-change throttling,
- direct replace where allowed,
- cancel-first replace for venues that require it,
- ack/cancel pending gating,
- cancel retry and cancel give-up lifecycle.

## Cancelling Quotes

Use `request_cancel(instrument_id, now_ns)` when you no longer want a quote.
Common reasons are stale theo, invalid market, session close, product exposure
breach, or instrument risk suppression.

Do not create zero-volume quote cancels directly in trader code. The base class
knows whether the active target is a live quote or a pending quote and prevents
too-fast cancel retries.

If your strategy needs to refresh monitor state after lifecycle changes:

```cpp
void MyStrategy::on_quote_lifecycle_update(uint16_t instrument_id,
                                           int64_t now_ns,
                                           bool reevaluate) noexcept {
    update_monitor_state(instrument_id);
    if (reevaluate) {
        maybe_quote(instrument_id, now_ns);
    }
}
```

`reevaluate == true` means the base lifecycle just finished an ack/cancel/reject
transition and a deferred quote decision should be recomputed immediately.

If cancels exhaust retries:

```cpp
void MyStrategy::on_quote_cancel_give_up(
        uint16_t instrument_id,
        const QuoteLifecycleState& state,
        int64_t now_ns) noexcept {
    publish_alert(instrument_id, state.cancel_target_quote_id,
                  state.cancel_attempts, now_ns);
}
```

After `CancelFailed`, new quote sends are blocked for that instrument until the
quote is fully filled or operator action clears the condition.

## Orders And Hedges

Use `submit_order()` for hedge or auxiliary orders owned by the strategy:

```cpp
Order hedge{};
hedge.client_order_id = next_order_id();
hedge.instrument_id = underlying_id;
hedge.product_index = product_idx_;
hedge.side = Side::Sell;
hedge.order_type = OrderType::Market;
hedge.volume = hedge_qty;
hedge.is_hedge = true;
hedge.send_ts = now_ns;

const OrderId order_id = submit_order(hedge);
```

Use `cancel_order(order_id)` for tracked strategy orders. The base class creates
a lightweight cancel intent on the existing order ring buffer. The gateway
dispatcher translates it into `IGateway::cancel_order()` and normal order cancel
callbacks return through the strategy thread.

Do not call `gateway_->cancel_order()` from strategy code. Strategy code should
not hold a gateway pointer.

## Fill Handling

`on_fill_impl()` is called after the base class has updated quote/order lifecycle
and `instrument_state_[id].net_position`.

Use it for strategy-specific accounting:

```cpp
void MyStrategy::on_fill_impl(const Trade& trade) noexcept {
    if (trade.instrument_id >= MAX_INSTRUMENTS) return;

    const int32_t sign = trade.side == Side::Buy ? 1 : -1;
    product_delta_ += static_cast<double>(sign * trade.fill_volume) * last_delta_[trade.instrument_id];
    product_vega_ += static_cast<double>(sign * trade.fill_volume) * last_vega_[trade.instrument_id];
}
```

Keep this function small. Expensive PnL, database writes, and monitor formatting
belong outside the strategy hot path.

## Timer Handling

Use timer events for work that is not directly triggered by pricing signals:

- `HedgeCheck`: evaluate hedge thresholds and submit hedge orders.
- `QuoteRefresh`: periodically reevaluate quote staleness or live-age policy.
- `SessionClose`: call `request_cancel()` for active option instruments.
- `SessionOpen`: reset session gate state and allow quoting again.

Avoid scanning all instruments on every market signal. Full-product scans are
acceptable on timers or product regime transitions.

## Registration

Add your strategy type in `TradingEngine::init_strategies()`:

```cpp
if (std::strncmp(cfg_.products[i].strategy_type, "my_strategy",
                 sizeof(cfg_.products[i].strategy_type)) == 0) {
    auto* s = new MyStrategy();
    s->init(static_cast<uint8_t>(i),
            &quote_buf_[i],
            &order_buf_[i],
            &pre_risk_[i],
            &mm_params_[i],
            instruments_);
    strategies_[i].reset(s);
}
```

Then set the product config:

```yaml
products:
  - underlying_id: cu2501
    strategy_type: my_strategy
```

If your strategy needs snapshots, post-trade risk, or alert topics, follow the
`OptionMMCoreStrategy::init()` pattern and pass those dependencies explicitly.

## Hot-Path Rules

- Use fixed arrays indexed by `instrument_id`.
- Use `std::memory_order_relaxed` for parameter atomics unless you need ordering.
- Read runtime params once per decision and keep local copies.
- Round prices to tick before calling `request_quote()`.
- Let `PreTradeRisk` reject unsafe orders/quotes through base helpers.
- Keep strategy hooks `noexcept`; never throw.
- Do not allocate, lock, log, format strings, or touch the database in signal
  handling.
- Do not publish monitor text from the quote decision path. Store compact state
  and let monitor-side code format it.
- Prefer product-level regime gates over repeated per-instrument expensive checks.
- For production FEMAS latency runs, use `config/low_latency_femas.yaml` as the
  starting point: hot-path monitoring off, persistence off, low-latency spin on,
  and `pricing.hot_path_greeks_mode: compact`.
- Do not depend on every market signal refreshing the full Greeks snapshot. In
  compact/off modes the strategy gets fresh `PricingSignal` values immediately,
  while full Greeks are maintained by the cold refresh path for monitoring/risk.

## Testing Checklist

Add focused tests for every new strategy behavior:

- valid signal produces one quote,
- disabled/stale/invalid inputs do not quote,
- risk or position gate cancels existing quote,
- live quote replacement follows direct or cancel-first venue policy,
- deferred requote occurs after cancel/ack when needed,
- cancel retry reaches give-up alert after the configured attempts,
- fills update product exposure and hedge state,
- session close cancels all active quotes.

For engine integration tests, remember that the current pricer model emits
option pricing signals from underlying future ticks. Option ticks update market
snapshots and monitoring, but they do not directly trigger option repricing.

## Minimal Implementation Order

1. Copy `SimpleMMStrategy` into `MyStrategy`.
2. Implement `init()` and mark active option instruments.
3. Implement `on_signal_impl()` with quoting and suppression policy.
4. Implement `on_fill_impl()` for exposure accounting.
5. Implement `on_timer_impl()` for hedging and session actions.
6. Add lifecycle hooks only if you need monitor mirrors, deferred requote, or
   alerts.
7. Register `strategy_type` in `TradingEngine::init_strategies()`.
8. Add focused strategy tests before running full engine integration.
