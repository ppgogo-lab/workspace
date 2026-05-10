#pragma once

#include "common/types.h"

#include <algorithm>

namespace omm {

// Order lifecycle state machine. Tracks the state of a single order from
// submission through terminal states (Filled/Cancelled/Rejected).
enum class OrderLifecycleState : uint8_t {
    Idle = 0,         // No order active
    Pending,          // Submitted, awaiting ack
    Live,             // Acknowledged by exchange
    CancelPending,    // Cancel requested, awaiting confirmation
    Filled,           // Fully filled
    Cancelled,        // Cancelled by exchange
    Rejected,         // Rejected by exchange
};

// Per-order runtime state tracker. Stores all information needed to track
// an order through its lifecycle, including partial fills.
struct OrderLifecycleTracker {
    OrderLifecycleState status{OrderLifecycleState::Idle};
    OrderId order_id{0};
    uint16_t instrument_id{INVALID_INSTRUMENT_ID};
    Side side{Side::Buy};
    Volume original_volume{0};
    Volume filled_volume{0};
    Volume remaining_volume{0};
    double price{0.0};
    Timestamp submit_ts{0};
    Timestamp ack_ts{0};
    bool is_hedge{false};
    uint8_t _pad[7]{};
};

// Static utility class for managing order lifecycle state transitions.
// Provides stateless methods for tracking orders from submission through
// terminal states. All methods are noexcept and suitable for hot-path use.
//
// State transitions:
//   Idle → Pending (on note_order_submitted)
//   Pending → Live (on on_order_ack)
//   Pending → Rejected (on on_reject)
//   Live → Filled (on on_fill when fully filled)
//   Live → CancelPending (on cancel request)
//   Live → Cancelled (on on_cancel)
//   CancelPending → Cancelled (on on_cancel)
class OrderLifecycleController {
public:
    // Record order submission. Transitions from Idle to Pending.
    /**
     * @brief Note order submitted.
     * @param tracker Parameter supplied by the caller.
     * @param order Parameter supplied by the caller.
     * @param now_ns Parameter supplied by the caller.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    static void note_order_submitted(OrderLifecycleTracker& tracker,
                                     const Order& order,
                                     int64_t now_ns) noexcept {
        tracker.status = OrderLifecycleState::Pending;
        tracker.order_id = order.client_order_id;
        tracker.instrument_id = order.instrument_id;
        tracker.side = order.side;
        tracker.original_volume = order.volume;
        tracker.filled_volume = 0;
        tracker.remaining_volume = order.volume;
        tracker.price = order.price;
        tracker.submit_ts = now_ns;
        tracker.ack_ts = 0;
        tracker.is_hedge = order.is_hedge;
    }

    // Handle order acknowledgment. Transitions from Pending to Live.
    /**
     * @brief On order ack.
     * @param tracker Parameter supplied by the caller.
     * @param order Parameter supplied by the caller.
     * @param now_ns Parameter supplied by the caller.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    static void on_order_ack(OrderLifecycleTracker& tracker,
                            const Order& order,
                            int64_t now_ns) noexcept {
        if (tracker.status != OrderLifecycleState::Pending) return;
        if (tracker.order_id != order.client_order_id) return;

        tracker.status = OrderLifecycleState::Live;
        tracker.ack_ts = now_ns;
    }

    /**
     * @brief Note cancel submitted.
     * @param tracker Parameter supplied by the caller.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    static void note_cancel_submitted(OrderLifecycleTracker& tracker) noexcept {
        if (tracker.status != OrderLifecycleState::Pending
            && tracker.status != OrderLifecycleState::Live) {
            return;
        }
        tracker.status = OrderLifecycleState::CancelPending;
    }

    // Handle partial or full fill. Updates filled/remaining volumes.
    // Transitions to Filled when remaining_volume reaches zero.
    /**
     * @brief On fill.
     * @param tracker Parameter supplied by the caller.
     * @param fill_volume Parameter supplied by the caller.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    static void on_fill(OrderLifecycleTracker& tracker,
                       Volume fill_volume) noexcept {
        if (tracker.status != OrderLifecycleState::Live
            && tracker.status != OrderLifecycleState::CancelPending) {
            return;
        }

        tracker.filled_volume += fill_volume;
        tracker.remaining_volume = std::max<Volume>(
            0, tracker.original_volume - tracker.filled_volume);

        if (tracker.remaining_volume <= 0) {
            tracker.status = OrderLifecycleState::Filled;
        }
    }

    // Handle order cancellation. Transitions to Cancelled.
    /**
     * @brief On cancel.
     * @param tracker Parameter supplied by the caller.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    static void on_cancel(OrderLifecycleTracker& tracker) noexcept {
        if (tracker.status == OrderLifecycleState::Idle
            || tracker.status == OrderLifecycleState::Filled
            || tracker.status == OrderLifecycleState::Rejected) {
            return;
        }
        tracker.status = OrderLifecycleState::Cancelled;
    }

    // Handle order rejection. Transitions to Rejected.
    /**
     * @brief On reject.
     * @param tracker Parameter supplied by the caller.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    static void on_reject(OrderLifecycleTracker& tracker) noexcept {
        if (tracker.status == OrderLifecycleState::Idle
            || tracker.status == OrderLifecycleState::Filled
            || tracker.status == OrderLifecycleState::Cancelled) {
            return;
        }
        tracker.status = OrderLifecycleState::Rejected;
    }

    // Check if order is currently live (acknowledged and not yet terminal).
    /**
     * @brief Is live.
     * @param tracker Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] static bool is_live(const OrderLifecycleTracker& tracker) noexcept {
        return tracker.status == OrderLifecycleState::Live
            || tracker.status == OrderLifecycleState::CancelPending;
    }

    // Check if order has reached a terminal state (Filled/Cancelled/Rejected).
    /**
     * @brief Is terminal.
     * @param tracker Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] static bool is_terminal(const OrderLifecycleTracker& tracker) noexcept {
        return tracker.status == OrderLifecycleState::Filled
            || tracker.status == OrderLifecycleState::Cancelled
            || tracker.status == OrderLifecycleState::Rejected;
    }

    // Reset tracker to Idle state.
    /**
     * @brief Reset.
     * @param tracker Parameter supplied by the caller.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    static void reset(OrderLifecycleTracker& tracker) noexcept {
        tracker = OrderLifecycleTracker{};
    }
};

} // namespace omm
