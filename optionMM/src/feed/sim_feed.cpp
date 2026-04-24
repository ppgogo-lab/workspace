#include "feed/sim_feed.h"

#include "common/thread_utils.h"
#include "gateway/sim_gateway.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cmath>
#include <random>
#include <thread>

namespace omm {

namespace {

constexpr double kNsPerYear = 365.0 * 24.0 * 3600.0 * 1e9;

double round_to_tick(double price, double tick_size) noexcept {
    const double tick = std::max(0.0001, tick_size);
    return std::max(tick, std::round(price / tick) * tick);
}

double infer_reference_price(const Instrument& instrument,
                             const SimConfig& cfg) noexcept {
    const std::string_view code = instrument.code.view();
    if (code.size() >= 2) {
        const char a = static_cast<char>(std::tolower(static_cast<unsigned char>(code[0])));
        const char b = static_cast<char>(std::tolower(static_cast<unsigned char>(code[1])));
        if (a == 'a' && b == 'u') return cfg.au_reference_price;
        if (a == 'a' && b == 'g') return cfg.ag_reference_price;
    }
    return 100.0 + 25.0 * instrument.product_index;
}

void fill_top_of_book(TopOfBookTick& tick,
                      double mid_price,
                      double spread,
                      Volume top_size,
                      double tick_size) noexcept {
    const double min_step = std::max(tick_size, spread * 0.5);
    tick.bid_price[0] = round_to_tick(std::max(tick_size, mid_price - min_step), tick_size);
    tick.ask_price[0] = round_to_tick(mid_price + min_step, tick_size);
    tick.bid_volume[0] = std::max<Volume>(1, top_size);
    tick.ask_volume[0] = std::max<Volume>(1, top_size);
}

} // namespace

void SimFeedHandler::start() {
    stop_flag_.store(false, std::memory_order_relaxed);
    connected_.store(true, std::memory_order_release);
    thread_ = std::thread([this] { run_loop(); });
}

void SimFeedHandler::stop() {
    stop_flag_.store(true, std::memory_order_release);
    if (thread_.joinable()) thread_.join();
    connected_.store(false, std::memory_order_release);
}

void SimFeedHandler::run_loop() noexcept {
    if (!tick_buf_ || !instruments_ || n_instruments_ == 0) {
        err_count_.fetch_add(1, std::memory_order_relaxed);
        connected_.store(false, std::memory_order_release);
        return;
    }

    std::mt19937 rng(cfg_.random_seed);
    std::uniform_real_distribution<double> noise(-1.0, 1.0);
    std::array<double, MAX_PRODUCTS> future_mid{};
    const std::string_view scenario(cfg_.scenario);
    const bool scenario_vol_spike = scenario == "vol_spike";
    const bool scenario_selloff = scenario == "selloff";
    const bool scenario_rally = scenario == "rally";

    double phase = 0.0;
    uint64_t sequence_no = 1;

    while (!stop_flag_.load(std::memory_order_relaxed)) {
        const int64_t now_ns = get_monotonic_ns();
        phase += 0.17;

        for (uint16_t i = 0; i < n_instruments_; ++i) {
            const Instrument& instrument = instruments_[i];
            TopOfBookTick tick{};
            tick.recv_ts_ns = now_ns;
            tick.exchange_ts_ns = now_ns;
            tick.instrument_id = instrument.instrument_id;
            tick.sequence_no = sequence_no++;

            if (instrument.kind == InstrumentKind::Future) {
                const double base = infer_reference_price(instrument, cfg_);
                const double scenario_trend = scenario_selloff ? -0.012 : (scenario_rally ? 0.012 : 0.0);
                const double scenario_wave_scale = scenario_vol_spike ? 1.8 : 1.0;
                const double scenario_noise_scale = scenario_vol_spike ? 2.2 : 1.0;
                const double wave = base * (cfg_.future_wave_bps / 10'000.0) * scenario_wave_scale
                                  * std::sin(phase + instrument.product_index * 0.75);
                const double bump = base * (cfg_.future_noise_bps / 10'000.0) * scenario_noise_scale * noise(rng);
                const double mid = round_to_tick(base * (1.0 + scenario_trend) + wave + bump, instrument.tick_size);
                const double spread = std::max(instrument.tick_size, mid * 0.0002);

                future_mid[instrument.product_index] = mid;
                tick.last_price = mid;
                fill_top_of_book(tick, mid, spread, cfg_.top_level_volume, instrument.tick_size);
            } else {
                const double underlying = std::max(instrument.tick_size,
                                                   future_mid[instrument.product_index]);
                const double t = std::max(1.0 / 365.0,
                                          (instrument.expiry_epoch_ns - now_ns) / kNsPerYear);
                const double log_m = std::log(std::max(1e-6, underlying / instrument.strike));
                const double scenario_vol_bump = scenario_vol_spike ? 0.08 : (scenario_selloff ? 0.03 : 0.0);
                const double vol = 0.16
                                 + 0.03 * std::exp(-std::abs(log_m) * 2.0)
                                 + 0.015 * std::sin(phase * 0.5 + instrument.product_index)
                                 + scenario_vol_bump;
                const double intrinsic =
                    instrument.option_type == OptionType::Call
                        ? std::max(0.0, underlying - instrument.strike)
                        : std::max(0.0, instrument.strike - underlying);
                const double time_value =
                    underlying * vol * std::sqrt(t) * std::exp(-std::abs(log_m) * 2.2) * 0.085;
                const double mid = round_to_tick(std::max(instrument.tick_size, intrinsic + time_value),
                                                 instrument.tick_size);
                const double option_spread_bps = cfg_.option_spread_bps * (scenario_vol_spike ? 1.35 : 1.0);
                const double spread = std::max(instrument.tick_size,
                                               mid * (option_spread_bps / 10'000.0));

                tick.last_price = mid;
                fill_top_of_book(tick, mid, spread, std::max<Volume>(4, cfg_.top_level_volume / 2),
                                 instrument.tick_size);
            }

            if (sim_gateway_) {
                sim_gateway_->set_last_price(instrument.instrument_id, tick.last_price);
            }

            if (!tick_buf_->try_push(tick)) {
                dropped_count_.fetch_add(1, std::memory_order_relaxed);
            } else {
                msg_count_.fetch_add(1, std::memory_order_relaxed);
            }
        }

        std::this_thread::sleep_for(
            std::chrono::milliseconds(std::max(10, cfg_.tick_interval_ms)));
    }
}

} // namespace omm
