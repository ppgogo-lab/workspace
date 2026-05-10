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
    /**
     * @brief Init.
     * @param product_idx Parameter supplied by the caller.
     * @param intent_buf Parameter supplied by the caller.
     * @param params Parameter supplied by the caller.
     * @param instruments Parameter supplied by the caller.
     * @param tick_snapshot Parameter supplied by the caller.
     * @param greeks_snapshot Parameter supplied by the caller.
     * @param risk_free_rate Parameter supplied by the caller.
     * @param hard_risk_cfg Parameter supplied by the caller.
     * @param account_id Parameter supplied by the caller.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    void init(uint8_t product_idx,
              SPSCRingBuffer<ArbIntent, 256>* intent_buf,
              AtomicArbParams* params,
              const Instrument* instruments,
              const SnapshotArray<TopOfBookTick, MAX_INSTRUMENTS>* tick_snapshot,
              const SnapshotArray<Greeks, MAX_INSTRUMENTS>* greeks_snapshot,
              double risk_free_rate,
              const HardRiskConfig& hard_risk_cfg,
              const AccountId& account_id) noexcept;

    /**
     * @brief Evaluate.
     * @param now_ns Parameter supplied by the caller.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    void evaluate(Timestamp now_ns) noexcept override;
    /**
     * @brief On market update.
     * @param instrument_id Parameter supplied by the caller.
     * @param now_ns Parameter supplied by the caller.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    void on_market_update(uint16_t instrument_id, Timestamp now_ns) noexcept override;
    /**
     * @brief On timer.
     * @param now_ns Parameter supplied by the caller.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    void on_timer(Timestamp now_ns) noexcept override;
    /**
     * @brief On order ack.
     * @param order Parameter supplied by the caller.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    void on_order_ack(const Order& order) noexcept override;
    /**
     * @brief On fill.
     * @param trade Parameter supplied by the caller.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    void on_fill(const Trade& trade) noexcept override;
    /**
     * @brief On order cancel.
     * @param id Parameter supplied by the caller.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    void on_order_cancel(OrderId id) noexcept override;
    /**
     * @brief On order reject.
     * @param order Parameter supplied by the caller.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    void on_order_reject(const Order& order) noexcept override;

    /**
     * @brief Owns order id.
     * @param id Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] bool owns_order_id(OrderId id) const noexcept override;
    /**
     * @brief Is enabled.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] bool is_enabled() const noexcept override;
    /**
     * @brief Strategy type.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] ArbitrageStrategyType strategy_type() const noexcept override {
        return ArbitrageStrategyType::PCP;
    }
    /**
     * @brief Read monitor state.
     * @param out Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] bool read_monitor_state(ArbStrategyMonitorState* out) const noexcept override;
    /**
     * @brief Read pcp monitor states.
     * @param out Parameter supplied by the caller.
     * @param max_count Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
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

    /**
     * @brief Build pairs.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    void build_pairs() noexcept;
    /**
     * @brief Discount factor.
     * @param pair Parameter supplied by the caller.
     * @param now_ns Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] double discount_factor(const Pair& pair, Timestamp now_ns) const noexcept;
    /**
     * @brief Market valid.
     * @param tick Parameter supplied by the caller.
     * @param now_ns Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] bool market_valid(const TopOfBookTick& tick, Timestamp now_ns) const noexcept;
    /**
     * @brief Executable volume.
     * @param pair Parameter supplied by the caller.
     * @param dir Parameter supplied by the caller.
     * @param max_order_volume Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] Volume executable_volume(const Pair& pair, Direction dir, int max_order_volume) const noexcept;
    /**
     * @brief Scan best opportunity.
     * @param now_ns Parameter supplied by the caller.
     * @param best_pair Parameter supplied by the caller.
     * @param best_pair_index Parameter supplied by the caller.
     * @param best_dir Parameter supplied by the caller.
     * @param best_volume Parameter supplied by the caller.
     * @param best_edge_ticks Parameter supplied by the caller.
     * @param suppress_flags Parameter supplied by the caller.
     * @param trigger_instrument_id Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] bool scan_best_opportunity(Timestamp now_ns,
                                             Pair* best_pair,
                                             uint16_t* best_pair_index,
                                             Direction* best_dir,
                                             Volume* best_volume,
                                             double* best_edge_ticks,
                                             uint32_t* suppress_flags,
                                             uint16_t trigger_instrument_id = INVALID_INSTRUMENT_ID) noexcept;
    /**
     * @brief Next pair for instrument.
     * @param pair_index Parameter supplied by the caller.
     * @param instrument_id Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] uint16_t next_pair_for_instrument(uint16_t pair_index,
                                                    uint16_t instrument_id) const noexcept;
    /**
     * @brief Evaluate impl.
     * @param now_ns Parameter supplied by the caller.
     * @param trigger_instrument_id Parameter supplied by the caller.
     * @param force_pair_monitor_publish Parameter supplied by the caller.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    void evaluate_impl(Timestamp now_ns,
                       uint16_t trigger_instrument_id,
                       bool force_pair_monitor_publish) noexcept;
    /**
     * @brief Publish pair monitor states.
     * @param now_ns Parameter supplied by the caller.
     * @param selected_pair_index Parameter supplied by the caller.
     * @param selected_dir Parameter supplied by the caller.
     * @param selected_edge_ticks Parameter supplied by the caller.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    void publish_pair_monitor_states(Timestamp now_ns,
                                     uint16_t selected_pair_index,
                                     Direction selected_dir,
                                     double selected_edge_ticks) noexcept;
    /**
     * @brief Enqueue order.
     * @param instrument_id Parameter supplied by the caller.
     * @param side Parameter supplied by the caller.
     * @param price Parameter supplied by the caller.
     * @param volume Parameter supplied by the caller.
     * @param cleanup Parameter supplied by the caller.
     * @param edge_ticks Parameter supplied by the caller.
     * @param pair Parameter supplied by the caller.
     * @param now_ns Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] bool enqueue_order(uint16_t instrument_id,
                                     Side side,
                                     double price,
                                     Volume volume,
                                     bool cleanup,
                                     double edge_ticks,
                                     const Pair& pair,
                                     Timestamp now_ns) noexcept;
    /**
     * @brief Start attempt.
     * @param pair Parameter supplied by the caller.
     * @param dir Parameter supplied by the caller.
     * @param volume Parameter supplied by the caller.
     * @param edge_ticks Parameter supplied by the caller.
     * @param now_ns Parameter supplied by the caller.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    void start_attempt(const Pair& pair,
                       Direction dir,
                       Volume volume,
                       double edge_ticks,
                       Timestamp now_ns) noexcept;
    /**
     * @brief Cancel stale orders.
     * @param now_ns Parameter supplied by the caller.
     * @param timeout_ns Parameter supplied by the caller.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    void cancel_stale_orders(Timestamp now_ns, int64_t timeout_ns) noexcept;
    /**
     * @brief Maybe finalize attempt.
     * @param now_ns Parameter supplied by the caller.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    void maybe_finalize_attempt(Timestamp now_ns) noexcept;
    /**
     * @brief Submit cleanup orders.
     * @param now_ns Parameter supplied by the caller.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    void submit_cleanup_orders(Timestamp now_ns) noexcept;
    /**
     * @brief Clear attempt state.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    void clear_attempt_state() noexcept;
    /**
     * @brief Set active pair.
     * @param pair Parameter supplied by the caller.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    void set_active_pair(const Pair& pair) noexcept;
    /**
     * @brief Refresh monitor state.
     * @param suppress_flags Parameter supplied by the caller.
     * @param now_ns Parameter supplied by the caller.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    void refresh_monitor_state(uint32_t suppress_flags, Timestamp now_ns) noexcept;
    /**
     * @brief Live order count.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] int live_order_count() const noexcept;
    /**
     * @brief Find working order.
     * @param id Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] WorkingOrder* find_working_order(OrderId id) noexcept;
    /**
     * @brief Find working order.
     * @param id Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
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
