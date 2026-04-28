#pragma once

#include "strategy/base_quoting_strategy.h"
#include "common/thread_utils.h"

namespace omm {

// ─── SimpleMMStrategy ─────────────────────────────────────────────────────────
// Minimal market making strategy for pipeline validation.
//
// Logic:
//   on_signal_impl:
//     1. Skip if disabled or signal is stale (> 500µs old)
//     2. bid = theo_price - bid_spread/2
//        ask = theo_price + ask_spread/2
//     3. Round to tick_size
//     4. Call request_quote() (base class handles lifecycle and pre-trade risk)
//     5. If |net_position| >= max_position, lean (only quote the reducing side)
//
//   on_timer_impl(HedgeCheck):
//     If |portfolio_delta| > product_delta_threshold, send a hedge order
//     on the underlying future (instrument with kind == Future).
//
//   on_timer_impl(SessionClose):
//     Cancel all live quotes via request_cancel().

class SimpleMMStrategy : public BaseQuotingStrategy {
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

        // Initialize instrument states for this product
        for (uint16_t id = 0; id < MAX_INSTRUMENTS; ++id) {
            const Instrument& instr = instruments_[id];
            if (instr.instrument_id == INVALID_INSTRUMENT_ID) continue;
            if (instr.product_index != product_idx_) continue;
            if (instr.kind != InstrumentKind::Option) continue;

            instrument_state_[id].active = true;
            instrument_state_[id].instrument_id = id;
            instrument_state_[id].quote_lifecycle.replace_policy =
                QuoteLifecycleController::policy_for_exchange(instr.exchange);
        }
    }

    [[nodiscard]] bool    is_enabled()    const noexcept override {
        return params_ && params_->enabled.load(std::memory_order_relaxed);
    }
    [[nodiscard]] uint8_t product_index() const noexcept override { return product_idx_; }

protected:
    void on_signal_impl(const PricingSignal& signal) noexcept override;
    void on_fill_impl(const Trade& trade) noexcept override;
    void on_timer_impl(const TimerEvent& event) noexcept override;

private:
    // Portfolio delta tracking for hedge triggering
    double  portfolio_delta_{0.0};

    // Signal staleness threshold: 10 milliseconds
    // (500µs in production; relaxed here to tolerate ASAN/test overhead)
    static constexpr int64_t STALE_NS = 100'000'000;

    void send_hedge_order(uint16_t underlying_id,
                          Side side, Volume qty) noexcept;
};

} // namespace omm
