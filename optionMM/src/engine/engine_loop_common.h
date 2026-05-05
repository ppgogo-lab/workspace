#pragma once

#include "common/thread_utils.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <ctime>
#include <thread>

namespace omm {

constexpr int kStrategyGatewayBurstCap = 32;
constexpr int kStrategyTimerBurstCap = 8;
constexpr int kStrategySignalBurstCap = 128;
constexpr int kDispatcherCallbackLeadBurstCap = 16;
constexpr int kDispatcherCallbackInterleaveBurstCap = 8;
constexpr int kDispatcherOrderBurstCap = 64;
constexpr int kDispatcherQuoteBurstCap = 128;
constexpr int kDispatcherArbIntentBurstCap = 16;
constexpr int kCoalescedTimerSlotCount = 2;
constexpr int kArbEventBurstCap = 32;
constexpr int kArbMarketTriggerBurstCap = 128;
constexpr int64_t kArbMaintenanceIntervalNs = 5'000'000LL;
constexpr int64_t kTimerIdleSleepCapNs = 5'000'000LL;
constexpr int64_t kRiskIdleSleepCapNs = 5'000'000LL;
constexpr int64_t kRiskCheckIntervalNs = 5'000'000LL;

inline void pin_if_configured(int core_id) noexcept {
    if (core_id < 0) return;

    const int hw_threads = static_cast<int>(std::thread::hardware_concurrency());
    if (hw_threads > 0 && core_id >= hw_threads) return;

    pin_thread_to_core(core_id);
}

inline void apply_realtime_if_configured(bool enabled,
                                         int priority,
                                         const char* thread_name) noexcept {
    (void)thread_name;
    if (!enabled || priority <= 0) return;
    (void)try_set_realtime_priority(priority);
}

inline void sleep_for_ns_interruptible(const std::atomic<bool>& stop_flag,
                                       int64_t sleep_ns,
                                       int64_t chunk_cap_ns) noexcept {
    while (sleep_ns > 0 && !stop_flag.load(std::memory_order_relaxed)) {
        const int64_t chunk = std::min(sleep_ns, chunk_cap_ns);
        struct timespec ts{
            static_cast<time_t>(chunk / 1'000'000'000LL),
            static_cast<long>(chunk % 1'000'000'000LL),
        };
        nanosleep(&ts, nullptr);
        sleep_ns -= chunk;
    }
}

inline void update_max(uint32_t& metric, uint32_t candidate) noexcept {
    if (candidate > metric) {
        metric = candidate;
    }
}

} // namespace omm
