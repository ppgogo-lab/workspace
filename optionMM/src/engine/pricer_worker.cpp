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
// ─── Pricer thread ────────────────────────────────────────────────────────────
// Trigger model: a FUTURE tick drives repricing of ALL options for that product.
//   - Future tick arrives �?update tick_snapshot_ for the future �?for each
//     option in the product, compute Black-76 using the future price as F and
//     the vol surface for sigma �?emit one PricingSignal per option.
//   - Option tick arrives �?update tick_snapshot_ only (for the vol fitter to
//     read mid-prices for IV inversion). No PricingSignal emitted.
//
// This is the correct market-making model: the forward price drives repricing,
// not individual option prints. A single future tick reprices the whole book.

void TradingEngine::pricer_loop() noexcept {
    set_thread_name("omm-pricer");
    pin_if_configured(cfg_.affinity.pricer_core);
    apply_realtime_if_configured(cfg_.scheduling.enable_realtime,
                                 cfg_.scheduling.pricer_priority,
                                 "omm-pricer");

    // Bind to local NUMA node for optimal memory access
    if (numa_available_multi_node()) {
        bind_thread_to_local_numa_node();
    }

    TopOfBookTick tick{};
    TopOfBookTick pending_future_tick[MAX_PRODUCTS]{};
    bool pending_product[MAX_PRODUCTS]{};
    uint16_t next_option_offset[MAX_PRODUCTS]{};
    uint16_t cold_greeks_offset[MAX_PRODUCTS]{};
    int64_t next_cold_greeks_due_ns[MAX_PRODUCTS]{};
    int rr_cursor = 0;
    int cold_rr_cursor = 0;
    int spin_count = 0;  // Adaptive spinning counter
    const int64_t cold_greeks_interval_ns =
        static_cast<int64_t>(std::max(1, cfg_.pricing.cold_greeks_interval_ms)) * 1'000'000LL;
    const uint16_t cold_greeks_batch_cap = static_cast<uint16_t>(
        std::max(1, std::min(cfg_.pricing.cold_greeks_batch_size, 128)));

    auto refresh_cold_greeks_batch = [&](int product_count) noexcept -> bool {
        const int64_t now = get_monotonic_ns();
        int selected = -1;
        for (int scan = 0; scan < product_count; ++scan) {
            const int p = (cold_rr_cursor + scan) % product_count;
            if (option_count_[p] > 0 && now >= next_cold_greeks_due_ns[p]) {
                selected = p;
                cold_rr_cursor = (p + 1) % product_count;
                break;
            }
        }
        if (selected < 0) return false;

        const uint8_t prod = static_cast<uint8_t>(selected);
        const uint16_t n = option_count_[prod];
        const uint16_t start = cold_greeks_offset[prod];
        const uint16_t batch_n = std::min<uint16_t>(cold_greeks_batch_cap, n - start);
        if (batch_n == 0) {
            cold_greeks_offset[prod] = 0;
            next_cold_greeks_due_ns[prod] = now + cold_greeks_interval_ns;
            return true;
        }

        const uint16_t first_opt_id = option_ids_[prod][0];
        if (first_opt_id >= MAX_INSTRUMENTS) {
            next_cold_greeks_due_ns[prod] = now + cold_greeks_interval_ns;
            return true;
        }
        const uint16_t future_id = instruments_[first_opt_id].underlying_id;
        if (future_id >= MAX_INSTRUMENTS) {
            next_cold_greeks_due_ns[prod] = now + cold_greeks_interval_ns;
            return true;
        }
        TopOfBookTick future_tick{};
        if (!tick_snapshot_.read(future_id, &future_tick)) {
            next_cold_greeks_due_ns[prod] = now + cold_greeks_interval_ns;
            return true;
        }
        const double F_mid = future_tick.last_price;
        if (F_mid < 1e-10) {
            next_cold_greeks_due_ns[prod] = now + cold_greeks_interval_ns;
            return true;
        }

        const IVolSurface* surf = nullptr;
        if (cfg_.pricing.vol_method == VolMethod::Wing) {
            surf = wing_surfaces_[prod].get();
        } else if (cfg_.pricing.vol_method == VolMethod::OrcWing) {
            surf = orc_wing_surfaces_[prod].get();
        } else {
            surf = vol_surfaces_[prod].get();
        }

        alignas(32) double F_arr[128];
        alignas(32) double K_arr[128];
        alignas(32) double T_arr[128];
        alignas(32) double sqrt_T_arr[128];
        alignas(32) double disc_arr[128];
        alignas(32) double sigma_arr[128];
        alignas(32) uint8_t is_call_arr[128];
        alignas(32) Black76Result results[128];
        const double log_F_mid = std::log(F_mid);

        for (uint16_t bi = 0; bi < batch_n; ++bi) {
            const uint16_t oi = start + bi;
            const uint16_t opt_id = option_ids_[prod][oi];
            const Instrument& opt = instruments_[opt_id];
            F_arr[bi] = F_mid;
            K_arr[bi] = opt.strike;
            T_arr[bi] = option_T_[prod][oi];
            sqrt_T_arr[bi] = option_sqrt_T_[prod][oi];
            disc_arr[bi] = option_disc_[prod][oi];
            sigma_arr[bi] = 0.20;
            is_call_arr[bi] = (opt.option_type == OptionType::Call) ? 1 : 0;
        }

        if (cfg_.pricing.vol_method == VolMethod::OrcWing) {
            const auto* orc_surf = static_cast<const OrcWingVolSurface*>(surf);
            orc_surf->get_vols_by_strike(F_mid, K_arr, T_arr, sigma_arr, batch_n);
        } else {
            for (uint16_t bi = 0; bi < batch_n; ++bi) {
                const uint16_t oi = start + bi;
                sigma_arr[bi] = surf->get_vol(option_log_K_[prod][oi] - log_F_mid, T_arr[bi]);
            }
        }

        compute_batch_precomputed(F_arr, K_arr, T_arr, sqrt_T_arr, disc_arr,
                                  sigma_arr, is_call_arr, results, batch_n);

        for (uint16_t bi = 0; bi < batch_n; ++bi) {
            const uint16_t opt_id = option_ids_[prod][start + bi];
            Greeks greek{};
            (void)greeks_snapshot_.read(opt_id, &greek);
            greek.theta = results[bi].theta;
            greek.rho = results[bi].rho;
            greek.T = T_arr[bi];
            greeks_snapshot_.publish(opt_id, greek);
        }

        cold_greeks_offset[prod] = static_cast<uint16_t>(start + batch_n);
        if (cold_greeks_offset[prod] >= n) {
            cold_greeks_offset[prod] = 0;
            next_cold_greeks_due_ns[prod] = now + cold_greeks_interval_ns;
        }
        return true;
    };

    while (!stop_flag_.load(std::memory_order_relaxed)) {
        bool did_work = false;
        if (tick_buf_.try_pop(tick)) {
            did_work = true;

            const uint16_t id = tick.instrument_id;
            if (id < MAX_INSTRUMENTS) {
                const Instrument& instr = instruments_[id];
                if (instr.instrument_id != INVALID_INSTRUMENT_ID) {
                    tick_snapshot_.publish(id, tick);
                    publish_monitor_tick(tick);

                    const uint8_t tick_prod = instr_to_product_[id];
                    if (tick_prod < MAX_PRODUCTS
                        && cfg_.products[tick_prod].arbitrage_strategy_count > 0) {
                        ArbMarketTrigger trigger{};
                        trigger.instrument_id = id;
                        trigger.product_index = tick_prod;
                        trigger.sequence_no = tick.sequence_no;
                        trigger.recv_ts_ns = tick.recv_ts_ns;
                        (void)arb_market_trigger_buf_[tick_prod].try_push(trigger);
                    }

                    if (instr.kind == InstrumentKind::Future) {
                        const uint8_t prod = tick_prod;
                        if (prod < MAX_PRODUCTS && tick.last_price > 1e-10) {
                            pending_future_tick[prod] = tick;
                            if (pending_product[prod]) {
                                ++pending_future_tick_overwrites_[prod];  // Plain increment (single writer)
                            } else {
                                pending_product[prod] = true;
                                next_option_offset[prod] = 0;
                            }
                        }
                    }
                }
            }
        }

        const int product_count = std::max(1, std::min(cfg_.product_count, static_cast<int>(MAX_PRODUCTS)));
        int selected_prod = -1;
        for (int scan = 0; scan < product_count; ++scan) {
            const int p = (rr_cursor + scan) % product_count;
            if (pending_product[p]) {
                selected_prod = p;
                rr_cursor = (p + 1) % product_count;
                break;
            }
        }

        if (selected_prod < 0) {
            if (refresh_cold_greeks_batch(product_count)) {
                did_work = true;
            }
            if (!did_work) {
                idle_pause(cfg_.scheduling.low_latency_spin, spin_count);
            } else {
                spin_count = 0;  // Reset on work
            }
            continue;
        }

        const uint8_t prod = static_cast<uint8_t>(selected_prod);
        const TopOfBookTick& future_tick = pending_future_tick[prod];
        const double F_mid = future_tick.last_price;
        const double F_bid = future_tick.bid_price[0] > 0.0 ? future_tick.bid_price[0] : F_mid;
        const double F_ask = future_tick.ask_price[0] > F_bid ? future_tick.ask_price[0] : F_mid;
        const uint16_t n = option_count_[prod];
        if (n == 0 || F_mid < 1e-10) {
            pending_product[prod] = false;
            next_option_offset[prod] = 0;
            continue;
        }

        const IVolSurface* surf = nullptr;
        if (cfg_.pricing.vol_method == VolMethod::Wing) {
            surf = wing_surfaces_[prod].get();
        } else if (cfg_.pricing.vol_method == VolMethod::OrcWing) {
            surf = orc_wing_surfaces_[prod].get();
        } else {
            surf = vol_surfaces_[prod].get();
        }

        const int64_t now = get_monotonic_ns();
        constexpr uint16_t MAX_BATCH = 128;
        alignas(32) double F_mid_arr[MAX_BATCH];
        alignas(32) double F_bid_arr[MAX_BATCH];
        alignas(32) double F_ask_arr[MAX_BATCH];
        alignas(32) double K_arr[MAX_BATCH];
        alignas(32) double T_arr[MAX_BATCH];
        alignas(32) double sqrt_T_arr[MAX_BATCH];
        alignas(32) double disc_arr[MAX_BATCH];
        alignas(32) double sigma_arr[MAX_BATCH];
        alignas(32) uint8_t is_call_arr[MAX_BATCH];
        alignas(32) Black76QuoteResult mid_results[MAX_BATCH];
        alignas(32) Black76QuoteResult bid_results[MAX_BATCH];
        alignas(32) Black76QuoteResult ask_results[MAX_BATCH];
        alignas(64) PricingSignal sigs[MAX_BATCH];

        const uint16_t start = next_option_offset[prod];
        const uint16_t batch_n = std::min<uint16_t>(MAX_BATCH, n - start);
        const double log_F_mid = std::log(F_mid);
        const uint64_t surface_version = surface_versions_[prod];  // Plain read (eventual consistency OK)

        // Populate arrays (F, K, T, sqrt_T, disc, is_call)
        for (uint16_t bi = 0; bi < batch_n; ++bi) {
            const uint16_t oi = start + bi;
            const uint16_t opt_id = option_ids_[prod][oi];
            const Instrument& opt = instruments_[opt_id];

            F_mid_arr[bi] = F_mid;
            F_bid_arr[bi] = F_bid;
            F_ask_arr[bi] = F_ask;
            K_arr[bi] = opt.strike;
            T_arr[bi] = option_T_[prod][oi];
            sqrt_T_arr[bi] = option_sqrt_T_[prod][oi];
            disc_arr[bi] = option_disc_[prod][oi];
            is_call_arr[bi] = (opt.option_type == OptionType::Call) ? 1 : 0;
        }

        // Compute volatilities (hoisted vol method dispatch eliminates branch misprediction + virtual calls)
        if (cfg_.pricing.vol_method == VolMethod::SVI) {
            const auto* svi_surf = static_cast<const SVIVolSurface*>(surf);
            for (uint16_t bi = 0; bi < batch_n; ++bi) {
                const uint16_t oi = start + bi;
                const int8_t slice_idx = option_expiry_slice_[prod][oi];
                if (slice_idx >= 0) {
                    sigma_arr[bi] = svi_surf->get_vol_cached(
                        option_log_K_[prod][oi] - log_F_mid, T_arr[bi], slice_idx);
                } else {
                    sigma_arr[bi] = svi_surf->get_vol(
                        option_log_K_[prod][oi] - log_F_mid, T_arr[bi]);
                }
            }
        } else if (cfg_.pricing.vol_method == VolMethod::OrcWing) {
            const auto* orc_surf = static_cast<const OrcWingVolSurface*>(surf);
            orc_surf->get_vols_by_strike(F_mid, K_arr, T_arr, sigma_arr, batch_n);
        } else if (cfg_.pricing.vol_method == VolMethod::Wing) {
            const auto* wing_surf = static_cast<const WingVolSurface*>(surf);
            for (uint16_t bi = 0; bi < batch_n; ++bi) {
                sigma_arr[bi] = wing_surf->get_vol_by_strike(F_mid, K_arr[bi], T_arr[bi]);
            }
        } else {
            // SABR, CubicSpline, or other generic surfaces
            for (uint16_t bi = 0; bi < batch_n; ++bi) {
                const uint16_t oi = start + bi;
                sigma_arr[bi] = surf->get_vol(option_log_K_[prod][oi] - log_F_mid, T_arr[bi]);
            }
        }

        // Fused batch pricing: computes bid, mid, ask in single pass (3× �?1×)
        compute_batch_quote_fused(F_bid_arr, F_mid_arr, F_ask_arr, K_arr,
                                  sqrt_T_arr, disc_arr, sigma_arr, is_call_arr,
                                  bid_results, mid_results, ask_results, batch_n);

        alignas(64) PricingSignal emitted_sigs[MAX_BATCH];
        uint16_t emitted_slots[MAX_BATCH];
        int emitted_count = 0;

        for (uint16_t bi = 0; bi < batch_n; ++bi) {
            const uint16_t oi = start + bi;
            const uint16_t opt_id = option_ids_[prod][oi];
            const Instrument& opt = instruments_[opt_id];
            const Black76QuoteResult& mid_res = mid_results[bi];

            PricingSignal& sig = sigs[bi];
            sig.instrument_id = opt_id;
            sig.underlying_id = opt.underlying_id;
            sig.flags = PricingFlagHasUnderlyingRef;
            sig.sequence_no = future_tick.sequence_no;
            sig.calc_ts_ns = now;
            sig.theo_bid = std::min(bid_results[bi].price, ask_results[bi].price);
            sig.theo_ask = std::max(bid_results[bi].price, ask_results[bi].price);
            sig.delta = static_cast<float>(mid_res.delta);
            sig.vega = static_cast<float>(mid_res.vega);
            sig.underlying_ref_bid = static_cast<float>(future_tick.bid_price[0]);
            sig.underlying_ref_ask = static_cast<float>(future_tick.ask_price[0]);

            Greeks greek{};
            (void)greeks_snapshot_.read(opt_id, &greek);
            greek.instrument_id = opt_id;
            greek.theo_price = mid_res.price;
            greek.delta = mid_res.delta;
            greek.gamma = mid_res.gamma;
            greek.vega = mid_res.vega;
            greek.iv = sigma_arr[bi];
            greek.T = T_arr[bi];
            greek.calc_ts_ns = now;
            greeks_snapshot_.publish(opt_id, greek);

            if (!should_emit_signal(prod, oi, sig, surface_version)) {
                ++signal_suppressed_count_[prod];  // Plain increment (single writer)
                continue;
            }

            emitted_slots[emitted_count] = oi;
            emitted_sigs[emitted_count] = sig;
            ++emitted_count;
        }

        const uint32_t queued_before_push = static_cast<uint32_t>(signal_buf_[prod].size_approx());
        const uint32_t projected_queue_depth = std::min<uint32_t>(
            static_cast<uint32_t>(signal_buf_[prod].capacity()),
            queued_before_push + static_cast<uint32_t>(emitted_count));
        update_max(max_signal_queue_depth_[prod], projected_queue_depth);
        if (!signal_buf_[prod].try_push_batch(emitted_sigs, emitted_count)) {
            // Phase 5: if the bounded signal ring is saturated, collapse this
            // batch into latest-only per-option mailboxes instead of spinning.
            // The strategy drains the mailbox within its normal signal budget,
            // so stale theo work is overwritten rather than backpressuring the
            // pricer thread indefinitely.
            update_max(max_signal_queue_depth_[prod],
                       static_cast<uint32_t>(signal_buf_[prod].capacity()));
            for (int bi = 0; bi < emitted_count; ++bi) {
                coalesce_signal(prod, emitted_slots[bi], emitted_sigs[bi]);
            }
        }

        for (int bi = 0; bi < emitted_count; ++bi) {
            note_signal_emitted(prod,
                                emitted_slots[bi],
                                emitted_sigs[bi].instrument_id,
                                emitted_sigs[bi],
                                surface_version);
        }

        next_option_offset[prod] = static_cast<uint16_t>(start + batch_n);
        if (next_option_offset[prod] >= n) {
            pending_product[prod] = false;
            next_option_offset[prod] = 0;
        }
    }
}


} // namespace omm
