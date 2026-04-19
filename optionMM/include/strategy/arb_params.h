#pragma once

#include "common/config.h"

#include <atomic>

namespace omm {

struct alignas(64) AtomicArbParams {
    std::atomic<double> min_edge_ticks{2.0};
    std::atomic<double> cooldown_ms{25.0};
    std::atomic<double> scan_interval_ms{1.0};
    std::atomic<double> cleanup_timeout_ms{25.0};
    std::atomic<int32_t> max_order_volume{1};
    std::atomic<int32_t> max_live_orders{8};
    std::atomic<bool> cleanup_on_partial{true};
    std::atomic<bool> enabled{false};

    [[nodiscard]] ArbParamsConfig snapshot() const noexcept {
        ArbParamsConfig s{};
        s.min_edge_ticks = min_edge_ticks.load(std::memory_order_relaxed);
        s.cooldown_ms = cooldown_ms.load(std::memory_order_relaxed);
        s.scan_interval_ms = scan_interval_ms.load(std::memory_order_relaxed);
        s.cleanup_timeout_ms = cleanup_timeout_ms.load(std::memory_order_relaxed);
        s.max_order_volume = max_order_volume.load(std::memory_order_relaxed);
        s.max_live_orders = max_live_orders.load(std::memory_order_relaxed);
        s.cleanup_on_partial = cleanup_on_partial.load(std::memory_order_relaxed);
        s.enabled = enabled.load(std::memory_order_relaxed);
        return s;
    }

    void apply(const ArbParamsConfig& c) noexcept {
        min_edge_ticks.store(c.min_edge_ticks, std::memory_order_release);
        cooldown_ms.store(c.cooldown_ms, std::memory_order_release);
        scan_interval_ms.store(c.scan_interval_ms, std::memory_order_release);
        cleanup_timeout_ms.store(c.cleanup_timeout_ms, std::memory_order_release);
        max_order_volume.store(c.max_order_volume, std::memory_order_release);
        max_live_orders.store(c.max_live_orders, std::memory_order_release);
        cleanup_on_partial.store(c.cleanup_on_partial, std::memory_order_release);
        enabled.store(c.enabled, std::memory_order_release);
    }

    AtomicArbParams() = default;
    AtomicArbParams(const AtomicArbParams&) = delete;
    AtomicArbParams& operator=(const AtomicArbParams&) = delete;
};

} // namespace omm
