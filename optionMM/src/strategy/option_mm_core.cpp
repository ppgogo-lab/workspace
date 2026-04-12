#include "strategy/option_mm_core.h"

#include <algorithm>
#include <cmath>

namespace omm {

namespace {

double clamp01(double value) noexcept {
    return std::clamp(value, 0.0, 1.0);
}

double round_down_to_tick(double price, double tick) noexcept {
    return std::floor(price / tick) * tick;
}

double round_up_to_tick(double price, double tick) noexcept {
    return std::ceil(price / tick) * tick;
}

} // namespace

void OptionMMCoreStrategy::init(uint8_t product_idx,
                                SPSCRingBuffer<Quote, 512>* quote_buf,
                                SPSCRingBuffer<Order, 512>* order_buf,
                                PreTradeRisk* pre_risk,
                                AtomicMMParams* params,
                                const Instrument* instruments,
                                const MarketTick* tick_snapshot,
                                const PostTradeRisk* post_risk) noexcept {
    product_idx_ = product_idx;
    quote_buf_ = quote_buf;
    order_buf_ = order_buf;
    pre_risk_ = pre_risk;
    params_ = params;
    instruments_ = instruments;
    tick_snapshot_ = tick_snapshot;
    post_risk_ = post_risk;

    option_count_ = 0;
    product_net_delta_ = 0.0;
    product_net_vega_ = 0.0;
    session_open_ = true;
    for (auto& state : option_state_) state = OptionState{};

    for (uint16_t id = 0; id < MAX_INSTRUMENTS; ++id) {
        const Instrument& instr = instruments_[id];
        if (instr.instrument_id == INVALID_INSTRUMENT_ID) continue;
        if (instr.product_index != product_idx_) continue;
        if (instr.kind != InstrumentKind::Option) continue;
        if (option_count_ >= MAX_INSTRUMENTS) break;

        option_ids_[option_count_++] = id;
        OptionState& state = option_state_[id];
        state.active = true;
        state.instrument_id = id;
        state.underlying_id = instr.underlying_id;
    }
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
    state.last_underlying_px = 0.5 * (signal.underlying_ref_bid + signal.underlying_ref_ask);
    state.last_signal_ts = signal.calc_ts_ns;

    update_product_exposure(state, old_delta, old_vega);
    maybe_quote(id);
}

void OptionMMCoreStrategy::on_fill(const Trade& trade) noexcept {
    if (trade.instrument_id >= MAX_INSTRUMENTS) return;
    OptionState& state = option_state_[trade.instrument_id];
    if (!state.active) return;

    const int signed_qty = (trade.side == Side::Buy ? 1 : -1) * trade.fill_volume;
    state.net_position += signed_qty;
    product_net_delta_ += state.last_delta * signed_qty;
    product_net_vega_ += state.last_vega * signed_qty;

    if (trade.client_order_id == state.live_quote_id && state.quote_state == QuoteState::ReplacePending) {
        state.quote_state = QuoteState::Live;
    }

    maybe_quote(trade.instrument_id);
}

void OptionMMCoreStrategy::on_order_ack(const Order& order) noexcept {
    if (pre_risk_) pre_risk_->on_order_ack(order);
}

void OptionMMCoreStrategy::on_quote_ack(const Quote& quote) noexcept {
    if (quote.instrument_id >= MAX_INSTRUMENTS) return;
    OptionState& state = option_state_[quote.instrument_id];
    if (!state.active) return;

    state.live_quote_id = quote.client_quote_id;
    state.live_bid = quote.bid_price;
    state.live_ask = quote.ask_price;
    state.live_bid_vol = quote.bid_volume;
    state.live_ask_vol = quote.ask_volume;
    state.quote_state = (quote.bid_volume == 0 && quote.ask_volume == 0)
        ? QuoteState::Suppressed
        : QuoteState::Live;
}

void OptionMMCoreStrategy::on_order_cancel(OrderId id) noexcept {
    if (pre_risk_) pre_risk_->on_order_cancel(id);
}

void OptionMMCoreStrategy::on_order_reject(const Order&) noexcept {}

void OptionMMCoreStrategy::on_timer(const TimerEvent& event) noexcept {
    switch (event.type) {
    case TimerEventType::QuoteRefresh:
    case TimerEventType::HedgeCheck:
        reevaluate_all();
        break;
    case TimerEventType::SessionOpen:
        session_open_ = true;
        reevaluate_all();
        break;
    case TimerEventType::SessionClose:
        session_open_ = false;
        for (uint16_t i = 0; i < option_count_; ++i) {
            send_cancel(option_state_[option_ids_[i]], event.trigger_ts_ns);
        }
        break;
    default:
        break;
    }
}

void OptionMMCoreStrategy::reevaluate_all() noexcept {
    for (uint16_t i = 0; i < option_count_; ++i) {
        maybe_quote(option_ids_[i]);
    }
}

void OptionMMCoreStrategy::maybe_quote(uint16_t instrument_id) noexcept {
    if (instrument_id >= MAX_INSTRUMENTS) return;
    OptionState& state = option_state_[instrument_id];
    if (!state.active) return;

    const int64_t now_ns = get_monotonic_ns();
    const QuoteDecision decision = build_decision(state, now_ns);
    if (decision.cancel_only) {
        send_cancel(state, now_ns);
        return;
    }
    if (!decision.valid) {
        return;
    }
    send_quote(state, decision, now_ns);
}

OptionMMCoreStrategy::QuoteDecision
OptionMMCoreStrategy::build_decision(OptionState& state, int64_t now_ns) const noexcept {
    QuoteDecision decision{};
    const Instrument& instr = instruments_[state.instrument_id];
    const double tick = instr.tick_size > 0.0 ? instr.tick_size : 0.01;
    const double theo_bid = state.last_theo_bid;
    const double theo_ask = state.last_theo_ask;
    const double theo_mid = 0.5 * (theo_bid + theo_ask);

    if (!params_ || !session_open_ || !params_->enabled.load(std::memory_order_relaxed)) {
        decision.cancel_only = (state.quote_state == QuoteState::Live
            || state.quote_state == QuoteState::ReplacePending);
        decision.suppress_flags |= SuppressSession;
        return decision;
    }

    if (post_risk_ && post_risk_->any_breach()) {
        decision.cancel_only = (state.quote_state == QuoteState::Live
            || state.quote_state == QuoteState::ReplacePending);
        decision.suppress_flags |= SuppressRisk;
        return decision;
    }

    if (theo_bid <= 0.0 || theo_ask < theo_bid
        || state.last_signal_ts == 0 || now_ns - state.last_signal_ts > STALE_NS) {
        decision.suppress_flags |= SuppressStaleTheo;
    }
    if (theo_bid <= 0.0 || theo_ask < theo_bid
        || state.last_signal_ts == 0 || now_ns - state.last_signal_ts > STALE_NS) {
        decision.cancel_only = (state.quote_state == QuoteState::Live
            || state.quote_state == QuoteState::ReplacePending);
        return decision;
    }

    const MarketTick& md = tick_snapshot_[state.instrument_id];
    const bool has_market = md.recv_ts_ns > 0
        && md.bid_price[0] > 0.0
        && md.ask_price[0] > md.bid_price[0];
    if (!has_market || now_ns - md.recv_ts_ns > STALE_NS) {
        decision.cancel_only = (state.quote_state == QuoteState::Live
            || state.quote_state == QuoteState::ReplacePending);
        decision.suppress_flags |= SuppressInvalidMarket;
        return decision;
    }

    const int32_t max_pos = params_->max_position.load(std::memory_order_relaxed);
    const int32_t warning_pos = std::max<int32_t>(1, params_->warning_position.load(std::memory_order_relaxed));
    if (std::abs(state.net_position) >= max_pos) {
        decision.cancel_only = (state.quote_state == QuoteState::Live
            || state.quote_state == QuoteState::ReplacePending);
        decision.suppress_flags |= SuppressPosition;
        return decision;
    }

    const double follow_weight = clamp01(params_->follow_weight.load(std::memory_order_relaxed));
    const double market_mid = 0.5 * (md.bid_price[0] + md.ask_price[0]);
    const double center = theo_mid * (1.0 - follow_weight) + market_mid * follow_weight;

    double half_spread_ticks = params_->base_half_spread_ticks.load(std::memory_order_relaxed);
    half_spread_ticks = std::max(half_spread_ticks,
                                 params_->min_half_spread_ticks.load(std::memory_order_relaxed));
    half_spread_ticks = std::min(half_spread_ticks,
                                 params_->max_half_spread_ticks.load(std::memory_order_relaxed));

    const double market_width_ticks = (md.ask_price[0] - md.bid_price[0]) / tick;
    if (market_width_ticks > params_->market_width_widen_threshold_ticks.load(std::memory_order_relaxed)) {
        half_spread_ticks += 0.5 * (market_width_ticks -
                                    params_->market_width_widen_threshold_ticks.load(std::memory_order_relaxed));
    }

    const double product_delta_threshold = params_->hedge_delta_threshold.load(std::memory_order_relaxed);
    if (product_delta_threshold > 0.0 && std::fabs(product_net_delta_) > product_delta_threshold) {
        half_spread_ticks *= 1.5;
    }
    half_spread_ticks = std::min(half_spread_ticks,
                                 params_->max_half_spread_ticks.load(std::memory_order_relaxed));

    const double inv_skew_ticks =
        params_->inventory_skew_per_lot_ticks.load(std::memory_order_relaxed) * state.net_position;
    const double inventory_pressure = std::min(1.0, std::abs(static_cast<double>(state.net_position)) / warning_pos);
    half_spread_ticks = std::min(half_spread_ticks * (1.0 + inventory_pressure),
                                 params_->max_half_spread_ticks.load(std::memory_order_relaxed));

    const double shift = inv_skew_ticks * tick;
    const double bid_raw = center - shift - half_spread_ticks * tick;
    const double ask_raw = center - shift + half_spread_ticks * tick;

    Volume bid_vol = params_->quote_volume.load(std::memory_order_relaxed);
    Volume ask_vol = bid_vol;
    if (params_->use_one_sided_at_limits.load(std::memory_order_relaxed)
        && std::abs(state.net_position) >= warning_pos) {
        if (state.net_position > 0) bid_vol = 0;
        if (state.net_position < 0) ask_vol = 0;
    }
    if (bid_vol == 0 && ask_vol == 0) {
        decision.cancel_only = (state.quote_state == QuoteState::Live
            || state.quote_state == QuoteState::ReplacePending);
        decision.suppress_flags |= SuppressPosition;
        return decision;
    }

    const double bid = round_down_to_tick(bid_raw, tick);
    const double ask = round_up_to_tick(ask_raw, tick);
    if (bid <= 0.0 || ask <= bid) {
        decision.cancel_only = (state.quote_state == QuoteState::Live
            || state.quote_state == QuoteState::ReplacePending);
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

    state.live_quote_id = quote.client_quote_id;
    state.live_bid = quote.bid_price;
    state.live_ask = quote.ask_price;
    state.live_bid_vol = quote.bid_volume;
    state.live_ask_vol = quote.ask_volume;
    state.last_quote_ts = now_ns;
    state.quote_state = QuoteState::ReplacePending;
}

void OptionMMCoreStrategy::send_cancel(OptionState& state, int64_t now_ns) noexcept {
    if (state.quote_state == QuoteState::Idle || state.quote_state == QuoteState::Suppressed) {
        return;
    }

    Quote cancel{};
    cancel.client_quote_id = next_order_id();
    cancel.instrument_id = state.instrument_id;
    cancel.product_index = product_idx_;
    cancel.bid_volume = 0;
    cancel.ask_volume = 0;
    cancel.send_ts = now_ns;
    if (quote_buf_ && quote_buf_->try_push(cancel)) {
        state.live_quote_id = cancel.client_quote_id;
        state.live_bid_vol = 0;
        state.live_ask_vol = 0;
        state.last_quote_ts = now_ns;
        state.quote_state = QuoteState::Suppressed;
    }
}

void OptionMMCoreStrategy::update_product_exposure(OptionState& state,
                                                   double old_delta,
                                                   double old_vega) noexcept {
    const double pos = static_cast<double>(state.net_position);
    product_net_delta_ += (state.last_delta - old_delta) * pos;
    product_net_vega_ += (state.last_vega - old_vega) * pos;
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
