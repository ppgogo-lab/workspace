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
// ─── Risk monitor thread ──────────────────────────────────────────────────────

void TradingEngine::risk_monitor_loop() noexcept {
    set_thread_name("omm-risk");
    pin_if_configured(cfg_.affinity.risk_monitor_core);
    apply_realtime_if_configured(cfg_.scheduling.enable_realtime,
                                 cfg_.scheduling.risk_monitor_priority,
                                 "omm-risk");
    int64_t last_snapshot_ts = 0;
    int64_t last_limit_check_ts = 0;
    uint8_t last_breach_mask = 0;

    while (!stop_flag_.load(std::memory_order_relaxed)) {
        bool did_work = false;
        // Process fills from risk_buf_
        Trade trade{};
        while (risk_buf_.try_pop(trade)) {
            did_work = true;
            post_risk_.on_fill(trade);
            std::lock_guard<std::mutex> lock(book_state_mutex_);
            rebuild_book_position_locked(trade);
        }

        const int64_t now_ns = get_monotonic_ns();
        if (did_work || now_ns - last_limit_check_ts >= kRiskCheckIntervalNs) {
            std::array<Greeks, MAX_INSTRUMENTS> greeks{};
            (void)read_all_greeks(greeks.data(), n_instruments_);
            post_risk_.check_limits(greeks.data(), n_instruments_);
            last_limit_check_ts = now_ns;

            const uint8_t breach_mask =
                (post_risk_.position_breach() ? 1u << 0 : 0u)
                | (post_risk_.delta_breach() ? 1u << 1 : 0u)
                | (post_risk_.gamma_breach() ? 1u << 2 : 0u)
                | (post_risk_.vega_breach() ? 1u << 3 : 0u);
            if (breach_mask != 0 && breach_mask != last_breach_mask) {
                OMM_LOG_WARN("risk", "breach flags: pos={} delta={} gamma={} vega={}",
                             (int)post_risk_.position_breach(),
                             (int)post_risk_.delta_breach(),
                             (int)post_risk_.gamma_breach(),
                             (int)post_risk_.vega_breach());
            }
            last_breach_mask = breach_mask;
        }

        if (repository_) {
            const int64_t snapshot_interval_ns =
                static_cast<int64_t>(cfg_.persistence.snapshot_interval_ms) * 1'000'000LL;
            if (snapshot_interval_ns > 0 && now_ns - last_snapshot_ts >= snapshot_interval_ns) {
                PositionSnapshotEvent snapshot{};
                snapshot.snapshot_ts = now_ns;
                snapshot.n_instruments = n_instruments_;
                const Position* positions = post_risk_.positions();
                for (uint16_t i = 0; i < n_instruments_; ++i) {
                    snapshot.positions[i] = positions[i];
                }
                if (!repository_->enqueue_positions_snapshot(snapshot)) {
                    OMM_LOG_WARN("repo", "position snapshot queue full");
                }
                last_snapshot_ts = now_ns;
            }
        }

        const int64_t next_snapshot_due = repository_
            ? last_snapshot_ts
                + static_cast<int64_t>(cfg_.persistence.snapshot_interval_ms) * 1'000'000LL
            : now_ns + kRiskIdleSleepCapNs;
        const int64_t next_limit_due = last_limit_check_ts + kRiskCheckIntervalNs;
        const int64_t next_due = std::min(next_snapshot_due, next_limit_due);
        const int64_t sleep_ns = std::max<int64_t>(0, next_due - get_monotonic_ns());
        if (!did_work) {
            sleep_for_ns_interruptible(stop_flag_, sleep_ns, kRiskIdleSleepCapNs);
        }
    }
}


} // namespace omm
