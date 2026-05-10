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

/**
 * @brief Implements Vol fitter loop.
 * @return None.
 * @note Noexcept API preserves hot-path failure and latency invariants.
 */
void TradingEngine::vol_fitter_loop() noexcept {
    set_thread_name("omm-volfitter");
    apply_realtime_if_configured(cfg_.scheduling.enable_realtime,
                                 cfg_.scheduling.vol_fitter_priority,
                                 "omm-volfitter");
    pin_if_configured(cfg_.affinity.vol_fitter_core);

    // Bind to local NUMA node for optimal memory access
    if (numa_available_multi_node()) {
        bind_thread_to_local_numa_node();
    }

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
                    TopOfBookTick t{};
                    if (!tick_snapshot_.read(id, &t)) continue;
                    if (t.last_price > 1e-10) {
                        for (int e = 0; e < MAX_EXPIRIES; ++e)
                            if (fwd_prices[e] < 1e-10) fwd_prices[e] = t.last_price;
                    }
                    continue;
                }

                if (instr.kind != InstrumentKind::Option) continue;

                TopOfBookTick t{};
                if (!tick_snapshot_.read(id, &t)) continue;
                if (t.recv_ts_ns == 0) continue;  // no tick yet

                // Compute trading-session-adjusted T for this instrument.
                double T = option_time_to_expiry_years(instr);
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
                        TopOfBookTick ut{};
                        if (!tick_snapshot_.read(instr.underlying_id, &ut)) continue;
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

                TopOfBookTick t{};
                if (!tick_snapshot_.read(id, &t)) continue;
                if (t.recv_ts_ns == 0) continue;

                double T = option_time_to_expiry_years(instr);
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
                double bid = t.bid_price, ask = t.ask_price;
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

                if (surf->n_slices > 0) {
                    wing_surfaces_[p].publish();
                    ++surface_versions_[p];  // Plain increment (single writer)
                }
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
                    const OrcWingParams* seed = nullptr;
                    const OrcWingVolSurface* cur =
                        static_cast<const OrcWingVolSurface*>(orc_wing_surfaces_[p].get());
                    for (int s = 0; s < cur->n_slices; ++s) {
                        if (std::fabs(cur->slices[s].expiry_T - expiry_Ts[e]) < 1.0 / 365.0) {
                            seed = &cur->slices[s];
                            break;
                        }
                    }
                    const bool ok = fit_orc_wing_slice_seeded(
                        strikes + expiry_start[e],
                        mkt_vols + expiry_start[e],
                        cnt, fwd_prices[e], expiry_Ts[e], seed, params);

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

                if (surf->n_slices > 0) {
                    orc_wing_surfaces_[p].publish();
                    ++surface_versions_[p];  // Plain increment (single writer)
                }
            } else {
                SVIVolSurface* surf = vol_surfaces_[p].get_inactive();
                surf->n_slices = 0;

                for (int e = 0; e < n_expiries; ++e) {
                    int cnt = expiry_counts[e];
                    if (cnt < 5) {
                        // Not enough quotes �?copy existing slice if available
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

                if (surf->n_slices > 0) {
                    vol_surfaces_[p].publish();
                    ++surface_versions_[p];  // Plain increment (single writer)
                }
            }
        }
    }
}


} // namespace omm
