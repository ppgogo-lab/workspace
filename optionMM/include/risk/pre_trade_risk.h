#pragma once

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

    explicit PreTradeRisk(const HardRiskConfig& cfg) noexcept : cfg_(cfg) {
        reset();
    }

    // Check an outgoing order. Returns OK or the rejection reason.
    // Called from the strategy thread — must be noexcept, zero allocation.
    [[nodiscard]] RejectReason check_order(const Order& order) noexcept;

    // Check an outgoing quote with stateless hard checks only.
    // Quote lifecycle and live quote ownership remain in the strategy.
    [[nodiscard]] RejectReason check_quote(const Quote& quote) noexcept;

    // Lifecycle callbacks — keep open order state accurate
    void on_order_ack   (const Order& order) noexcept;
    void on_order_fill  (OrderId id, Volume filled_qty, bool fully_filled) noexcept;
    void on_order_cancel(OrderId id) noexcept;

    // Query
    [[nodiscard]] int open_order_count() const noexcept { return open_count_; }

    // Reset all state (used at session open)
    void reset() noexcept;

private:
    const HardRiskConfig& cfg_;

    // Open orders indexed by slot (linear scan — n is small, cache-hot)
    struct OpenOrderEntry {
        OrderId  id{0};
        uint16_t instrument_id{INVALID_INSTRUMENT_ID};
        Side     side{Side::Buy};
        double   price{0.0};
        Volume   remaining{0};
        bool     used{false};
    };
    OpenOrderEntry slots_[MAX_OPEN_ORDERS]{};
    int open_count_{0};

    // Find slot by order id; returns -1 if not found
    [[nodiscard]] int find_slot(OrderId id) const noexcept;
    // Find a free slot; returns -1 if full
    [[nodiscard]] int alloc_slot() noexcept;

    // Check self-trade: would this new order match any existing resting order?
    [[nodiscard]] bool would_self_trade(uint16_t instrument_id,
                                         Side side, double price) const noexcept;
};

} // namespace omm
