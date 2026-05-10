#include "risk/pre_trade_risk.h"

namespace omm {

/**
 * @brief Implements Reset.
 * @return None.
 * @note Rebuilds the fixed free-slot stack and clears fixed lookup tables; no
 * dynamic allocation is performed.
 */
void PreTradeRisk::reset() noexcept {
    for (auto& s : slots_) s = OpenOrderEntry{};
    order_index_.clear();
    for (auto& state : instrument_state_) state = InstrumentOpenState{};
    for (uint16_t i = 0; i < MAX_OPEN_ORDERS; ++i) {
        free_stack_[i] = static_cast<uint16_t>(MAX_OPEN_ORDERS - 1 - i);
    }
    free_count_ = MAX_OPEN_ORDERS;
    open_count_ = 0;
}

/**
 * @brief Implements Find slot.
 * @param id Parameter supplied by the caller.
 * @return Return value produced by the operation.
 * @note Uses the fixed order id index instead of scanning open slots.
 */
int PreTradeRisk::find_slot(OrderId id) const noexcept {
    const uint16_t* slot = order_index_.find(id);
    if (slot == nullptr || *slot >= MAX_OPEN_ORDERS) return -1;
    const OpenOrderEntry& entry = slots_[*slot];
    return entry.used && entry.id == id ? static_cast<int>(*slot) : -1;
}

/**
 * @brief Implements Alloc slot.
 * @return Return value produced by the operation.
 * @note Pops from the fixed free-slot stack in O(1).
 */
int PreTradeRisk::alloc_slot() noexcept {
    if (free_count_ == 0) return -1;
    return static_cast<int>(free_stack_[--free_count_]);
}

/**
 * @brief Implements Release slot.
 * @param slot Parameter supplied by the caller.
 * @return None.
 * @note Pushes back to the fixed free-slot stack in O(1).
 */
void PreTradeRisk::release_slot(uint16_t slot) noexcept {
    if (slot >= MAX_OPEN_ORDERS || free_count_ >= MAX_OPEN_ORDERS) return;
    free_stack_[free_count_++] = slot;
}

/**
 * @brief Implements Note side add.
 * @param entry Parameter supplied by the caller.
 * @return None.
 * @note Maintains per-instrument best own bid/ask for O(1) self-trade checks.
 */
void PreTradeRisk::note_side_add(const OpenOrderEntry& entry) noexcept {
    if (entry.instrument_id >= MAX_INSTRUMENTS) return;
    InstrumentOpenState& state = instrument_state_[entry.instrument_id];
    if (entry.side == Side::Buy) {
        ++state.bid_count;
        if (state.bid_count == 1 || entry.price > state.best_bid) {
            state.best_bid = entry.price;
        }
    } else {
        ++state.ask_count;
        if (state.ask_count == 1 || entry.price < state.best_ask) {
            state.best_ask = entry.price;
        }
    }
}

/**
 * @brief Implements Note side remove.
 * @param entry Parameter supplied by the caller.
 * @return None.
 * @note Recomputes only when removing the current best price for that side.
 */
void PreTradeRisk::note_side_remove(const OpenOrderEntry& entry) noexcept {
    if (entry.instrument_id >= MAX_INSTRUMENTS) return;
    InstrumentOpenState& state = instrument_state_[entry.instrument_id];
    if (entry.side == Side::Buy) {
        if (state.bid_count > 0) --state.bid_count;
        if (state.bid_count == 0) {
            state.best_bid = 0.0;
        } else if (entry.price >= state.best_bid) {
            recompute_instrument_side(entry.instrument_id, Side::Buy);
        }
    } else {
        if (state.ask_count > 0) --state.ask_count;
        if (state.ask_count == 0) {
            state.best_ask = 0.0;
        } else if (entry.price <= state.best_ask) {
            recompute_instrument_side(entry.instrument_id, Side::Sell);
        }
    }
}

/**
 * @brief Implements Recompute instrument side.
 * @param instrument_id Parameter supplied by the caller.
 * @param side Parameter supplied by the caller.
 * @return None.
 * @note This cold repair path runs only when the current best own price is
 * removed; the outgoing order check remains O(1).
 */
void PreTradeRisk::recompute_instrument_side(uint16_t instrument_id, Side side) noexcept {
    if (instrument_id >= MAX_INSTRUMENTS) return;
    InstrumentOpenState& state = instrument_state_[instrument_id];
    if (side == Side::Buy) {
        double best_bid = 0.0;
        uint16_t count = 0;
        for (const OpenOrderEntry& entry : slots_) {
            if (!entry.used || entry.instrument_id != instrument_id || entry.side != Side::Buy) {
                continue;
            }
            ++count;
            if (count == 1 || entry.price > best_bid) best_bid = entry.price;
        }
        state.bid_count = count;
        state.best_bid = count > 0 ? best_bid : 0.0;
    } else {
        double best_ask = 0.0;
        uint16_t count = 0;
        for (const OpenOrderEntry& entry : slots_) {
            if (!entry.used || entry.instrument_id != instrument_id || entry.side != Side::Sell) {
                continue;
            }
            ++count;
            if (count == 1 || entry.price < best_ask) best_ask = entry.price;
        }
        state.ask_count = count;
        state.best_ask = count > 0 ? best_ask : 0.0;
    }
}

/**
 * @brief Implements Remove slot.
 * @param slot Parameter supplied by the caller.
 * @return None.
 * @note Clears all fixed indexes before returning the slot to the free stack.
 */
void PreTradeRisk::remove_slot(uint16_t slot) noexcept {
    if (slot >= MAX_OPEN_ORDERS) return;
    OpenOrderEntry& entry = slots_[slot];
    if (!entry.used) return;

    const OpenOrderEntry old = entry;
    entry.used = false;
    note_side_remove(old);
    (void)order_index_.erase(old.id);
    entry = OpenOrderEntry{};
    release_slot(slot);
    if (open_count_ > 0) --open_count_;
}

/**
 * @brief Implements Would self trade.
 * @param instrument_id Parameter supplied by the caller.
 * @param side Parameter supplied by the caller.
 * @param price Parameter supplied by the caller.
 * @return Return value produced by the operation.
 * @note Reads per-instrument best own bid/ask, avoiding an open-order scan on
 * every outgoing order.
 */
bool PreTradeRisk::would_self_trade(uint16_t instrument_id,
                                     Side side, double price) const noexcept {
    if (instrument_id >= MAX_INSTRUMENTS) return false;
    const InstrumentOpenState& state = instrument_state_[instrument_id];
    if (side == Side::Buy) {
        return state.ask_count > 0 && state.best_ask <= price;
    }
    return state.bid_count > 0 && state.best_bid >= price;
}

PreTradeRisk::RejectReason
/**
 * @brief Implements Check order.
 * @param order Parameter supplied by the caller.
 * @return Return value produced by the operation.
 * @note Hot-path checks are fixed-cost except for bounded hash probing.
 */
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
/**
 * @brief Implements Check quote.
 * @param quote Parameter supplied by the caller.
 * @return Return value produced by the operation.
 * @note Quote hard-risk checks remain stateless so quote lifecycle ownership
 * stays inside the strategy.
 */
PreTradeRisk::check_quote(const Quote& quote) noexcept {
    if (quote.bid_volume > cfg_.max_volume_per_order ||
        quote.ask_volume > cfg_.max_volume_per_order)
        return RejectReason::MAX_VOLUME;

    return RejectReason::OK;
}

/**
 * @brief Implements On order ack.
 * @param order Parameter supplied by the caller.
 * @return None.
 * @note Inserts into fixed slot, order-id index, and per-instrument side summary.
 */
void PreTradeRisk::on_order_ack(const Order& order) noexcept {
    if (order.client_order_id == 0 || order.instrument_id >= MAX_INSTRUMENTS) return;
    const int existing = find_slot(order.client_order_id);
    if (existing >= 0) {
        remove_slot(static_cast<uint16_t>(existing));
    }

    const int slot = alloc_slot();
    if (slot < 0) return;

    OpenOrderEntry& entry = slots_[slot];
    entry.id            = order.client_order_id;
    entry.instrument_id = order.instrument_id;
    entry.side          = order.side;
    entry.price         = order.price;
    entry.remaining     = order.volume;
    entry.used          = true;

    if (!order_index_.insert(order.client_order_id, static_cast<uint16_t>(slot))) {
        entry = OpenOrderEntry{};
        release_slot(static_cast<uint16_t>(slot));
        return;
    }

    note_side_add(entry);
    ++open_count_;
}

/**
 * @brief Implements On order fill.
 * @param id Parameter supplied by the caller.
 * @param filled_qty Parameter supplied by the caller.
 * @param fully_filled Parameter supplied by the caller.
 * @return None.
 * @note Removes the order from fixed indexes when remaining volume reaches zero.
 */
void PreTradeRisk::on_order_fill(OrderId id, Volume filled_qty,
                                  bool fully_filled) noexcept {
    const int slot = find_slot(id);
    if (slot < 0) return;
    OpenOrderEntry& entry = slots_[slot];
    entry.remaining -= filled_qty;
    if (fully_filled || entry.remaining <= 0) {
        remove_slot(static_cast<uint16_t>(slot));
    }
}

/**
 * @brief Implements On order cancel.
 * @param id Parameter supplied by the caller.
 * @return None.
 * @note Removes the order from fixed indexes and per-instrument side summary.
 */
void PreTradeRisk::on_order_cancel(OrderId id) noexcept {
    const int slot = find_slot(id);
    if (slot < 0) return;
    remove_slot(static_cast<uint16_t>(slot));
}

} // namespace omm
