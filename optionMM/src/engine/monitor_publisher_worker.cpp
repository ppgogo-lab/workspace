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
/**
 * @brief Implements Monitor publish loop.
 * @return None.
 * @note Noexcept API preserves hot-path failure and latency invariants.
 */
void TradingEngine::monitor_publish_loop() noexcept {
    set_thread_name("omm-monitor");

    // Bind to local NUMA node for optimal memory access
    if (numa_available_multi_node()) {
        bind_thread_to_local_numa_node();
    }

    constexpr int kMonitorPublishBurstCap = 128;
    constexpr int kMonitorBatchSize = 16;  // Larger batch for monitoring (less latency-sensitive)

    while (gateway_dispatcher_running_.load(std::memory_order_acquire)
           || !deferred_monitor_ticks_.empty_approx()
           || !deferred_monitor_orders_.empty_approx()
           || !deferred_monitor_quotes_.empty_approx()
           || !deferred_monitor_trades_.empty_approx()
           || !deferred_persist_order_events_.empty_approx()
           || !deferred_persist_quote_events_.empty_approx()
           || !deferred_persist_trades_.empty_approx()) {
        bool did_work = false;

        // Batch-drain monitoring events to reduce atomic overhead
        alignas(64) TopOfBookTick tick_batch[kMonitorBatchSize];
        int tick_budget = kMonitorPublishBurstCap;
        while (tick_budget > 0) {
            const int batch_size = deferred_monitor_ticks_.try_pop_batch(tick_batch,
                                                                          std::min(tick_budget, kMonitorBatchSize));
            if (batch_size == 0) break;
            did_work = true;
            tick_budget -= batch_size;
            for (int i = 0; i < batch_size; ++i) {
                monitor_ticks_.publish(tick_batch[i]);
            }
        }

        alignas(64) Order order_batch[kMonitorBatchSize];
        int order_budget = kMonitorPublishBurstCap;
        while (order_budget > 0) {
            const int batch_size = deferred_monitor_orders_.try_pop_batch(order_batch,
                                                                           std::min(order_budget, kMonitorBatchSize));
            if (batch_size == 0) break;
            did_work = true;
            order_budget -= batch_size;
            for (int i = 0; i < batch_size; ++i) {
                monitor_orders_.publish(order_batch[i]);
            }
        }

        alignas(64) Quote quote_batch[kMonitorBatchSize];
        int quote_budget = kMonitorPublishBurstCap;
        while (quote_budget > 0) {
            const int batch_size = deferred_monitor_quotes_.try_pop_batch(quote_batch,
                                                                           std::min(quote_budget, kMonitorBatchSize));
            if (batch_size == 0) break;
            did_work = true;
            quote_budget -= batch_size;
            for (int i = 0; i < batch_size; ++i) {
                monitor_quotes_.publish(quote_batch[i]);
            }
        }

        alignas(64) Trade trade_batch[kMonitorBatchSize];
        int trade_budget = kMonitorPublishBurstCap;
        while (trade_budget > 0) {
            const int batch_size = deferred_monitor_trades_.try_pop_batch(trade_batch,
                                                                           std::min(trade_budget, kMonitorBatchSize));
            if (batch_size == 0) break;
            did_work = true;
            trade_budget -= batch_size;
            for (int i = 0; i < batch_size; ++i) {
                monitor_trades_.publish(trade_batch[i]);
            }
        }

        if (repository_) {
            // Batch-drain persistence events to reduce atomic overhead
            constexpr int kPersistenceBatchSize = 16;

            alignas(64) OrderPersistenceEvent order_event_batch[kPersistenceBatchSize];
            int order_budget = kMonitorPublishBurstCap;
            while (order_budget > 0) {
                const int batch_size = deferred_persist_order_events_.try_pop_batch(
                    order_event_batch, std::min(order_budget, kPersistenceBatchSize));
                if (batch_size == 0) break;
                did_work = true;
                order_budget -= batch_size;

                const int enqueued = repository_->enqueue_order_events_batch(order_event_batch, batch_size);
                if (enqueued < batch_size) {
                    deferred_persistence_drops_.fetch_add(batch_size - enqueued, std::memory_order_relaxed);
                }
            }

            alignas(64) QuotePersistenceEvent quote_event_batch[kPersistenceBatchSize];
            int quote_budget = kMonitorPublishBurstCap;
            while (quote_budget > 0) {
                const int batch_size = deferred_persist_quote_events_.try_pop_batch(
                    quote_event_batch, std::min(quote_budget, kPersistenceBatchSize));
                if (batch_size == 0) break;
                did_work = true;
                quote_budget -= batch_size;

                const int enqueued = repository_->enqueue_quote_events_batch(quote_event_batch, batch_size);
                if (enqueued < batch_size) {
                    deferred_persistence_drops_.fetch_add(batch_size - enqueued, std::memory_order_relaxed);
                }
            }

            alignas(64) Trade trade_batch[kPersistenceBatchSize];
            int trade_budget = kMonitorPublishBurstCap;
            while (trade_budget > 0) {
                const int batch_size = deferred_persist_trades_.try_pop_batch(
                    trade_batch, std::min(trade_budget, kPersistenceBatchSize));
                if (batch_size == 0) break;
                did_work = true;
                trade_budget -= batch_size;

                const int enqueued = repository_->enqueue_trades_batch(trade_batch, batch_size);
                if (enqueued < batch_size) {
                    deferred_persistence_drops_.fetch_add(batch_size - enqueued, std::memory_order_relaxed);
                }
            }
        }

        if (!did_work) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
}


} // namespace omm
