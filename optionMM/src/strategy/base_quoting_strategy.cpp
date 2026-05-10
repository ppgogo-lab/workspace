#include "strategy/base_quoting_strategy.h"

#include <algorithm>
#include <cmath>

namespace omm {

// ─── Lifecycle Management Helpers ─────────────────────────────────────────────

/**
 * @brief Implements Request quote.
 * @param instrument_id Parameter supplied by the caller.
 * @param bid Parameter supplied by the caller.
 * @param ask Parameter supplied by the caller.
 * @param bid_vol Parameter supplied by the caller.
 * @param ask_vol Parameter supplied by the caller.
 * @param now_ns Parameter supplied by the caller.
 * @return None.
 * @note Noexcept API preserves hot-path failure and latency invariants.
 */
void BaseQuotingStrategy::request_quote(uint16_t instrument_id,
                                        double bid, double ask,
                                        Volume bid_vol, Volume ask_vol,
                                        int64_t now_ns) noexcept {
    if (instrument_id >= MAX_INSTRUMENTS) return;
    InstrumentState& state = instrument_state_[instrument_id];
    if (!state.active) return;

    // Lifecycle management runs first to handle timeouts and retries
    if (manage_quote_lifecycle(instrument_id, now_ns)) return;

    // Check if we're waiting for a quote update (ack or cancel)
    if (QuoteLifecycleController::waiting_for_quote_update(state.quote_lifecycle)) {
        return;
    }

    // Build intent for material change detection
    const QuoteLifecycleIntent intent{true, bid, ask, bid_vol, ask_vol};

    // Check if this quote differs enough from the live quote to warrant sending
    const Instrument& instr = instruments_[instrument_id];
    const double tick = instr.tick_size > 0.0 ? instr.tick_size : 0.01;
    const double epsilon_px = params_
        ? params_->requote_price_epsilon_ticks.load(std::memory_order_relaxed) * tick
        : tick;
    const int64_t min_interval_ns = params_
        ? static_cast<int64_t>(params_->min_quote_interval_ms.load(std::memory_order_relaxed) * 1'000'000.0)
        : 100'000'000LL;

    if (!QuoteLifecycleController::is_material_change(state.quote_lifecycle,
                                                      intent,
                                                      epsilon_px,
                                                      min_interval_ns,
                                                      now_ns)) {
        return;
    }

    send_quote_internal(instrument_id, bid, ask, bid_vol, ask_vol, now_ns);
}

/**
 * @brief Implements Request cancel.
 * @param instrument_id Parameter supplied by the caller.
 * @param now_ns Parameter supplied by the caller.
 * @return None.
 * @note Noexcept API preserves hot-path failure and latency invariants.
 */
void BaseQuotingStrategy::request_cancel(uint16_t instrument_id,
                                         int64_t now_ns) noexcept {
    if (instrument_id >= MAX_INSTRUMENTS) return;
    InstrumentState& state = instrument_state_[instrument_id];
    if (!state.active) return;

    if (manage_quote_lifecycle(instrument_id, now_ns)) return;

    send_cancel_internal(instrument_id, now_ns);
}

/**
 * @brief Implements Submit order.
 * @param order Parameter supplied by the caller.
 * @return Return value produced by the operation.
 * @note Noexcept API preserves hot-path failure and latency invariants.
 */
OrderId BaseQuotingStrategy::submit_order(const Order& order) noexcept {
    if (!order_buf_) return 0;

    // Pre-trade risk check
    if (pre_risk_ && pre_risk_->check_order(order) != PreTradeRisk::RejectReason::OK) {
        return 0;
    }

    // Push to ring buffer (non-blocking)
    if (!order_buf_->try_push(order)) {
        return 0;
    }

    // Allocate tracker and record submission
    OrderLifecycleTracker* tracker = allocate_order_tracker();
    if (tracker) {
        OrderLifecycleController::note_order_submitted(*tracker, order, get_monotonic_ns());
    }

    return order.client_order_id;
}

/**
 * @brief Implements Cancel order.
 * @param id Parameter supplied by the caller.
 * @return None.
 * @note Noexcept API preserves hot-path failure and latency invariants.
 */
void BaseQuotingStrategy::cancel_order(OrderId id) noexcept {
    if (!order_buf_ || id == 0) return;

    OrderLifecycleTracker* tracker = find_order_tracker(id);
    if (!tracker || !OrderLifecycleController::is_live(*tracker)) {
        return;
    }

    Order cancel{};
    cancel.client_order_id = id;
    cancel.instrument_id = tracker->instrument_id;
    cancel.product_index = product_idx_;
    cancel.side = tracker->side;
    cancel.price = tracker->price;
    cancel.volume = tracker->remaining_volume;
    cancel.send_ts = get_monotonic_ns();
    cancel.is_hedge = tracker->is_hedge;
    cancel.is_cancel = true;

    if (order_buf_->try_push(cancel)) {
        OrderLifecycleController::note_cancel_submitted(*tracker);
    }
}

// ─── State Access ─────────────────────────────────────────────────────────────

/**
 * @brief Implements Get quote state.
 * @param instrument_id Parameter supplied by the caller.
 * @return Return value produced by the operation.
 * @note Noexcept API preserves hot-path failure and latency invariants.
 */
const QuoteLifecycleState* BaseQuotingStrategy::get_quote_state(
    uint16_t instrument_id) const noexcept {
    if (instrument_id >= MAX_INSTRUMENTS) return nullptr;
    const InstrumentState& state = instrument_state_[instrument_id];
    if (!state.active) return nullptr;
    return &state.quote_lifecycle;
}

/**
 * @brief Implements Get order tracker.
 * @param id Parameter supplied by the caller.
 * @return Return value produced by the operation.
 * @note Noexcept API preserves hot-path failure and latency invariants.
 */
const OrderLifecycleTracker* BaseQuotingStrategy::get_order_tracker(
    OrderId id) const noexcept {
    return const_cast<BaseQuotingStrategy*>(this)->find_order_tracker(id);
}

// ─── IMarketMaker Implementation ──────────────────────────────────────────────

/**
 * @brief Implements On signal.
 * @param signal Parameter supplied by the caller.
 * @return None.
 * @note Noexcept API preserves hot-path failure and latency invariants.
 */
void BaseQuotingStrategy::on_signal(const PricingSignal& signal) noexcept {
    // Route to subclass implementation
    on_signal_impl(signal);
}

/**
 * @brief Implements On signals.
 * @param signals Parameter supplied by the caller.
 * @param count Parameter supplied by the caller.
 * @return None.
 * @note Noexcept API preserves hot-path failure and latency invariants.
 */
void BaseQuotingStrategy::on_signals(const PricingSignal* signals, int count) noexcept {
    on_signals_impl(signals, count);
}

/**
 * @brief Implements On signals impl.
 * @param signals Parameter supplied by the caller.
 * @param count Parameter supplied by the caller.
 * @return None.
 * @note Noexcept API preserves hot-path failure and latency invariants.
 */
void BaseQuotingStrategy::on_signals_impl(const PricingSignal* signals, int count) noexcept {
    if (signals == nullptr || count <= 0) return;
    for (int i = 0; i < count; ++i) {
        on_signal_impl(signals[i]);
    }
}

/**
 * @brief Implements On fill.
 * @param trade Parameter supplied by the caller.
 * @return None.
 * @note Noexcept API preserves hot-path failure and latency invariants.
 */
void BaseQuotingStrategy::on_fill(const Trade& trade) noexcept {
    // Update quote lifecycle if this fill is for a quote
    if (trade.client_order_id != 0 && trade.instrument_id < MAX_INSTRUMENTS) {
        InstrumentState& state = instrument_state_[trade.instrument_id];
        if (state.active) {
            (void)QuoteLifecycleController::note_quote_fill(
                state.quote_lifecycle,
                trade.client_order_id,
                trade.side,
                trade.fill_volume,
                state.quote_lifecycle.last_quote_ts);
        }
    }

    // Update order lifecycle if this fill is for a tracked order
    OrderLifecycleTracker* tracker = find_order_tracker(trade.client_order_id);
    if (tracker) {
        OrderLifecycleController::on_fill(*tracker, trade.fill_volume);
    }

    // Update position tracking
    if (trade.instrument_id < MAX_INSTRUMENTS) {
        InstrumentState& state = instrument_state_[trade.instrument_id];
        if (state.active) {
            const int signed_qty = (trade.side == Side::Buy ? 1 : -1) * trade.fill_volume;
            state.net_position += signed_qty;
        }
    }

    // Route to subclass implementation
    on_fill_impl(trade);
}

/**
 * @brief Implements On quote ack.
 * @param quote Parameter supplied by the caller.
 * @return None.
 * @note Noexcept API preserves hot-path failure and latency invariants.
 */
void BaseQuotingStrategy::on_quote_ack(const Quote& quote) noexcept {
    if (quote.instrument_id >= MAX_INSTRUMENTS) return;
    InstrumentState& state = instrument_state_[quote.instrument_id];
    if (!state.active) return;

    bool request_requote = false;
    const int64_t now_ns = get_monotonic_ns();
    if (!QuoteLifecycleController::on_quote_ack(state.quote_lifecycle,
                                                quote,
                                                now_ns,
                                                &request_requote)) {
        return;
    }

    on_quote_lifecycle_update(quote.instrument_id, now_ns, request_requote);
}

/**
 * @brief Implements On quote cancel.
 * @param quote Parameter supplied by the caller.
 * @return None.
 * @note Noexcept API preserves hot-path failure and latency invariants.
 */
void BaseQuotingStrategy::on_quote_cancel(const Quote& quote) noexcept {
    if (quote.instrument_id >= MAX_INSTRUMENTS) return;
    InstrumentState& state = instrument_state_[quote.instrument_id];
    if (!state.active) return;

    bool request_requote = false;
    if (QuoteLifecycleController::on_quote_cancel(state.quote_lifecycle,
                                                  quote,
                                                  &request_requote)) {
        on_quote_lifecycle_update(quote.instrument_id, get_monotonic_ns(), request_requote);
    }
}

/**
 * @brief Implements On quote reject.
 * @param quote Parameter supplied by the caller.
 * @return None.
 * @note Noexcept API preserves hot-path failure and latency invariants.
 */
void BaseQuotingStrategy::on_quote_reject(const Quote& quote) noexcept {
    if (quote.instrument_id >= MAX_INSTRUMENTS) return;
    InstrumentState& state = instrument_state_[quote.instrument_id];
    if (!state.active) return;

    bool request_requote = false;
    if (QuoteLifecycleController::on_quote_reject(state.quote_lifecycle,
                                                  quote,
                                                  &request_requote)) {
        on_quote_lifecycle_update(quote.instrument_id, get_monotonic_ns(), request_requote);
    }
}

/**
 * @brief Implements On order ack.
 * @param order Parameter supplied by the caller.
 * @return None.
 * @note Noexcept API preserves hot-path failure and latency invariants.
 */
void BaseQuotingStrategy::on_order_ack(const Order& order) noexcept {
    // Update pre-trade risk
    if (pre_risk_) {
        pre_risk_->on_order_ack(order);
    }

    // Update order lifecycle
    OrderLifecycleTracker* tracker = find_order_tracker(order.client_order_id);
    if (tracker) {
        OrderLifecycleController::on_order_ack(*tracker, order, get_monotonic_ns());
    }
}

/**
 * @brief Implements On order cancel.
 * @param id Parameter supplied by the caller.
 * @return None.
 * @note Noexcept API preserves hot-path failure and latency invariants.
 */
void BaseQuotingStrategy::on_order_cancel(OrderId id) noexcept {
    // Update pre-trade risk
    if (pre_risk_) {
        pre_risk_->on_order_cancel(id);
    }

    // Update order lifecycle
    OrderLifecycleTracker* tracker = find_order_tracker(id);
    if (tracker) {
        OrderLifecycleController::on_cancel(*tracker);
    }
}

/**
 * @brief Implements On order reject.
 * @param order Parameter supplied by the caller.
 * @return None.
 * @note Noexcept API preserves hot-path failure and latency invariants.
 */
void BaseQuotingStrategy::on_order_reject(const Order& order) noexcept {
    // Update order lifecycle
    OrderLifecycleTracker* tracker = find_order_tracker(order.client_order_id);
    if (tracker) {
        OrderLifecycleController::on_reject(*tracker);
    }
}

/**
 * @brief Implements On timer.
 * @param event Parameter supplied by the caller.
 * @return None.
 * @note Noexcept API preserves hot-path failure and latency invariants.
 */
void BaseQuotingStrategy::on_timer(const TimerEvent& event) noexcept {
    // Route to subclass implementation
    on_timer_impl(event);
}

// ─── Internal Lifecycle Management ───────────────────────────────────────────

/**
 * @brief Implements Manage quote lifecycle.
 * @param instrument_id Parameter supplied by the caller.
 * @param now_ns Parameter supplied by the caller.
 * @return Return value produced by the operation.
 * @note Noexcept API preserves hot-path failure and latency invariants.
 */
bool BaseQuotingStrategy::manage_quote_lifecycle(uint16_t instrument_id,
                                                 int64_t now_ns) noexcept {
    if (instrument_id >= MAX_INSTRUMENTS) return true;
    InstrumentState& state = instrument_state_[instrument_id];
    if (!state.active) return true;

    QuoteLifecycleWork work = QuoteLifecycleController::manage(
        state.quote_lifecycle, quote_config_, now_ns);

    if (!work.block_new_quote) {
        return false;
    }

    // Lifecycle management requires action (timeout, retry, or give-up)
    if (work.send_cancel && work.cancel_target_quote_id != 0) {
        send_cancel_internal(instrument_id, now_ns);
    }

    if (work.publish_cancel_failed_alert) {
        on_quote_cancel_give_up(instrument_id, state.quote_lifecycle, now_ns);
    }

    on_quote_lifecycle_update(instrument_id, now_ns, false);

    return true;
}

/**
 * @brief Implements Send quote internal.
 * @param instrument_id Parameter supplied by the caller.
 * @param bid Parameter supplied by the caller.
 * @param ask Parameter supplied by the caller.
 * @param bid_vol Parameter supplied by the caller.
 * @param ask_vol Parameter supplied by the caller.
 * @param now_ns Parameter supplied by the caller.
 * @return None.
 * @note Noexcept API preserves hot-path failure and latency invariants.
 */
void BaseQuotingStrategy::send_quote_internal(uint16_t instrument_id,
                                              double bid, double ask,
                                              Volume bid_vol, Volume ask_vol,
                                              int64_t now_ns) noexcept {
    if (instrument_id >= MAX_INSTRUMENTS) return;
    InstrumentState& state = instrument_state_[instrument_id];
    if (!state.active) return;

    QuoteLifecycleState& quote_lifecycle = state.quote_lifecycle;

    // Some gateways cannot replace a live quote in one step; cancel first
    if (quote_lifecycle.replace_policy == QuoteReplacePolicy::CancelFirst
        && quote_lifecycle.status == StrategyQuoteMonitorState::Live
        && quote_lifecycle.live_quote_id != 0) {
        QuoteLifecycleController::mark_requote_after_update(quote_lifecycle);
        send_cancel_internal(instrument_id, now_ns);
        return;
    }

    // Don't send if waiting for ack or cancel
    if (quote_lifecycle.status == StrategyQuoteMonitorState::AckPending
        || quote_lifecycle.status == StrategyQuoteMonitorState::CancelPending
        || quote_lifecycle.status == StrategyQuoteMonitorState::CancelFailed) {
        return;
    }

    // Construct quote
    Quote quote{};
    quote.client_quote_id = next_order_id();
    quote.instrument_id = instrument_id;
    quote.product_index = product_idx_;
    quote.bid_price = bid;
    quote.ask_price = ask;
    quote.bid_volume = bid_vol;
    quote.ask_volume = ask_vol;
    quote.bid_offset = OffsetFlag::Open;
    quote.ask_offset = OffsetFlag::Open;
    quote.send_ts = now_ns;

    // Pre-trade risk check
    if (pre_risk_ && pre_risk_->check_quote(quote) != PreTradeRisk::RejectReason::OK) {
        return;
    }

    // Push to ring buffer (non-blocking)
    if (!quote_buf_ || !quote_buf_->try_push(quote)) {
        return;
    }

    // Update lifecycle state
    const QuoteLifecycleIntent intent{true, bid, ask, bid_vol, ask_vol};
    QuoteLifecycleController::note_quote_submitted(quote_lifecycle,
                                                   quote.client_quote_id,
                                                   intent,
                                                   now_ns);
}

/**
 * @brief Implements Send cancel internal.
 * @param instrument_id Parameter supplied by the caller.
 * @param now_ns Parameter supplied by the caller.
 * @return None.
 * @note Noexcept API preserves hot-path failure and latency invariants.
 */
void BaseQuotingStrategy::send_cancel_internal(uint16_t instrument_id,
                                               int64_t now_ns) noexcept {
    if (instrument_id >= MAX_INSTRUMENTS) return;
    InstrumentState& state = instrument_state_[instrument_id];
    if (!state.active) return;

    QuoteLifecycleState& quote_lifecycle = state.quote_lifecycle;

    // Don't cancel if already idle or suppressed
    if (quote_lifecycle.status == StrategyQuoteMonitorState::Idle
        || quote_lifecycle.status == StrategyQuoteMonitorState::Suppressed
        || quote_lifecycle.status == StrategyQuoteMonitorState::CancelFailed) {
        return;
    }

    // Don't retry cancel too quickly
    static constexpr int64_t CANCEL_RETRY_NS = 1'000'000'000LL;
    if (quote_lifecycle.status == StrategyQuoteMonitorState::CancelPending
        && now_ns - quote_lifecycle.cancel_last_send_ts < CANCEL_RETRY_NS) {
        return;
    }

    // Don't retry cancel after max attempts
    static constexpr uint8_t MAX_CANCEL_ATTEMPTS = 3;
    if (quote_lifecycle.status == StrategyQuoteMonitorState::CancelPending
        && quote_lifecycle.cancel_attempts >= MAX_CANCEL_ATTEMPTS) {
        return;
    }

    const QuoteId target_quote_id =
        QuoteLifecycleController::cancel_target_quote_id(quote_lifecycle);
    if (target_quote_id == 0) return;

    // Construct cancel quote (zero volume)
    Quote cancel{};
    cancel.client_quote_id = target_quote_id;
    cancel.instrument_id = instrument_id;
    cancel.product_index = product_idx_;
    cancel.bid_price = 0.0;
    cancel.ask_price = 0.0;
    cancel.bid_volume = 0;
    cancel.ask_volume = 0;
    cancel.send_ts = now_ns;

    // Push to ring buffer
    if (quote_buf_ && quote_buf_->try_push(cancel)) {
        QuoteLifecycleController::note_cancel_submitted(quote_lifecycle,
                                                        target_quote_id,
                                                        now_ns,
                                                        quote_config_);
    }
}

/**
 * @brief Implements Find order tracker.
 * @param id Parameter supplied by the caller.
 * @return Return value produced by the operation.
 * @note Noexcept API preserves hot-path failure and latency invariants.
 */
OrderLifecycleTracker* BaseQuotingStrategy::find_order_tracker(OrderId id) noexcept {
    for (uint16_t i = 0; i < order_tracker_count_; ++i) {
        if (order_trackers_[i].order_id == id) {
            return &order_trackers_[i];
        }
    }
    return nullptr;
}

/**
 * @brief Implements Allocate order tracker.
 * @return Return value produced by the operation.
 * @note Noexcept API preserves hot-path failure and latency invariants.
 */
OrderLifecycleTracker* BaseQuotingStrategy::allocate_order_tracker() noexcept {
    // First try to find an idle or terminal tracker to reuse
    for (uint16_t i = 0; i < order_tracker_count_; ++i) {
        if (OrderLifecycleController::is_terminal(order_trackers_[i])
            || order_trackers_[i].status == OrderLifecycleState::Idle) {
            OrderLifecycleController::reset(order_trackers_[i]);
            return &order_trackers_[i];
        }
    }

    // Allocate a new slot if space available
    if (order_tracker_count_ < MAX_TRACKED_ORDERS) {
        return &order_trackers_[order_tracker_count_++];
    }

    return nullptr;
}

} // namespace omm
