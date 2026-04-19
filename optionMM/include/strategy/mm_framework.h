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
    virtual void on_signal(const PricingSignal& signal) noexcept = 0;

    // Called when a fill report arrives for an order sent by this strategy.
    virtual void on_fill(const Trade& trade) noexcept = 0;

    // Called when an order ack arrives (exchange accepted our order).
    virtual void on_order_ack(const Order& order) noexcept = 0;

    // Called when a quote ack arrives (exchange accepted or replaced our quote).
    virtual void on_quote_ack(const Quote& quote) noexcept = 0;

    // Called when a quote is cancelled or fully withdrawn.
    virtual void on_quote_cancel(const Quote& quote) noexcept = 0;

    // Called when a quote is rejected by the gateway/exchange.
    virtual void on_quote_reject(const Quote& quote) noexcept = 0;

    // Called when an order is cancelled.
    virtual void on_order_cancel(OrderId id) noexcept = 0;

    // Called when an order is rejected.
    virtual void on_order_reject(const Order& order) noexcept = 0;

    // Called by the timer thread (hedge check, quote refresh, session open/close).
    virtual void on_timer(const TimerEvent& event) noexcept = 0;

    // Return true if this strategy is currently enabled and quoting.
    [[nodiscard]] virtual bool is_enabled() const noexcept = 0;

    // Return the product index (strategy slot index).
    [[nodiscard]] virtual uint8_t product_index() const noexcept = 0;

    [[nodiscard]] virtual bool read_product_monitor_state(ProductMonitorState* out) const noexcept {
        if (out != nullptr) *out = ProductMonitorState{};
        return false;
    }

    [[nodiscard]] virtual int read_instrument_monitor_states(InstrumentMonitorState* out,
                                                             int max_count) const noexcept {
        (void)out;
        (void)max_count;
        return 0;
    }

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
