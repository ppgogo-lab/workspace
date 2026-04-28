#pragma once

#include "common/ring_buffer.h"
#include "common/thread_utils.h"
#include "common/types.h"
#include "risk/pre_trade_risk.h"
#include "strategy/mm_framework.h"
#include "strategy/mm_params.h"
#include "strategy/order_lifecycle.h"
#include "strategy/quote_lifecycle.h"

namespace omm {

// ─── BaseQuotingStrategy ──────────────────────────────────────────────────────
// Abstract base class for market making strategies that handles quote and order
// lifecycle management. Subclasses implement pricing logic via hooks while the
// base class manages state machines, acks, cancels, and retries.
//
// Responsibilities:
// - Quote lifecycle management using QuoteLifecycleController
// - Order lifecycle tracking using OrderLifecycleController
// - Quote/order submission and cancellation
// - Pre-trade risk integration
// - Event routing to subclass hooks
//
// Subclasses must implement:
// - on_signal_impl() - Strategy-specific signal handling
// - on_fill_impl() - Strategy-specific fill handling
// - on_timer_impl() - Strategy-specific timer logic
//
// Subclasses use these protected helpers:
// - request_quote() - Submit quote with lifecycle management
// - request_cancel() - Cancel live quote
// - submit_order() - Submit order with lifecycle tracking
// - get_quote_state() - Access quote lifecycle state
// - get_order_tracker() - Access order lifecycle state
//
// Design: Final implementations of IMarketMaker callbacks route events through
// lifecycle controllers before calling subclass hooks. This ensures consistent
// lifecycle management across all strategies while allowing custom pricing logic.

class BaseQuotingStrategy : public IMarketMaker {
protected:
    // ─── Lifecycle Management Helpers ─────────────────────────────────────────
    // Submit a quote for the given instrument. Base class handles lifecycle
    // management, material change detection, pre-trade risk, and retry logic.
    void request_quote(uint16_t instrument_id,
                      double bid, double ask,
                      Volume bid_vol, Volume ask_vol,
                      int64_t now_ns) noexcept;

    // Cancel the live quote for the given instrument. Base class handles
    // lifecycle state transitions and retry logic.
    void request_cancel(uint16_t instrument_id, int64_t now_ns) noexcept;

    // Submit an order with lifecycle tracking. Returns the order ID.
    // Base class tracks the order through acks, fills, and cancels.
    OrderId submit_order(const Order& order) noexcept;

    // Cancel an order by ID. Base class handles lifecycle state transitions.
    void cancel_order(OrderId id) noexcept;

    // ─── State Access ─────────────────────────────────────────────────────────
    // Access quote lifecycle state for an instrument. Returns nullptr if
    // instrument is not active.
    [[nodiscard]] const QuoteLifecycleState* get_quote_state(
        uint16_t instrument_id) const noexcept;

    // Access order lifecycle tracker by order ID. Returns nullptr if order
    // is not tracked.
    [[nodiscard]] const OrderLifecycleTracker* get_order_tracker(
        OrderId id) const noexcept;

    // ─── Subclass Hooks (Pure Virtual) ────────────────────────────────────────
    // Called when a pricing signal arrives. Subclass implements pricing logic
    // and calls request_quote() or request_cancel() as needed.
    virtual void on_signal_impl(const PricingSignal& signal) noexcept = 0;

    // Called when a fill report arrives. Subclass updates positions and risk
    // metrics. Base class has already updated quote/order lifecycle state.
    virtual void on_fill_impl(const Trade& trade) noexcept = 0;

    // Called on timer events (hedge check, quote refresh, session open/close).
    // Subclass implements periodic tasks like hedging and session management.
    virtual void on_timer_impl(const TimerEvent& event) noexcept = 0;

    // ─── IMarketMaker Implementation (Final) ──────────────────────────────────
    // These methods are final to ensure consistent lifecycle management.
    // Subclasses cannot override; use the _impl hooks instead.
    void on_signal(const PricingSignal& signal) noexcept final;
    void on_fill(const Trade& trade) noexcept final;
    void on_quote_ack(const Quote& quote) noexcept final;
    void on_quote_cancel(const Quote& quote) noexcept final;
    void on_quote_reject(const Quote& quote) noexcept final;
    void on_order_ack(const Order& order) noexcept final;
    void on_order_cancel(OrderId id) noexcept final;
    void on_order_reject(const Order& order) noexcept final;
    void on_timer(const TimerEvent& event) noexcept final;

    // ─── Per-Instrument State ─────────────────────────────────────────────────
    // Stores quote lifecycle state and position for each instrument.
    struct InstrumentState {
        bool active{false};
        uint16_t instrument_id{INVALID_INSTRUMENT_ID};
        QuoteLifecycleState quote_lifecycle{};
        int32_t net_position{0};
    };

    InstrumentState instrument_state_[MAX_INSTRUMENTS]{};

    // ─── Order Tracking ───────────────────────────────────────────────────────
    // Fixed-size array of order trackers. Orders are tracked from submission
    // through terminal states (Filled/Cancelled/Rejected).
    static constexpr size_t MAX_TRACKED_ORDERS = 256;
    OrderLifecycleTracker order_trackers_[MAX_TRACKED_ORDERS]{};
    uint16_t order_tracker_count_{0};

    // ─── Configuration ────────────────────────────────────────────────────────
    // Quote lifecycle configuration (timeouts, retry limits).
    QuoteLifecycleConfig quote_config_{
        3'000'000'000LL,  // quote_max_live_ns (3s)
        1'000'000'000LL,  // cancel_retry_ns (1s)
        3                 // max_cancel_attempts
    };

private:
    // ─── Internal Lifecycle Management ───────────────────────────────────────
    // Manage quote lifecycle for an instrument (timeouts, retries, cancel failures).
    // Returns true if lifecycle management blocks new quotes.
    bool manage_quote_lifecycle(uint16_t instrument_id, int64_t now_ns) noexcept;

    // Internal quote submission. Constructs Quote struct, checks pre-trade risk,
    // pushes to ring buffer, and updates lifecycle state.
    void send_quote_internal(uint16_t instrument_id,
                            double bid, double ask,
                            Volume bid_vol, Volume ask_vol,
                            int64_t now_ns) noexcept;

    // Internal cancel submission. Constructs cancel Quote (zero volume),
    // pushes to ring buffer, and updates lifecycle state.
    void send_cancel_internal(uint16_t instrument_id, int64_t now_ns) noexcept;

    // Find order tracker by order ID. Returns nullptr if not found.
    OrderLifecycleTracker* find_order_tracker(OrderId id) noexcept;

    // Allocate a new order tracker slot. Returns nullptr if all slots are full.
    OrderLifecycleTracker* allocate_order_tracker() noexcept;
};

} // namespace omm