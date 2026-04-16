#pragma once

#include "strategy/mm_framework.h"
#include "common/thread_utils.h"
#include "monitoring/topic.h"
#include "risk/post_trade_risk.h"

namespace omm {

class OptionMMCoreStrategy : public IMarketMaker {
public:
    void init(uint8_t product_idx,
              SPSCRingBuffer<Quote, 512>* quote_buf,
              SPSCRingBuffer<Order, 512>* order_buf,
              PreTradeRisk* pre_risk,
              AtomicMMParams* params,
              const Instrument* instruments,
              const MarketTick* tick_snapshot,
              const PostTradeRisk* post_risk,
              MonitoringTopic<SystemAlert, 256>* alert_topic) noexcept;

    void on_signal(const PricingSignal& signal) noexcept override;
    void on_fill(const Trade& trade) noexcept override;
    void on_order_ack(const Order& order) noexcept override;
    void on_quote_ack(const Quote& quote) noexcept override;
    void on_quote_cancel(const Quote& quote) noexcept override;
    void on_quote_reject(const Quote& quote) noexcept override;
    void on_order_cancel(OrderId id) noexcept override;
    void on_order_reject(const Order& order) noexcept override;
    void on_timer(const TimerEvent& event) noexcept override;

    [[nodiscard]] bool is_enabled() const noexcept override;
    [[nodiscard]] uint8_t product_index() const noexcept override { return product_idx_; }

private:
    enum SuppressFlags : uint32_t {
        SuppressNone = 0,
        SuppressStaleTheo = 1u << 0,
        SuppressInvalidMarket = 1u << 1,
        SuppressPosition = 1u << 2,
        SuppressRisk = 1u << 3,
        SuppressSession = 1u << 4,
        SuppressThrottle = 1u << 5,
        SuppressUnderlyingShock = 1u << 6,
        SuppressProductExposure = 1u << 7,
        SuppressCancelStuck = 1u << 8,
    };

    enum class QuoteState : uint8_t {
        Idle,
        Live,
        ReplacePending,
        CancelPending,
        CancelFailed,
        Suppressed,
    };

    struct OptionState {
        bool active{false};
        uint16_t instrument_id{INVALID_INSTRUMENT_ID};
        uint16_t underlying_id{INVALID_INSTRUMENT_ID};
        int32_t net_position{0};
        double last_theo_bid{0.0};
        double last_theo_ask{0.0};
        double last_delta{0.0};
        double last_vega{0.0};
        double last_underlying_px{0.0};
        int64_t last_signal_ts{0};
        double live_bid{0.0};
        double live_ask{0.0};
        Volume live_bid_vol{0};
        Volume live_ask_vol{0};
        QuoteId live_quote_id{0};
        QuoteId pending_quote_id{0};
        QuoteId cancel_target_quote_id{0};
        int64_t last_quote_ts{0};
        int64_t live_since_ts{0};
        int64_t cancel_last_send_ts{0};
        uint8_t cancel_attempts{0};
        uint8_t _pad0[7]{};
        QuoteState quote_state{QuoteState::Idle};
        uint32_t suppress_flags{SuppressNone};
    };

    struct QuoteDecision {
        bool valid{false};
        bool cancel_only{false};
        double bid{0.0};
        double ask{0.0};
        Volume bid_vol{0};
        Volume ask_vol{0};
        uint32_t suppress_flags{SuppressNone};
    };

    struct ProductRegime {
        bool product_suppressed{false};
        bool exposure_breached{false};
        bool underlying_shock_suppressed{false};
    };

    static constexpr int64_t STALE_NS = 100'000'000LL;
    static constexpr int64_t QUOTE_MAX_LIVE_NS = 3'000'000'000LL;
    static constexpr int64_t CANCEL_RETRY_NS = 1'000'000'000LL;
    static constexpr uint8_t MAX_CANCEL_ATTEMPTS = 3;

    const MarketTick* tick_snapshot_{nullptr};
    const PostTradeRisk* post_risk_{nullptr};
    MonitoringTopic<SystemAlert, 256>* alert_topic_{nullptr};
    OptionState option_state_[MAX_INSTRUMENTS]{};
    uint16_t option_ids_[MAX_INSTRUMENTS]{};
    uint16_t option_count_{0};
    bool session_open_{true};
    uint16_t underlying_id_{INVALID_INSTRUMENT_ID};
    int32_t underlying_net_position_{0};
    double product_net_delta_{0.0};
    double product_net_vega_{0.0};
    double last_underlying_mid_{0.0};
    int64_t suppress_until_ns_{0};
    int64_t last_hedge_ts_ns_{0};
    OrderId live_hedge_order_id_{0};
    Volume live_hedge_remaining_{0};
    ProductRegime regime_state_{};

    void reevaluate_all(int64_t now_ns) noexcept;
    void cancel_all_live(int64_t now_ns) noexcept;
    void maybe_quote(uint16_t instrument_id, int64_t now_ns) noexcept;
    QuoteDecision build_decision(OptionState& state, int64_t now_ns) const noexcept;
    void send_quote(OptionState& state, const QuoteDecision& decision, int64_t now_ns) noexcept;
    void send_cancel(OptionState& state, int64_t now_ns) noexcept;
    [[nodiscard]] bool manage_quote_lifecycle(OptionState& state, int64_t now_ns) noexcept;
    void reset_quote_tracking(OptionState& state, QuoteState next_state) noexcept;
    void publish_cancel_failed_alert(const OptionState& state, int64_t now_ns) noexcept;
    [[nodiscard]] bool quote_fully_filled(const OptionState& state) const noexcept;
    void maybe_trigger_hedge(int64_t now_ns) noexcept;
    void update_product_exposure(OptionState& state,
                                 double old_delta,
                                 double old_vega) noexcept;
    [[nodiscard]] ProductRegime capture_product_regime(int64_t now_ns) const noexcept;
    [[nodiscard]] bool handle_product_regime_transition(int64_t now_ns) noexcept;
    [[nodiscard]] bool product_exposure_breached() const noexcept;
    [[nodiscard]] bool product_temporarily_suppressed(int64_t now_ns) const noexcept;
    [[nodiscard]] bool is_material_change(const OptionState& state,
                                          const QuoteDecision& decision,
                                          double epsilon_px,
                                          int64_t min_interval_ns,
                                          int64_t now_ns) const noexcept;
};

} // namespace omm
