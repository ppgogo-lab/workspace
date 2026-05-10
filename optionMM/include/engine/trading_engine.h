#pragma once

#include "common/types.h"
#include "common/config.h"
#include "common/fixed_hash_table.h"
#include "common/latest_snapshot.h"
#include "common/ring_buffer.h"
#include "feed/feed_handler.h"
#include "pricing/black76.h"
#include "pricing/vol_surface.h"
#include "pricing/svi.h"
#include "pricing/sabr.h"
#include "pricing/cubic_spline.h"
#include "pricing/wing.h"
#include "pricing/orc_wing.h"
#include "gateway/gateway.h"
#include "strategy/arbitrage_strategy.h"
#include "strategy/arb_params.h"
#include "strategy/mm_framework.h"
#include "strategy/mm_params.h"
#include "risk/pre_trade_risk.h"
#include "risk/post_trade_risk.h"
#include "monitoring/topic.h"
#include "persistence/data_repository.h"

#include <thread>
#include <atomic>
#include <memory>
#include <array>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace omm {

class PricerWorker;
class StrategyWorker;
class ArbitrageWorker;
class GatewayDispatcherWorker;
class MonitorPublisherWorker;
class VolFitterWorker;
class RiskMonitorWorker;
class TimerWorker;

// ─── TradingEngine ────────────────────────────────────────────────────────────
// Owns all ring buffers, threads, and component instances.
// Startup sequence: see start().
//
// Thread map (default core layout from SystemConfig):
//   feed_core             → feed thread
//   pricer_core           → pricer thread
//   products[i].core      → strategy thread i
//   gateway_dispatcher_core → gateway dispatcher thread
//   vol_fitter_core       → vol surface fitter thread
//   risk_monitor_core     → post-trade risk monitor thread
//   timer_core            → timer thread (hedge check, quote refresh)

class TradingEngine {
    friend class PricerWorker;
    friend class StrategyWorker;
    friend class ArbitrageWorker;
    friend class GatewayDispatcherWorker;
    friend class MonitorPublisherWorker;
    friend class VolFitterWorker;
    friend class RiskMonitorWorker;
    friend class TimerWorker;

public:
    /**
     * @brief TradingEngine.
     * @param cfg Parameter supplied by the caller.
     * @param gateway Parameter supplied by the caller.
     * @param feed Parameter supplied by the caller.
     * @return None.
     */
    explicit TradingEngine(const SystemConfig& cfg,
                           std::unique_ptr<IGateway> gateway,
                           std::unique_ptr<IFeedHandler> feed);
    /**
     * @brief TradingEngine.
     * @return None.
     */
    ~TradingEngine();

    // Non-copyable
    /**
     * @brief TradingEngine.
     * @param TradingEngine Parameter supplied by the caller.
     * @return None.
     */
    TradingEngine(const TradingEngine&)            = delete;
    TradingEngine& operator=(const TradingEngine&) = delete;

    // Start all threads. Blocks until stop() is called.
    /**
     * @brief Start.
     * @return None.
     */
    void start();
    /**
     * @brief Stop.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    void stop() noexcept;

    // Enable transparent huge pages for large memory regions to reduce TLB misses.
    // Should be called after construction but before start().
    // Returns the number of regions successfully enabled for huge pages.
    /**
     * @brief Enable huge pages for large arrays.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    int enable_huge_pages_for_large_arrays() noexcept;

    // Enable NUMA-aware thread placement for multi-socket systems.
    // Binds each thread to its local NUMA node to reduce remote memory access.
    // Should be called after construction but before start().
    // Returns true if NUMA awareness was successfully enabled.
    /**
     * @brief Enable numa awareness.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    bool enable_numa_awareness() noexcept;

    // Access for testing / gRPC server
    /**
     * @brief Gateway.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] IGateway*   gateway()    const noexcept { return gateway_.get(); }
    /**
     * @brief Mm params.
     * @param i Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] AtomicMMParams& mm_params(int i) noexcept { return mm_params_[i]; }
    /**
     * @brief Product pricing.
     * @param i Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] ProductPricingConfig product_pricing(int i) const noexcept;
    /**
     * @brief Set product pricing.
     * @param i Parameter supplied by the caller.
     * @param pricing Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] bool set_product_pricing(int i, const ProductPricingConfig& pricing) noexcept;
    /**
     * @brief Post risk.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] const PostTradeRisk& post_risk() const noexcept { return post_risk_; }
    /**
     * @brief Post risk mutable.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] PostTradeRisk& post_risk_mutable() noexcept { return post_risk_; }
    /**
     * @brief Product count.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] int product_count() const noexcept { return cfg_.product_count; }
    /**
     * @brief N instruments.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] int n_instruments() const noexcept { return n_instruments_; }
    /**
     * @brief Instruments.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] const Instrument* instruments() const noexcept { return instruments_; }
    /**
     * @brief Read tick snapshot.
     * @param id Parameter supplied by the caller.
     * @param out Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] bool read_tick_snapshot(uint16_t id, TopOfBookTick* out) const noexcept {
        return tick_snapshot_.read(id, out);
    }
    /**
     * @brief Orc wing surface.
     * @param i Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] const VolSurfaceManager<OrcWingVolSurface>& orc_wing_surface(int i) const noexcept {
        return orc_wing_surfaces_[i];
    }
    /**
     * @brief Wing surface.
     * @param i Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] const VolSurfaceManager<WingVolSurface>& wing_surface(int i) const noexcept {
        return wing_surfaces_[i];
    }
    /**
     * @brief Svi surface.
     * @param i Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] const VolSurfaceManager<SVIVolSurface>& svi_surface(int i) const noexcept {
        return vol_surfaces_[i];
    }
    /**
     * @brief Option count.
     * @param i Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] uint16_t option_count(int i) const noexcept { return option_count_[i]; }
    /**
     * @brief Option id.
     * @param product_idx Parameter supplied by the caller.
     * @param option_idx Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] uint16_t option_id(int product_idx, uint16_t option_idx) const noexcept {
        return option_ids_[product_idx][option_idx];
    }

    // Greeks snapshot – last computed Greeks per instrument (written by pricer thread)
    /**
     * @brief Read greeks snapshot.
     * @param id Parameter supplied by the caller.
     * @param out Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] bool read_greeks_snapshot(uint16_t id, Greeks* out) const noexcept {
        return greeks_snapshot_.read(id, out);
    }
    /**
     * @brief Read all greeks.
     * @param out Parameter supplied by the caller.
     * @param max_count Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] int read_all_greeks(Greeks* out, int max_count) const noexcept;
    /**
     * @brief Monitor ticks.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] const MonitoringTopic<TopOfBookTick, 8192>& monitor_ticks() const noexcept {
        return monitor_ticks_;
    }
    /**
     * @brief Monitor orders.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] const MonitoringTopic<Order, 4096>& monitor_orders() const noexcept {
        return monitor_orders_;
    }
    /**
     * @brief Monitor quotes.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] const MonitoringTopic<Quote, 4096>& monitor_quotes() const noexcept {
        return monitor_quotes_;
    }
    /**
     * @brief Monitor trades.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] const MonitoringTopic<Trade, 4096>& monitor_trades() const noexcept {
        return monitor_trades_;
    }
    /**
     * @brief Monitor alerts.
     * @param i Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] const MonitoringTopic<SystemAlert, 256>& monitor_alerts(int i) const noexcept {
        return monitor_alerts_[i];
    }
    /**
     * @brief Product monitor state.
     * @param product_idx Parameter supplied by the caller.
     * @param out Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] bool product_monitor_state(int product_idx,
                                             ProductMonitorState* out) const noexcept {
        if (product_idx < 0 || product_idx >= product_count()) return false;
        if (!strategies_[product_idx]) return false;
        return strategies_[product_idx]->read_product_monitor_state(out);
    }
    /**
     * @brief Instrument monitor states.
     * @param product_idx Parameter supplied by the caller.
     * @param out Parameter supplied by the caller.
     * @param max_count Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] int instrument_monitor_states(int product_idx,
                                                InstrumentMonitorState* out,
                                                int max_count) const noexcept {
        if (product_idx < 0 || product_idx >= product_count()) return 0;
        if (!strategies_[product_idx]) return 0;
        return strategies_[product_idx]->read_instrument_monitor_states(out, max_count);
    }
    /**
     * @brief Total coalesced signal writes.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] uint64_t total_coalesced_signal_writes() const noexcept;
    /**
     * @brief Total coalesced signal overwrites.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] uint64_t total_coalesced_signal_overwrites() const noexcept;
    /**
     * @brief Total coalesced timer writes.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] uint64_t total_coalesced_timer_writes() const noexcept;
    /**
     * @brief Total coalesced timer overwrites.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] uint64_t total_coalesced_timer_overwrites() const noexcept;
    /**
     * @brief Total signal emit count.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] uint64_t total_signal_emit_count() const noexcept;
    /**
     * @brief Total signal suppressed count.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] uint64_t total_signal_suppressed_count() const noexcept;
    /**
     * @brief Total pending future tick overwrites.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] uint64_t total_pending_future_tick_overwrites() const noexcept;
    /**
     * @brief Deferred monitor drops.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] uint64_t deferred_monitor_drops() const noexcept {
        return deferred_monitor_drops_.load(std::memory_order_relaxed);
    }
    /**
     * @brief Deferred persistence drops.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] uint64_t deferred_persistence_drops() const noexcept {
        return deferred_persistence_drops_.load(std::memory_order_relaxed);
    }
    /**
     * @brief Live state drops.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] uint64_t live_state_drops() const noexcept {
        return live_state_drops_.load(std::memory_order_relaxed);
    }
    /**
     * @brief Max signal queue depth.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] uint32_t max_signal_queue_depth() const noexcept;
    /**
     * @brief Max signal mailbox depth.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] uint32_t max_signal_mailbox_depth() const noexcept;
    /**
     * @brief Max timer queue depth.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] uint32_t max_timer_queue_depth() const noexcept;
    /**
     * @brief Last signal emit ts.
     * @param instrument_id Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] int64_t last_signal_emit_ts(uint16_t instrument_id) const noexcept;
    /**
     * @brief Last strategy signal ts.
     * @param instrument_id Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] int64_t last_strategy_signal_ts(uint16_t instrument_id) const noexcept;
    /**
     * @brief Last quote ack route ts.
     * @param instrument_id Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] int64_t last_quote_ack_route_ts(uint16_t instrument_id) const noexcept;
    /**
     * @brief Last quote cancel route ts.
     * @param instrument_id Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] int64_t last_quote_cancel_route_ts(uint16_t instrument_id) const noexcept;
    /**
     * @brief Last quote ack route latency ns.
     * @param instrument_id Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] int64_t last_quote_ack_route_latency_ns(uint16_t instrument_id) const noexcept;
    /**
     * @brief Last quote cancel route latency ns.
     * @param instrument_id Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] int64_t last_quote_cancel_route_latency_ns(uint16_t instrument_id) const noexcept;
    /**
     * @brief Strategy runtime stats.
     * @param product_idx Parameter supplied by the caller.
     * @param out Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] bool strategy_runtime_stats(int product_idx,
                                              StrategyRuntimeStats* out) const noexcept;
    /**
     * @brief Arbitrage strategy count.
     * @param product_idx Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] int arbitrage_strategy_count(int product_idx) const noexcept {
        if (product_idx < 0 || product_idx >= product_count()) return 0;
        return cfg_.products[product_idx].arbitrage_strategy_count;
    }
    /**
     * @brief Arbitrage strategy type.
     * @param product_idx Parameter supplied by the caller.
     * @param slot Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] ArbitrageStrategyType arbitrage_strategy_type(int product_idx,
                                                                int slot) const noexcept {
        if (product_idx < 0 || product_idx >= product_count()) return ArbitrageStrategyType::None;
        if (slot < 0 || slot >= MAX_ARBITRAGE_STRATEGIES_PER_PRODUCT) {
            return ArbitrageStrategyType::None;
        }
        return arb_strategy_types_[product_idx][slot];
    }
    /**
     * @brief Arbitrage strategy state.
     * @param product_idx Parameter supplied by the caller.
     * @param type Parameter supplied by the caller.
     * @param out Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] bool arbitrage_strategy_state(int product_idx,
                                                ArbitrageStrategyType type,
                                                ArbStrategyMonitorState* out) const noexcept;
    /**
     * @brief Arbitrage pcp monitor states.
     * @param product_idx Parameter supplied by the caller.
     * @param type Parameter supplied by the caller.
     * @param out Parameter supplied by the caller.
     * @param max_count Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] int arbitrage_pcp_monitor_states(int product_idx,
                                                   ArbitrageStrategyType type,
                                                   PCPPairMonitorState* out,
                                                   int max_count) const noexcept;
    /**
     * @brief Arbitrage params snapshot.
     * @param product_idx Parameter supplied by the caller.
     * @param type Parameter supplied by the caller.
     * @param out Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] bool arbitrage_params_snapshot(int product_idx,
                                                 ArbitrageStrategyType type,
                                                 ArbParamsConfig* out) const noexcept;
    /**
     * @brief Set arbitrage enabled.
     * @param product_idx Parameter supplied by the caller.
     * @param type Parameter supplied by the caller.
     * @param enabled Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] bool set_arbitrage_enabled(int product_idx,
                                             ArbitrageStrategyType type,
                                             bool enabled) noexcept;
    /**
     * @brief Arbitrage params.
     * @param product_idx Parameter supplied by the caller.
     * @param type Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] AtomicArbParams* arbitrage_params(int product_idx,
                                                    ArbitrageStrategyType type) noexcept;
    /**
     * @brief Identity state.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] const IdentityState& identity_state() const noexcept { return identity_state_; }
    /**
     * @brief Option time to expiry years.
     * @param opt Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] double option_time_to_expiry_years(const Instrument& opt) const noexcept;
    /**
     * @brief Zero session shutdown active.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] bool zero_session_shutdown_active() const noexcept {
        return strategy_dispatch_suspended_.load(std::memory_order_acquire);
    }
    /**
     * @brief Book positions snapshot.
     * @param out Parameter supplied by the caller.
     * @param max_count Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] int book_positions_snapshot(BookPosition* out, int max_count) const noexcept;
    /**
     * @brief Book portfolios snapshot.
     * @param out Parameter supplied by the caller.
     * @param max_count Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] int book_portfolios_snapshot(BookPortfolioGreeks* out, int max_count) const noexcept;
    /**
     * @brief Note session activated.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    void note_session_activated() noexcept;
    /**
     * @brief Shutdown on zero sessions.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    void shutdown_on_zero_sessions() noexcept;

    // Manual order submission from gRPC (bypasses strategy, goes direct to gateway dispatcher)
    /**
     * @brief Next manual order id.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] OrderId next_manual_order_id() noexcept {
        return manual_order_seq_.fetch_add(1, std::memory_order_relaxed)
               | (static_cast<uint64_t>(0xFF) << 32);  // high byte 0xFF = manual
    }
    /**
     * @brief Submit manual order.
     * @param o Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    bool submit_manual_order(const Order& o) noexcept;
    /**
     * @brief Cancel order.
     * @param id Parameter supplied by the caller.
     * @param instrument_id Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    bool cancel_order(OrderId id, uint16_t instrument_id) noexcept;
    /**
     * @brief Cancel quote.
     * @param id Parameter supplied by the caller.
     * @param instrument_id Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    bool cancel_quote(QuoteId id, uint16_t instrument_id) noexcept;
    /**
     * @brief Persist end of day snapshot.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    void persist_end_of_day_snapshot() noexcept;
    /**
     * @brief Persist mm params update.
     * @param product_index Parameter supplied by the caller.
     * @param params Parameter supplied by the caller.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    void persist_mm_params_update(int product_index, const MMParamsConfig& params) noexcept;
    /**
     * @brief Persist arb params update.
     * @param product_index Parameter supplied by the caller.
     * @param type Parameter supplied by the caller.
     * @param params Parameter supplied by the caller.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    void persist_arb_params_update(int product_index,
                                   ArbitrageStrategyType type,
                                   const ArbParamsConfig& params) noexcept;
    /**
     * @brief Persist risk limits update.
     * @param cfg Parameter supplied by the caller.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    void persist_risk_limits_update(const SoftRiskConfig& cfg) noexcept;

    // Allow tests to push ticks directly
    /**
     * @brief Tick buf.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] SPSCRingBuffer<TopOfBookTick, 1024>& tick_buf() noexcept {
        return tick_buf_;
    }
    // Allow tests to drain quotes
    /**
     * @brief Quote buf.
     * @param i Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] SPSCRingBuffer<Quote, 512>& quote_buf(int i) noexcept {
        return quote_buf_[i];
    }
    /**
     * @brief Order buf.
     * @param i Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] SPSCRingBuffer<Order, 512>& order_buf(int i) noexcept {
        return order_buf_[i];
    }

private:
    struct LiveOrderState {
        Order order{};
    };

    struct LiveQuoteState {
        Quote quote{};
        Volume remaining_bid{0};
        Volume remaining_ask{0};
        OrderId bid_order_id{0};
        OrderId ask_order_id{0};
    };

    SystemConfig cfg_;

    // ── Ring buffers (engine owns all) ───────────────────────────────────────
    alignas(64) SPSCRingBuffer<TopOfBookTick, 1024> tick_buf_;
    alignas(64) SPSCRingBuffer<PricingSignal,  256> signal_buf_[MAX_PRODUCTS];
    alignas(64) SPSCRingBuffer<GatewayEvent,   512> gateway_event_buf_[MAX_PRODUCTS];
    alignas(64) SPSCRingBuffer<TimerEvent,      64> timer_buf_[MAX_PRODUCTS];
    alignas(64) SPSCRingBuffer<Order,          512> order_buf_ [MAX_PRODUCTS];
    alignas(64) SPSCRingBuffer<Quote,          512> quote_buf_ [MAX_PRODUCTS];
    alignas(64) SPSCRingBuffer<Trade,          256> risk_buf_;
    alignas(64) SPSCRingBuffer<ArbIntent,      256> arb_intent_buf_[MAX_PRODUCTS];
    alignas(64) SPSCRingBuffer<GatewayEvent,   256> arb_event_buf_[MAX_PRODUCTS];
    alignas(64) SPSCRingBuffer<ArbMarketTrigger, 2048> arb_market_trigger_buf_[MAX_PRODUCTS];
    alignas(64) SPSCRingBuffer<Order,         4096> deferred_monitor_orders_;
    alignas(64) SPSCRingBuffer<Quote,         4096> deferred_monitor_quotes_;
    alignas(64) SPSCRingBuffer<Trade,         4096> deferred_monitor_trades_;
    alignas(64) SPSCRingBuffer<TopOfBookTick, 8192> deferred_monitor_ticks_;
    alignas(64) SPSCRingBuffer<OrderPersistenceEvent, 4096> deferred_persist_order_events_;
    alignas(64) SPSCRingBuffer<QuotePersistenceEvent, 2048> deferred_persist_quote_events_;
    alignas(64) SPSCRingBuffer<Trade,         4096> deferred_persist_trades_;
    alignas(64) SPSCRingBuffer<uint16_t,      2048> coalesced_signal_index_buf_[MAX_PRODUCTS];

    alignas(64) PricingSignal coalesced_signal_mailbox_[MAX_PRODUCTS][MAX_INSTRUMENTS]{};
    alignas(64) std::atomic<uint64_t> coalesced_signal_versions_[MAX_PRODUCTS][MAX_INSTRUMENTS]{};
    alignas(64) std::atomic<bool> coalesced_signal_rescan_needed_[MAX_PRODUCTS]{};

    alignas(64) TimerEvent coalesced_timer_mailbox_[MAX_PRODUCTS][2]{};
    alignas(64) std::atomic<uint64_t> coalesced_timer_versions_[MAX_PRODUCTS][2]{};

    alignas(64) std::atomic<uint64_t> coalesced_signal_writes_[MAX_PRODUCTS]{};
    alignas(64) std::atomic<uint64_t> coalesced_signal_overwrites_[MAX_PRODUCTS]{};
    alignas(64) std::atomic<uint64_t> coalesced_timer_writes_[MAX_PRODUCTS]{};
    alignas(64) std::atomic<uint64_t> coalesced_timer_overwrites_[MAX_PRODUCTS]{};

    // Single-writer statistics (no atomic needed - only pricer writes)
    alignas(64) uint64_t signal_emit_count_[MAX_PRODUCTS]{};
    alignas(64) uint64_t signal_suppressed_count_[MAX_PRODUCTS]{};
    alignas(64) uint64_t pending_future_tick_overwrites_[MAX_PRODUCTS]{};

    // Multi-writer statistics (must remain atomic)
    alignas(64) std::atomic<uint64_t> deferred_monitor_drops_{0};
    alignas(64) std::atomic<uint64_t> deferred_persistence_drops_{0};
    alignas(64) std::atomic<uint64_t> live_state_drops_{0};

    // Single-writer per-product statistics (no atomic needed - only pricer writes)
    alignas(64) uint64_t surface_versions_[MAX_PRODUCTS]{};
    alignas(64) uint32_t max_signal_queue_depth_[MAX_PRODUCTS]{};
    alignas(64) uint32_t max_signal_mailbox_depth_[MAX_PRODUCTS]{};
    alignas(64) uint32_t max_timer_queue_depth_[MAX_PRODUCTS]{};

    struct SignalEmitState {
        bool     valid{false};
        uint8_t  _pad0[7]{};
        double   theo_bid{0.0};
        double   theo_ask{0.0};
        float    delta{0.0F};
        float    vega{0.0F};
        float    underlying_bid{0.0F};
        float    underlying_ask{0.0F};
        uint64_t surface_version{0};
    };
    alignas(64) SignalEmitState last_emitted_signal_[MAX_PRODUCTS][MAX_INSTRUMENTS]{};

    // Single-writer per-instrument timestamps (no atomic needed - dedicated writer per instrument)
    alignas(64) int64_t last_signal_emit_ts_[MAX_INSTRUMENTS]{};
    alignas(64) int64_t last_strategy_signal_ts_[MAX_INSTRUMENTS]{};
    alignas(64) int64_t last_quote_ack_route_ts_[MAX_INSTRUMENTS]{};
    alignas(64) int64_t last_quote_cancel_route_ts_[MAX_INSTRUMENTS]{};
    alignas(64) int64_t last_quote_ack_route_latency_ns_[MAX_INSTRUMENTS]{};
    alignas(64) int64_t last_quote_cancel_route_latency_ns_[MAX_INSTRUMENTS]{};

    // ── Components ───────────────────────────────────────────────────────────
    std::unique_ptr<IGateway>      gateway_;
    std::unique_ptr<IFeedHandler>  feed_;
    std::unique_ptr<DataRepository> repository_;
    TradingCalendarService trading_calendar_;
    bool trading_calendar_ready_{false};

    // Strategy slots (one per product)
    std::array<std::unique_ptr<IMarketMaker>, MAX_PRODUCTS> strategies_;
    std::array<AtomicMMParams,  MAX_PRODUCTS> mm_params_;
    std::array<std::atomic<uint8_t>, MAX_PRODUCTS> product_base_offset_type_{};
    std::array<std::atomic<double>, MAX_PRODUCTS> product_base_offset_value_{};
    std::array<std::array<std::unique_ptr<IArbitrageStrategy>, MAX_ARBITRAGE_STRATEGIES_PER_PRODUCT>, MAX_PRODUCTS>
        arbitrage_strategies_;
    std::array<std::array<AtomicArbParams, MAX_ARBITRAGE_STRATEGIES_PER_PRODUCT>, MAX_PRODUCTS>
        arb_params_;
    std::array<std::array<ArbitrageStrategyType, MAX_ARBITRAGE_STRATEGIES_PER_PRODUCT>, MAX_PRODUCTS>
        arb_strategy_types_{};
    std::array<PreTradeRisk,    MAX_PRODUCTS> pre_risk_;
    PostTradeRisk                             post_risk_;

    // Vol surfaces (one per product)
    std::array<VolSurfaceManager<SVIVolSurface>,  MAX_PRODUCTS> vol_surfaces_;
    std::array<VolSurfaceManager<WingVolSurface>, MAX_PRODUCTS> wing_surfaces_;
    std::array<VolSurfaceManager<OrcWingVolSurface>, MAX_PRODUCTS> orc_wing_surfaces_;
    IdentityState identity_state_{};
    std::array<BookId, MAX_PRODUCTS> mm_book_ids_{};
    std::array<std::array<BookId, MAX_ARBITRAGE_STRATEGIES_PER_PRODUCT>, MAX_PRODUCTS> arb_book_ids_{};

    // ── Instrument registry ───────────────────────────────────────────────────
    Instrument instruments_[MAX_INSTRUMENTS]{};
    uint16_t   n_instruments_{0};
    // product routing: instrument_id → product_index
    uint8_t    instr_to_product_[MAX_INSTRUMENTS];

    // Per-product option index: built once at startup, used by pricer_loop to
    // reprice all options for a product when a future tick arrives.
    // option_ids_[p][0..option_count_[p]-1] are the option instrument ids.
    uint16_t option_ids_[MAX_PRODUCTS][MAX_INSTRUMENTS]{};
    uint16_t option_count_[MAX_PRODUCTS]{};
    // Per-product cached log(K) for each option — eliminates log() in hot pricer path
    double   option_log_K_[MAX_PRODUCTS][MAX_INSTRUMENTS]{};
    // Per-product cached T (time to expiry in years) — refreshed every second by
    // timer_loop; read by pricer_loop. Stored as plain doubles: pricer reads a
    // slightly stale value at worst (1s drift ≈ 0.001% for a 3-month option).
    // alignas(64) keeps the array on its own cache line to avoid false sharing
    // with the write side in timer_loop.
    alignas(64) double option_T_[MAX_PRODUCTS][MAX_INSTRUMENTS]{};
    // Per-product cached sqrt(T) and exp(-r*T) — refreshed every second alongside
    // option_T_. These are the two expensive transcendentals that are constant
    // across all options in a product (same expiry, same r).
    alignas(64) double option_sqrt_T_[MAX_PRODUCTS][MAX_INSTRUMENTS]{};
    alignas(64) double option_disc_[MAX_PRODUCTS][MAX_INSTRUMENTS]{};
    // Cached expiry slice indices for vol surface lookups (eliminates linear scan)
    // Precomputed once during option registration, used in hot path
    alignas(64) int8_t option_expiry_slice_[MAX_PRODUCTS][MAX_INSTRUMENTS]{};

    // Greeks snapshot — updated by pricer thread, read by gRPC server
    alignas(64) SnapshotArray<Greeks, MAX_INSTRUMENTS> greeks_snapshot_;
    // Tick snapshot — updated by pricer thread, read by vol fitter thread.
    // Written with relaxed stores (eventual consistency is fine for fitting).
    alignas(64) SnapshotArray<TopOfBookTick, MAX_INSTRUMENTS> tick_snapshot_;

    // Manual order sequence counter (gRPC server thread)
    std::atomic<uint32_t> manual_order_seq_{1};

    // Monitoring histories for gRPC/UI readers. Single-writer per topic.
    MonitoringTopic<TopOfBookTick, 8192> monitor_ticks_;
    MonitoringTopic<Order, 4096>      monitor_orders_;
    MonitoringTopic<Quote, 4096>      monitor_quotes_;
    MonitoringTopic<Trade, 4096>      monitor_trades_;
    std::array<MonitoringTopic<SystemAlert, 256>, MAX_PRODUCTS> monitor_alerts_;
    FixedHashTable<OrderId, LiveOrderState, MAX_OPEN_ORDERS * 2> live_orders_;
    FixedHashTable<QuoteId, LiveQuoteState, MAX_OPEN_ORDERS> live_quotes_;
    FixedHashTable<OrderId, QuoteId, MAX_OPEN_ORDERS * 2> quote_leg_to_quote_;
    mutable std::mutex book_state_mutex_;
    std::unordered_map<uint64_t, BookPosition> book_positions_;

    // ── Threads ───────────────────────────────────────────────────────────────
    std::atomic<bool> stop_flag_{false};
    std::atomic<bool> gateway_dispatcher_running_{false};
    std::atomic<bool> strategy_dispatch_suspended_{false};
    std::atomic<bool> cancel_all_live_requested_{false};
    std::thread feed_thread_;
    std::thread pricer_thread_;
    std::thread strategy_threads_[MAX_PRODUCTS];
    std::thread arb_threads_[MAX_PRODUCTS];
    std::thread gateway_dispatcher_thread_;
    std::thread monitor_publisher_thread_;
    std::thread vol_fitter_thread_;
    std::thread risk_monitor_thread_;
    std::thread timer_thread_;

    // ── Internal thread functions ─────────────────────────────────────────────
    /**
     * @brief Pricer loop.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    void pricer_loop() noexcept;
    /**
     * @brief Strategy loop.
     * @param product_idx Parameter supplied by the caller.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    void strategy_loop(int product_idx) noexcept;
    /**
     * @brief Arb loop.
     * @param product_idx Parameter supplied by the caller.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    void arb_loop(int product_idx) noexcept;
    /**
     * @brief Gateway dispatcher loop.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    void gateway_dispatcher_loop() noexcept;
    /**
     * @brief Monitor publish loop.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    void monitor_publish_loop() noexcept;
    /**
     * @brief Vol fitter loop.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    void vol_fitter_loop() noexcept;
    /**
     * @brief Risk monitor loop.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    void risk_monitor_loop() noexcept;
    /**
     * @brief Timer loop.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    void timer_loop() noexcept;

    // ── Startup helpers ───────────────────────────────────────────────────────
    /**
     * @brief Populate instrument registry.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    void populate_instrument_registry() noexcept;
    /**
     * @brief Init strategies.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    void init_strategies() noexcept;
    /**
     * @brief Init arbitrage strategies.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    void init_arbitrage_strategies() noexcept;
    /**
     * @brief Init vol surfaces.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    void init_vol_surfaces() noexcept;
    /**
     * @brief Init persistence.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    void init_persistence() noexcept;
    /**
     * @brief Init trading calendar.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] bool init_trading_calendar() noexcept;
    /**
     * @brief Init identity from config.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    void init_identity_from_config() noexcept;
    /**
     * @brief Apply identity state.
     * @param state Parameter supplied by the caller.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    void apply_identity_state(const IdentityState& state) noexcept;
    /**
     * @brief Persist shutdown state.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    void persist_shutdown_state() noexcept;
    // Recomputes option_T_ for all products using current monotonic time.
    // Called once at startup and then every second from timer_loop.
    /**
     * @brief Refresh option T.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    void refresh_option_T() noexcept;
    /**
     * @brief Monitoring deferred mode.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] bool monitoring_deferred_mode() const noexcept {
        return cfg_.monitoring.hot_path_publish_mode != MonitoringPublishMode::Off;
    }
    /**
     * @brief Coalesce signal.
     * @param product_idx Parameter supplied by the caller.
     * @param option_slot Parameter supplied by the caller.
     * @param sig Parameter supplied by the caller.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    void coalesce_signal(uint8_t product_idx, uint16_t option_slot,
                         const PricingSignal& sig) noexcept;
    /**
     * @brief Drain coalesced signals.
     * @param product_idx Parameter supplied by the caller.
     * @param seen_versions Parameter supplied by the caller.
     * @param budget Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    int drain_coalesced_signals(int product_idx,
                                uint64_t* seen_versions,
                                int budget) noexcept;
    /**
     * @brief Coalesce timer event.
     * @param product_idx Parameter supplied by the caller.
     * @param ev Parameter supplied by the caller.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    void coalesce_timer_event(int product_idx, const TimerEvent& ev) noexcept;
    /**
     * @brief Drain coalesced timers.
     * @param product_idx Parameter supplied by the caller.
     * @param seen_versions Parameter supplied by the caller.
     * @param budget Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    int drain_coalesced_timers(int product_idx,
                               uint64_t* seen_versions,
                               int budget) noexcept;
    /**
     * @brief Should emit signal.
     * @param product_idx Parameter supplied by the caller.
     * @param option_slot Parameter supplied by the caller.
     * @param sig Parameter supplied by the caller.
     * @param surface_version Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] bool should_emit_signal(uint8_t product_idx,
                                          uint16_t option_slot,
                                          const PricingSignal& sig,
                                          uint64_t surface_version) const noexcept;
    /**
     * @brief Note signal emitted.
     * @param product_idx Parameter supplied by the caller.
     * @param option_slot Parameter supplied by the caller.
     * @param instrument_id Parameter supplied by the caller.
     * @param sig Parameter supplied by the caller.
     * @param surface_version Parameter supplied by the caller.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    void note_signal_emitted(uint8_t product_idx,
                             uint16_t option_slot,
                             uint16_t instrument_id,
                             const PricingSignal& sig,
                             uint64_t surface_version) noexcept;
    /**
     * @brief Publish monitor tick.
     * @param tick Parameter supplied by the caller.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    void publish_monitor_tick(const TopOfBookTick& tick) noexcept;
    /**
     * @brief Publish monitor order.
     * @param order Parameter supplied by the caller.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    void publish_monitor_order(const Order& order) noexcept;
    /**
     * @brief Publish monitor quote.
     * @param quote Parameter supplied by the caller.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    void publish_monitor_quote(const Quote& quote) noexcept;
    /**
     * @brief Publish monitor trade.
     * @param trade Parameter supplied by the caller.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    void publish_monitor_trade(const Trade& trade) noexcept;
    /**
     * @brief Defer order persistence.
     * @param type Parameter supplied by the caller.
     * @param order Parameter supplied by the caller.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    void defer_order_persistence(OrderPersistenceEventType type,
                                 const Order& order) noexcept;
    /**
     * @brief Defer quote persistence.
     * @param type Parameter supplied by the caller.
     * @param quote Parameter supplied by the caller.
     * @param unused Parameter supplied by the caller.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    void defer_quote_persistence(QuotePersistenceEventType type,
                                 const Quote& quote,
                                 const void* unused = nullptr) noexcept;
    /**
     * @brief Defer trade persistence.
     * @param trade Parameter supplied by the caller.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    void defer_trade_persistence(const Trade& trade) noexcept;
    /**
     * @brief Rebuild book position locked.
     * @param trade Parameter supplied by the caller.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    void rebuild_book_position_locked(const Trade& trade) noexcept;
    /**
     * @brief Track live order submit.
     * @param order Parameter supplied by the caller.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    void track_live_order_submit(const Order& order) noexcept;
    /**
     * @brief Track live quote submit.
     * @param quote Parameter supplied by the caller.
     * @param bid_order_id Parameter supplied by the caller.
     * @param ask_order_id Parameter supplied by the caller.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    void track_live_quote_submit(const Quote& quote,
                                 OrderId bid_order_id,
                                 OrderId ask_order_id) noexcept;
    /**
     * @brief Handle gateway order update.
     * @param order Parameter supplied by the caller.
     * @param type Parameter supplied by the caller.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    void handle_gateway_order_update(Order& order,
                                     GatewayEventType type) noexcept;
    /**
     * @brief Handle gateway quote update.
     * @param quote Parameter supplied by the caller.
     * @param type Parameter supplied by the caller.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    void handle_gateway_quote_update(Quote& quote,
                                     GatewayEventType type) noexcept;
    /**
     * @brief Handle gateway fill.
     * @param trade Parameter supplied by the caller.
     * @param type Parameter supplied by the caller.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    void handle_gateway_fill(Trade* trade, GatewayEventType* type) noexcept;
    /**
     * @brief Request cancel all live orders and quotes.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    void request_cancel_all_live_orders_and_quotes() noexcept;
    /**
     * @brief Cancel all live orders and quotes.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    void cancel_all_live_orders_and_quotes() noexcept;
    /**
     * @brief Arb book id for type.
     * @param product_idx Parameter supplied by the caller.
     * @param type Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] BookId arb_book_id_for_type(int product_idx,
                                              ArbitrageStrategyType type) const noexcept;
    /**
     * @brief Book position key.
     * @param book_id Parameter supplied by the caller.
     * @param instrument_id Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] static uint64_t book_position_key(BookId book_id,
                                                    uint16_t instrument_id) noexcept;
    /**
     * @brief Find arbitrage slot.
     * @param product_idx Parameter supplied by the caller.
     * @param type Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] int find_arbitrage_slot(int product_idx,
                                          ArbitrageStrategyType type) const noexcept;
};

} // namespace omm
