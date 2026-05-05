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
// ─── Gateway dispatcher thread ────────────────────────────────────────────────

void TradingEngine::gateway_dispatcher_loop() noexcept {
    set_thread_name("omm-gw-disp");
    pin_if_configured(cfg_.affinity.gateway_dispatcher_core);
    apply_realtime_if_configured(cfg_.scheduling.enable_realtime,
                                 cfg_.scheduling.gateway_dispatcher_priority,
                                 "omm-gw-disp");

    // Bind to local NUMA node for optimal memory access
    if (numa_available_multi_node()) {
        bind_thread_to_local_numa_node();
    }

    struct DeferredCallbackSideEffect {
        GatewayEvent event{};
    };

    int spin_count = 0;  // Adaptive spinning counter
    while (!stop_flag_.load(std::memory_order_relaxed)) {
        bool did_work = false;
        if (cancel_all_live_requested_.exchange(false, std::memory_order_acquire)) {
            cancel_all_live_orders_and_quotes();
            did_work = true;
        }
        auto drain_callbacks = [&](int burst_cap) {
            DeferredCallbackSideEffect deferred[kDispatcherCallbackLeadBurstCap]{};
            int deferred_count = 0;
            GatewayEvent ev{};
            for (int drained = 0;
                 drained < burst_cap && gateway_->callback_buf.try_pop(ev);
                 ++drained) {
                did_work = true;
                const int p = ev.product_index;
                if (p >= MAX_PRODUCTS) continue;

                DeferredCallbackSideEffect normalized{};
                normalized.event = ev;
                bool route_to_arbitrage = false;
                switch (normalized.event.type) {
                case GatewayEventType::OrderAck:
                    handle_gateway_order_update(normalized.event.order,
                                                normalized.event.type);
                    route_to_arbitrage = is_arb_order_id(normalized.event.order.client_order_id);
                    break;
                case GatewayEventType::QuoteAck:
                    handle_gateway_quote_update(normalized.event.quote,
                                                normalized.event.type);
                    break;
                case GatewayEventType::QuoteCancel:
                    handle_gateway_quote_update(normalized.event.quote,
                                                normalized.event.type);
                    break;
                case GatewayEventType::QuoteReject:
                    handle_gateway_quote_update(normalized.event.quote,
                                                normalized.event.type);
                    break;
                case GatewayEventType::OrderFill:
                case GatewayEventType::QuoteFill:
                    handle_gateway_fill(&normalized.event.trade, &normalized.event.type);
                    route_to_arbitrage = is_arb_order_id(normalized.event.trade.client_order_id);
                    break;
                case GatewayEventType::OrderCancel:
                    handle_gateway_order_update(normalized.event.order,
                                                normalized.event.type);
                    route_to_arbitrage = is_arb_order_id(normalized.event.order.client_order_id);
                    break;
                case GatewayEventType::OrderReject:
                    handle_gateway_order_update(normalized.event.order,
                                                normalized.event.type);
                    route_to_arbitrage = is_arb_order_id(normalized.event.order.client_order_id);
                    break;
                default:
                    break;
                }

                if (route_to_arbitrage) {
                    while (!stop_flag_.load(std::memory_order_relaxed)
                        && !arb_event_buf_[p].try_push(normalized.event)) {
                        spin_pause();
                    }
                } else {
                    while (!stop_flag_.load(std::memory_order_relaxed)
                        && !gateway_event_buf_[p].try_push(normalized.event)) {
                        spin_pause();
                    }
                }

                deferred[deferred_count++] = normalized;
            }

            for (int i = 0; i < deferred_count; ++i) {
                DeferredCallbackSideEffect& side = deferred[i];
                switch (side.event.type) {
                case GatewayEventType::OrderAck:
                    publish_monitor_order(side.event.order);
                    defer_order_persistence(OrderPersistenceEventType::Ack,
                                            side.event.order);
                    break;
                case GatewayEventType::QuoteAck: {
                    publish_monitor_quote(side.event.quote);
                    defer_quote_persistence(QuotePersistenceEventType::Ack,
                                            side.event.quote,
                                            nullptr);
                    break;
                }
                case GatewayEventType::QuoteCancel:
                    publish_monitor_quote(side.event.quote);
                    defer_quote_persistence(QuotePersistenceEventType::Cancel,
                                            side.event.quote,
                                            nullptr);
                    break;
                case GatewayEventType::QuoteReject:
                    publish_monitor_quote(side.event.quote);
                    defer_quote_persistence(QuotePersistenceEventType::Reject,
                                            side.event.quote,
                                            nullptr);
                    break;
                case GatewayEventType::OrderFill:
                case GatewayEventType::QuoteFill: {
                    publish_monitor_trade(side.event.trade);
                    defer_trade_persistence(side.event.trade);

                    Order filled{};
                    filled.client_order_id = side.event.trade.client_order_id;
                    filled.instrument_id   = side.event.trade.instrument_id;
                    filled.product_index   = side.event.trade.product_index;
                    filled.exchange_id     = side.event.trade.exchange_id;
                    filled.side            = side.event.trade.side;
                    filled.status          = OrderStatus::Filled;
                    filled.price           = side.event.trade.fill_price;
                    filled.volume          = side.event.trade.fill_volume;
                    filled.avg_fill_price  = side.event.trade.fill_price;
                    filled.filled_volume   = side.event.trade.fill_volume;
                    filled.ack_ts          = side.event.trade.fill_ts;
                    filled.book_id         = side.event.trade.book_id;
                    publish_monitor_order(filled);
                    (void)risk_buf_.try_push(side.event.trade);
                    break;
                }
                case GatewayEventType::OrderCancel:
                    publish_monitor_order(side.event.order);
                    defer_order_persistence(OrderPersistenceEventType::Cancel,
                                            side.event.order);
                    break;
                case GatewayEventType::OrderReject:
                    publish_monitor_order(side.event.order);
                    defer_order_persistence(OrderPersistenceEventType::Reject,
                                            side.event.order);
                    break;
                default:
                    break;
                }
            }
        };

        // Phase 3 fairness policy: give callbacks a bounded head start so acks
        // and fills reach the strategy sooner, then interleave smaller callback
        // slices between per-product send bursts so the send path is not
        // starved in the other direction.
        drain_callbacks(kDispatcherCallbackLeadBurstCap);

        // Round-robin over all strategy output buffers
        for (int i = 0; i < cfg_.product_count && i < MAX_PRODUCTS; ++i) {
            Order order{};
            for (int drained = 0;
                 drained < kDispatcherOrderBurstCap && order_buf_[i].try_pop(order);
                 ++drained) {
                did_work = true;
                if (strategy_dispatch_suspended_.load(std::memory_order_acquire) && !order.is_manual) {
                    continue;
                }
                if (!order.is_manual) {
                    order.book_id = mm_book_ids_[i];
                }
                const bool sent = gateway_->send_order(order);
                publish_monitor_order(order);
                if (sent) {
                    track_live_order_submit(order);
                    defer_order_persistence(OrderPersistenceEventType::Submit, order);
                } else {
                    Order rejected = order;
                    rejected.status = OrderStatus::Rejected;
                    rejected.ack_ts = get_monotonic_ns();
                    defer_order_persistence(OrderPersistenceEventType::Reject, rejected);
                }
                if (((drained + 1) % kDispatcherCallbackInterleaveBurstCap) == 0) {
                    drain_callbacks(kDispatcherCallbackInterleaveBurstCap);
                }
            }

            Quote quote{};
            for (int drained = 0;
                 drained < kDispatcherQuoteBurstCap && quote_buf_[i].try_pop(quote);
                 ++drained) {
                did_work = true;
                if (strategy_dispatch_suspended_.load(std::memory_order_acquire)) {
                    continue;
                }
                quote.book_id = mm_book_ids_[i];
                OrderId bid_order_id = 0;
                OrderId ask_order_id = 0;
                const bool sent = gateway_->send_quote(quote, &bid_order_id, &ask_order_id);
                publish_monitor_quote(quote);
                if (sent && (quote.bid_volume > 0 || quote.ask_volume > 0)) {
                    track_live_quote_submit(quote, bid_order_id, ask_order_id);
                    defer_quote_persistence(QuotePersistenceEventType::Submit, quote, nullptr);
                } else if (!sent) {
                    Quote rejected = quote;
                    rejected.bid_status = OrderStatus::Rejected;
                    rejected.ask_status = OrderStatus::Rejected;
                    rejected.ack_ts = get_monotonic_ns();
                    defer_quote_persistence(QuotePersistenceEventType::Reject, rejected, nullptr);
                }
                if (((drained + 1) % kDispatcherCallbackInterleaveBurstCap) == 0) {
                    drain_callbacks(kDispatcherCallbackInterleaveBurstCap);
                }
            }

            ArbIntent intent{};
            for (int drained = 0;
                 drained < kDispatcherArbIntentBurstCap && arb_intent_buf_[i].try_pop(intent);
                 ++drained) {
                did_work = true;
                if (strategy_dispatch_suspended_.load(std::memory_order_acquire)) {
                    continue;
                }
                if (intent.kind == ArbIntentKind::SubmitOrder) {
                    intent.order.book_id = arb_book_id_for_type(i, intent.strategy_type);
                    const bool sent = gateway_->send_order(intent.order);
                    publish_monitor_order(intent.order);
                    if (sent) {
                        track_live_order_submit(intent.order);
                        defer_order_persistence(OrderPersistenceEventType::Submit, intent.order);
                    } else {
                        Order rejected = intent.order;
                        rejected.status = OrderStatus::Rejected;
                        rejected.ack_ts = get_monotonic_ns();
                        defer_order_persistence(OrderPersistenceEventType::Reject, rejected);
                    }
                } else if (intent.kind == ArbIntentKind::CancelOrder) {
                    gateway_->cancel_order(intent.order.client_order_id, intent.order.instrument_id);
                }
                if (((drained + 1) % kDispatcherCallbackInterleaveBurstCap) == 0) {
                    drain_callbacks(kDispatcherCallbackInterleaveBurstCap);
                }
            }
            drain_callbacks(kDispatcherCallbackInterleaveBurstCap);
        }
        if (!did_work) {
            idle_pause(cfg_.scheduling.low_latency_spin, spin_count);
        } else {
            spin_count = 0;  // Reset on work
        }
    }
    gateway_dispatcher_running_.store(false, std::memory_order_release);
}


} // namespace omm
