#include "engine/trading_engine.h"
#include "strategy/simple_mm.h"
#include "feed/multicast_feed.h"
#include "feed/fpga_feed.h"
#include "feed/femas_feed.h"
#include "common/thread_utils.h"
#include "logger/logger.h"
#include "pricing/black76.h"

#include <cstring>
#include <cmath>
#include <algorithm>

namespace omm {

// ─── Construction / destruction ───────────────────────────────────────────────

TradingEngine::TradingEngine(const SystemConfig& cfg,
                              std::unique_ptr<IGateway>     gateway,
                              std::unique_ptr<IFeedHandler> feed)
    : cfg_(cfg)
    , gateway_(std::move(gateway))
    , feed_(std::move(feed))
    , pre_risk_{
        PreTradeRisk(cfg.risk.hard), PreTradeRisk(cfg.risk.hard),
        PreTradeRisk(cfg.risk.hard), PreTradeRisk(cfg.risk.hard),
        PreTradeRisk(cfg.risk.hard), PreTradeRisk(cfg.risk.hard),
        PreTradeRisk(cfg.risk.hard), PreTradeRisk(cfg.risk.hard),
        PreTradeRisk(cfg.risk.hard), PreTradeRisk(cfg.risk.hard),
        PreTradeRisk(cfg.risk.hard), PreTradeRisk(cfg.risk.hard),
        PreTradeRisk(cfg.risk.hard), PreTradeRisk(cfg.risk.hard),
        PreTradeRisk(cfg.risk.hard), PreTradeRisk(cfg.risk.hard),
        PreTradeRisk(cfg.risk.hard), PreTradeRisk(cfg.risk.hard),
        PreTradeRisk(cfg.risk.hard), PreTradeRisk(cfg.risk.hard),
        PreTradeRisk(cfg.risk.hard), PreTradeRisk(cfg.risk.hard),
        PreTradeRisk(cfg.risk.hard), PreTradeRisk(cfg.risk.hard),
        PreTradeRisk(cfg.risk.hard), PreTradeRisk(cfg.risk.hard),
        PreTradeRisk(cfg.risk.hard), PreTradeRisk(cfg.risk.hard),
        PreTradeRisk(cfg.risk.hard), PreTradeRisk(cfg.risk.hard),
        PreTradeRisk(cfg.risk.hard), PreTradeRisk(cfg.risk.hard),
      }
    , post_risk_(cfg.risk.soft)
{
    std::memset(instr_to_product_, 0xFF, sizeof(instr_to_product_));
    // Apply initial MM params from config
    for (int i = 0; i < cfg_.product_count && i < MAX_PRODUCTS; ++i)
        mm_params_[i].apply(cfg_.products[i].params);
    // Wire the feed's tick_buf pointer now that tick_buf_ is constructed
    if (feed_) feed_->set_tick_buf(&tick_buf_);
}

TradingEngine::~TradingEngine() { stop(); }

// ─── Startup ──────────────────────────────────────────────────────────────────

void TradingEngine::populate_instrument_registry() noexcept {
    // Query instruments from gateway (blocking at startup)
    gateway_->query_instruments(instruments_, &n_instruments_, MAX_INSTRUMENTS);

    // Build instr_to_product_ mapping and per-product option index
    std::memset(option_count_, 0, sizeof(option_count_));

    for (uint16_t i = 0; i < n_instruments_; ++i) {
        instruments_[i].instrument_id = i;
        instruments_[i].product_index = 0xFF;
        for (int p = 0; p < cfg_.product_count; ++p) {
            if (instruments_[i].code == cfg_.products[p].underlying_id ||
                instruments_[i].underlying_code == cfg_.products[p].underlying_id) {
                instruments_[i].product_index = static_cast<uint8_t>(p);
                instr_to_product_[i] = static_cast<uint8_t>(p);
                break;
            }
        }
    }

    for (uint16_t i = 0; i < n_instruments_; ++i) {
        if (instruments_[i].kind != InstrumentKind::Option) continue;
        for (uint16_t u = 0; u < n_instruments_; ++u) {
            if (instruments_[u].kind != InstrumentKind::Future) continue;
            if (!(instruments_[u].code == instruments_[i].underlying_code)) continue;
            instruments_[i].underlying_id = u;
            break;
        }

        // Index options per product for fast batch repricing on future ticks
        const Instrument& instr = instruments_[i];
        if (instr.kind == InstrumentKind::Option) {
            uint8_t p = instr.product_index;
            if (p < MAX_PRODUCTS && option_count_[p] < MAX_INSTRUMENTS) {
                option_ids_[p][option_count_[p]] = i;
                // Cache log(K) so pricer_loop avoids log() per option per tick
                option_log_K_[p][option_count_[p]] = std::log(instr.strike);
                option_count_[p]++;
            }
        }
    }

    // Wire instrument registry to feed handler
    if (feed_)
        feed_->set_instruments(instruments_, n_instruments_);

    // Seed option_T_ so pricer_loop has valid values before timer fires
    refresh_option_T();
}

void TradingEngine::refresh_option_T() noexcept {
    const int64_t now_ns = get_monotonic_ns();
    static constexpr double NS_PER_YEAR = 365.0 * 24.0 * 3600.0 * 1e9;
    const double r = cfg_.pricing.risk_free_rate;
    for (int p = 0; p < cfg_.product_count && p < MAX_PRODUCTS; ++p) {
        for (uint16_t oi = 0; oi < option_count_[p]; ++oi) {
            const Instrument& opt = instruments_[option_ids_[p][oi]];
            double T = (opt.expiry_epoch_ns - now_ns) / NS_PER_YEAR;
            if (T < 1e-4) T = 1e-4;
            option_T_[p][oi]      = T;
            option_sqrt_T_[p][oi] = std::sqrt(T);
            option_disc_[p][oi]   = std::exp(-r * T);
        }
    }
}

void TradingEngine::init_strategies() noexcept {
    for (int i = 0; i < cfg_.product_count && i < MAX_PRODUCTS; ++i) {
        auto* s = new SimpleMMStrategy();
        s->init(static_cast<uint8_t>(i),
                &quote_buf_[i],
                &order_buf_[i],
                &pre_risk_[i],
                &mm_params_[i],
                instruments_);
        strategies_[i].reset(s);
    }
}

void TradingEngine::init_vol_surfaces() noexcept {
    for (int i = 0; i < cfg_.product_count && i < MAX_PRODUCTS; ++i) {
        // Initialise both ping-pong buffers with a flat default surface
        for (int pass = 0; pass < 2; ++pass) {
            SVIVolSurface* s = vol_surfaces_[i].get_inactive();
            s->n_slices = 1;
            s->slices[0].a       = cfg_.pricing.risk_free_rate * 0.04;
            s->slices[0].b       = 0.05;
            s->slices[0].rho     = -0.2;
            s->slices[0].m       = 0.0;
            s->slices[0].sigma   = 0.1;
            s->slices[0].expiry_T = 0.25;
            s->slices[0].valid   = true;
            vol_surfaces_[i].publish();

            WingVolSurface* w = wing_surfaces_[i].get_inactive();
            w->n_slices = 1;
            w->slices[0].ATM_vol    = 0.20;
            w->slices[0].slope_call = -0.1;
            w->slices[0].slope_put  =  0.1;
            w->slices[0].curve_call =  0.05;
            w->slices[0].curve_put  =  0.05;
            w->slices[0].expiry_T   =  0.25;
            w->slices[0].valid      =  true;
            wing_surfaces_[i].publish();

            OrcWingVolSurface* ow = orc_wing_surfaces_[i].get_inactive();
            ow->n_slices = 1;
            ow->slices[0].ref_price      = 100.0;
            ow->slices[0].atm_forward    = 100.0;
            ow->slices[0].ssr            = 1.0;
            ow->slices[0].vol_ref        = 0.20;
            ow->slices[0].slope_ref      = -0.1;
            ow->slices[0].vcr            = 0.0;
            ow->slices[0].scr            = 0.0;
            ow->slices[0].put_curv       = 0.05;
            ow->slices[0].call_curv      = 0.05;
            ow->slices[0].down_cutoff    = -0.15;
            ow->slices[0].up_cutoff      = 0.15;
            ow->slices[0].down_smoothing = 0.5;
            ow->slices[0].up_smoothing   = 0.5;
            ow->slices[0].expiry_T       = 0.25;
            ow->slices[0].valid          = true;
            orc_wing_surfaces_[i].publish();
        }
    }
}

void TradingEngine::start() {
    setup_fp_environment();
    populate_instrument_registry();
    init_strategies();
    init_vol_surfaces();

    stop_flag_.store(false, std::memory_order_relaxed);

    OMM_LOG_INFO("startup", "TradingEngine starting: {} products, {} instruments",
                 cfg_.product_count, n_instruments_);

    // Spawn threads in order: gateway dispatcher first so it is ready
    // to handle acks before any orders are sent.
    gateway_dispatcher_thread_ = std::thread([this] { gateway_dispatcher_loop(); });
    pricer_thread_              = std::thread([this] { pricer_loop(); });

    for (int i = 0; i < cfg_.product_count && i < MAX_PRODUCTS; ++i) {
        strategy_threads_[i] = std::thread([this, i] { strategy_loop(i); });
    }

    vol_fitter_thread_    = std::thread([this] { vol_fitter_loop(); });
    risk_monitor_thread_  = std::thread([this] { risk_monitor_loop(); });
    timer_thread_         = std::thread([this] { timer_loop(); });

    if (feed_) feed_->start();
}

void TradingEngine::stop() noexcept {
    OMM_LOG_INFO("shutdown", "TradingEngine stopping");
    stop_flag_.store(true, std::memory_order_release);

    if (feed_) feed_->stop();

    auto join = [](std::thread& t) { if (t.joinable()) t.join(); };
    join(pricer_thread_);
    for (auto& t : strategy_threads_) join(t);
    join(gateway_dispatcher_thread_);
    join(vol_fitter_thread_);
    join(risk_monitor_thread_);
    join(timer_thread_);

    if (gateway_) gateway_->disconnect();
}

// ─── Pricer thread ────────────────────────────────────────────────────────────
// Trigger model: a FUTURE tick drives repricing of ALL options for that product.
//   - Future tick arrives → update tick_snapshot_ for the future → for each
//     option in the product, compute Black-76 using the future price as F and
//     the vol surface for sigma → emit one PricingSignal per option.
//   - Option tick arrives → update tick_snapshot_ only (for the vol fitter to
//     read mid-prices for IV inversion). No PricingSignal emitted.
//
// This is the correct market-making model: the forward price drives repricing,
// not individual option prints. A single future tick reprices the whole book.

void TradingEngine::pricer_loop() noexcept {
    set_thread_name("omm-pricer");

    MarketTick tick{};
    while (!stop_flag_.load(std::memory_order_relaxed)) {
        if (!tick_buf_.try_pop(tick)) { spin_pause(); continue; }

        const uint16_t id = tick.instrument_id;
        if (id >= MAX_INSTRUMENTS) continue;

        const Instrument& instr = instruments_[id];
        if (instr.instrument_id == INVALID_INSTRUMENT_ID) continue;

        // Always update tick snapshot (used by vol fitter for IV inversion)
        tick_snapshot_[id] = tick;

        // Only future ticks trigger a full option repricing pass
        if (instr.kind != InstrumentKind::Future) continue;

        const uint8_t prod  = instr_to_product_[id];
        if (prod >= MAX_PRODUCTS) continue;
        const double  F     = tick.last_price;
        if (F < 1e-10) continue;  // no valid forward yet

        const IVolSurface* surf = nullptr;
        if (cfg_.pricing.vol_method == VolMethod::Wing) {
            surf = wing_surfaces_[prod].get();
        } else if (cfg_.pricing.vol_method == VolMethod::OrcWing) {
            surf = orc_wing_surfaces_[prod].get();
        } else {
            surf = vol_surfaces_[prod].get();
        }
        const int64_t      now  = get_monotonic_ns();
        const uint16_t     n    = option_count_[prod];
        if (n == 0) continue;

        // ── Batch Black-76 computation (AVX2 SIMD) ───────────────────────────
        // Allocate aligned stack arrays for batch pricing (max 128 options/product)
        constexpr uint16_t MAX_BATCH = 128;
        const uint16_t batch_n = (n < MAX_BATCH) ? n : MAX_BATCH;

        alignas(32) double     F_arr[MAX_BATCH];
        alignas(32) double     K_arr[MAX_BATCH];
        alignas(32) double     T_arr[MAX_BATCH];
        alignas(32) double     sqrt_T_arr[MAX_BATCH];
        alignas(32) double     disc_arr[MAX_BATCH];
        alignas(32) double     sigma_arr[MAX_BATCH];
        alignas(32) uint8_t    is_call_arr[MAX_BATCH];
        alignas(32) Black76Result results[MAX_BATCH];
        alignas(64) PricingSignal sigs[MAX_BATCH];

        // Pass 1: populate input arrays
        const double log_F = std::log(F);  // computed once per future tick

        for (uint16_t oi = 0; oi < batch_n; ++oi) {
            const uint16_t    opt_id = option_ids_[prod][oi];
            const Instrument& opt    = instruments_[opt_id];

            F_arr[oi]        = F;
            K_arr[oi]        = opt.strike;
            T_arr[oi]        = option_T_[prod][oi];        // pre-computed, refreshed every 1s
            sqrt_T_arr[oi]   = option_sqrt_T_[prod][oi];   // pre-computed, refreshed every 1s
            disc_arr[oi]     = option_disc_[prod][oi];     // pre-computed, refreshed every 1s
            sigma_arr[oi]    = (cfg_.pricing.vol_method == VolMethod::OrcWing)
                ? surf->get_vol_by_strike(F, opt.strike, T_arr[oi])
                : surf->get_vol(option_log_K_[prod][oi] - log_F, T_arr[oi]);
            is_call_arr[oi]  = (opt.option_type == OptionType::Call) ? 1 : 0;
        }

        // Batch Black-76 computation using pre-computed sqrt(T) and disc
        compute_batch_precomputed(F_arr, K_arr, T_arr, sqrt_T_arr, disc_arr,
                                  sigma_arr, is_call_arr, results, batch_n);

        // Pass 2: build PricingSignals (collect all, then single batch push)
        for (uint16_t oi = 0; oi < batch_n; ++oi) {
            const uint16_t     opt_id = option_ids_[prod][oi];
            const Black76Result& res  = results[oi];

            PricingSignal& sig = sigs[oi];
            sig.greeks.instrument_id = opt_id;
            sig.greeks.theo_price    = res.price;
            sig.greeks.delta         = res.delta;
            sig.greeks.gamma         = res.gamma;
            sig.greeks.vega          = res.vega;
            sig.greeks.theta         = res.theta;
            sig.greeks.rho           = res.rho;
            sig.greeks.iv            = sigma_arr[oi];
            sig.greeks.T             = T_arr[oi];
            sig.greeks.calc_ts_ns    = now;
            sig.trigger_tick         = tick;  // carry the triggering future tick

            // Update snapshots for gRPC / risk monitor
            greeks_snapshot_[opt_id] = sig.greeks;
        }
        // Single release-store publishes all N signals atomically
        (void)signal_buf_[prod].try_push_batch(sigs, batch_n);
    }
}

// ─── Strategy thread ──────────────────────────────────────────────────────────

void TradingEngine::strategy_loop(int idx) noexcept {
    set_thread_name("omm-strat");

    PricingSignal sig{};

    while (!stop_flag_.load(std::memory_order_relaxed)) {
        if (signal_buf_[idx].try_pop(sig))
            strategies_[idx]->on_signal(sig);
        // Timer events could be routed here if a timer_buf per product is added
        spin_pause();
    }
}

// ─── Gateway dispatcher thread ────────────────────────────────────────────────

void TradingEngine::gateway_dispatcher_loop() noexcept {
    set_thread_name("omm-gw-disp");

    while (!stop_flag_.load(std::memory_order_relaxed)) {
        // Round-robin over all strategy output buffers
        for (int i = 0; i < cfg_.product_count && i < MAX_PRODUCTS; ++i) {
            Order order{};
            if (order_buf_[i].try_pop(order))
                gateway_->send_order(order);

            Quote quote{};
            if (quote_buf_[i].try_pop(quote))
                gateway_->send_quote(quote);
        }

        // Drain gateway callbacks and route to strategy threads
        GatewayEvent ev{};
        while (gateway_->callback_buf.try_pop(ev)) {
            int p = ev.product_index;
            if (p >= MAX_PRODUCTS || !strategies_[p]) continue;

            switch (ev.type) {
            case GatewayEventType::OrderAck:
            case GatewayEventType::QuoteAck:
                strategies_[p]->on_order_ack(ev.order);
                break;
            case GatewayEventType::OrderFill:
            case GatewayEventType::QuoteFill:
                strategies_[p]->on_fill(ev.trade);
                (void)risk_buf_.try_push(ev.trade);  // forward to risk monitor
                OMM_LOG_INFO("fill", "instr={} side={} qty={} price={:.4f} order_id={}",
                             ev.trade.instrument_id,
                             ev.trade.side == Side::Buy ? "buy" : "sell",
                             ev.trade.fill_volume,
                             ev.trade.fill_price,
                             ev.trade.client_order_id);
                break;
            case GatewayEventType::OrderCancel:
                strategies_[p]->on_order_cancel(ev.order.client_order_id);
                break;
            default:
                break;
            }
        }
        spin_pause();
    }
}

// ─── Vol fitter thread ────────────────────────────────────────────────────────
// Runs every fit_interval_seconds. For each product:
//   1. Collect option instruments grouped by expiry.
//   2. For each expiry slice, read the latest tick snapshot to get market mid-prices.
//   3. Invert Black-76 to get market implied vols.
//   4. Call fit_svi_slice() to fit SVI parameters.
//   5. Publish the new surface via VolSurfaceManager atomic swap.
//
// Needs >= 5 valid option quotes per expiry to attempt a fit; otherwise keeps
// the existing surface unchanged.

void TradingEngine::vol_fitter_loop() noexcept {
    set_thread_name("omm-volfitter");

    // Working buffers (stack-allocated, reused each iteration)
    double  strikes[MAX_STRIKES];
    double  mkt_vols[MAX_STRIKES];
    double  fwd_prices[MAX_EXPIRIES];
    double  expiry_Ts[MAX_EXPIRIES];
    int     expiry_counts[MAX_EXPIRIES];
    // Per-expiry strike/vol accumulation (indices into strikes[]/mkt_vols[])
    int     expiry_start[MAX_EXPIRIES];

    while (!stop_flag_.load(std::memory_order_relaxed)) {
        // Sleep in 100ms chunks so we can respond to stop quickly
        for (int slept = 0;
             slept < cfg_.pricing.fit_interval_seconds * 1000;
             slept += 100) {
            if (stop_flag_.load(std::memory_order_relaxed)) return;
            struct timespec ts{0, 100'000'000};
            nanosleep(&ts, nullptr);
        }

        const double r = cfg_.pricing.risk_free_rate;

        for (int p = 0; p < cfg_.product_count && p < MAX_PRODUCTS; ++p) {
            // ── Pass 1: discover expiry slices and collect forward prices ──────
            int n_expiries = 0;
            std::memset(expiry_counts, 0, sizeof(expiry_counts));
            std::memset(fwd_prices,    0, sizeof(fwd_prices));
            std::memset(expiry_Ts,     0, sizeof(expiry_Ts));

            for (uint16_t id = 0; id < n_instruments_; ++id) {
                const Instrument& instr = instruments_[id];
                if (instr.instrument_id == INVALID_INSTRUMENT_ID) continue;
                if (instr.product_index != static_cast<uint8_t>(p)) continue;

                if (instr.kind == InstrumentKind::Future) {
                    // Use this future's last price as forward for all expiries
                    // (in production, match by expiry; here use a single forward)
                    const MarketTick& t = tick_snapshot_[id];
                    if (t.last_price > 1e-10) {
                        for (int e = 0; e < MAX_EXPIRIES; ++e)
                            if (fwd_prices[e] < 1e-10) fwd_prices[e] = t.last_price;
                    }
                    continue;
                }

                if (instr.kind != InstrumentKind::Option) continue;

                const MarketTick& t = tick_snapshot_[id];
                if (t.recv_ts_ns == 0) continue;  // no tick yet

                // Compute T for this instrument
                double T = (instr.expiry_epoch_ns - t.recv_ts_ns)
                           / (365.0 * 24.0 * 3600.0 * 1e9);
                if (T < 1.0 / 365.0) continue;  // skip expiries < 1 day out

                // Find or create expiry bucket (match within 1 day = 1/365 years)
                int ei = -1;
                for (int e = 0; e < n_expiries; ++e) {
                    if (std::fabs(expiry_Ts[e] - T) < 1.0 / 365.0) { ei = e; break; }
                }
                if (ei < 0) {
                    if (n_expiries >= MAX_EXPIRIES) continue;
                    ei = n_expiries++;
                    expiry_Ts[ei] = T;
                    // Forward price for this expiry: use underlying's last tick
                    if (instr.underlying_id < n_instruments_) {
                        const MarketTick& ut = tick_snapshot_[instr.underlying_id];
                        if (ut.last_price > 1e-10) fwd_prices[ei] = ut.last_price;
                    }
                }
                expiry_counts[ei]++;
            }

            if (n_expiries == 0) continue;

            // Sort expiry slices by T ascending
            // Simple insertion sort (n_expiries <= MAX_EXPIRIES = 24)
            for (int i = 1; i < n_expiries; ++i) {
                double tT = expiry_Ts[i]; double tF = fwd_prices[i]; int tc = expiry_counts[i];
                int j = i - 1;
                while (j >= 0 && expiry_Ts[j] > tT) {
                    expiry_Ts[j+1] = expiry_Ts[j]; fwd_prices[j+1] = fwd_prices[j];
                    expiry_counts[j+1] = expiry_counts[j]; --j;
                }
                expiry_Ts[j+1] = tT; fwd_prices[j+1] = tF; expiry_counts[j+1] = tc;
            }

            // Compute start indices into the flat strikes[]/mkt_vols[] arrays
            expiry_start[0] = 0;
            for (int e = 1; e < n_expiries; ++e)
                expiry_start[e] = expiry_start[e-1] + expiry_counts[e-1];

            // Reset counts for pass 2
            std::memset(expiry_counts, 0, sizeof(expiry_counts));

            // ── Pass 2: collect (strike, market_iv) pairs per expiry ──────────
            for (uint16_t id = 0; id < n_instruments_; ++id) {
                const Instrument& instr = instruments_[id];
                if (instr.instrument_id == INVALID_INSTRUMENT_ID) continue;
                if (instr.product_index != static_cast<uint8_t>(p)) continue;
                if (instr.kind != InstrumentKind::Option) continue;

                const MarketTick& t = tick_snapshot_[id];
                if (t.recv_ts_ns == 0) continue;

                double T = (instr.expiry_epoch_ns - t.recv_ts_ns)
                           / (365.0 * 24.0 * 3600.0 * 1e9);
                if (T < 1.0 / 365.0) continue;

                // Find expiry bucket
                int ei = -1;
                for (int e = 0; e < n_expiries; ++e) {
                    if (std::fabs(expiry_Ts[e] - T) < 1.0 / 365.0) { ei = e; break; }
                }
                if (ei < 0) continue;

                double F = fwd_prices[ei];
                if (F < 1e-10) continue;

                // Mid-price from best bid/ask
                double bid = t.bid_price[0], ask = t.ask_price[0];
                if (bid <= 0.0 || ask <= 0.0 || ask < bid) continue;
                double mid = 0.5 * (bid + ask);

                bool is_call = (instr.option_type == OptionType::Call);
                double iv = implied_vol(mid, F, instr.strike, T, r, is_call);
                if (iv < 0.01 || iv > 3.0) continue;  // reject implausible vols

                int slot = expiry_start[ei] + expiry_counts[ei];
                if (slot >= MAX_STRIKES * MAX_EXPIRIES) continue;
                strikes[slot]  = instr.strike;
                mkt_vols[slot] = iv;
                expiry_counts[ei]++;
            }

            // ── Pass 3: fit per expiry, build new surface ─────────────────────
            if (cfg_.pricing.vol_method == VolMethod::Wing) {
                WingVolSurface* surf = wing_surfaces_[p].get_inactive();
                surf->n_slices = 0;

                for (int e = 0; e < n_expiries; ++e) {
                    int cnt = expiry_counts[e];
                    if (cnt < 5) {
                        const WingVolSurface* cur =
                            static_cast<const WingVolSurface*>(wing_surfaces_[p].get());
                        for (int s = 0; s < cur->n_slices; ++s) {
                            if (std::fabs(cur->slices[s].expiry_T - expiry_Ts[e]) < 1.0/365.0) {
                                surf->slices[surf->n_slices++] = cur->slices[s];
                                break;
                            }
                        }
                        continue;
                    }

                    WingParams params{};
                    params.expiry_T = expiry_Ts[e];
                    bool ok = fit_wing_slice(
                        strikes  + expiry_start[e],
                        mkt_vols + expiry_start[e],
                        cnt, fwd_prices[e], expiry_Ts[e], params);

                    if (ok) {
                        surf->slices[surf->n_slices++] = params;
                        OMM_LOG_INFO("volfitter",
                            "product={} expiry_T={:.4f} n={} ATM={:.4f} sc={:.4f} sp={:.4f} cc={:.4f} cp={:.4f}",
                            p, expiry_Ts[e], cnt,
                            params.ATM_vol, params.slope_call, params.slope_put,
                            params.curve_call, params.curve_put);
                    } else {
                        OMM_LOG_WARN("volfitter",
                            "product={} expiry_T={:.4f} wing fit failed (n={})", p, expiry_Ts[e], cnt);
                    }
                }

                if (surf->n_slices > 0)
                    wing_surfaces_[p].publish();
            } else if (cfg_.pricing.vol_method == VolMethod::OrcWing) {
                OrcWingVolSurface* surf = orc_wing_surfaces_[p].get_inactive();
                surf->n_slices = 0;

                for (int e = 0; e < n_expiries; ++e) {
                    int cnt = expiry_counts[e];
                    if (cnt < 8) {
                        const OrcWingVolSurface* cur =
                            static_cast<const OrcWingVolSurface*>(orc_wing_surfaces_[p].get());
                        for (int s = 0; s < cur->n_slices; ++s) {
                            if (std::fabs(cur->slices[s].expiry_T - expiry_Ts[e]) < 1.0 / 365.0) {
                                surf->slices[surf->n_slices++] = cur->slices[s];
                                break;
                            }
                        }
                        continue;
                    }

                    OrcWingParams params{};
                    params.ref_price = fwd_prices[e];
                    params.atm_forward = fwd_prices[e];
                    params.expiry_T = expiry_Ts[e];
                    const bool ok = fit_orc_wing_slice(
                        strikes + expiry_start[e],
                        mkt_vols + expiry_start[e],
                        cnt, fwd_prices[e], expiry_Ts[e], params);

                    if (ok) {
                        surf->slices[surf->n_slices++] = params;
                        OMM_LOG_INFO("volfitter",
                            "product={} expiry_T={:.4f} n={} vr={:.4f} sr={:.4f} pc={:.4f} cc={:.4f} dc={:.4f} uc={:.4f} dsm={:.4f} usm={:.4f}",
                            p, expiry_Ts[e], cnt,
                            params.vol_ref, params.slope_ref, params.put_curv, params.call_curv,
                            params.down_cutoff, params.up_cutoff, params.down_smoothing, params.up_smoothing);
                    } else {
                        OMM_LOG_WARN("volfitter",
                            "product={} expiry_T={:.4f} orcWing fit failed (n={})", p, expiry_Ts[e], cnt);
                    }
                }

                if (surf->n_slices > 0)
                    orc_wing_surfaces_[p].publish();
            } else {
                SVIVolSurface* surf = vol_surfaces_[p].get_inactive();
                surf->n_slices = 0;

                for (int e = 0; e < n_expiries; ++e) {
                    int cnt = expiry_counts[e];
                    if (cnt < 5) {
                        // Not enough quotes — copy existing slice if available
                        const SVIVolSurface* cur =
                            static_cast<const SVIVolSurface*>(vol_surfaces_[p].get());
                        for (int s = 0; s < cur->n_slices; ++s) {
                            if (std::fabs(cur->slices[s].expiry_T - expiry_Ts[e]) < 1.0/365.0) {
                                surf->slices[surf->n_slices++] = cur->slices[s];
                                break;
                            }
                        }
                        continue;
                    }

                    SVIParams params{};
                    params.expiry_T = expiry_Ts[e];
                    bool ok = fit_svi_slice(
                        strikes  + expiry_start[e],
                        mkt_vols + expiry_start[e],
                        cnt, fwd_prices[e], expiry_Ts[e], params);

                    if (ok) {
                        surf->slices[surf->n_slices++] = params;
                        OMM_LOG_INFO("volfitter",
                            "product={} expiry_T={:.4f} n={} a={:.4f} b={:.4f} rho={:.3f} m={:.4f} sigma={:.4f}",
                            p, expiry_Ts[e], cnt,
                            params.a, params.b, params.rho, params.m, params.sigma);
                    } else {
                        OMM_LOG_WARN("volfitter",
                            "product={} expiry_T={:.4f} fit failed (n={})", p, expiry_Ts[e], cnt);
                    }
                }

                if (surf->n_slices > 0)
                    vol_surfaces_[p].publish();
            }
        }
    }
}

// ─── Risk monitor thread ──────────────────────────────────────────────────────

void TradingEngine::risk_monitor_loop() noexcept {
    set_thread_name("omm-risk");

    while (!stop_flag_.load(std::memory_order_relaxed)) {
        // Process fills from risk_buf_
        Trade trade{};
        while (risk_buf_.try_pop(trade))
            post_risk_.on_fill(trade);

        // Periodic Greeks limit check against latest snapshot
        post_risk_.check_limits(greeks_snapshot_, n_instruments_);

        // Log any new risk breaches
        if (post_risk_.any_breach())
            OMM_LOG_WARN("risk", "breach flags: pos={} delta={} gamma={} vega={}",
                         (int)post_risk_.position_breach(),
                         (int)post_risk_.delta_breach(),
                         (int)post_risk_.gamma_breach(),
                         (int)post_risk_.vega_breach());

        struct timespec ts{0, 100'000'000};
        nanosleep(&ts, nullptr);
    }
}

// ─── Timer thread ─────────────────────────────────────────────────────────────

void TradingEngine::timer_loop() noexcept {
    set_thread_name("omm-timer");

    int64_t last_hedge_ns   = get_monotonic_ns();
    int64_t last_T_refresh_ns = last_hedge_ns;
    const int64_t hedge_interval_ns =
        static_cast<int64_t>(cfg_.timer.hedge_check_interval_ms) * 1'000'000LL;
    static constexpr int64_t T_REFRESH_NS = 1'000'000'000LL;  // 1 second

    while (!stop_flag_.load(std::memory_order_relaxed)) {
        // Sleep 100ms at a time so stop_flag is checked promptly
        struct timespec ts{0, 100'000'000};
        nanosleep(&ts, nullptr);

        if (stop_flag_.load(std::memory_order_relaxed)) break;

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
                if (strategies_[i])
                    strategies_[i]->on_timer(ev);
            }
            last_hedge_ns = now;
        }
    }
}

// ─── Manual order / cancel (called from gRPC server thread) ──────────────────

bool TradingEngine::submit_manual_order(const Order& o) noexcept {
    if (!gateway_ || !gateway_->is_connected()) return false;
    // Route through the product's order buffer so the gateway dispatcher sends it
    uint8_t prod = (o.product_index < MAX_PRODUCTS) ? o.product_index : 0;
    return order_buf_[prod].try_push(o);
}

bool TradingEngine::cancel_order(OrderId id, uint16_t instrument_id) noexcept {
    if (!gateway_ || !gateway_->is_connected()) return false;
    return gateway_->cancel_order(id, instrument_id);
}

} // namespace omm
