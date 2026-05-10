#include "strategy/option_mm_core.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace omm {

namespace {

double clamp01(double value) noexcept {
    return std::clamp(value, 0.0, 1.0);
}

double microprice_from_market(const TopOfBookTick& md) noexcept {
    if (md.bid_price <= 0.0 || md.ask_price <= md.bid_price) {
        return 0.0;
    }

    const int64_t bid_vol = std::max<int64_t>(0, md.bid_volume);
    const int64_t ask_vol = std::max<int64_t>(0, md.ask_volume);
    const int64_t total_vol = bid_vol + ask_vol;
    if (total_vol <= 0) {
        return 0.5 * (md.bid_price + md.ask_price);
    }

    return (md.ask_price * static_cast<double>(bid_vol)
          + md.bid_price * static_cast<double>(ask_vol))
         / static_cast<double>(total_vol);
}

double round_down_to_tick(double price, double tick) noexcept {
    return std::floor(price / tick) * tick;
}

double round_up_to_tick(double price, double tick) noexcept {
    return std::ceil(price / tick) * tick;
}

Volume scale_volume(Volume base, double scale) noexcept {
    if (base <= 0 || scale <= 0.0) return 0;

    const double clipped = std::clamp(scale, 0.0, 1.0);
    return std::max<Volume>(
        1,
        static_cast<Volume>(std::lround(static_cast<double>(base) * clipped)));
}

} // namespace

void OptionMMCoreStrategy::init(uint8_t product_idx,
                                SPSCRingBuffer<Quote, 512>* quote_buf,
                                SPSCRingBuffer<Order, 512>* order_buf,
                                PreTradeRisk* pre_risk,
                                AtomicMMParams* params,
                                const Instrument* instruments,
                                const SnapshotArray<TopOfBookTick, MAX_INSTRUMENTS>* tick_snapshot,
                                const PostTradeRisk* post_risk,
                                MonitoringTopic<SystemAlert, 256>* alert_topic) noexcept {
    product_idx_ = product_idx;
    quote_buf_ = quote_buf;
    order_buf_ = order_buf;
    pre_risk_ = pre_risk;
    params_ = params;
    instruments_ = instruments;
    tick_snapshot_ = tick_snapshot;
    post_risk_ = post_risk;
    alert_topic_ = alert_topic;

    option_count_ = 0;
    session_open_ = true;
    underlying_id_ = INVALID_INSTRUMENT_ID;
    underlying_net_position_ = 0;
    product_net_delta_ = 0.0;
    product_net_vega_ = 0.0;
    last_underlying_mid_ = 0.0;
    suppress_until_ns_ = 0;
    last_hedge_ts_ns_ = 0;
    live_hedge_order_id_ = 0;
    live_hedge_remaining_ = 0;
    regime_state_ = ProductRegime{};
    runtime_full_book_reevaluations_.store(0, std::memory_order_relaxed);
    runtime_single_instrument_reevaluations_.store(0, std::memory_order_relaxed);
    for (auto& state : option_state_) state = OptionState{};
    for (uint16_t id = 0; id < MAX_INSTRUMENTS; ++id) {
        monitor_quote_state_[id].store(static_cast<uint8_t>(StrategyQuoteMonitorState::Idle),
                                       std::memory_order_relaxed);
        monitor_cancel_attempts_[id].store(0, std::memory_order_relaxed);
        monitor_net_position_[id].store(0, std::memory_order_relaxed);
        monitor_suppress_flags_[id].store(0, std::memory_order_relaxed);
        monitor_last_quote_ts_ns_[id].store(0, std::memory_order_relaxed);
    }

    for (uint16_t id = 0; id < MAX_INSTRUMENTS; ++id) {
        const Instrument& instr = instruments_[id];
        if (instr.instrument_id == INVALID_INSTRUMENT_ID) continue;
        if (instr.product_index != product_idx_) continue;

        if (instr.kind == InstrumentKind::Future && underlying_id_ == INVALID_INSTRUMENT_ID) {
            underlying_id_ = id;
            continue;
        }

        if (instr.kind != InstrumentKind::Option) continue;
        if (option_count_ >= MAX_INSTRUMENTS) break;

        option_ids_[option_count_++] = id;
        OptionState& state = option_state_[id];
        state.active = true;
        state.instrument_id = id;
        state.underlying_id = instr.underlying_id;

        // Initialize base class instrument state
        instrument_state_[id].active = true;
        instrument_state_[id].instrument_id = id;
        instrument_state_[id].quote_lifecycle.replace_policy =
            QuoteLifecycleController::policy_for_exchange(instr.exchange);
    }

    regime_state_ = capture_product_regime(get_monotonic_ns());
    update_all_monitor_states();
}

bool OptionMMCoreStrategy::is_enabled() const noexcept {
    return params_ && params_->enabled.load(std::memory_order_relaxed) && session_open_;
}

bool OptionMMCoreStrategy::read_product_monitor_state(ProductMonitorState* out) const noexcept {
    if (out == nullptr) return false;

    out->product_index = product_idx_;
    out->strategy_enabled = params_ && params_->enabled.load(std::memory_order_relaxed);
    out->session_open = monitor_session_open_.load(std::memory_order_relaxed);
    out->exposure_breached = monitor_exposure_breached_.load(std::memory_order_relaxed);
    out->underlying_shock_suppressed =
        monitor_underlying_shock_suppressed_.load(std::memory_order_relaxed);
    out->risk_breach = post_risk_ && post_risk_->any_breach();
    out->product_suppressed =
        !out->strategy_enabled
        || !out->session_open
        || out->exposure_breached
        || out->underlying_shock_suppressed
        || out->risk_breach
        || monitor_product_suppressed_.load(std::memory_order_relaxed);
    return true;
}

int OptionMMCoreStrategy::read_instrument_monitor_states(InstrumentMonitorState* out,
                                                         int max_count) const noexcept {
    if (out == nullptr || max_count <= 0) return 0;

    const int count = std::min<int>(option_count_, max_count);
    for (int i = 0; i < count; ++i) {
        const uint16_t instrument_id = option_ids_[i];
        out[i].instrument_id = instrument_id;
        out[i].product_index = product_idx_;
        out[i].quote_state = static_cast<StrategyQuoteMonitorState>(
            monitor_quote_state_[instrument_id].load(std::memory_order_relaxed));
        out[i].cancel_attempts =
            monitor_cancel_attempts_[instrument_id].load(std::memory_order_relaxed);
        out[i].net_position = monitor_net_position_[instrument_id].load(std::memory_order_relaxed);
        out[i].suppress_flags =
            monitor_suppress_flags_[instrument_id].load(std::memory_order_relaxed);
        out[i].last_quote_ts_ns =
            monitor_last_quote_ts_ns_[instrument_id].load(std::memory_order_relaxed);
    }
    return count;
}

bool OptionMMCoreStrategy::read_runtime_stats(StrategyRuntimeStats* out) const noexcept {
    if (out == nullptr) return false;

    out->full_book_reevaluations =
        runtime_full_book_reevaluations_.load(std::memory_order_relaxed);
    out->single_instrument_reevaluations =
        runtime_single_instrument_reevaluations_.load(std::memory_order_relaxed);
    return true;
}

void OptionMMCoreStrategy::on_signal_impl(const PricingSignal& signal) noexcept {
    if (!params_) return;
    const uint16_t id = signal.instrument_id;
    if (id >= MAX_INSTRUMENTS) return;
    OptionState& state = option_state_[id];
    if (!state.active) return;

    const double old_delta = state.last_delta;
    const double old_vega = state.last_vega;

    state.last_theo_bid = signal.theo_bid;
    state.last_theo_ask = signal.theo_ask;
    state.last_delta = signal.delta;
    state.last_vega = signal.vega;
    state.last_signal_ts = signal.calc_ts_ns;
    if (signal.underlying_id < MAX_INSTRUMENTS) {
        state.underlying_id = signal.underlying_id;
        if (underlying_id_ == INVALID_INSTRUMENT_ID) {
            underlying_id_ = signal.underlying_id;
        }
    }

    const double underlying_mid =
        0.5 * (static_cast<double>(signal.underlying_ref_bid) + static_cast<double>(signal.underlying_ref_ask));
    state.last_underlying_px = underlying_mid;
    if (underlying_mid > 0.0) {
        const uint16_t ref_underlying_id = state.underlying_id;
        const double underlying_tick = (ref_underlying_id < MAX_INSTRUMENTS
            && instruments_[ref_underlying_id].tick_size > 0.0)
            ? instruments_[ref_underlying_id].tick_size
            : 1.0;
        const double shock_threshold_ticks =
            params_->underlying_move_widen_threshold_ticks.load(std::memory_order_relaxed);
        if (shock_threshold_ticks > 0.0
            && last_underlying_mid_ > 0.0
            && std::fabs(underlying_mid - last_underlying_mid_) >= shock_threshold_ticks * underlying_tick) {
            const double min_interval_ms = params_->min_quote_interval_ms.load(std::memory_order_relaxed);
            const int64_t shock_hold_ns = std::max<int64_t>(
                25'000'000LL,
                static_cast<int64_t>(min_interval_ms * 1'000'000.0));
            suppress_until_ns_ = std::max(suppress_until_ns_, get_monotonic_ns() + shock_hold_ns);
        }
        last_underlying_mid_ = underlying_mid;
    }

    update_product_exposure(state, old_delta, old_vega);

    const int64_t now_ns = get_monotonic_ns();
    maybe_trigger_hedge(now_ns);
    if (handle_product_regime_transition(now_ns) || regime_state_.product_suppressed) {
        return;
    }
    reevaluate_one(id, now_ns);
}

void OptionMMCoreStrategy::on_fill_impl(const Trade& trade) noexcept {
    // Base class has already updated instrument_state_[].net_position and
    // handled quote lifecycle fills. We handle strategy-specific logic here.

    if (trade.instrument_id >= MAX_INSTRUMENTS) return;

    const int signed_qty = (trade.side == Side::Buy ? 1 : -1) * trade.fill_volume;

    // Handle hedge order fills
    if (trade.client_order_id != 0 && trade.client_order_id == live_hedge_order_id_) {
        underlying_net_position_ += signed_qty;
        live_hedge_remaining_ = std::max<Volume>(0, live_hedge_remaining_ - trade.fill_volume);
        if (live_hedge_remaining_ <= 0) {
            live_hedge_order_id_ = 0;
        }
        const int64_t now_ns = get_monotonic_ns();
        (void)handle_product_regime_transition(now_ns);
        return;
    }

    // Handle option fills
    OptionState& state = option_state_[trade.instrument_id];
    if (!state.active) return;

    // Sync position from base class to option state
    state.net_position = instrument_state_[trade.instrument_id].net_position;

    // Update product exposure
    product_net_delta_ += state.last_delta * static_cast<double>(signed_qty);
    product_net_vega_ += state.last_vega * static_cast<double>(signed_qty);

    update_monitor_state(state);

    const int64_t now_ns = get_monotonic_ns();
    maybe_trigger_hedge(now_ns);
    if (handle_product_regime_transition(now_ns) || regime_state_.product_suppressed) {
        return;
    }
    reevaluate_one(trade.instrument_id, now_ns);
}

// Note: Quote lifecycle event handlers (on_quote_ack, on_quote_cancel, on_quote_reject)
// are now handled by BaseQuotingStrategy. The base class manages quote state transitions.
// Strategy-specific logic (suppress flags, requote triggers) happens in maybe_quote() and
// build_decision().

// Note: Order lifecycle event handlers (on_order_ack, on_order_cancel, on_order_reject)
// are now handled by BaseQuotingStrategy. We override on_fill_impl to handle hedge orders.

void OptionMMCoreStrategy::on_timer_impl(const TimerEvent& event) noexcept {
    switch (event.type) {
    case TimerEventType::QuoteRefresh:
        regime_state_ = capture_product_regime(event.trigger_ts_ns);
        update_monitor_product_state();
        if (regime_state_.product_suppressed) {
            cancel_all_live(event.trigger_ts_ns);
            break;
        }
        reevaluate_all(event.trigger_ts_ns);
        break;
    case TimerEventType::HedgeCheck:
        maybe_trigger_hedge(event.trigger_ts_ns);
        (void)handle_product_regime_transition(event.trigger_ts_ns);
        break;
    case TimerEventType::SessionOpen:
        session_open_ = true;
        regime_state_ = capture_product_regime(event.trigger_ts_ns);
        update_monitor_product_state();
        reevaluate_all(event.trigger_ts_ns);
        break;
    case TimerEventType::SessionClose:
        session_open_ = false;
        regime_state_ = capture_product_regime(event.trigger_ts_ns);
        update_monitor_product_state();
        cancel_all_live(event.trigger_ts_ns);
        break;
    default:
        break;
    }
}

void OptionMMCoreStrategy::on_quote_lifecycle_update(uint16_t instrument_id,
                                                     int64_t now_ns,
                                                     bool reevaluate) noexcept {
    if (instrument_id >= MAX_INSTRUMENTS) return;
    OptionState& state = option_state_[instrument_id];
    if (!state.active) return;

    update_monitor_state(state);
    if (reevaluate) {
        maybe_quote(instrument_id, now_ns);
    }
}

void OptionMMCoreStrategy::on_quote_cancel_give_up(
        uint16_t instrument_id,
        const QuoteLifecycleState& quote_lifecycle,
        int64_t now_ns) noexcept {
    publish_cancel_failed_alert(instrument_id, quote_lifecycle, now_ns);
}

void OptionMMCoreStrategy::reevaluate_all(int64_t now_ns) noexcept {
    runtime_full_book_reevaluations_.fetch_add(1, std::memory_order_relaxed);
    if (product_exposure_breached() || product_temporarily_suppressed(now_ns)) {
        cancel_all_live(now_ns);
        return;
    }

    for (uint16_t i = 0; i < option_count_; ++i) {
        maybe_quote(option_ids_[i], now_ns);
    }
}

void OptionMMCoreStrategy::reevaluate_one(uint16_t instrument_id, int64_t now_ns) noexcept {
    runtime_single_instrument_reevaluations_.fetch_add(1, std::memory_order_relaxed);
    maybe_quote(instrument_id, now_ns);
}

void OptionMMCoreStrategy::cancel_all_live(int64_t now_ns) noexcept {
    for (uint16_t i = 0; i < option_count_; ++i) {
        request_cancel(option_ids_[i], now_ns);
    }
}

void OptionMMCoreStrategy::maybe_quote(uint16_t instrument_id, int64_t now_ns) noexcept {
    if (instrument_id >= MAX_INSTRUMENTS) return;
    OptionState& state = option_state_[instrument_id];
    if (!state.active) return;

    QuoteDecision decision = build_decision(state, now_ns);
    state.suppress_flags = decision.suppress_flags;
    update_monitor_state(state);

    if (decision.cancel_only) {
        request_cancel(instrument_id, now_ns);
        return;
    }
    if (!decision.valid) return;

    // Use base class helper to submit quote with lifecycle management
    request_quote(instrument_id,
                 decision.bid, decision.ask,
                 decision.bid_vol, decision.ask_vol,
                 now_ns);
}

OptionMMCoreStrategy::QuoteDecision
OptionMMCoreStrategy::build_decision(OptionState& state, int64_t now_ns) const noexcept {
    QuoteDecision decision{};

    // Get quote lifecycle state from base class
    const QuoteLifecycleState* quote_lifecycle_ptr = get_quote_state(state.instrument_id);
    if (!quote_lifecycle_ptr) return decision;
    const QuoteLifecycleState& quote_lifecycle = *quote_lifecycle_ptr;

    const bool cancel_tracked_quote =
        QuoteLifecycleController::should_cancel_current_quote(quote_lifecycle);

    if (quote_lifecycle.status == StrategyQuoteMonitorState::CancelFailed) {
        decision.suppress_flags |= SuppressCancelStuck;
        return decision;
    }

    if (!params_ || !session_open_ || !params_->enabled.load(std::memory_order_relaxed)) {
        decision.cancel_only = cancel_tracked_quote;
        decision.suppress_flags |= SuppressSession;
        return decision;
    }

    if (product_exposure_breached()) {
        decision.cancel_only = cancel_tracked_quote;
        decision.suppress_flags |= SuppressProductExposure;
        return decision;
    }

    if (product_temporarily_suppressed(now_ns)) {
        decision.cancel_only = cancel_tracked_quote;
        decision.suppress_flags |= SuppressUnderlyingShock;
        return decision;
    }

    if (post_risk_ && post_risk_->any_breach()) {
        decision.cancel_only = cancel_tracked_quote;
        decision.suppress_flags |= SuppressRisk;
        return decision;
    }

    const double theo_bid = state.last_theo_bid;
    const double theo_ask = state.last_theo_ask;
    if (theo_bid <= 0.0 || theo_ask < theo_bid
        || state.last_signal_ts == 0 || now_ns - state.last_signal_ts > STALE_NS) {
        decision.cancel_only = cancel_tracked_quote;
        decision.suppress_flags |= SuppressStaleTheo;
        return decision;
    }

    TopOfBookTick md{};
    if (!tick_snapshot_ || !tick_snapshot_->read(state.instrument_id, &md)) {
        decision.cancel_only = cancel_tracked_quote;
        decision.suppress_flags |= SuppressInvalidMarket;
        return decision;
    }
    const bool has_market = md.recv_ts_ns > 0
        && md.bid_price > 0.0
        && md.ask_price > md.bid_price;
    if (!has_market || now_ns - md.recv_ts_ns > STALE_NS) {
        decision.cancel_only = cancel_tracked_quote;
        decision.suppress_flags |= SuppressInvalidMarket;
        return decision;
    }

    const int32_t max_pos = params_->max_position.load(std::memory_order_relaxed);
    const int32_t warning_pos = std::max<int32_t>(
        1, std::min(max_pos, params_->warning_position.load(std::memory_order_relaxed)));
    if (std::abs(state.net_position) >= max_pos) {
        decision.cancel_only = cancel_tracked_quote;
        decision.suppress_flags |= SuppressPosition;
        return decision;
    }

    const Instrument& instr = instruments_[state.instrument_id];
    const double tick = instr.tick_size > 0.0 ? instr.tick_size : 0.01;
    const double follow_weight = clamp01(params_->follow_weight.load(std::memory_order_relaxed));
    const double theo_width_ticks = std::max(0.0, (theo_ask - theo_bid) / (2.0 * tick));
    const double theo_mid = 0.5 * (theo_bid + theo_ask);
    const double market_ref = microprice_from_market(md);
    const double center_ref =
        theo_mid * (1.0 - follow_weight) + market_ref * follow_weight;

    // Spread starts from configured baseline, then widens for theo uncertainty,
    // wide markets, inventory pressure, and product-level delta/vega pressure.
    double half_spread_ticks = params_->base_half_spread_ticks.load(std::memory_order_relaxed);
    const double max_half_spread_ticks =
        params_->max_half_spread_ticks.load(std::memory_order_relaxed);
    half_spread_ticks = std::max(half_spread_ticks,
                                 params_->min_half_spread_ticks.load(std::memory_order_relaxed));
    half_spread_ticks = std::max(half_spread_ticks, theo_width_ticks);
    half_spread_ticks = std::min(half_spread_ticks, max_half_spread_ticks);

    const double market_width_ticks = (md.ask_price - md.bid_price) / tick;
    const double widen_threshold =
        params_->market_width_widen_threshold_ticks.load(std::memory_order_relaxed);
    if (market_width_ticks > widen_threshold) {
        half_spread_ticks += 0.5 * (market_width_ticks - widen_threshold);
    }

    const double inventory_pressure =
        std::min(1.0, std::abs(static_cast<double>(state.net_position)) / warning_pos);
    double product_pressure = 0.0;
    const double total_delta = product_net_delta_ + static_cast<double>(underlying_net_position_);
    const double delta_threshold = params_->product_delta_threshold.load(std::memory_order_relaxed);
    if (delta_threshold > 0.0) {
        product_pressure = std::max(product_pressure, std::fabs(total_delta) / delta_threshold);
    }
    const double vega_threshold = params_->product_vega_threshold.load(std::memory_order_relaxed);
    if (vega_threshold > 0.0) {
        product_pressure = std::max(product_pressure, std::fabs(product_net_vega_) / vega_threshold);
    }
    product_pressure = std::clamp(product_pressure, 0.0, 1.0);
    half_spread_ticks = std::min(
        half_spread_ticks * (1.0 + inventory_pressure + 0.75 * product_pressure),
        max_half_spread_ticks);

    const double inv_skew_ticks =
        params_->inventory_skew_per_lot_ticks.load(std::memory_order_relaxed) * state.net_position;
    const double center = center_ref - inv_skew_ticks * tick;

    // Size is reduced as the instrument/product gets riskier, and can become one-sided
    // near position limits so the strategy only quotes the risk-reducing side.
    const bool use_one_sided = params_->use_one_sided_at_limits.load(std::memory_order_relaxed);
    const Volume base_quote_vol = std::max<Volume>(
        0, params_->quote_volume.load(std::memory_order_relaxed));
    const Volume scaled_base_vol = scale_volume(
        base_quote_vol,
        std::max(0.35, 1.0 - 0.5 * product_pressure));
    Volume bid_vol = scaled_base_vol;
    Volume ask_vol = scaled_base_vol;
    if (state.net_position > 0 && bid_vol > 0) {
        bid_vol = scale_volume(scaled_base_vol, 1.0 - inventory_pressure);
        if (bid_vol == 0 && !use_one_sided) bid_vol = 1;
    } else if (state.net_position < 0 && ask_vol > 0) {
        ask_vol = scale_volume(scaled_base_vol, 1.0 - inventory_pressure);
        if (ask_vol == 0 && !use_one_sided) ask_vol = 1;
    }
    if (use_one_sided && std::abs(state.net_position) >= warning_pos) {
        if (state.net_position > 0) bid_vol = 0;
        if (state.net_position < 0) ask_vol = 0;
    }
    if (bid_vol == 0 && ask_vol == 0) {
        decision.cancel_only = cancel_tracked_quote;
        decision.suppress_flags |= SuppressPosition;
        return decision;
    }

    double bid = round_down_to_tick(center - half_spread_ticks * tick, tick);
    double ask = round_up_to_tick(center + half_spread_ticks * tick, tick);

    const double max_passive_bid = round_down_to_tick(std::max(0.0, md.ask_price - tick), tick);
    const double min_passive_ask = round_up_to_tick(md.bid_price + tick, tick);
    const double max_theo_bid = round_down_to_tick(theo_bid, tick);
    const double min_theo_ask = round_up_to_tick(theo_ask, tick);
    if (bid_vol > 0) bid = std::min(bid, max_passive_bid);
    if (ask_vol > 0) ask = std::max(ask, min_passive_ask);
    if (bid_vol > 0) bid = std::min(bid, max_theo_bid);
    if (ask_vol > 0) ask = std::max(ask, min_theo_ask);
    if (bid_vol == 0) bid = 0.0;
    if (ask_vol == 0) ask = 0.0;

    if ((bid_vol > 0 && bid <= 0.0)
        || (ask_vol > 0 && ask <= 0.0)
        || (bid_vol > 0 && ask_vol > 0 && ask <= bid)) {
        decision.cancel_only = cancel_tracked_quote;
        decision.suppress_flags |= SuppressInvalidMarket;
        return decision;
    }

    decision.valid = true;
    decision.bid = bid;
    decision.ask = ask;
    decision.bid_vol = bid_vol;
    decision.ask_vol = ask_vol;
    return decision;
}

// REMOVED: send_quote, send_cancel, manage_quote_lifecycle
// These methods are now implemented in BaseQuotingStrategy.
// The base class handles all quote lifecycle management.

void OptionMMCoreStrategy::publish_cancel_failed_alert(
        uint16_t instrument_id,
        const QuoteLifecycleState& quote_lifecycle,
        int64_t now_ns) noexcept {
    if (!alert_topic_) return;

    SystemAlert alert{};
    alert.ts_ns = now_ns;
    alert.instrument_id = instrument_id;
    alert.product_index = product_idx_;
    alert.type = SystemAlertType::QuoteCancelGiveUp;
    std::snprintf(alert.message,
                  sizeof(alert.message),
                  "quote cancel failed after %u attempts for instrument %u quote %llu",
                  static_cast<unsigned>(quote_lifecycle.cancel_attempts),
                  static_cast<unsigned>(instrument_id),
                  static_cast<unsigned long long>(quote_lifecycle.cancel_target_quote_id));
    alert_topic_->publish(alert);
}

void OptionMMCoreStrategy::maybe_trigger_hedge(int64_t now_ns) noexcept {
    if (!params_ || !session_open_ || !params_->enabled.load(std::memory_order_relaxed)) return;
    if (!order_buf_ || !pre_risk_ || !tick_snapshot_ || underlying_id_ >= MAX_INSTRUMENTS) return;
    if (live_hedge_order_id_ != 0 && live_hedge_remaining_ > 0) return;

    // Hedge threshold uses total product delta including the already-filled future hedge,
    // so repeated hedge orders shrink as the future inventory catches up.
    const double threshold = params_->product_delta_threshold.load(std::memory_order_relaxed);
    if (threshold <= 0.0) return;

    const double total_delta = product_net_delta_ + static_cast<double>(underlying_net_position_);
    const double excess_delta = std::fabs(total_delta) - threshold;
    if (excess_delta <= 0.0) return;

    const int64_t min_hedge_interval_ns = std::max<int64_t>(
        10'000'000LL,
        static_cast<int64_t>(params_->min_quote_interval_ms.load(std::memory_order_relaxed) * 1'000'000.0));
    if (now_ns - last_hedge_ts_ns_ < min_hedge_interval_ns) return;

    TopOfBookTick underlying_md{};
    if (!tick_snapshot_->read(underlying_id_, &underlying_md)) {
        return;
    }
    if (underlying_md.recv_ts_ns == 0
        || now_ns - underlying_md.recv_ts_ns > STALE_NS
        || underlying_md.bid_price <= 0.0
        || underlying_md.ask_price <= underlying_md.bid_price) {
        return;
    }

    Order hedge{};
    hedge.client_order_id = next_order_id();
    hedge.instrument_id = underlying_id_;
    hedge.product_index = product_idx_;
    hedge.side = (total_delta > 0.0) ? Side::Sell : Side::Buy;
    hedge.offset = OffsetFlag::Open;
    hedge.price_type = OrderPriceType::Limit;
    hedge.order_type = OrderType::FAK;
    hedge.price = (hedge.side == Side::Buy) ? underlying_md.ask_price : underlying_md.bid_price;
    hedge.volume = std::max<Volume>(
        1,
        std::min<Volume>(
            static_cast<Volume>(std::ceil(excess_delta)),
            std::max<Volume>(1, params_->quote_volume.load(std::memory_order_relaxed))));
    hedge.send_ts = now_ns;
    hedge.is_hedge = true;

    if (pre_risk_->check_order(hedge) != PreTradeRisk::RejectReason::OK) {
        return;
    }
    if (!order_buf_->try_push(hedge)) {
        return;
    }

    live_hedge_order_id_ = hedge.client_order_id;
    live_hedge_remaining_ = hedge.volume;
    last_hedge_ts_ns_ = now_ns;
}

void OptionMMCoreStrategy::update_product_exposure(OptionState& state,
                                                   double old_delta,
                                                   double old_vega) noexcept {
    const double pos = static_cast<double>(state.net_position);
    product_net_delta_ += (state.last_delta - old_delta) * pos;
    product_net_vega_ += (state.last_vega - old_vega) * pos;
}

OptionMMCoreStrategy::ProductRegime
OptionMMCoreStrategy::capture_product_regime(int64_t now_ns) const noexcept {
    ProductRegime regime{};
    // Product gating is intentionally conservative: any session/risk/exposure/shock issue
    // suppresses the whole product and tells the monitor why.
    regime.exposure_breached = product_exposure_breached();
    regime.underlying_shock_suppressed = product_temporarily_suppressed(now_ns);
    regime.product_suppressed =
        !params_
        || !session_open_
        || !params_->enabled.load(std::memory_order_relaxed)
        || regime.exposure_breached
        || regime.underlying_shock_suppressed
        || (post_risk_ && post_risk_->any_breach());
    return regime;
}

bool OptionMMCoreStrategy::handle_product_regime_transition(int64_t now_ns) noexcept {
    const ProductRegime next = capture_product_regime(now_ns);
    const bool product_changed =
        next.product_suppressed != regime_state_.product_suppressed;
    const bool exposure_changed =
        next.exposure_breached != regime_state_.exposure_breached;
    const bool shock_changed =
        next.underlying_shock_suppressed != regime_state_.underlying_shock_suppressed;

    regime_state_ = next;
    update_monitor_product_state();
    if (!product_changed && !exposure_changed && !shock_changed) {
        return false;
    }

    // Full-book work is reserved for actual product-wide gating transitions.
    if (next.product_suppressed) {
        cancel_all_live(now_ns);
    } else {
        reevaluate_all(now_ns);
    }
    return true;
}

bool OptionMMCoreStrategy::product_exposure_breached() const noexcept {
    if (!params_) return false;

    const double total_delta = product_net_delta_ + static_cast<double>(underlying_net_position_);
    const double delta_threshold = params_->product_delta_threshold.load(std::memory_order_relaxed);
    const double vega_threshold = params_->product_vega_threshold.load(std::memory_order_relaxed);

    const bool delta_breach = delta_threshold > 0.0 && std::fabs(total_delta) > delta_threshold;
    const bool vega_breach = vega_threshold > 0.0 && std::fabs(product_net_vega_) > vega_threshold;
    return delta_breach || vega_breach;
}

bool OptionMMCoreStrategy::product_temporarily_suppressed(int64_t now_ns) const noexcept {
    return suppress_until_ns_ > now_ns;
}

void OptionMMCoreStrategy::update_monitor_state(const OptionState& state) noexcept {
    if (!state.active || state.instrument_id >= MAX_INSTRUMENTS) return;

    // Get quote lifecycle state from base class
    const QuoteLifecycleState* quote_lifecycle_ptr = get_quote_state(state.instrument_id);
    if (!quote_lifecycle_ptr) return;
    const QuoteLifecycleState& quote_lifecycle = *quote_lifecycle_ptr;

    monitor_quote_state_[state.instrument_id].store(
        static_cast<uint8_t>(quote_lifecycle.status), std::memory_order_relaxed);
    monitor_cancel_attempts_[state.instrument_id].store(
        quote_lifecycle.cancel_attempts, std::memory_order_relaxed);
    monitor_net_position_[state.instrument_id].store(
        state.net_position, std::memory_order_relaxed);
    monitor_suppress_flags_[state.instrument_id].store(
        state.suppress_flags, std::memory_order_relaxed);
    monitor_last_quote_ts_ns_[state.instrument_id].store(
        quote_lifecycle.last_quote_ts, std::memory_order_relaxed);
}

void OptionMMCoreStrategy::update_monitor_product_state() noexcept {
    monitor_session_open_.store(session_open_, std::memory_order_relaxed);
    monitor_product_suppressed_.store(regime_state_.product_suppressed, std::memory_order_relaxed);
    monitor_exposure_breached_.store(regime_state_.exposure_breached, std::memory_order_relaxed);
    monitor_underlying_shock_suppressed_.store(
        regime_state_.underlying_shock_suppressed, std::memory_order_relaxed);
}

void OptionMMCoreStrategy::update_all_monitor_states() noexcept {
    update_monitor_product_state();
    for (uint16_t i = 0; i < option_count_; ++i) {
        update_monitor_state(option_state_[option_ids_[i]]);
    }
}

} // namespace omm
