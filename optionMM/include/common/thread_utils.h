#pragma once

#include <cstdint>
#include <string_view>
#include <time.h>    // struct timespec, clock_gettime, CLOCK_MONOTONIC_RAW

namespace omm {

// ─── Thread utilities ─────────────────────────────────────────────────────────
// All functions are Linux-only. Failures are fatal: the trading engine cannot
// run correctly without guaranteed core isolation, so we abort rather than
// silently degrade to time-shared scheduling.

// Pin the calling thread to a single logical CPU core.
// Uses pthread_setaffinity_np. Validates core_id against hardware_concurrency().
// Aborts the process on failure — silent degradation is not acceptable.
void pin_thread_to_core(int core_id) noexcept;

// Set the name of the calling thread (visible in top, htop, perf, /proc).
// Truncates to 15 characters (Linux limit for pthread_setname_np).
void set_thread_name(std::string_view name) noexcept;

// Set the calling thread to SCHED_FIFO real-time scheduling at the given
// priority (1–99). Requires CAP_SYS_NICE or root. Aborts on failure.
void set_realtime_priority(int priority) noexcept;
[[nodiscard]] bool try_set_realtime_priority(int priority) noexcept;

// Return the NUMA node for a given logical CPU core.
// Reads /sys/devices/system/cpu/cpuN/node*/cpumap.
// Returns -1 if the information is not available.
int get_numa_node_for_core(int core_id) noexcept;

// Return the physical core ID (HT sibling group) for a logical CPU.
// Reads /sys/devices/system/cpu/cpuN/topology/core_id.
// Two logical CPUs with the same physical core ID share L1/L2.
int get_physical_core_id(int logical_cpu) noexcept;

// Return the first HT sibling of a logical CPU (the "other" logical CPU
// on the same physical core). Returns -1 if HT is disabled or unavailable.
int get_ht_sibling(int logical_cpu) noexcept;

// Validate that two logical CPU IDs are not HT siblings of each other.
// Call at startup for each pair of latency-critical thread cores.
// Aborts with a clear message if they share a physical core.
void assert_not_ht_siblings(int core_a, int core_b,
                             std::string_view name_a,
                             std::string_view name_b) noexcept;

// Get current time in nanoseconds using CLOCK_MONOTONIC_RAW.
// MONOTONIC_RAW is not adjusted by NTP, has no leap-second jumps, and cannot
// go backwards. Use this for all latency measurements and rate-limiting.
// Never use std::chrono::system_clock on the trading path.
[[nodiscard]] inline int64_t get_monotonic_ns() noexcept {
    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1'000'000'000LL + ts.tv_nsec;
}

} // namespace omm
