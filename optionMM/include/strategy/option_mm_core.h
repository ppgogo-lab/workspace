#pragma once

#include "common/thread_utils.h"
#include "common/latest_snapshot.h"
#include "monitoring/topic.h"
#include "risk/post_trade_risk.h"
#include "strategy/base_quoting_strategy.h"
#include "strategy/quote_lifecycle.h"

#include <array>
#include <atomic>

namespace omm {

// Core option market-making strategy.
// Responsibilities:
// - maintain one quote state machine per option instrument (via BaseQuotingStrategy)
// - track product-level delta/vega exposure and trigger hedge orders
// - gate quoting on session, risk, exposure, and temporary shock conditions
// - publish a monitor-friendly mirror of internal state
class OptionMMCoreStrategy : public BaseQuotingStrategy {
public:
    // Wire the strategy to the product, IO buffers, shared params, and monitor sinks.
    void init(uint8_t product_idx,
              SPSCRingBuffer<Quote, 512>* quote_buf,
              SPSCRingBuffer<Order, 512>* order_buf,
              PreTradeRisk* pre_risk,
              AtomicMMParams* params,
              const Instrument* instruments,
              const SnapshotArray<TopOfBookTick, MAX_INSTRUMENTS>* tick_snapshot,
              const PostTradeRisk* post_risk,
              MonitoringTopic<SystemAlert, 256>* alert_topic) noexcept;

    // Monitor-facing snapshot accessors.
    [[nodiscard]] bool is_enabled() const noexcept override;
    [[nodiscard]] uint8_t product_index() const noexcept override { return product_idx_; }
    [[nodiscard]] bool read_product_monitor_state(ProductMonitorState* out) const noexcept override;
    [[nodiscard]] int read_instrument_monitor_states(InstrumentMonitorState* out,
                                                     int max_count) const noexcept override;
    [[nodiscard]] bool read_runtime_stats(StrategyRuntimeStats* out) const noexcept override;

protected:
    // BaseQuotingStrategy hooks
    void on_signal_impl(const PricingSignal& signal) noexcept override;
    void on_fill_impl(const Trade& trade) noexcept override;
    void on_timer_impl(const TimerEvent& event) noexcept override;

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
        // REMOVED: QuoteLifecycleState quote_lifecycle{};  // Now managed by BaseQuotingStrategy
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

    const SnapshotArray<TopOfBookTick, MAX_INSTRUMENTS>* tick_snapshot_{nullptr};
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
    std::atomic<uint64_t> runtime_full_book_reevaluations_{0};
    std::atomic<uint64_t> runtime_single_instrument_reevaluations_{0};

    // Product-wide work orchestration.
    void reevaluate_all(int64_t now_ns) noexcept;
    void reevaluate_one(uint16_t instrument_id, int64_t now_ns) noexcept;
    void cancel_all_live(int64_t now_ns) noexcept;

    // Per-instrument quote lifecycle.
    void maybe_quote(uint16_t instrument_id, int64_t now_ns) noexcept;
    QuoteDecision build_decision(OptionState& state, int64_t now_ns) const noexcept;
    // REMOVED: send_quote, send_cancel, manage_quote_lifecycle - now in BaseQuotingStrategy
    void publish_cancel_failed_alert(const OptionState& state, int64_t now_ns) noexcept;

    // Product exposure and hedging helpers.
    void maybe_trigger_hedge(int64_t now_ns) noexcept;
    void update_product_exposure(OptionState& state, double old_delta, double old_vega) noexcept;
    [[nodiscard]] ProductRegime capture_product_regime(int64_t now_ns) const noexcept;
    [[nodiscard]] bool handle_product_regime_transition(int64_t now_ns) noexcept;
    [[nodiscard]] bool product_exposure_breached() const noexcept;
    [[nodiscard]] bool product_temporarily_suppressed(int64_t now_ns) const noexcept;
    void update_monitor_state(const OptionState& state) noexcept;
    void update_monitor_product_state() noexcept;
    void update_all_monitor_states() noexcept;
};

} // namespace omm
