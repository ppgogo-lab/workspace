#pragma once

#include "common/types.h"
#include "common/config.h"
#include <atomic>

namespace omm {

// ─── AtomicMMParams ───────────────────────────────────────────────────────────
// Runtime-adjustable strategy parameters shared between the gRPC server (writer)
// and the strategy thread (reader).
//
// Thread model:
//   gRPC server thread: store(value, memory_order_release) per field
//   Strategy thread:    load(memory_order_relaxed) per field
//
// Rationale for relaxed loads: eventual consistency is acceptable. A momentarily
// stale spread value causes at most one tick with the wrong spread — not dangerous.
// Each field is independently meaningful, so no need to synchronise across fields.
//
// Layout: alignas(64) ensures the struct occupies its own cache line(s), preventing
// false sharing with adjacent data owned by other threads.

struct alignas(64) AtomicMMParams {
    std::atomic<double>  bid_spread{0.5};
    std::atomic<double>  ask_spread{0.5};
    std::atomic<double>  hedge_delta_threshold{50.0};
    std::atomic<int32_t> quote_volume{10};
    std::atomic<int32_t> max_position{500};
    std::atomic<bool>    enabled{true};

    // Load all fields into a plain snapshot (for logging / gRPC read)
    MMParamsConfig snapshot() const noexcept {
        MMParamsConfig s{};
        s.bid_spread              = bid_spread.load(std::memory_order_relaxed);
        s.ask_spread              = ask_spread.load(std::memory_order_relaxed);
        s.hedge_delta_threshold   = hedge_delta_threshold.load(std::memory_order_relaxed);
        s.quote_volume            = quote_volume.load(std::memory_order_relaxed);
        s.max_position            = max_position.load(std::memory_order_relaxed);
        s.enabled                 = enabled.load(std::memory_order_relaxed);
        return s;
    }

    // Apply a full config update (called from gRPC server thread)
    void apply(const MMParamsConfig& c) noexcept {
        bid_spread.store(c.bid_spread,                     std::memory_order_release);
        ask_spread.store(c.ask_spread,                     std::memory_order_release);
        hedge_delta_threshold.store(c.hedge_delta_threshold, std::memory_order_release);
        quote_volume.store(c.quote_volume,                 std::memory_order_release);
        max_position.store(c.max_position,                 std::memory_order_release);
        enabled.store(c.enabled,                           std::memory_order_release);
    }

    // Non-copyable: atomics cannot be copied
    AtomicMMParams() = default;
    AtomicMMParams(const AtomicMMParams&) = delete;
    AtomicMMParams& operator=(const AtomicMMParams&) = delete;
};

} // namespace omm
