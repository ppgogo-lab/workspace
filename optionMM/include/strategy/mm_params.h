#pragma once

#include "common/config.h"
#include "common/types.h"

#include <atomic>

namespace omm {

// Atomic mirror of MMParamsConfig used for live tuning from the monitor.
// Runtime-adjustable strategy parameters shared between the gRPC server (writer)
// and the strategy thread (reader).
//
// Thread model:
//   gRPC server thread: store(value, memory_order_release) per field
//   Strategy thread:    load(memory_order_relaxed) per field
//
// Rationale for relaxed loads: eventual consistency is acceptable. A momentarily
// stale spread value causes at most one tick with the wrong spread, which is
// acceptable. Each field is independently meaningful, so no need to synchronize
// across fields.
//
// Layout: alignas(64) keeps the struct on its own cache line(s) and reduces
// false sharing with adjacent data owned by other threads.
struct alignas(64) AtomicMMParams {
    std::atomic<double> bid_spread{0.5};  // Bid offset, in ticks, from the chosen quote center.
    std::atomic<double> ask_spread{0.5};  // Ask offset, in ticks, from the chosen quote center.
    std::atomic<double> product_delta_threshold{50.0};  // Product delta threshold for hedge orders and MM exposure gating.
    std::atomic<double> product_vega_threshold{1000.0};  // Product vega threshold for MM exposure gating.
    std::atomic<double> min_quote_interval_ms{100.0};  // Minimum time between material quote updates or hedge attempts.
    std::atomic<int32_t> quote_volume{10};  // Default per-side quote size before inventory/product scaling.
    std::atomic<int32_t> max_position{500};  // Hard per-instrument inventory limit for quoting.
    std::atomic<int32_t> warning_position{250};  // Inventory level where size starts scaling down or one-sided quoting can begin.
    std::atomic<double> base_half_spread_ticks{1.0};  // Baseline half-spread before theo/market/risk widening.
    std::atomic<double> min_half_spread_ticks{1.0};  // Lower bound on half-spread even in calm markets.
    std::atomic<double> max_half_spread_ticks{8.0};  // Upper bound on half-spread after all widening logic.
    std::atomic<double> inventory_skew_per_lot_ticks{0.01};  // Center shift per lot of inventory to lean out of risk.
    std::atomic<double> follow_weight{0.35};  // Blend between theo midpoint and market microprice when picking the quote center.
    std::atomic<double> requote_price_epsilon_ticks{1.0};  // Minimum price move, in ticks, required before replacing a live quote.
    std::atomic<double> market_width_widen_threshold_ticks{6.0};  // Market width beyond this level adds extra spread widening.
    std::atomic<double> underlying_move_widen_threshold_ticks{2.0};  // Underlying move threshold that triggers a temporary product shock hold.
    std::atomic<bool> use_one_sided_at_limits{true};  // Allow quoting only the risk-reducing side near warning/max position.
    std::atomic<bool> enabled{false};  // Default stopped so startup never quotes before trader approval.

    // Load the current atomic view into a plain struct for logging, UI, and RPC snapshots.
    MMParamsConfig snapshot() const noexcept {
        MMParamsConfig s{};
        s.bid_spread = bid_spread.load(std::memory_order_relaxed);
        s.ask_spread = ask_spread.load(std::memory_order_relaxed);
        s.product_delta_threshold = product_delta_threshold.load(std::memory_order_relaxed);
        s.product_vega_threshold = product_vega_threshold.load(std::memory_order_relaxed);
        s.min_quote_interval_ms = min_quote_interval_ms.load(std::memory_order_relaxed);
        s.quote_volume = quote_volume.load(std::memory_order_relaxed);
        s.max_position = max_position.load(std::memory_order_relaxed);
        s.warning_position = warning_position.load(std::memory_order_relaxed);
        s.base_half_spread_ticks = base_half_spread_ticks.load(std::memory_order_relaxed);
        s.min_half_spread_ticks = min_half_spread_ticks.load(std::memory_order_relaxed);
        s.max_half_spread_ticks = max_half_spread_ticks.load(std::memory_order_relaxed);
        s.inventory_skew_per_lot_ticks = inventory_skew_per_lot_ticks.load(std::memory_order_relaxed);
        s.follow_weight = follow_weight.load(std::memory_order_relaxed);
        s.requote_price_epsilon_ticks = requote_price_epsilon_ticks.load(std::memory_order_relaxed);
        s.market_width_widen_threshold_ticks =
            market_width_widen_threshold_ticks.load(std::memory_order_relaxed);
        s.underlying_move_widen_threshold_ticks =
            underlying_move_widen_threshold_ticks.load(std::memory_order_relaxed);
        s.use_one_sided_at_limits = use_one_sided_at_limits.load(std::memory_order_relaxed);
        s.enabled = enabled.load(std::memory_order_relaxed);
        return s;
    }

    // Apply a full parameter update from the control-plane thread.
    void apply(const MMParamsConfig& c) noexcept {
        bid_spread.store(c.bid_spread, std::memory_order_release);
        ask_spread.store(c.ask_spread, std::memory_order_release);
        product_delta_threshold.store(c.product_delta_threshold, std::memory_order_release);
        product_vega_threshold.store(c.product_vega_threshold, std::memory_order_release);
        min_quote_interval_ms.store(c.min_quote_interval_ms, std::memory_order_release);
        quote_volume.store(c.quote_volume, std::memory_order_release);
        max_position.store(c.max_position, std::memory_order_release);
        warning_position.store(c.warning_position, std::memory_order_release);
        base_half_spread_ticks.store(c.base_half_spread_ticks, std::memory_order_release);
        min_half_spread_ticks.store(c.min_half_spread_ticks, std::memory_order_release);
        max_half_spread_ticks.store(c.max_half_spread_ticks, std::memory_order_release);
        inventory_skew_per_lot_ticks.store(c.inventory_skew_per_lot_ticks, std::memory_order_release);
        follow_weight.store(c.follow_weight, std::memory_order_release);
        requote_price_epsilon_ticks.store(c.requote_price_epsilon_ticks, std::memory_order_release);
        market_width_widen_threshold_ticks.store(c.market_width_widen_threshold_ticks,
                                                 std::memory_order_release);
        underlying_move_widen_threshold_ticks.store(c.underlying_move_widen_threshold_ticks,
                                                    std::memory_order_release);
        use_one_sided_at_limits.store(c.use_one_sided_at_limits, std::memory_order_release);
        enabled.store(c.enabled, std::memory_order_release);
    }

    // Non-copyable: atomics cannot be copied safely as a plain aggregate.
    AtomicMMParams() = default;
    AtomicMMParams(const AtomicMMParams&) = delete;
    AtomicMMParams& operator=(const AtomicMMParams&) = delete;
};

} // namespace omm
