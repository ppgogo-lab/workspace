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

double microprice_from_market(const MarketTick& md) noexcept {
    if (md.bid_price[0] <= 0.0 || md.ask_price[0] <= md.bid_price[0]) {
        return 0.0;
    }

    const int64_t bid_vol = std::max<int64_t>(0, md.bid_volume[0]);
    const int64_t ask_vol = std::max<int64_t>(0, md.ask_volume[0]);
    const int64_t total_vol = bid_vol + ask_vol;
    if (total_vol <= 0) {
        return 0.5 * (md.bid_price[0] + md.ask_price[0]);
    }

    return (md.ask_price[0] * static_cast<double>(bid_vol)
          + md.bid_price[0] * static_cast<double>(ask_vol))
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
                                const MarketTick* tick_snapshot,
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
    for (auto& state : option_state_) state = OptionState{};

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
    }

    regime_state_ = capture_product_regime(get_monotonic_ns());
}

bool OptionMMCoreStrategy::is_enabled() const noexcept {
    return params_ && params_->enabled.load(std::memory_order_relaxed) && session_open_;
}

void OptionMMCoreStrategy::on_signal(const PricingSignal& signal) noexcept {
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
    maybe_quote(id, now_ns);
}

void OptionMMCoreStrategy::on_fill(const Trade& trade) noexcept {
    if (trade.instrument_id >= MAX_INSTRUMENTS) return;

    const int signed_qty = (trade.side == Side::Buy ? 1 : -1) * trade.fill_volume;

    if (trade.client_order_id != 0 && trade.client_order_id == live_hedge_order_id_) {
        underlying_net_position_ += signed_qty;
        if (pre_risk_) {
            live_hedge_remaining_ = std::max<Volume>(0, live_hedge_remaining_ - trade.fill_volume);
            pre_risk_->on_order_fill(live_hedge_order_id_,
                                     trade.fill_volume,
                                     live_hedge_remaining_ <= 0);
        }
        if (live_hedge_remaining_ <= 0) {
            live_hedge_order_id_ = 0;
        }
        const int64_t now_ns = get_monotonic_ns();
        (void)handle_product_regime_transition(now_ns);
        return;
    }

    OptionState& state = option_state_[trade.instrument_id];
    if (!state.active) return;

    state.net_position += signed_qty;
    product_net_delta_ += state.last_delta * static_cast<double>(signed_qty);
    product_net_vega_ += state.last_vega * static_cast<double>(signed_qty);

    const bool matches_live_quote =
        trade.client_order_id != 0
        && (trade.client_order_id == state.live_quote_id
            || trade.client_order_id == state.pending_quote_id
            || trade.client_order_id == state.cancel_target_quote_id);
    if (matches_live_quote) {
        if (trade.side == Side::Buy) {
            state.live_bid_vol = std::max<Volume>(0, state.live_bid_vol - trade.fill_volume);
        } else {
            state.live_ask_vol = std::max<Volume>(0, state.live_ask_vol - trade.fill_volume);
        }

        if (trade.client_order_id == state.pending_quote_id && state.quote_state == QuoteState::ReplacePending) {
            state.live_quote_id = state.pending_quote_id;
            state.pending_quote_id = 0;
            state.live_since_ts = state.last_quote_ts;
            state.quote_state = QuoteState::Live;
        } else if (trade.client_order_id == state.live_quote_id
                   && state.quote_state == QuoteState::ReplacePending) {
            state.quote_state = QuoteState::Live;
        }

        if (quote_fully_filled(state)) {
            reset_quote_tracking(state, QuoteState::Idle);
        }
    }

    const int64_t now_ns = get_monotonic_ns();
    maybe_trigger_hedge(now_ns);
    if (handle_product_regime_transition(now_ns) || regime_state_.product_suppressed) {
        return;
    }
    maybe_quote(trade.instrument_id, now_ns);
}

void OptionMMCoreStrategy::on_order_ack(const Order& order) noexcept {
    if (pre_risk_) pre_risk_->on_order_ack(order);
}

void OptionMMCoreStrategy::on_quote_ack(const Quote& quote) noexcept {
    if (quote.instrument_id >= MAX_INSTRUMENTS) return;
    OptionState& state = option_state_[quote.instrument_id];
    if (!state.active) return;

    const bool ack_matches_pending =
        state.pending_quote_id != 0 && quote.client_quote_id == state.pending_quote_id;
    const bool ack_matches_cancel_target =
        state.quote_state == QuoteState::CancelPending
        && state.cancel_target_quote_id != 0
        && quote.client_quote_id == state.cancel_target_quote_id;
    if (state.pending_quote_id != 0 && !ack_matches_pending && !ack_matches_cancel_target) return;

    state.live_quote_id = quote.client_quote_id;
    if (ack_matches_pending) {
        state.pending_quote_id = 0;
    }
    state.live_bid = quote.bid_price;
    state.live_ask = quote.ask_price;
    state.live_bid_vol = quote.bid_volume;
    state.live_ask_vol = quote.ask_volume;
    state.live_since_ts = quote.ack_ts != 0 ? quote.ack_ts : get_monotonic_ns();
    state.suppress_flags = (quote.bid_volume == 0 && quote.ask_volume == 0)
        ? state.suppress_flags
        : SuppressNone;
    if (quote.bid_volume == 0 && quote.ask_volume == 0) {
        reset_quote_tracking(state, QuoteState::Suppressed);
        return;
    }
    state.quote_state = ack_matches_cancel_target ? QuoteState::CancelPending : QuoteState::Live;
    if (!ack_matches_cancel_target) {
        state.cancel_target_quote_id = 0;
        state.cancel_last_send_ts = 0;
        state.cancel_attempts = 0;
    }
}

void OptionMMCoreStrategy::on_quote_cancel(const Quote& quote) noexcept {
    if (quote.instrument_id >= MAX_INSTRUMENTS) return;
    OptionState& state = option_state_[quote.instrument_id];
    if (!state.active) return;

    reset_quote_tracking(state, QuoteState::Suppressed);

    maybe_quote(quote.instrument_id, get_monotonic_ns());
}

void OptionMMCoreStrategy::on_quote_reject(const Quote& quote) noexcept {
    if (quote.instrument_id >= MAX_INSTRUMENTS) return;
    OptionState& state = option_state_[quote.instrument_id];
    if (!state.active) return;

    reset_quote_tracking(state, QuoteState::Suppressed);
    state.suppress_flags |= SuppressInvalidMarket;
}

void OptionMMCoreStrategy::on_order_cancel(OrderId id) noexcept {
    if (pre_risk_) pre_risk_->on_order_cancel(id);
    if (id == live_hedge_order_id_) {
        live_hedge_order_id_ = 0;
        live_hedge_remaining_ = 0;
    }
}

void OptionMMCoreStrategy::on_order_reject(const Order& order) noexcept {
    if (order.client_order_id == live_hedge_order_id_) {
        live_hedge_order_id_ = 0;
        live_hedge_remaining_ = 0;
    }
}

void OptionMMCoreStrategy::on_timer(const TimerEvent& event) noexcept {
    switch (event.type) {
    case TimerEventType::QuoteRefresh:
        regime_state_ = capture_product_regime(event.trigger_ts_ns);
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
        reevaluate_all(event.trigger_ts_ns);
        break;
    case TimerEventType::SessionClose:
        session_open_ = false;
        regime_state_ = capture_product_regime(event.trigger_ts_ns);
        cancel_all_live(event.trigger_ts_ns);
        break;
    default:
        break;
    }
}

void OptionMMCoreStrategy::reevaluate_all(int64_t now_ns) noexcept {
    if (product_exposure_breached() || product_temporarily_suppressed(now_ns)) {
        cancel_all_live(now_ns);
        return;
    }

    for (uint16_t i = 0; i < option_count_; ++i) {
        maybe_quote(option_ids_[i], now_ns);
    }
}

void OptionMMCoreStrategy::cancel_all_live(int64_t now_ns) noexcept {
    for (uint16_t i = 0; i < option_count_; ++i) {
        send_cancel(option_state_[option_ids_[i]], now_ns);
    }
}

void OptionMMCoreStrategy::maybe_quote(uint16_t instrument_id, int64_t now_ns) noexcept {
    if (instrument_id >= MAX_INSTRUMENTS) return;
    OptionState& state = option_state_[instrument_id];
    if (!state.active) return;
    if (manage_quote_lifecycle(state, now_ns)) return;

    QuoteDecision decision = build_decision(state, now_ns);
    state.suppress_flags = decision.suppress_flags;
    if (decision.cancel_only) {
        send_cancel(state, now_ns);
        return;
    }
    if (!decision.valid) return;
    send_quote(state, decision, now_ns);
}

OptionMMCoreStrategy::QuoteDecision
OptionMMCoreStrategy::build_decision(OptionState& state, int64_t now_ns) const noexcept {
    QuoteDecision decision{};

    if (state.quote_state == QuoteState::CancelFailed) {
        decision.suppress_flags |= SuppressCancelStuck;
        return decision;
    }

    if (!params_ || !session_open_ || !params_->enabled.load(std::memory_order_relaxed)) {
        decision.cancel_only = state.quote_state == QuoteState::Live
            || state.quote_state == QuoteState::ReplacePending
            || state.quote_state == QuoteState::CancelPending;
        decision.suppress_flags |= SuppressSession;
        return decision;
    }

    if (product_exposure_breached()) {
        decision.cancel_only = state.quote_state == QuoteState::Live
            || state.quote_state == QuoteState::ReplacePending
            || state.quote_state == QuoteState::CancelPending;
        decision.suppress_flags |= SuppressProductExposure;
        return decision;
    }

    if (product_temporarily_suppressed(now_ns)) {
        decision.cancel_only = state.quote_state == QuoteState::Live
            || state.quote_state == QuoteState::ReplacePending
            || state.quote_state == QuoteState::CancelPending;
        decision.suppress_flags |= SuppressUnderlyingShock;
        return decision;
    }

    if (post_risk_ && post_risk_->any_breach()) {
        decision.cancel_only = state.quote_state == QuoteState::Live
            || state.quote_state == QuoteState::ReplacePending
            || state.quote_state == QuoteState::CancelPending;
        decision.suppress_flags |= SuppressRisk;
        return decision;
    }

    const double theo_bid = state.last_theo_bid;
    const double theo_ask = state.last_theo_ask;
    if (theo_bid <= 0.0 || theo_ask < theo_bid
        || state.last_signal_ts == 0 || now_ns - state.last_signal_ts > STALE_NS) {
        decision.cancel_only = state.quote_state == QuoteState::Live
            || state.quote_state == QuoteState::ReplacePending
            || state.quote_state == QuoteState::CancelPending;
        decision.suppress_flags |= SuppressStaleTheo;
        return decision;
    }

    const MarketTick& md = tick_snapshot_[state.instrument_id];
    const bool has_market = md.recv_ts_ns > 0
        && md.bid_price[0] > 0.0
        && md.ask_price[0] > md.bid_price[0];
    if (!has_market || now_ns - md.recv_ts_ns > STALE_NS) {
        decision.cancel_only = state.quote_state == QuoteState::Live
            || state.quote_state == QuoteState::ReplacePending
            || state.quote_state == QuoteState::CancelPending;
        decision.suppress_flags |= SuppressInvalidMarket;
        return decision;
    }

    const int32_t max_pos = params_->max_position.load(std::memory_order_relaxed);
    const int32_t warning_pos = std::max<int32_t>(
        1, std::min(max_pos, params_->warning_position.load(std::memory_order_relaxed)));
    if (std::abs(state.net_position) >= max_pos) {
        decision.cancel_only = state.quote_state == QuoteState::Live
            || state.quote_state == QuoteState::ReplacePending
            || state.quote_state == QuoteState::CancelPending;
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

    double half_spread_ticks = params_->base_half_spread_ticks.load(std::memory_order_relaxed);
    const double max_half_spread_ticks =
        params_->max_half_spread_ticks.load(std::memory_order_relaxed);
    half_spread_ticks = std::max(half_spread_ticks,
                                 params_->min_half_spread_ticks.load(std::memory_order_relaxed));
    half_spread_ticks = std::max(half_spread_ticks, theo_width_ticks);
    half_spread_ticks = std::min(half_spread_ticks, max_half_spread_ticks);

    const double market_width_ticks = (md.ask_price[0] - md.bid_price[0]) / tick;
    const double widen_threshold =
        params_->market_width_widen_threshold_ticks.load(std::memory_order_relaxed);
    if (market_width_ticks > widen_threshold) {
        half_spread_ticks += 0.5 * (market_width_ticks - widen_threshold);
    }

    const double inventory_pressure =
        std::min(1.0, std::abs(static_cast<double>(state.net_position)) / warning_pos);
    double product_pressure = 0.0;
    const double total_delta = product_net_delta_ + static_cast<double>(underlying_net_position_);
    const double delta_threshold = params_->hedge_delta_threshold.load(std::memory_order_relaxed);
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
        decision.cancel_only = state.quote_state == QuoteState::Live
            || state.quote_state == QuoteState::ReplacePending
            || state.quote_state == QuoteState::CancelPending;
        decision.suppress_flags |= SuppressPosition;
        return decision;
    }

    double bid = round_down_to_tick(center - half_spread_ticks * tick, tick);
    double ask = round_up_to_tick(center + half_spread_ticks * tick, tick);

    const double max_passive_bid = round_down_to_tick(std::max(0.0, md.ask_price[0] - tick), tick);
    const double min_passive_ask = round_up_to_tick(md.bid_price[0] + tick, tick);
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
        decision.cancel_only = state.quote_state == QuoteState::Live
            || state.quote_state == QuoteState::ReplacePending
            || state.quote_state == QuoteState::CancelPending;
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

void OptionMMCoreStrategy::send_quote(OptionState& state,
                                      const QuoteDecision& decision,
                                      int64_t now_ns) noexcept {
    if (state.quote_state == QuoteState::Live && state.live_quote_id != 0) {
        send_cancel(state, now_ns);
        return;
    }

    if (state.quote_state == QuoteState::ReplacePending
        || state.quote_state == QuoteState::CancelPending
        || state.quote_state == QuoteState::CancelFailed) {
        return;
    }

    const double tick = instruments_[state.instrument_id].tick_size > 0.0
        ? instruments_[state.instrument_id].tick_size
        : 0.01;
    const double epsilon_px =
        params_->requote_price_epsilon_ticks.load(std::memory_order_relaxed) * tick;
    const int64_t min_interval_ns = static_cast<int64_t>(
        params_->min_quote_interval_ms.load(std::memory_order_relaxed) * 1'000'000.0);
    if (!is_material_change(state, decision, epsilon_px, min_interval_ns, now_ns)) {
        return;
    }

    Quote quote{};
    quote.client_quote_id = next_order_id();
    quote.instrument_id = state.instrument_id;
    quote.product_index = product_idx_;
    quote.bid_price = decision.bid;
    quote.ask_price = decision.ask;
    quote.bid_volume = decision.bid_vol;
    quote.ask_volume = decision.ask_vol;
    quote.bid_offset = OffsetFlag::Open;
    quote.ask_offset = OffsetFlag::Open;
    quote.send_ts = now_ns;

    if (!pre_risk_ || pre_risk_->check_quote(quote) != PreTradeRisk::RejectReason::OK) {
        return;
    }
    if (!quote_buf_ || !quote_buf_->try_push(quote)) {
        return;
    }

    state.pending_quote_id = quote.client_quote_id;
    state.live_bid = decision.bid;
    state.live_ask = decision.ask;
    state.live_bid_vol = decision.bid_vol;
    state.live_ask_vol = decision.ask_vol;
    state.last_quote_ts = now_ns;
    state.live_since_ts = 0;
    state.quote_state = QuoteState::ReplacePending;
}

void OptionMMCoreStrategy::send_cancel(OptionState& state, int64_t now_ns) noexcept {
    if (state.quote_state == QuoteState::Idle
        || state.quote_state == QuoteState::Suppressed
        || state.quote_state == QuoteState::CancelFailed) {
        return;
    }
    if (state.quote_state == QuoteState::CancelPending
        && now_ns - state.cancel_last_send_ts < CANCEL_RETRY_NS) {
        return;
    }
    if (state.quote_state == QuoteState::CancelPending && state.cancel_attempts >= MAX_CANCEL_ATTEMPTS) {
        return;
    }

    QuoteId target_quote_id = 0;
    if (state.quote_state == QuoteState::CancelPending) {
        target_quote_id = state.cancel_target_quote_id;
    } else if (state.live_quote_id != 0) {
        target_quote_id = state.live_quote_id;
    } else {
        target_quote_id = state.pending_quote_id;
    }
    if (target_quote_id == 0) return;

    Quote cancel{};
    cancel.client_quote_id = target_quote_id;
    cancel.instrument_id = state.instrument_id;
    cancel.product_index = product_idx_;
    cancel.bid_price = 0.0;
    cancel.ask_price = 0.0;
    cancel.bid_volume = 0;
    cancel.ask_volume = 0;
    cancel.send_ts = now_ns;
    if (quote_buf_ && quote_buf_->try_push(cancel)) {
        state.cancel_target_quote_id = target_quote_id;
        state.cancel_last_send_ts = now_ns;
        state.cancel_attempts = static_cast<uint8_t>(std::min<int>(
            MAX_CANCEL_ATTEMPTS,
            static_cast<int>(state.cancel_attempts) + 1));
        state.last_quote_ts = now_ns;
        state.quote_state = QuoteState::CancelPending;
    }
}

bool OptionMMCoreStrategy::manage_quote_lifecycle(OptionState& state, int64_t now_ns) noexcept {
    if (state.quote_state == QuoteState::CancelFailed) {
        if (quote_fully_filled(state)) {
            reset_quote_tracking(state, QuoteState::Idle);
            return false;
        }
        state.suppress_flags |= SuppressCancelStuck;
        return true;
    }

    if (state.quote_state == QuoteState::CancelPending) {
        if (quote_fully_filled(state)) {
            reset_quote_tracking(state, QuoteState::Idle);
            return false;
        }
        if (now_ns - state.cancel_last_send_ts < CANCEL_RETRY_NS) {
            return true;
        }
        if (state.cancel_attempts >= MAX_CANCEL_ATTEMPTS) {
            state.quote_state = QuoteState::CancelFailed;
            state.suppress_flags |= SuppressCancelStuck;
            publish_cancel_failed_alert(state, now_ns);
            return true;
        }
        send_cancel(state, now_ns);
        return true;
    }

    const int64_t live_since_ts = state.live_since_ts != 0 ? state.live_since_ts : state.last_quote_ts;
    if (state.quote_state == QuoteState::Live
        && state.live_quote_id != 0
        && live_since_ts > 0
        && now_ns - live_since_ts >= QUOTE_MAX_LIVE_NS) {
        send_cancel(state, now_ns);
        return true;
    }

    return false;
}

void OptionMMCoreStrategy::reset_quote_tracking(OptionState& state, QuoteState next_state) noexcept {
    state.live_bid = 0.0;
    state.live_ask = 0.0;
    state.live_bid_vol = 0;
    state.live_ask_vol = 0;
    state.live_quote_id = 0;
    state.pending_quote_id = 0;
    state.cancel_target_quote_id = 0;
    state.live_since_ts = 0;
    state.cancel_last_send_ts = 0;
    state.cancel_attempts = 0;
    state.quote_state = next_state;
}

void OptionMMCoreStrategy::publish_cancel_failed_alert(const OptionState& state, int64_t now_ns) noexcept {
    if (!alert_topic_) return;

    SystemAlert alert{};
    alert.ts_ns = now_ns;
    alert.instrument_id = state.instrument_id;
    alert.product_index = product_idx_;
    alert.type = SystemAlertType::QuoteCancelGiveUp;
    std::snprintf(alert.message,
                  sizeof(alert.message),
                  "quote cancel failed after %u attempts for instrument %u quote %llu",
                  static_cast<unsigned>(state.cancel_attempts),
                  static_cast<unsigned>(state.instrument_id),
                  static_cast<unsigned long long>(state.cancel_target_quote_id));
    alert_topic_->publish(alert);
}

bool OptionMMCoreStrategy::quote_fully_filled(const OptionState& state) const noexcept {
    const bool quote_known = state.live_quote_id != 0
        || state.pending_quote_id != 0
        || state.cancel_target_quote_id != 0;
    return quote_known && state.live_bid_vol <= 0 && state.live_ask_vol <= 0;
}

void OptionMMCoreStrategy::maybe_trigger_hedge(int64_t now_ns) noexcept {
    if (!params_ || !session_open_ || !params_->enabled.load(std::memory_order_relaxed)) return;
    if (!order_buf_ || !pre_risk_ || underlying_id_ >= MAX_INSTRUMENTS) return;
    if (live_hedge_order_id_ != 0 && live_hedge_remaining_ > 0) return;

    const double threshold = params_->hedge_delta_threshold.load(std::memory_order_relaxed);
    if (threshold <= 0.0) return;

    const double total_delta = product_net_delta_ + static_cast<double>(underlying_net_position_);
    const double excess_delta = std::fabs(total_delta) - threshold;
    if (excess_delta <= 0.0) return;

    const int64_t min_hedge_interval_ns = std::max<int64_t>(
        10'000'000LL,
        static_cast<int64_t>(params_->min_quote_interval_ms.load(std::memory_order_relaxed) * 1'000'000.0));
    if (now_ns - last_hedge_ts_ns_ < min_hedge_interval_ns) return;

    const MarketTick& underlying_md = tick_snapshot_[underlying_id_];
    if (underlying_md.recv_ts_ns == 0
        || now_ns - underlying_md.recv_ts_ns > STALE_NS
        || underlying_md.bid_price[0] <= 0.0
        || underlying_md.ask_price[0] <= underlying_md.bid_price[0]) {
        return;
    }

    Order hedge{};
    hedge.client_order_id = next_order_id();
    hedge.instrument_id = underlying_id_;
    hedge.product_index = product_idx_;
    hedge.side = (total_delta > 0.0) ? Side::Sell : Side::Buy;
    hedge.offset = OffsetFlag::Open;
    hedge.order_type = OrderType::FAK;
    hedge.price = (hedge.side == Side::Buy) ? underlying_md.ask_price[0] : underlying_md.bid_price[0];
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
    const double delta_threshold = params_->hedge_delta_threshold.load(std::memory_order_relaxed);
    const double vega_threshold = params_->product_vega_threshold.load(std::memory_order_relaxed);

    const bool delta_breach = delta_threshold > 0.0 && std::fabs(total_delta) > delta_threshold;
    const bool vega_breach = vega_threshold > 0.0 && std::fabs(product_net_vega_) > vega_threshold;
    return delta_breach || vega_breach;
}

bool OptionMMCoreStrategy::product_temporarily_suppressed(int64_t now_ns) const noexcept {
    return suppress_until_ns_ > now_ns;
}

bool OptionMMCoreStrategy::is_material_change(const OptionState& state,
                                              const QuoteDecision& decision,
                                              double epsilon_px,
                                              int64_t min_interval_ns,
                                              int64_t now_ns) const noexcept {
    if (state.quote_state == QuoteState::Idle || state.quote_state == QuoteState::Suppressed) {
        return true;
    }
    if (now_ns - state.last_quote_ts < min_interval_ns) {
        return false;
    }
    if (std::fabs(state.live_bid - decision.bid) > epsilon_px) return true;
    if (std::fabs(state.live_ask - decision.ask) > epsilon_px) return true;
    if (state.live_bid_vol != decision.bid_vol) return true;
    if (state.live_ask_vol != decision.ask_vol) return true;
    return false;
}

} // namespace omm
