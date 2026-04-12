#include "risk/pre_trade_risk.h"
#include <cstring>

namespace omm {

void PreTradeRisk::reset() noexcept {
    for (auto& s : slots_) s = OpenOrderEntry{};
    open_count_ = 0;
}

int PreTradeRisk::find_slot(OrderId id) const noexcept {
    for (int i = 0; i < MAX_OPEN_ORDERS; ++i)
        if (slots_[i].used && slots_[i].id == id) return i;
    return -1;
}

int PreTradeRisk::alloc_slot() noexcept {
    for (int i = 0; i < MAX_OPEN_ORDERS; ++i)
        if (!slots_[i].used) return i;
    return -1;
}

bool PreTradeRisk::would_self_trade(uint16_t instrument_id,
                                     Side side, double price) const noexcept {
    // A new BUY crosses if there is a resting own SELL at price <= new buy price
    // A new SELL crosses if there is a resting own BUY at price >= new sell price
    for (int i = 0; i < MAX_OPEN_ORDERS; ++i) {
        const auto& s = slots_[i];
        if (!s.used || s.instrument_id != instrument_id) continue;
        if (side == Side::Buy  && s.side == Side::Sell && s.price <= price) return true;
        if (side == Side::Sell && s.side == Side::Buy  && s.price >= price) return true;
    }
    return false;
}

PreTradeRisk::RejectReason
PreTradeRisk::check_order(const Order& order) noexcept {
    // 1. Max volume per order
    if (order.volume > cfg_.max_volume_per_order)
        return RejectReason::MAX_VOLUME;

    // 2. Open order count guard
    if (open_count_ >= MAX_OPEN_ORDERS)
        return RejectReason::TOO_MANY_OPEN_ORDERS;

    // 3. Self-trade check
    if (would_self_trade(order.instrument_id, order.side, order.price))
        return RejectReason::SELF_TRADE;

    return RejectReason::OK;
}

PreTradeRisk::RejectReason
PreTradeRisk::check_quote(const Quote& quote) noexcept {
    if (quote.bid_volume > cfg_.max_volume_per_order ||
        quote.ask_volume > cfg_.max_volume_per_order)
        return RejectReason::MAX_VOLUME;

    return RejectReason::OK;
}

void PreTradeRisk::on_order_ack(const Order& order) noexcept {
    int slot = alloc_slot();
    if (slot < 0) return;  // no space — should not happen if check passed
    slots_[slot].id            = order.client_order_id;
    slots_[slot].instrument_id = order.instrument_id;
    slots_[slot].side          = order.side;
    slots_[slot].price         = order.price;
    slots_[slot].remaining     = order.volume;
    slots_[slot].used          = true;
    ++open_count_;
}

void PreTradeRisk::on_order_fill(OrderId id, Volume filled_qty,
                                  bool fully_filled) noexcept {
    int slot = find_slot(id);
    if (slot < 0) return;
    slots_[slot].remaining -= filled_qty;
    if (fully_filled || slots_[slot].remaining <= 0) {
        slots_[slot].used = false;
        --open_count_;
    }
}

void PreTradeRisk::on_order_cancel(OrderId id) noexcept {
    int slot = find_slot(id);
    if (slot < 0) return;
    slots_[slot].used = false;
    --open_count_;
}

} // namespace omm
