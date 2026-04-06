#include "strategy/simple_mm.h"
#include <cmath>

namespace omm {

void SimpleMMStrategy::on_signal(const PricingSignal& signal) noexcept {
    if (!params_ || !params_->enabled.load(std::memory_order_relaxed)) return;

    // Staleness check: reject signals older than 500µs
    int64_t now = get_monotonic_ns();
    if (now - signal.greeks.calc_ts_ns > STALE_NS) return;

    const uint16_t id = signal.greeks.instrument_id;
    if (id >= MAX_INSTRUMENTS) return;

    // Load params (relaxed — eventual consistency acceptable)
    double bid_spread = params_->bid_spread.load(std::memory_order_relaxed);
    double ask_spread = params_->ask_spread.load(std::memory_order_relaxed);
    Volume quote_vol  = params_->quote_volume.load(std::memory_order_relaxed);
    int32_t max_pos   = params_->max_position.load(std::memory_order_relaxed);

    double theo = signal.greeks.theo_price;
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
    int32_t pos = net_position_[id];
    Volume bid_vol = quote_vol, ask_vol = quote_vol;
    if (pos >= max_pos)  bid_vol = 0;   // long limit: stop buying
    if (pos <= -max_pos) ask_vol = 0;   // short limit: stop selling
    if (bid_vol == 0 && ask_vol == 0) return;

    send_quote(signal, bid, ask, bid_vol, ask_vol);
}

void SimpleMMStrategy::send_quote(const PricingSignal& signal,
                                   double bid, double ask,
                                   Volume bid_vol, Volume ask_vol) noexcept {
    const uint16_t id = signal.greeks.instrument_id;

    Quote q{};
    q.client_quote_id = next_order_id();
    q.instrument_id   = id;
    q.product_index   = product_idx_;
    q.bid_price       = bid;
    q.ask_price       = ask;
    q.bid_volume      = bid_vol;
    q.ask_volume      = ask_vol;
    q.send_ts         = get_monotonic_ns();

    // Pre-trade risk check
    if (pre_risk_->check_quote(q) != PreTradeRisk::RejectReason::OK) return;

    // Push to gateway dispatcher (non-blocking; drop if full — back-pressure)
    if (quote_buf_->try_push(q)) {
        last_quote_[id].bid  = bid;
        last_quote_[id].ask  = ask;
        last_quote_[id].id   = q.client_quote_id;
        last_quote_[id].live = true;
    }
}

void SimpleMMStrategy::on_fill(const Trade& trade) noexcept {
    if (trade.instrument_id >= MAX_INSTRUMENTS) return;

    int32_t qty = trade.fill_volume;
    if (trade.side == Side::Buy)
        net_position_[trade.instrument_id] += qty;
    else
        net_position_[trade.instrument_id] -= qty;

    // Update portfolio delta (approximate: delta ≈ 0.5 per option unit)
    // Exact delta is updated by the risk monitor via PostTradeRisk.
    // Here we just track a rough delta for hedge triggering.
    portfolio_delta_ += (trade.side == Side::Buy ? 1.0 : -1.0) * qty * 0.5;

    pre_risk_->on_order_fill(trade.client_order_id, qty, true);
}

void SimpleMMStrategy::on_order_ack(const Order& order) noexcept {
    pre_risk_->on_order_ack(order);
}

void SimpleMMStrategy::on_order_cancel(OrderId id) noexcept {
    pre_risk_->on_order_cancel(id);
    // Mark quote as no longer live
    for (auto& lq : last_quote_)
        if (lq.id == id) { lq.live = false; break; }
}

void SimpleMMStrategy::on_timer(const TimerEvent& event) noexcept {
    switch (event.type) {
    case TimerEventType::HedgeCheck: {
        if (!params_) break;
        double threshold = params_->hedge_delta_threshold.load(std::memory_order_relaxed);
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
        // Cancel all live quotes by sending zero-volume quotes
        for (uint16_t i = 0; i < MAX_INSTRUMENTS; ++i) {
            if (!last_quote_[i].live) continue;
            Quote cancel{};
            cancel.client_quote_id = next_order_id();
            cancel.instrument_id   = i;
            cancel.product_index   = product_idx_;
            cancel.bid_volume      = 0;
            cancel.ask_volume      = 0;
            (void)quote_buf_->try_push(cancel);
            last_quote_[i].live = false;
        }
        break;
    default:
        break;
    }
}

void SimpleMMStrategy::send_hedge_order(uint16_t underlying_id,
                                         Side side, Volume qty) noexcept {
    if (!order_buf_) return;
    Order o{};
    o.client_order_id = next_order_id();
    o.instrument_id   = underlying_id;
    o.product_index   = product_idx_;
    o.side            = side;
    o.order_type      = OrderType::Market;
    o.volume          = qty;
    o.is_hedge        = true;
    o.send_ts         = get_monotonic_ns();

    if (pre_risk_->check_order(o) == PreTradeRisk::RejectReason::OK)
        (void)order_buf_->try_push(o);
}

} // namespace omm
