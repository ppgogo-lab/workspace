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
// Strategy thread ──────────────────────────────────────────────────────────

void TradingEngine::strategy_loop(int idx) noexcept {
    set_thread_name("omm-strat");
    if (idx >= 0 && idx < cfg_.product_count && idx < MAX_PRODUCTS) {
        pin_if_configured(cfg_.products[idx].strategy_core);
    }
    apply_realtime_if_configured(cfg_.scheduling.enable_realtime,
                                 cfg_.scheduling.strategy_priority,
                                 "omm-strat");

    // Bind to local NUMA node for optimal memory access
    if (numa_available_multi_node()) {
        bind_thread_to_local_numa_node();
    }

    GatewayEvent ev{};
    TimerEvent timer_ev{};
    uint64_t coalesced_signal_seen_versions[MAX_INSTRUMENTS]{};
    uint64_t coalesced_timer_seen_versions[kCoalescedTimerSlotCount]{};
    int spin_count = 0;  // Adaptive spinning counter

    while (!stop_flag_.load(std::memory_order_relaxed)) {
        bool did_work = false;
        update_max(max_timer_queue_depth_[idx],
                   static_cast<uint32_t>(timer_buf_[idx].size_approx()));

        // Gateway events (order/quote acks, fills, cancels) arrive in bursts.
        // Batch-pop to amortize atomic overhead. Each event is independent, so
        // we can process them in any order. The burst cap prevents callback
        // floods from starving pricing signals.
        constexpr int kGatewayBatchSize = 8;
        alignas(64) GatewayEvent ev_batch[kGatewayBatchSize];
        int gateway_budget = kStrategyGatewayBurstCap;
        while (gateway_budget > 0) {
            const int batch_size = gateway_event_buf_[idx].try_pop_batch(ev_batch,
                                                                          std::min(gateway_budget, kGatewayBatchSize));
            if (batch_size == 0) break;  // No more events available
            did_work = true;
            gateway_budget -= batch_size;
            // Process all events in the batch
            for (int i = 0; i < batch_size; ++i) {
                const GatewayEvent& ev = ev_batch[i];
                switch (ev.type) {
                case GatewayEventType::OrderAck:
                    strategies_[idx]->on_order_ack(ev.order);
                    break;
                case GatewayEventType::QuoteAck:
                    if (ev.quote.instrument_id < MAX_INSTRUMENTS) {
                        const int64_t now_ns = get_monotonic_ns();
                        last_quote_ack_route_ts_[ev.quote.instrument_id] = now_ns;  // Plain store (single writer)
                        last_quote_ack_route_latency_ns_[ev.quote.instrument_id] =
                            ev.quote.ack_ts > 0 ? std::max<int64_t>(0, now_ns - ev.quote.ack_ts) : 0;  // Plain store (single writer)
                    }
                    strategies_[idx]->on_quote_ack(ev.quote);
                    break;
                case GatewayEventType::QuoteCancel:
                    if (ev.quote.instrument_id < MAX_INSTRUMENTS) {
                        const int64_t now_ns = get_monotonic_ns();
                        last_quote_cancel_route_ts_[ev.quote.instrument_id] = now_ns;  // Plain store (single writer)
                        last_quote_cancel_route_latency_ns_[ev.quote.instrument_id] =
                            ev.quote.ack_ts > 0 ? std::max<int64_t>(0, now_ns - ev.quote.ack_ts) : 0;  // Plain store (single writer)
                    }
                    strategies_[idx]->on_quote_cancel(ev.quote);
                    break;
                case GatewayEventType::QuoteReject:
                    strategies_[idx]->on_quote_reject(ev.quote);
                    break;
                case GatewayEventType::OrderFill:
                case GatewayEventType::QuoteFill:
                    strategies_[idx]->on_fill(ev.trade);
                    break;
                case GatewayEventType::OrderCancel:
                    strategies_[idx]->on_order_cancel(ev.order.client_order_id);
                    break;
                case GatewayEventType::OrderReject:
                    strategies_[idx]->on_order_reject(ev.order);
                    break;
                default:
                    break;
                }
            }
        }

        int timer_budget = kStrategyTimerBurstCap;
        for (; timer_budget > 0 && timer_buf_[idx].try_pop(timer_ev); --timer_budget) {
            did_work = true;
            strategies_[idx]->on_timer(timer_ev);
        }
        const int coalesced_timers =
            drain_coalesced_timers(idx, coalesced_timer_seen_versions, timer_budget);
        if (coalesced_timers > 0) did_work = true;

        // Batch-pop pricing signals to amortize atomic overhead
        constexpr int kSignalBatchSize = 8;
        alignas(64) PricingSignal sig_batch[kSignalBatchSize];
        int signal_budget = kStrategySignalBurstCap;
        while (signal_budget > 0) {
            const int batch_size = signal_buf_[idx].try_pop_batch(sig_batch,
                                                                   std::min(signal_budget, kSignalBatchSize));
            if (batch_size == 0) break;  // No more signals available
            did_work = true;
            signal_budget -= batch_size;
            for (int i = 0; i < batch_size; ++i) {
                const PricingSignal& sig = sig_batch[i];
                if (sig.instrument_id < MAX_INSTRUMENTS) {
                    last_strategy_signal_ts_[sig.instrument_id] = get_monotonic_ns();  // Plain store (single writer)
                }
            }
            strategies_[idx]->on_signals(sig_batch, batch_size);
        }
        const int coalesced_signals =
            drain_coalesced_signals(idx, coalesced_signal_seen_versions, signal_budget);
        if (coalesced_signals > 0) did_work = true;

        if (!did_work) {
            idle_pause(cfg_.scheduling.low_latency_spin, spin_count);
        } else {
            spin_count = 0;  // Reset on work
        }
    }
}


} // namespace omm
