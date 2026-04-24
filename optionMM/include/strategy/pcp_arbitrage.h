#pragma once

#include "strategy/arbitrage_strategy.h"

#include <array>
#include <atomic>
#include <memory>

namespace omm {

// PCP (put-call parity) arbitrage for options on a futures product.
//
// Strategy principle:
//   For European options on a future, put-call parity is:
//
//       C - P = DF * (F - K)
//
//   where:
//     C  = call price
//     P  = put price
//     F  = futures price
//     K  = strike
//     DF = exp(-rT)
//
//   A parity violation means the synthetic future created by call/put is
//   temporarily rich or cheap versus the listed future. This strategy scans
//   both directions:
//     1. long synthetic / short future
//     2. short synthetic / long future
//
// Implementation model:
//   - Runs on a separate lower-priority arb thread.
//   - Reads only shared snapshots; never touches MM quote state.
//   - Emits order intents into the engine's arb queue.
//   - Tracks one active basket attempt at a time.
//   - If the basket fills partially, optionally sends cleanup orders to flatten
//     the residual inventory left behind by the partially executed basket.
class PCPArbitrageStrategy final : public IArbitrageStrategy {
public:
    void init(uint8_t product_idx,
              SPSCRingBuffer<ArbIntent, 256>* intent_buf,
              AtomicArbParams* params,
              const Instrument* instruments,
              const MarketTick* tick_snapshot,
              const Greeks* greeks_snapshot,
              double risk_free_rate,
              const HardRiskConfig& hard_risk_cfg,
              const AccountId& account_id) noexcept;

    void evaluate(Timestamp now_ns) noexcept override;
    void on_market_update(uint16_t instrument_id, Timestamp now_ns) noexcept override;
    void on_timer(Timestamp now_ns) noexcept override;
    void on_order_ack(const Order& order) noexcept override;
    void on_fill(const Trade& trade) noexcept override;
    void on_order_cancel(OrderId id) noexcept override;
    void on_order_reject(const Order& order) noexcept override;

    [[nodiscard]] bool owns_order_id(OrderId id) const noexcept override;
    [[nodiscard]] bool is_enabled() const noexcept override;
    [[nodiscard]] ArbitrageStrategyType strategy_type() const noexcept override {
        return ArbitrageStrategyType::PCP;
    }
    [[nodiscard]] bool read_monitor_state(ArbStrategyMonitorState* out) const noexcept override;
    [[nodiscard]] int read_pcp_monitor_states(PCPPairMonitorState* out,
                                              int max_count) const noexcept override;

private:
    // The two executable PCP directions. "Synthetic" refers to the call/put
    // combination that replicates futures exposure under parity.
    enum class Direction : uint8_t {
        None = 0,
        LongSyntheticShortFuture,
        ShortSyntheticLongFuture,
    };

    // One tradable PCP tuple for a single (expiry, strike) on this product.
    struct Pair {
        bool     active{false};
        uint16_t call_id{INVALID_INSTRUMENT_ID};
        uint16_t put_id{INVALID_INSTRUMENT_ID};
        uint16_t future_id{INVALID_INSTRUMENT_ID};
        int32_t  expiry_date{0};
        double   strike{0.0};
    };

    // Local execution state for each live order submitted by this strategy.
    // "cleanup" distinguishes residual-flattening orders from the initial
    // three-leg parity basket.
    struct WorkingOrder {
        bool      used{false};
        bool      done{false};
        bool      cleanup{false};
        bool      acked{false};
        bool      cancel_requested{false};
        Order     order{};
        Volume    filled_volume{0};
        Timestamp send_ts{0};
    };

    static constexpr int64_t kMarketStaleNs = 100'000'000LL;
    static constexpr int64_t kPairMonitorPublishIntervalNs = 200'000'000LL;
    static constexpr int kMaxPairs = MAX_INSTRUMENTS;
    static constexpr int kMaxWorkingOrders = 24;
    static constexpr uint16_t kInvalidPairIndex = 0xFFFF;

    void build_pairs() noexcept;
    [[nodiscard]] double discount_factor(const Pair& pair, Timestamp now_ns) const noexcept;
    [[nodiscard]] bool market_valid(const MarketTick& tick, Timestamp now_ns) const noexcept;
    [[nodiscard]] Volume executable_volume(const Pair& pair, Direction dir, int max_order_volume) const noexcept;
    [[nodiscard]] bool scan_best_opportunity(Timestamp now_ns,
                                             Pair* best_pair,
                                             uint16_t* best_pair_index,
                                             Direction* best_dir,
                                             Volume* best_volume,
                                             double* best_edge_ticks,
                                             uint32_t* suppress_flags,
                                             uint16_t trigger_instrument_id = INVALID_INSTRUMENT_ID) noexcept;
    [[nodiscard]] uint16_t next_pair_for_instrument(uint16_t pair_index,
                                                    uint16_t instrument_id) const noexcept;
    void evaluate_impl(Timestamp now_ns,
                       uint16_t trigger_instrument_id,
                       bool force_pair_monitor_publish) noexcept;
    void publish_pair_monitor_states(Timestamp now_ns,
                                     uint16_t selected_pair_index,
                                     Direction selected_dir,
                                     double selected_edge_ticks) noexcept;
    [[nodiscard]] bool enqueue_order(uint16_t instrument_id,
                                     Side side,
                                     double price,
                                     Volume volume,
                                     bool cleanup,
                                     double edge_ticks,
                                     const Pair& pair,
                                     Timestamp now_ns) noexcept;
    void start_attempt(const Pair& pair,
                       Direction dir,
                       Volume volume,
                       double edge_ticks,
                       Timestamp now_ns) noexcept;
    void cancel_stale_orders(Timestamp now_ns, int64_t timeout_ns) noexcept;
    void maybe_finalize_attempt(Timestamp now_ns) noexcept;
    void submit_cleanup_orders(Timestamp now_ns) noexcept;
    void clear_attempt_state() noexcept;
    void set_active_pair(const Pair& pair) noexcept;
    void refresh_monitor_state(uint32_t suppress_flags, Timestamp now_ns) noexcept;
    [[nodiscard]] int live_order_count() const noexcept;
    [[nodiscard]] WorkingOrder* find_working_order(OrderId id) noexcept;
    [[nodiscard]] const WorkingOrder* find_working_order(OrderId id) const noexcept;

    std::unique_ptr<PreTradeRisk> pre_risk_;
    std::array<Pair, kMaxPairs> pairs_{};
    uint16_t pair_count_{0};
    std::array<WorkingOrder, kMaxWorkingOrders> working_orders_{};
    uint64_t local_order_seq_{0};
    double risk_free_rate_{0.025};
    Timestamp last_scan_ts_ns_{0};
    Timestamp next_trigger_ts_ns_{0};
    Timestamp last_pair_monitor_publish_ts_ns_{0};
    bool attempt_active_{false};
    bool cleanup_active_{false};
    uint16_t active_call_id_{INVALID_INSTRUMENT_ID};
    uint16_t active_put_id_{INVALID_INSTRUMENT_ID};
    uint16_t active_future_id_{INVALID_INSTRUMENT_ID};
    uint32_t last_suppress_flags_{ArbSuppressNone};

    std::atomic<bool> monitor_running_{false};
    std::atomic<bool> monitor_cleanup_active_{false};
    std::atomic<uint8_t> monitor_live_orders_{0};
    std::atomic<uint32_t> monitor_suppress_flags_{ArbSuppressNone};
    std::atomic<uint16_t> monitor_active_call_id_{INVALID_INSTRUMENT_ID};
    std::atomic<uint16_t> monitor_active_put_id_{INVALID_INSTRUMENT_ID};
    std::atomic<uint16_t> monitor_active_future_id_{INVALID_INSTRUMENT_ID};
    std::atomic<double> monitor_last_edge_ticks_{0.0};
    std::atomic<double> monitor_last_trigger_edge_ticks_{0.0};
    std::atomic<Timestamp> monitor_last_eval_ts_ns_{0};
    std::atomic<Timestamp> monitor_last_trigger_ts_ns_{0};
    std::atomic<uint64_t> monitor_pair_snapshot_version_{0};
    std::array<PCPPairMonitorState, kMaxPairs> monitor_pairs_{};
    uint16_t monitor_pair_count_{0};
    std::array<uint16_t, MAX_INSTRUMENTS> first_pair_for_instrument_{};
    std::array<std::array<uint16_t, 3>, kMaxPairs> next_pair_for_instrument_{};
};

} // namespace omm
