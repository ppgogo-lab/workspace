#pragma once

#include "strategy/base_quoting_strategy.h"
#include "common/thread_utils.h"

namespace omm {

// ─── TemplateMMStrategy ───────────────────────────────────────────────────────
// Example template for creating new market making strategies.
//
// This template demonstrates how to extend BaseQuotingStrategy to implement
// custom pricing logic. The base class handles all quote/order lifecycle
// management, allowing you to focus on strategy-specific logic.
//
// To create a new strategy:
// 1. Copy this file and rename the class
// 2. Implement on_signal_impl() with your pricing logic
// 3. Implement on_fill_impl() to update positions and risk metrics
// 4. Implement on_timer_impl() for periodic tasks (hedging, session management)
// 5. Add strategy-specific state as private members
//
// The base class provides:
// - request_quote() - Submit quote with lifecycle management
// - request_cancel() - Cancel live quote
// - submit_order() - Submit order with lifecycle tracking
// - get_quote_state() - Access quote lifecycle state
// - get_order_tracker() - Access order lifecycle state
//
// Example usage:
//   auto* strategy = new TemplateMMStrategy();
//   strategy->init(product_idx, quote_buf, order_buf, pre_risk, params, instruments);

class TemplateMMStrategy : public BaseQuotingStrategy {
public:
    // Called by TradingEngine after construction to wire up all dependencies.
    void init(uint8_t product_idx,
              SPSCRingBuffer<Quote, 512>* quote_buf,
              SPSCRingBuffer<Order, 512>* order_buf,
              PreTradeRisk* pre_risk,
              AtomicMMParams* params,
              const Instrument* instruments) noexcept {
        product_idx_ = product_idx;
        quote_buf_ = quote_buf;
        order_buf_ = order_buf;
        pre_risk_ = pre_risk;
        params_ = params;
        instruments_ = instruments;

        // Initialize strategy-specific state here
        for (auto& pos : net_position_) pos = 0;
        portfolio_delta_ = 0.0;

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

    [[nodiscard]] bool is_enabled() const noexcept override {
        return params_ && params_->enabled.load(std::memory_order_relaxed);
    }

    [[nodiscard]] uint8_t product_index() const noexcept override {
        return product_idx_;
    }

protected:
    // ─── Subclass Hooks ───────────────────────────────────────────────────────
    // Called when a pricing signal arrives. Implement your pricing logic here.
    void on_signal_impl(const PricingSignal& signal) noexcept override {
        // 1. Check if strategy is enabled
        if (!params_ || !params_->enabled.load(std::memory_order_relaxed)) return;

        // 2. Validate signal
        const uint16_t id = signal.instrument_id;
        if (id >= MAX_INSTRUMENTS) return;
        if (!instrument_state_[id].active) return;

        // 3. Check signal staleness
        const int64_t now_ns = get_monotonic_ns();
        static constexpr int64_t STALE_NS = 100'000'000LL;  // 100ms
        if (now_ns - signal.calc_ts_ns > STALE_NS) return;

        // 4. Extract theo prices
        const double theo_bid = signal.theo_bid;
        const double theo_ask = signal.theo_ask;
        if (theo_bid <= 0.0 || theo_ask < theo_bid) return;

        // 5. Build quote decision (YOUR PRICING LOGIC HERE)
        const Instrument& instr = instruments_[id];
        const double tick = instr.tick_size > 0.0 ? instr.tick_size : 0.01;

        // Example: Simple spread-based pricing
        const double spread = params_->bid_spread.load(std::memory_order_relaxed);
        const double theo_mid = 0.5 * (theo_bid + theo_ask);
        double bid = theo_mid - spread * 0.5;
        double ask = theo_mid + spread * 0.5;

        // Round to tick size
        bid = std::floor(bid / tick) * tick;
        ask = std::ceil(ask / tick) * tick;

        if (bid <= 0.0 || ask <= bid) return;

        // 6. Size logic (YOUR SIZING LOGIC HERE)
        Volume quote_vol = params_->quote_volume.load(std::memory_order_relaxed);
        Volume bid_vol = quote_vol;
        Volume ask_vol = quote_vol;

        // Example: Inventory lean at position limits
        const int32_t max_pos = params_->max_position.load(std::memory_order_relaxed);
        const int32_t pos = instrument_state_[id].net_position;
        if (pos >= max_pos) bid_vol = 0;   // long limit: stop buying
        if (pos <= -max_pos) ask_vol = 0;  // short limit: stop selling
        if (bid_vol == 0 && ask_vol == 0) return;

        // 7. Submit quote (base class handles lifecycle)
        request_quote(id, bid, ask, bid_vol, ask_vol, now_ns);
    }

    // Called when a fill report arrives. Update positions and risk metrics.
    void on_fill_impl(const Trade& trade) noexcept override {
        // Base class has already updated quote/order lifecycle state and
        // instrument_state_[].net_position. Add strategy-specific logic here.

        if (trade.instrument_id >= MAX_INSTRUMENTS) return;

        // Example: Update portfolio delta for hedge triggering
        const int32_t signed_qty = (trade.side == Side::Buy ? 1 : -1) * trade.fill_volume;
        portfolio_delta_ += signed_qty * 0.5;  // Approximate delta per lot
    }

    // Called on timer events. Implement periodic tasks here.
    void on_timer_impl(const TimerEvent& event) noexcept override {
        switch (event.type) {
        case TimerEventType::HedgeCheck:
            // Example: Trigger hedge if portfolio delta exceeds threshold
            if (!params_) break;
            {
                const double threshold = params_->product_delta_threshold.load(
                    std::memory_order_relaxed);
                if (std::fabs(portfolio_delta_) < threshold) break;

                // Find underlying future for this product
                for (uint16_t i = 0; i < MAX_INSTRUMENTS; ++i) {
                    const Instrument& instr = instruments_[i];
                    if (instr.instrument_id == INVALID_INSTRUMENT_ID) continue;
                    if (instr.kind != InstrumentKind::Future) continue;
                    if (instr.product_index != product_idx_) continue;

                    // Submit hedge order
                    Order hedge{};
                    hedge.client_order_id = next_order_id();
                    hedge.instrument_id = i;
                    hedge.product_index = product_idx_;
                    hedge.side = (portfolio_delta_ > 0) ? Side::Sell : Side::Buy;
                    hedge.order_type = OrderType::Market;
                    hedge.volume = static_cast<Volume>(std::fabs(portfolio_delta_));
                    hedge.is_hedge = true;
                    hedge.send_ts = event.trigger_ts_ns;

                    submit_order(hedge);
                    break;
                }
            }
            break;

        case TimerEventType::SessionClose:
            // Cancel all live quotes
            for (uint16_t i = 0; i < MAX_INSTRUMENTS; ++i) {
                if (instrument_state_[i].active) {
                    request_cancel(i, event.trigger_ts_ns);
                }
            }
            break;

        default:
            break;
        }
    }

private:
    // Strategy-specific state
    int32_t net_position_[MAX_INSTRUMENTS]{};
    double portfolio_delta_{0.0};
};

} // namespace omm