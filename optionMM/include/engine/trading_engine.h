#pragma once

#include "common/types.h"
#include "common/config.h"
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
#include "strategy/mm_framework.h"
#include "strategy/mm_params.h"
#include "risk/pre_trade_risk.h"
#include "risk/post_trade_risk.h"
#include "monitoring/topic.h"

#include <thread>
#include <atomic>
#include <memory>
#include <array>

namespace omm {

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
public:
    explicit TradingEngine(const SystemConfig& cfg,
                           std::unique_ptr<IGateway> gateway,
                           std::unique_ptr<IFeedHandler> feed);
    ~TradingEngine();

    // Non-copyable
    TradingEngine(const TradingEngine&)            = delete;
    TradingEngine& operator=(const TradingEngine&) = delete;

    // Start all threads. Blocks until stop() is called.
    void start();
    void stop() noexcept;

    // Access for testing / gRPC server
    [[nodiscard]] IGateway*   gateway()    const noexcept { return gateway_.get(); }
    [[nodiscard]] AtomicMMParams& mm_params(int i) noexcept { return mm_params_[i]; }
    [[nodiscard]] const PostTradeRisk& post_risk() const noexcept { return post_risk_; }
    [[nodiscard]] PostTradeRisk& post_risk_mutable() noexcept { return post_risk_; }
    [[nodiscard]] int product_count() const noexcept { return cfg_.product_count; }
    [[nodiscard]] int n_instruments() const noexcept { return n_instruments_; }
    [[nodiscard]] const Instrument* instruments() const noexcept { return instruments_; }
    [[nodiscard]] const MarketTick* tick_snapshot() const noexcept { return tick_snapshot_; }
    [[nodiscard]] const VolSurfaceManager<OrcWingVolSurface>& orc_wing_surface(int i) const noexcept {
        return orc_wing_surfaces_[i];
    }
    [[nodiscard]] const VolSurfaceManager<WingVolSurface>& wing_surface(int i) const noexcept {
        return wing_surfaces_[i];
    }
    [[nodiscard]] const VolSurfaceManager<SVIVolSurface>& svi_surface(int i) const noexcept {
        return vol_surfaces_[i];
    }
    [[nodiscard]] uint16_t option_count(int i) const noexcept { return option_count_[i]; }
    [[nodiscard]] uint16_t option_id(int product_idx, uint16_t option_idx) const noexcept {
        return option_ids_[product_idx][option_idx];
    }

    // Greeks snapshot – last computed Greeks per instrument (written by pricer thread)
    [[nodiscard]] const Greeks* greeks_snapshot() const noexcept { return greeks_snapshot_; }
    [[nodiscard]] const MonitoringTopic<MarketTick, 8192>& monitor_ticks() const noexcept {
        return monitor_ticks_;
    }
    [[nodiscard]] const MonitoringTopic<Order, 4096>& monitor_orders() const noexcept {
        return monitor_orders_;
    }
    [[nodiscard]] const MonitoringTopic<Quote, 4096>& monitor_quotes() const noexcept {
        return monitor_quotes_;
    }
    [[nodiscard]] const MonitoringTopic<Trade, 4096>& monitor_trades() const noexcept {
        return monitor_trades_;
    }

    // Manual order submission from gRPC (bypasses strategy, goes direct to gateway dispatcher)
    [[nodiscard]] OrderId next_manual_order_id() noexcept {
        return manual_order_seq_.fetch_add(1, std::memory_order_relaxed)
               | (static_cast<uint64_t>(0xFF) << 32);  // high byte 0xFF = manual
    }
    bool submit_manual_order(const Order& o) noexcept;
    bool cancel_order(OrderId id, uint16_t instrument_id) noexcept;

    // Allow tests to push ticks directly
    [[nodiscard]] SPSCRingBuffer<MarketTick, 1024>& tick_buf() noexcept {
        return tick_buf_;
    }
    // Allow tests to drain quotes
    [[nodiscard]] SPSCRingBuffer<Quote, 512>& quote_buf(int i) noexcept {
        return quote_buf_[i];
    }
    [[nodiscard]] SPSCRingBuffer<Order, 512>& order_buf(int i) noexcept {
        return order_buf_[i];
    }

private:
    SystemConfig cfg_;

    // ── Ring buffers (engine owns all) ───────────────────────────────────────
    alignas(64) SPSCRingBuffer<MarketTick,    1024> tick_buf_;
    alignas(64) SPSCRingBuffer<PricingSignal,  256> signal_buf_[MAX_PRODUCTS];
    alignas(64) SPSCRingBuffer<Order,          512> order_buf_ [MAX_PRODUCTS];
    alignas(64) SPSCRingBuffer<Quote,          512> quote_buf_ [MAX_PRODUCTS];
    alignas(64) SPSCRingBuffer<Trade,          256> risk_buf_;

    // ── Components ───────────────────────────────────────────────────────────
    std::unique_ptr<IGateway>      gateway_;
    std::unique_ptr<IFeedHandler>  feed_;

    // Strategy slots (one per product)
    std::array<std::unique_ptr<IMarketMaker>, MAX_PRODUCTS> strategies_;
    std::array<AtomicMMParams,  MAX_PRODUCTS> mm_params_;
    std::array<PreTradeRisk,    MAX_PRODUCTS> pre_risk_;
    PostTradeRisk                             post_risk_;

    // Vol surfaces (one per product)
    std::array<VolSurfaceManager<SVIVolSurface>,  MAX_PRODUCTS> vol_surfaces_;
    std::array<VolSurfaceManager<WingVolSurface>, MAX_PRODUCTS> wing_surfaces_;
    std::array<VolSurfaceManager<OrcWingVolSurface>, MAX_PRODUCTS> orc_wing_surfaces_;

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

    // Greeks snapshot — updated by pricer thread, read by gRPC server
    alignas(64) Greeks     greeks_snapshot_[MAX_INSTRUMENTS]{};
    // Tick snapshot — updated by pricer thread, read by vol fitter thread.
    // Written with relaxed stores (eventual consistency is fine for fitting).
    alignas(64) MarketTick tick_snapshot_[MAX_INSTRUMENTS]{};

    // Manual order sequence counter (gRPC server thread)
    std::atomic<uint32_t> manual_order_seq_{1};

    // Monitoring histories for gRPC/UI readers. Single-writer per topic.
    MonitoringTopic<MarketTick, 8192> monitor_ticks_;
    MonitoringTopic<Order, 4096>      monitor_orders_;
    MonitoringTopic<Quote, 4096>      monitor_quotes_;
    MonitoringTopic<Trade, 4096>      monitor_trades_;

    // ── Threads ───────────────────────────────────────────────────────────────
    std::atomic<bool> stop_flag_{false};
    std::thread feed_thread_;
    std::thread pricer_thread_;
    std::thread strategy_threads_[MAX_PRODUCTS];
    std::thread gateway_dispatcher_thread_;
    std::thread vol_fitter_thread_;
    std::thread risk_monitor_thread_;
    std::thread timer_thread_;

    // ── Internal thread functions ─────────────────────────────────────────────
    void pricer_loop() noexcept;
    void strategy_loop(int product_idx) noexcept;
    void gateway_dispatcher_loop() noexcept;
    void vol_fitter_loop() noexcept;
    void risk_monitor_loop() noexcept;
    void timer_loop() noexcept;

    // ── Startup helpers ───────────────────────────────────────────────────────
    void populate_instrument_registry() noexcept;
    void init_strategies() noexcept;
    void init_vol_surfaces() noexcept;
    // Recomputes option_T_ for all products using current monotonic time.
    // Called once at startup and then every second from timer_loop.
    void refresh_option_T() noexcept;
};

} // namespace omm
