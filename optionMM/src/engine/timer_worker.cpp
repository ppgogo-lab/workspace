#include "engine/trading_engine.h"
#include "engine_loop_common.h"

#include "common/numa_utils.h"
#include "logger/logger.h"
#include "pricing/black76.h"
#include "pricing/orc_wing.h"
#include "pricing/svi.h"
#include "pricing/wing.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>

namespace omm {
// ─── Timer thread ─────────────────────────────────────────────────────────────

void TradingEngine::timer_loop() noexcept {
    set_thread_name("omm-timer");
    pin_if_configured(cfg_.affinity.timer_core);
    apply_realtime_if_configured(cfg_.scheduling.enable_realtime,
                                 cfg_.scheduling.timer_priority,
                                 "omm-timer");

    // Bind to local NUMA node for optimal memory access
    if (numa_available_multi_node()) {
        bind_thread_to_local_numa_node();
    }

    int64_t last_hedge_ns   = get_monotonic_ns();
    int64_t last_quote_refresh_ns = last_hedge_ns;
    int64_t last_T_refresh_ns = last_hedge_ns;
    const int64_t hedge_interval_ns =
        static_cast<int64_t>(cfg_.timer.hedge_check_interval_ms) * 1'000'000LL;
    const int64_t quote_refresh_interval_ns =
        static_cast<int64_t>(cfg_.timer.quote_refresh_interval_ms) * 1'000'000LL;
    static constexpr int64_t T_REFRESH_NS = 1'000'000'000LL;  // 1 second

    while (!stop_flag_.load(std::memory_order_relaxed)) {
        const int64_t now = get_monotonic_ns();

        // Refresh option_T_ every second (T changes by ~1/86400 per day)
        if (now - last_T_refresh_ns >= T_REFRESH_NS) {
            refresh_option_T();
            last_T_refresh_ns = now;
        }

        // Hedge check at configured interval
        if (now - last_hedge_ns >= hedge_interval_ns) {
            TimerEvent ev{};
            ev.trigger_ts_ns = now;
            ev.type          = TimerEventType::HedgeCheck;

            for (int i = 0; i < cfg_.product_count && i < MAX_PRODUCTS; ++i) {
                if (!strategies_[i]) continue;
                // Duplicate hedge checks are idempotent; keep only the latest
                // one outstanding per product instead of spinning on timer_buf_.
                coalesce_timer_event(i, ev);
            }
            last_hedge_ns = now;
        }

        if (quote_refresh_interval_ns > 0 && now - last_quote_refresh_ns >= quote_refresh_interval_ns) {
            TimerEvent ev{};
            ev.trigger_ts_ns = now;
            ev.type = TimerEventType::QuoteRefresh;
            for (int i = 0; i < cfg_.product_count && i < MAX_PRODUCTS; ++i) {
                if (!strategies_[i]) continue;
                // Quote refresh is also latest-only safe: if the strategy is
                // behind, a newer refresh supersedes an older pending refresh.
                coalesce_timer_event(i, ev);
            }
            last_quote_refresh_ns = now;
        }

        int64_t next_deadline = last_T_refresh_ns + T_REFRESH_NS;
        if (hedge_interval_ns > 0) {
            next_deadline = std::min(next_deadline, last_hedge_ns + hedge_interval_ns);
        }
        if (quote_refresh_interval_ns > 0) {
            next_deadline = std::min(next_deadline,
                                     last_quote_refresh_ns + quote_refresh_interval_ns);
        }
        const int64_t sleep_ns =
            std::max<int64_t>(0, next_deadline - get_monotonic_ns());
        sleep_for_ns_interruptible(stop_flag_, sleep_ns, kTimerIdleSleepCapNs);
    }
}


} // namespace omm
