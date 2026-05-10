#include "strategy/simple_mm.h"
#include <cmath>

namespace omm {

/**
 * @brief Implements On signal impl.
 * @param signal Parameter supplied by the caller.
 * @return None.
 * @note Noexcept API preserves hot-path failure and latency invariants.
 */
void SimpleMMStrategy::on_signal_impl(const PricingSignal& signal) noexcept {
    if (!params_ || !params_->enabled.load(std::memory_order_relaxed)) return;

    // Staleness check: reject signals older than 500µs
    int64_t now = get_monotonic_ns();
    if (now - signal.calc_ts_ns > STALE_NS) return;

    const uint16_t id = signal.instrument_id;
    if (id >= MAX_INSTRUMENTS) return;
    if (!instrument_state_[id].active) return;

    // Load params (relaxed — eventual consistency acceptable)
    double bid_spread = params_->bid_spread.load(std::memory_order_relaxed);
    double ask_spread = params_->ask_spread.load(std::memory_order_relaxed);
    Volume quote_vol  = params_->quote_volume.load(std::memory_order_relaxed);
    int32_t max_pos   = params_->max_position.load(std::memory_order_relaxed);

    const double theo_bid = signal.theo_bid;
    const double theo_ask = signal.theo_ask;
    double theo = 0.5 * (theo_bid + theo_ask);
    if (theo_bid <= 0.0 || theo_ask < theo_bid) return;
    if (theo <= 0.0) return;

    double bid = theo - bid_spread * 0.5;
    double ask = theo + ask_spread * 0.5;

    // Round to tick size
    const Instrument& instr = instruments_[id];
    double tick = instr.tick_size > 0.0 ? instr.tick_size : 0.01;
    bid = std::floor(bid / tick) * tick;
    ask = std::ceil (ask / tick) * tick;

    if (bid <= 0.0 || ask <= bid) return;

    // Inventory lean: if at position limit, only quote the reducing side
    int32_t pos = instrument_state_[id].net_position;
    Volume bid_vol = quote_vol, ask_vol = quote_vol;
    if (pos >= max_pos)  bid_vol = 0;   // long limit: stop buying
    if (pos <= -max_pos) ask_vol = 0;   // short limit: stop selling
    if (bid_vol == 0 && ask_vol == 0) return;

    // Submit quote (base class handles lifecycle and pre-trade risk)
    request_quote(id, bid, ask, bid_vol, ask_vol, now);
}

/**
 * @brief Implements On fill impl.
 * @param trade Parameter supplied by the caller.
 * @return None.
 * @note Noexcept API preserves hot-path failure and latency invariants.
 */
void SimpleMMStrategy::on_fill_impl(const Trade& trade) noexcept {
    // Base class has already updated instrument_state_[].net_position
    if (trade.instrument_id >= MAX_INSTRUMENTS) return;

    // Update portfolio delta (approximate: delta ≈ 0.5 per option unit)
    // Exact delta is updated by the risk monitor via PostTradeRisk.
    // Here we just track a rough delta for hedge triggering.
    portfolio_delta_ += (trade.side == Side::Buy ? 1.0 : -1.0) * trade.fill_volume * 0.5;
}

/**
 * @brief Implements On timer impl.
 * @param event Parameter supplied by the caller.
 * @return None.
 * @note Noexcept API preserves hot-path failure and latency invariants.
 */
void SimpleMMStrategy::on_timer_impl(const TimerEvent& event) noexcept {
    switch (event.type) {
    case TimerEventType::HedgeCheck: {
        if (!params_) break;
        double threshold = params_->product_delta_threshold.load(std::memory_order_relaxed);
        if (std::fabs(portfolio_delta_) < threshold) break;

        // Find the underlying future instrument for this product
        // (instrument with kind == Future and same product_index)
        for (uint16_t i = 0; i < MAX_INSTRUMENTS; ++i) {
            const Instrument& instr = instruments_[i];
            if (instr.instrument_id == INVALID_INSTRUMENT_ID) continue;
            if (instr.kind != InstrumentKind::Future) continue;
            if (instr.product_index != product_idx_) continue;

            // Hedge: sell if long delta, buy if short delta
            Side  side = (portfolio_delta_ > 0) ? Side::Sell : Side::Buy;
            Volume qty = static_cast<Volume>(std::fabs(portfolio_delta_));
            if (qty > 0) send_hedge_order(i, side, qty);
            break;
        }
        break;
    }
    case TimerEventType::SessionClose:
        // Cancel all live quotes via base class helper
        for (uint16_t i = 0; i < MAX_INSTRUMENTS; ++i) {
            if (instrument_state_[i].active) {
                request_cancel(i, event.trigger_ts_ns);
            }
        }
        break;
    default:
        break;
    }
}

/**
 * @brief Implements Send hedge order.
 * @param underlying_id Parameter supplied by the caller.
 * @param side Parameter supplied by the caller.
 * @param qty Parameter supplied by the caller.
 * @return None.
 * @note Noexcept API preserves hot-path failure and latency invariants.
 */
void SimpleMMStrategy::send_hedge_order(uint16_t underlying_id,
                                         Side side, Volume qty) noexcept {
    if (!order_buf_) return;
    Order o{};
    o.client_order_id = next_order_id();
    o.instrument_id   = underlying_id;
    o.product_index   = product_idx_;
    o.side            = side;
    o.price_type      = OrderPriceType::Market;
    o.order_type      = OrderType::GFD;
    o.volume          = qty;
    o.is_hedge        = true;
    o.send_ts         = get_monotonic_ns();

    // Use base class helper for order submission with lifecycle tracking
    submit_order(o);
}

} // namespace omm
