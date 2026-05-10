#pragma once

#include "common/fixed_hash_table.h"
#include "common/types.h"
#include "common/config.h"

namespace omm {

// ─── Pre-trade risk (hard limits) ────────────────────────────────────────────
// Checked synchronously on the critical path before every order/quote submission.
// One instance per strategy thread — never shared, never locked.
//
// Checks:
//   1. Self-trade: reject if order would cross a resting own order
//   2. Max volume: reject if order volume exceeds configured max
//
// State tracking:
//   - Open orders only
// Quote lifecycle is strategy-owned and intentionally kept out of this module.

class PreTradeRisk {
public:
    enum class RejectReason : uint8_t {
        OK = 0,
        SELF_TRADE,
        MAX_VOLUME,
        TOO_MANY_OPEN_ORDERS,
    };

    /**
     * @brief PreTradeRisk.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    explicit PreTradeRisk(const HardRiskConfig& cfg) noexcept : cfg_(cfg) {
        reset();
    }

    // Check an outgoing order. Returns OK or the rejection reason.
    // Called from the strategy thread — must be noexcept, zero allocation.
    /**
     * @brief Check order.
     * @param order Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] RejectReason check_order(const Order& order) noexcept;

    // Check an outgoing quote with stateless hard checks only.
    // Quote lifecycle and live quote ownership remain in the strategy.
    /**
     * @brief Check quote.
     * @param quote Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] RejectReason check_quote(const Quote& quote) noexcept;

    // Lifecycle callbacks — keep open order state accurate
    /**
     * @brief On order ack.
     * @param order Parameter supplied by the caller.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    void on_order_ack   (const Order& order) noexcept;
    /**
     * @brief On order fill.
     * @param id Parameter supplied by the caller.
     * @param filled_qty Parameter supplied by the caller.
     * @param fully_filled Parameter supplied by the caller.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    void on_order_fill  (OrderId id, Volume filled_qty, bool fully_filled) noexcept;
    /**
     * @brief On order cancel.
     * @param id Parameter supplied by the caller.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    void on_order_cancel(OrderId id) noexcept;

    // Query
    /**
     * @brief Open order count.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] int open_order_count() const noexcept { return open_count_; }

    // Reset all state (used at session open)
    /**
     * @brief Reset.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    void reset() noexcept;

private:
    const HardRiskConfig& cfg_;

    // Open orders indexed by slot (linear scan — n is small, cache-hot)
    // Slots are recycled through free_stack_ so allocation is O(1) on the
    // single strategy-thread owner.
    struct OpenOrderEntry {
        OrderId  id{0};
        uint16_t instrument_id{INVALID_INSTRUMENT_ID};
        Side     side{Side::Buy};
        double   price{0.0};
        Volume   remaining{0};
        bool     used{false};
    };

    // Per-instrument side summary used for O(1) self-trade checks. The best
    // price is recomputed only when the removed order was the current best.
    struct InstrumentOpenState {
        double best_bid{0.0};
        double best_ask{0.0};
        uint16_t bid_count{0};
        uint16_t ask_count{0};
    };

    OpenOrderEntry slots_[MAX_OPEN_ORDERS]{};
    FixedHashTable<OrderId, uint16_t, MAX_OPEN_ORDERS * 2> order_index_{};
    InstrumentOpenState instrument_state_[MAX_INSTRUMENTS]{};
    uint16_t free_stack_[MAX_OPEN_ORDERS]{};
    uint16_t free_count_{0};
    int open_count_{0};

    // Find slot by order id; returns -1 if not found
    /**
     * @brief Find slot.
     * @param id Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] int find_slot(OrderId id) const noexcept;
    // Find a free slot; returns -1 if full
    /**
     * @brief Alloc slot.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] int alloc_slot() noexcept;

    /**
     * @brief Release slot.
     * @param slot Parameter supplied by the caller.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    void release_slot(uint16_t slot) noexcept;

    /**
     * @brief Note side add.
     * @param entry Parameter supplied by the caller.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    void note_side_add(const OpenOrderEntry& entry) noexcept;

    /**
     * @brief Note side remove.
     * @param entry Parameter supplied by the caller.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    void note_side_remove(const OpenOrderEntry& entry) noexcept;

    /**
     * @brief Recompute instrument side.
     * @param instrument_id Parameter supplied by the caller.
     * @param side Parameter supplied by the caller.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    void recompute_instrument_side(uint16_t instrument_id, Side side) noexcept;

    /**
     * @brief Remove slot.
     * @param slot Parameter supplied by the caller.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    void remove_slot(uint16_t slot) noexcept;

    // Check self-trade: would this new order match any existing resting order?
    /**
     * @brief Would self trade.
     * @param instrument_id Parameter supplied by the caller.
     * @param side Parameter supplied by the caller.
     * @param price Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] bool would_self_trade(uint16_t instrument_id,
                                         Side side, double price) const noexcept;
};

} // namespace omm
