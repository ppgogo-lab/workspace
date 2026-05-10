#pragma once

#include "common/types.h"
#include "common/ring_buffer.h"
#include "risk/pre_trade_risk.h"
#include "strategy/mm_params.h"

namespace omm {

enum class StrategyQuoteMonitorState : uint8_t {
    Idle = 0,
    Live,
    AckPending,
    CancelPending,
    CancelFailed,
    Suppressed,
};

struct ProductMonitorState {
    uint8_t product_index{0xFF};
    bool strategy_enabled{false};
    bool session_open{true};
    bool product_suppressed{false};
    bool exposure_breached{false};
    bool underlying_shock_suppressed{false};
    bool risk_breach{false};
};

struct InstrumentMonitorState {
    uint16_t instrument_id{INVALID_INSTRUMENT_ID};
    uint8_t product_index{0xFF};
    StrategyQuoteMonitorState quote_state{StrategyQuoteMonitorState::Idle};
    uint8_t cancel_attempts{0};
    int32_t net_position{0};
    uint32_t suppress_flags{0};
    Timestamp last_quote_ts_ns{0};
};

struct StrategyRuntimeStats {
    uint64_t full_book_reevaluations{0};
    uint64_t single_instrument_reevaluations{0};
};

// ─── IMarketMaker ─────────────────────────────────────────────────────────────
// Abstract interface for a per-product market making strategy.
// One instance per product, running on a dedicated strategy thread.
//
// All on_* methods are called from the strategy thread only.
// They must be noexcept — an exception on a SCHED_FIFO thread calls std::terminate.
//
// Output goes directly into the ring buffers owned by TradingEngine.
// The strategy never allocates — orders and quotes are placed into pre-existing
// ring buffer slots.

class IMarketMaker {
public:
    // Called when the pricer thread delivers a new pricing signal for this product.
    /**
     * @brief On signal.
     * @param signal Parameter supplied by the caller.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    virtual void on_signal(const PricingSignal& signal) noexcept = 0;

    // Called when the strategy thread drains a batch of pricing signals.
    /**
     * @brief On signals.
     * @param signals Parameter supplied by the caller.
     * @param count Parameter supplied by the caller.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    virtual void on_signals(const PricingSignal* signals, int count) noexcept {
        if (signals == nullptr || count <= 0) return;
        for (int i = 0; i < count; ++i) {
            on_signal(signals[i]);
        }
    }

    // Called when a fill report arrives for an order sent by this strategy.
    /**
     * @brief On fill.
     * @param trade Parameter supplied by the caller.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    virtual void on_fill(const Trade& trade) noexcept = 0;

    // Called when an order ack arrives (exchange accepted our order).
    /**
     * @brief On order ack.
     * @param order Parameter supplied by the caller.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    virtual void on_order_ack(const Order& order) noexcept = 0;

    // Called when a quote ack arrives (exchange accepted or replaced our quote).
    /**
     * @brief On quote ack.
     * @param quote Parameter supplied by the caller.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    virtual void on_quote_ack(const Quote& quote) noexcept = 0;

    // Called when a quote is cancelled or fully withdrawn.
    /**
     * @brief On quote cancel.
     * @param quote Parameter supplied by the caller.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    virtual void on_quote_cancel(const Quote& quote) noexcept = 0;

    // Called when a quote is rejected by the gateway/exchange.
    /**
     * @brief On quote reject.
     * @param quote Parameter supplied by the caller.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    virtual void on_quote_reject(const Quote& quote) noexcept = 0;

    // Called when an order is cancelled.
    /**
     * @brief On order cancel.
     * @param id Parameter supplied by the caller.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    virtual void on_order_cancel(OrderId id) noexcept = 0;

    // Called when an order is rejected.
    /**
     * @brief On order reject.
     * @param order Parameter supplied by the caller.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    virtual void on_order_reject(const Order& order) noexcept = 0;

    // Called by the timer thread (hedge check, quote refresh, session open/close).
    /**
     * @brief On timer.
     * @param event Parameter supplied by the caller.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    virtual void on_timer(const TimerEvent& event) noexcept = 0;

    // Return true if this strategy is currently enabled and quoting.
    /**
     * @brief Is enabled.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] virtual bool is_enabled() const noexcept = 0;

    // Return the product index (strategy slot index).
    /**
     * @brief Product index.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] virtual uint8_t product_index() const noexcept = 0;

    /**
     * @brief Read product monitor state.
     * @param out Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] virtual bool read_product_monitor_state(ProductMonitorState* out) const noexcept {
        if (out != nullptr) *out = ProductMonitorState{};
        return false;
    }

    /**
     * @brief Read instrument monitor states.
     * @param out Parameter supplied by the caller.
     * @param max_count Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] virtual int read_instrument_monitor_states(InstrumentMonitorState* out,
                                                             int max_count) const noexcept {
        (void)out;
        (void)max_count;
        return 0;
    }

    /**
     * @brief Read runtime stats.
     * @param out Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] virtual bool read_runtime_stats(StrategyRuntimeStats* out) const noexcept {
        if (out != nullptr) *out = StrategyRuntimeStats{};
        return false;
    }

    /**
     * @brief IMarketMaker.
     * @return None.
     */
    virtual ~IMarketMaker() = default;

protected:
    // Set by TradingEngine at construction. Strategy pushes quotes/orders here.
    SPSCRingBuffer<Quote, 512>* quote_buf_{nullptr};
    SPSCRingBuffer<Order, 512>* order_buf_{nullptr};

    // Pre-trade risk checker for this strategy thread (not shared).
    PreTradeRisk* pre_risk_{nullptr};

    // Runtime-adjustable parameters (written by gRPC server, read here).
    AtomicMMParams* params_{nullptr};

    // Instrument registry pointer (read-only after startup).
    const Instrument* instruments_{nullptr};

    uint8_t  product_idx_{0};
    uint64_t order_seq_{0};   // local sequence for order ID generation

    // Generate a unique client order ID: (product_idx << 32) | local_seq
    [[nodiscard]] OrderId next_order_id() noexcept {
        return (static_cast<uint64_t>(product_idx_) << 32) | (++order_seq_);
    }
};

} // namespace omm
