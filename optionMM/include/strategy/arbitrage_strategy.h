#pragma once

#include "common/ring_buffer.h"
#include "common/types.h"
#include "risk/pre_trade_risk.h"
#include "strategy/arb_params.h"

namespace omm {

enum ArbStrategySuppressFlags : uint32_t {
    ArbSuppressNone = 0,
    ArbSuppressDisabled = 1u << 0,
    ArbSuppressNoPairs = 1u << 1,
    ArbSuppressInvalidMarket = 1u << 2,
    ArbSuppressCooldown = 1u << 3,
    ArbSuppressIntentBackpressure = 1u << 4,
    ArbSuppressLiveOrders = 1u << 5,
    ArbSuppressCleanupPending = 1u << 6,
};

struct ArbStrategyMonitorState {
    uint8_t               product_index{0xFF};
    ArbitrageStrategyType strategy_type{ArbitrageStrategyType::None};
    bool                  enabled{false};
    bool                  running{false};
    bool                  cleanup_active{false};
    uint8_t               live_orders{0};
    uint16_t              pair_count{0};
    uint16_t              active_call_id{INVALID_INSTRUMENT_ID};
    uint16_t              active_put_id{INVALID_INSTRUMENT_ID};
    uint16_t              active_future_id{INVALID_INSTRUMENT_ID};
    uint32_t              suppress_flags{ArbSuppressNone};
    double                last_edge_ticks{0.0};
    double                last_trigger_edge_ticks{0.0};
    Timestamp             last_eval_ts_ns{0};
    Timestamp             last_trigger_ts_ns{0};
};

enum class PCPMonitorDirection : uint8_t {
    None = 0,
    LongSyntheticShortFuture,
    ShortSyntheticLongFuture,
};

struct PCPPairMonitorState {
    uint8_t               product_index{0xFF};
    ArbitrageStrategyType strategy_type{ArbitrageStrategyType::None};
    uint16_t              call_id{INVALID_INSTRUMENT_ID};
    uint16_t              put_id{INVALID_INSTRUMENT_ID};
    uint16_t              future_id{INVALID_INSTRUMENT_ID};
    int32_t               expiry_date{0};
    double                strike{0.0};
    bool                  market_valid{false};
    bool                  selected{false};
    double                discount_factor{0.0};
    double                synthetic_bid{0.0};
    double                synthetic_ask{0.0};
    double                future_bid{0.0};
    double                future_ask{0.0};
    double                long_synth_edge_ticks{0.0};
    double                short_synth_edge_ticks{0.0};
    double                best_edge_ticks{0.0};
    PCPMonitorDirection   best_direction{PCPMonitorDirection::None};
    Volume                best_volume{0};
    Timestamp             eval_ts_ns{0};
};

constexpr uint8_t kArbOrderTag = 0xAEu;

[[nodiscard]] inline OrderId make_arb_order_id(uint8_t product_idx,
                                               ArbitrageStrategyType type,
                                               uint64_t seq) noexcept {
    return (static_cast<uint64_t>(kArbOrderTag) << 56)
        | (static_cast<uint64_t>(product_idx) << 48)
        | (static_cast<uint64_t>(type) << 40)
        | (seq & ((1ULL << 40) - 1ULL));
}

[[nodiscard]] inline bool is_arb_order_id(OrderId id) noexcept {
    return static_cast<uint8_t>(id >> 56) == kArbOrderTag;
}

[[nodiscard]] inline uint8_t arb_order_product(OrderId id) noexcept {
    return static_cast<uint8_t>((id >> 48) & 0xFFu);
}

[[nodiscard]] inline ArbitrageStrategyType arb_order_type(OrderId id) noexcept {
    return static_cast<ArbitrageStrategyType>((id >> 40) & 0xFFu);
}

class IArbitrageStrategy {
public:
    virtual ~IArbitrageStrategy() = default;

    virtual void evaluate(Timestamp now_ns) noexcept = 0;
    virtual void on_order_ack(const Order& order) noexcept = 0;
    virtual void on_fill(const Trade& trade) noexcept = 0;
    virtual void on_order_cancel(OrderId id) noexcept = 0;
    virtual void on_order_reject(const Order& order) noexcept = 0;

    [[nodiscard]] virtual bool owns_order_id(OrderId id) const noexcept = 0;
    [[nodiscard]] virtual bool is_enabled() const noexcept = 0;
    [[nodiscard]] virtual ArbitrageStrategyType strategy_type() const noexcept = 0;
    [[nodiscard]] virtual bool read_monitor_state(ArbStrategyMonitorState* out) const noexcept = 0;
    [[nodiscard]] virtual int read_pcp_monitor_states(PCPPairMonitorState* out,
                                                      int max_count) const noexcept {
        (void)out;
        (void)max_count;
        return 0;
    }

protected:
    uint8_t                        product_idx_{0};
    SPSCRingBuffer<ArbIntent, 256>* intent_buf_{nullptr};
    AtomicArbParams*              params_{nullptr};
    const Instrument*             instruments_{nullptr};
    const MarketTick*             tick_snapshot_{nullptr};
    const Greeks*                 greeks_snapshot_{nullptr};
    AccountId                     account_id_{};
};

} // namespace omm
