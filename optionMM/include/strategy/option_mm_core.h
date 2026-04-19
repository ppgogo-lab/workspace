#pragma once

#include "common/thread_utils.h"
#include "monitoring/topic.h"
#include "risk/post_trade_risk.h"
#include "strategy/mm_framework.h"

#include <array>
#include <atomic>

namespace omm {

// Core option market-making strategy.
// Responsibilities:
// - maintain one quote state machine per option instrument
// - track product-level delta/vega exposure and trigger hedge orders
// - gate quoting on session, risk, exposure, and temporary shock conditions
// - publish a monitor-friendly mirror of internal state
class OptionMMCoreStrategy : public IMarketMaker {
public:
    // Wire the strategy to the product, IO buffers, shared params, and monitor sinks.
    void init(uint8_t product_idx,
              SPSCRingBuffer<Quote, 512>* quote_buf,
              SPSCRingBuffer<Order, 512>* order_buf,
              PreTradeRisk* pre_risk,
              AtomicMMParams* params,
              const Instrument* instruments,
              const MarketTick* tick_snapshot,
              const PostTradeRisk* post_risk,
              MonitoringTopic<SystemAlert, 256>* alert_topic,
              bool supports_quote_replace = false) noexcept;

    // Feed-driven callbacks from pricing, execution, and timer subsystems.
    void on_signal(const PricingSignal& signal) noexcept override;
    void on_fill(const Trade& trade) noexcept override;
    void on_order_ack(const Order& order) noexcept override;
    void on_quote_ack(const Quote& quote) noexcept override;
    void on_quote_cancel(const Quote& quote) noexcept override;
    void on_quote_reject(const Quote& quote) noexcept override;
    void on_order_cancel(OrderId id) noexcept override;
    void on_order_reject(const Order& order) noexcept override;
    void on_timer(const TimerEvent& event) noexcept override;

    // Monitor-facing snapshot accessors.
    [[nodiscard]] bool is_enabled() const noexcept override;
    [[nodiscard]] uint8_t product_index() const noexcept override { return product_idx_; }
    [[nodiscard]] bool read_product_monitor_state(ProductMonitorState* out) const noexcept override;
    [[nodiscard]] int read_instrument_monitor_states(InstrumentMonitorState* out,
                                                     int max_count) const noexcept override;

private:
    // Reasons why the strategy is not willing to quote this instrument right now.
    enum SuppressFlags : uint32_t {
        SuppressNone = 0,  // No suppression reason is active.
        SuppressStaleTheo = 1u << 0,  // Theo is missing, crossed, or too old.
        SuppressInvalidMarket = 1u << 1,  // Market is stale/invalid or generated prices are unusable.
        SuppressPosition = 1u << 2,  // Instrument inventory is at or beyond quoting limits.
        SuppressRisk = 1u << 3,  // Post-trade risk has an active breach.
        SuppressSession = 1u << 4,  // Session closed or strategy disabled.
        SuppressThrottle = 1u << 5,  // Reserved for future explicit throttle signalling.
        SuppressUnderlyingShock = 1u << 6,  // Product is in a temporary underlying-move hold window.
        SuppressProductExposure = 1u << 7,  // Product delta/vega exceeded MM exposure thresholds.
        SuppressCancelStuck = 1u << 8,  // Quote cancel exhausted retries and needs trader attention.
    };

    // Quote lifecycle state for a single option instrument.
    enum class QuoteState : uint8_t {
        Idle,  // No live or pending quote is tracked.
        Live,  // One quote is acknowledged live in the market.
        ReplacePending,  // A new quote is pending ack and may replace the current live quote.
        CancelPending,  // A zero-volume cancel has been sent and we are waiting for confirmation.
        CancelFailed,  // Cancel retry budget was exhausted; strategy stops touching this instrument.
        Suppressed,  // Strategy intentionally has no quote due to current gating/suppression.
    };

    // Output of build_decision(): what to quote next, or why we decided not to.
    struct QuoteDecision {
        bool valid{false};  // True when bid/ask/size are populated and can be sent.
        bool cancel_only{false};  // True when current quote should be cancelled with no replacement.
        double bid{0.0};  // Target passive bid price.
        double ask{0.0};  // Target passive ask price.
        Volume bid_vol{0};  // Target bid size after scaling/one-sided logic.
        Volume ask_vol{0};  // Target ask size after scaling/one-sided logic.
        uint32_t suppress_flags{SuppressNone};  // Reasons surfaced to the monitor when valid is false.
    };

    // Per-option runtime state owned by the MM thread.
    struct OptionState {
        bool active{false};  // False for array slots not mapped to this product.
        uint16_t instrument_id{INVALID_INSTRUMENT_ID};  // Option instrument managed by this slot.
        uint16_t underlying_id{INVALID_INSTRUMENT_ID};  // Underlying instrument used for shock and hedge context.
        int32_t net_position{0};  // Filled option inventory for this instrument.
        double last_theo_bid{0.0};  // Latest bid theo from pricing.
        double last_theo_ask{0.0};  // Latest ask theo from pricing.
        double last_delta{0.0};  // Latest per-lot delta from pricing.
        double last_vega{0.0};  // Latest per-lot vega from pricing.
        double last_underlying_px{0.0};  // Last underlying reference price seen with the signal.
        int64_t last_signal_ts{0};  // Pricing signal timestamp used for stale-theo detection.
        double live_bid{0.0};  // Bid currently believed live in the market.
        double live_ask{0.0};  // Ask currently believed live in the market.
        Volume live_bid_vol{0};  // Remaining live bid size.
        Volume live_ask_vol{0};  // Remaining live ask size.
        QuoteId live_quote_id{0};  // Latest acknowledged quote id.
        QuoteId pending_quote_id{0};  // Quote id waiting for ack/reject/cancel.
        QuoteId cancel_target_quote_id{0};  // Quote id that the current cancel request is targeting.
        QuoteDecision pending_quote{};  // Proposed quote remembered until ack promotes it to live.
        int64_t last_quote_ts{0};  // Last send timestamp for quote or cancel traffic.
        int64_t live_since_ts{0};  // Ack time of the currently live quote.
        int64_t cancel_last_send_ts{0};  // Timestamp of the most recent cancel attempt.
        uint8_t cancel_attempts{0};  // Retry count for the current cancel sequence.
        bool reevaluate_after_quote_update{false};  // Defer quote rebuild until current replace/cancel resolves.
        uint8_t _pad0[6]{};
        QuoteState quote_state{QuoteState::Idle};  // Current lifecycle state for this instrument.
        uint32_t suppress_flags{SuppressNone};  // Last suppress reasons exported to the monitor.
    };

    // Product-wide regime flags used to gate all option quoting for the product.
    struct ProductRegime {
        bool product_suppressed{false};  // Aggregate quoting gate across session, risk, and MM exposure state.
        bool exposure_breached{false};  // MM delta/vega thresholds are currently exceeded.
        bool underlying_shock_suppressed{false};  // Temporary hold after a fast underlying move.
    };

    static constexpr int64_t STALE_NS = 100'000'000LL;  // Max age for theo/market inputs before quoting is suppressed.
    static constexpr int64_t QUOTE_MAX_LIVE_NS = 3'000'000'000LL;  // Safety timeout before a live quote is refreshed via cancel.
    static constexpr int64_t CANCEL_RETRY_NS = 1'000'000'000LL;  // Minimum spacing between repeated cancel attempts.
    static constexpr uint8_t MAX_CANCEL_ATTEMPTS = 3;  // Stop retrying cancels after this many sends.

    const MarketTick* tick_snapshot_{nullptr};  // Shared market data snapshot for options and the product future.
    const PostTradeRisk* post_risk_{nullptr};  // Portfolio soft-risk gate shared with the monitor.
    MonitoringTopic<SystemAlert, 256>* alert_topic_{nullptr};  // Alert sink for operator-visible failures.
    OptionState option_state_[MAX_INSTRUMENTS]{};  // Per-instrument quote state machine storage.
    uint16_t option_ids_[MAX_INSTRUMENTS]{};  // Dense list of option instrument ids for this product.
    uint16_t option_count_{0};  // Number of active option ids in option_ids_.
    bool session_open_{true};  // Session status from timer events.
    uint16_t underlying_id_{INVALID_INSTRUMENT_ID};  // Product future used for delta hedge orders.
    int32_t underlying_net_position_{0};  // Filled hedge position in the underlying future.
    double product_net_delta_{0.0};  // Product delta from option fills/signals, excluding underlying hedge fills.
    double product_net_vega_{0.0};  // Product vega aggregated across option inventory.
    double last_underlying_mid_{0.0};  // Previous underlying midpoint used to detect shock moves.
    int64_t suppress_until_ns_{0};  // End time of the temporary underlying-shock hold window.
    int64_t last_hedge_ts_ns_{0};  // Last hedge order send timestamp.
    OrderId live_hedge_order_id_{0};  // Outstanding hedge order id, if any.
    Volume live_hedge_remaining_{0};  // Remaining hedge volume tracked for pre-risk and fill handling.
    ProductRegime regime_state_{};  // Last computed product gate state.
    bool supports_quote_replace_{false};  // Whether the gateway can replace quotes without explicit cancel-first.

    // Atomics below are a read-mostly mirror consumed by the monitor thread.
    std::atomic<bool> monitor_session_open_{true};
    std::atomic<bool> monitor_product_suppressed_{false};
    std::atomic<bool> monitor_exposure_breached_{false};
    std::atomic<bool> monitor_underlying_shock_suppressed_{false};
    std::array<std::atomic<uint8_t>, MAX_INSTRUMENTS> monitor_quote_state_{};
    std::array<std::atomic<uint8_t>, MAX_INSTRUMENTS> monitor_cancel_attempts_{};
    std::array<std::atomic<int32_t>, MAX_INSTRUMENTS> monitor_net_position_{};
    std::array<std::atomic<uint32_t>, MAX_INSTRUMENTS> monitor_suppress_flags_{};
    std::array<std::atomic<int64_t>, MAX_INSTRUMENTS> monitor_last_quote_ts_ns_{};

    // Product-wide work orchestration.
    void reevaluate_all(int64_t now_ns) noexcept;
    void cancel_all_live(int64_t now_ns) noexcept;

    // Per-instrument quote lifecycle.
    void maybe_quote(uint16_t instrument_id, int64_t now_ns) noexcept;
    QuoteDecision build_decision(OptionState& state, int64_t now_ns) const noexcept;
    void send_quote(OptionState& state, const QuoteDecision& decision, int64_t now_ns) noexcept;
    void send_cancel(OptionState& state, int64_t now_ns) noexcept;
    [[nodiscard]] bool manage_quote_lifecycle(OptionState& state, int64_t now_ns) noexcept;
    void reset_quote_tracking(OptionState& state, QuoteState next_state) noexcept;
    void clear_live_quote(OptionState& state) noexcept;
    void clear_pending_quote(OptionState& state) noexcept;
    void promote_pending_quote_to_live(OptionState& state, int64_t ack_ts) noexcept;
    void maybe_requote_after_quote_update(OptionState& state, int64_t now_ns) noexcept;
    void publish_cancel_failed_alert(const OptionState& state, int64_t now_ns) noexcept;
    [[nodiscard]] bool quote_fully_filled(const OptionState& state) const noexcept;

    // Product exposure and hedging helpers.
    void maybe_trigger_hedge(int64_t now_ns) noexcept;
    void update_product_exposure(OptionState& state, double old_delta, double old_vega) noexcept;
    [[nodiscard]] ProductRegime capture_product_regime(int64_t now_ns) const noexcept;
    [[nodiscard]] bool handle_product_regime_transition(int64_t now_ns) noexcept;
    [[nodiscard]] bool product_exposure_breached() const noexcept;
    [[nodiscard]] bool product_temporarily_suppressed(int64_t now_ns) const noexcept;

    // Quote churn filter and monitor mirroring.
    [[nodiscard]] bool is_material_change(const OptionState& state,
                                          const QuoteDecision& decision,
                                          double epsilon_px,
                                          int64_t min_interval_ns,
                                          int64_t now_ns) const noexcept;
    void update_monitor_state(const OptionState& state) noexcept;
    void update_monitor_product_state() noexcept;
    void update_all_monitor_states() noexcept;
};

} // namespace omm
