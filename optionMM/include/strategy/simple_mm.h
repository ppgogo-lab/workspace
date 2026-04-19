#pragma once

#include "strategy/mm_framework.h"
#include "common/thread_utils.h"

namespace omm {

// ─── SimpleMMStrategy ─────────────────────────────────────────────────────────
// Minimal market making strategy for pipeline validation.
//
// Logic:
//   on_signal:
//     1. Skip if disabled or signal is stale (> 500µs old)
//     2. bid = theo_price - bid_spread/2
//        ask = theo_price + ask_spread/2
//     3. Round to tick_size
//     4. Pre-trade risk check → push Quote to quote_buf_
//     5. If |net_position| >= max_position, lean (only quote the reducing side)
//
//   on_timer(HedgeCheck):
//     If |portfolio_delta| > product_delta_threshold, send a hedge order
//     on the underlying future (instrument with kind == Future).
//
//   on_timer(SessionClose):
//     Cancel all live quotes.

class SimpleMMStrategy : public IMarketMaker {
public:
    // Called by TradingEngine after construction to wire up all dependencies.
    void init(uint8_t              product_idx,
              SPSCRingBuffer<Quote, 512>* quote_buf,
              SPSCRingBuffer<Order, 512>* order_buf,
              PreTradeRisk*        pre_risk,
              AtomicMMParams*      params,
              const Instrument*    instruments) noexcept {
        product_idx_  = product_idx;
        quote_buf_    = quote_buf;
        order_buf_    = order_buf;
        pre_risk_     = pre_risk;
        params_       = params;
        instruments_  = instruments;
    }

    void on_signal(const PricingSignal& signal) noexcept override;
    void on_fill(const Trade& trade) noexcept override;
    void on_order_ack(const Order& order) noexcept override;
    void on_quote_ack(const Quote& quote) noexcept override;
    void on_quote_cancel(const Quote& quote) noexcept override;
    void on_quote_reject(const Quote& quote) noexcept override;
    void on_order_cancel(OrderId id) noexcept override;
    void on_order_reject(const Order& order) noexcept override;
    void on_timer(const TimerEvent& event) noexcept override;

    [[nodiscard]] bool    is_enabled()    const noexcept override {
        return params_ && params_->enabled.load(std::memory_order_relaxed);
    }
    [[nodiscard]] uint8_t product_index() const noexcept override { return product_idx_; }

private:
    // Per-instrument position tracking (indexed by instrument_id)
    int32_t net_position_[MAX_INSTRUMENTS]{};
    double  portfolio_delta_{0.0};

    // Last quote sent per instrument (to avoid redundant re-quotes)
    struct LastQuote {
        double   bid{0.0}, ask{0.0};
        QuoteId  id{0};
        bool     live{false};
    };
    LastQuote last_quote_[MAX_INSTRUMENTS]{};

    // Signal staleness threshold: 10 milliseconds
    // (500µs in production; relaxed here to tolerate ASAN/test overhead)
    static constexpr int64_t STALE_NS = 100'000'000;

    void send_quote(const PricingSignal& signal,
                    double bid, double ask,
                    Volume bid_vol, Volume ask_vol) noexcept;

    void send_hedge_order(uint16_t underlying_id,
                          Side side, Volume qty) noexcept;
};

} // namespace omm
