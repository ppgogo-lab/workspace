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
void TradingEngine::arb_loop(int idx) noexcept {
    set_thread_name("omm-arb");
    if (idx >= 0 && idx < cfg_.product_count && idx < MAX_PRODUCTS) {
        const int arb_core = cfg_.products[idx].arbitrage_core;
        if (arb_core >= 0 && arb_core != cfg_.products[idx].strategy_core) {
            pin_if_configured(arb_core);
        }
    }
    apply_realtime_if_configured(cfg_.scheduling.enable_realtime,
                                 cfg_.scheduling.arbitrage_priority,
                                 "omm-arb");

    // Bind to local NUMA node for optimal memory access
    if (numa_available_multi_node()) {
        bind_thread_to_local_numa_node();
    }

    GatewayEvent ev{};
    ArbMarketTrigger trigger{};
    Timestamp next_maintenance_ns = get_monotonic_ns() + kArbMaintenanceIntervalNs;
    int spin_count = 0;  // Adaptive spinning counter
    while (!stop_flag_.load(std::memory_order_relaxed)) {
        bool did_work = false;

        for (int drained = 0;
             drained < kArbEventBurstCap && arb_event_buf_[idx].try_pop(ev);
             ++drained) {
            did_work = true;
            for (int slot = 0; slot < cfg_.products[idx].arbitrage_strategy_count
                 && slot < MAX_ARBITRAGE_STRATEGIES_PER_PRODUCT; ++slot) {
                auto& strategy = arbitrage_strategies_[idx][slot];
                if (!strategy) continue;
                switch (ev.type) {
                case GatewayEventType::OrderAck:
                    strategy->on_order_ack(ev.order);
                    break;
                case GatewayEventType::OrderFill:
                case GatewayEventType::QuoteFill:
                    strategy->on_fill(ev.trade);
                    break;
                case GatewayEventType::OrderCancel:
                    strategy->on_order_cancel(ev.order.client_order_id);
                    break;
                case GatewayEventType::OrderReject:
                    strategy->on_order_reject(ev.order);
                    break;
                default:
                    break;
                }
            }
        }

        const Timestamp now_ns = get_monotonic_ns();
        for (int drained = 0;
             drained < kArbMarketTriggerBurstCap && arb_market_trigger_buf_[idx].try_pop(trigger);
             ++drained) {
            did_work = true;
            const Timestamp eval_ts = get_monotonic_ns();
            for (int slot = 0; slot < cfg_.products[idx].arbitrage_strategy_count
                 && slot < MAX_ARBITRAGE_STRATEGIES_PER_PRODUCT; ++slot) {
                auto& strategy = arbitrage_strategies_[idx][slot];
                if (!strategy) continue;
                strategy->on_market_update(trigger.instrument_id, eval_ts);
            }
        }

        if (now_ns >= next_maintenance_ns) {
            did_work = true;
            next_maintenance_ns = now_ns + kArbMaintenanceIntervalNs;
            for (int slot = 0; slot < cfg_.products[idx].arbitrage_strategy_count
                 && slot < MAX_ARBITRAGE_STRATEGIES_PER_PRODUCT; ++slot) {
                auto& strategy = arbitrage_strategies_[idx][slot];
                if (!strategy) continue;
                strategy->on_timer(now_ns);
            }
        }

        if (!did_work) {
            adaptive_spin_pause(spin_count);
        } else {
            spin_count = 0;  // Reset on work
        }
    }
}


} // namespace omm
