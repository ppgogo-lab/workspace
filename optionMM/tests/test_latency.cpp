#include <gtest/gtest.h>

#include "common/config.h"
#include "common/thread_utils.h"
#include "common/types.h"
#include "engine/trading_engine.h"
#include "gateway/sim_gateway.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

using namespace omm;

#if defined(__SANITIZE_ADDRESS__)
#  define RUNNING_ASAN 1
#elif defined(__has_feature)
#  if __has_feature(address_sanitizer)
#    define RUNNING_ASAN 1
#  endif
#endif

// Records quote send timing at the gateway edge so the benchmark measures
// tick-to-dispatch latency without racing the strategy's SPSC quote ring.
class LatencySimGateway : public SimGateway {
public:
    bool send_quote(const Quote& q) noexcept override {
        if (q.instrument_id < MAX_INSTRUMENTS) {
            strategy_send_ts_[q.instrument_id].store(q.send_ts,
                                                     std::memory_order_relaxed);
            gateway_recv_ts_[q.instrument_id].store(get_monotonic_ns(),
                                                    std::memory_order_release);
            quote_count_[q.instrument_id].fetch_add(1, std::memory_order_release);
        }
        return SimGateway::send_quote(q);
    }

    int64_t get_last_strategy_send_ts(uint16_t id) const noexcept {
        return strategy_send_ts_[id].load(std::memory_order_acquire);
    }

    int64_t get_last_gateway_recv_ts(uint16_t id) const noexcept {
        return gateway_recv_ts_[id].load(std::memory_order_acquire);
    }

    uint64_t quote_count(uint16_t id) const noexcept {
        return quote_count_[id].load(std::memory_order_acquire);
    }

private:
    std::atomic<int64_t> strategy_send_ts_[MAX_INSTRUMENTS]{};
    std::atomic<int64_t> gateway_recv_ts_[MAX_INSTRUMENTS]{};
    std::atomic<uint64_t> quote_count_[MAX_INSTRUMENTS]{};
};

static Instrument make_future(uint16_t id, const char* code,
                              uint8_t product_idx) {
    Instrument f{};
    f.instrument_id = id;
    f.underlying_id = id;
    f.product_index = product_idx;
    f.kind = InstrumentKind::Future;
    f.multiplier = 1.0;
    f.tick_size = 10.0;
    std::strncpy(f.code.data, code, sizeof(f.code.data) - 1);
    std::strncpy(f.underlying_code.data, code, sizeof(f.underlying_code.data) - 1);
    f.exchange = Exchange::SHFE;
    f.expiry_epoch_ns = get_monotonic_ns()
                      + static_cast<int64_t>(0.25 * 365.0 * 24.0 * 3600.0 * 1e9);
    return f;
}

static Instrument make_option(uint16_t id, uint16_t underlying_id,
                              const char* underlying_code,
                              uint8_t product_idx,
                              double strike, OptionType otype,
                              double tick_size = 2.0) {
    Instrument o{};
    o.instrument_id = id;
    o.underlying_id = underlying_id;
    o.product_index = product_idx;
    o.kind = InstrumentKind::Option;
    o.option_type = otype;
    o.strike = strike;
    o.tick_size = tick_size;
    o.multiplier = 1.0;
    o.exchange = Exchange::SHFE;
    std::strncpy(o.underlying_code.data, underlying_code,
                 sizeof(o.underlying_code.data) - 1);
    o.expiry_epoch_ns = get_monotonic_ns()
                      + static_cast<int64_t>(0.25 * 365.0 * 24.0 * 3600.0 * 1e9);
    return o;
}

static MarketTick make_tick(uint16_t id, double last,
                            double bid, double ask) {
    MarketTick t{};
    t.instrument_id = id;
    t.last_price = last;
    t.bid_price[0] = bid;
    t.ask_price[0] = ask;
    t.bid_volume[0] = 10;
    t.ask_volume[0] = 10;
    t.recv_ts_ns = get_monotonic_ns();
    t.exchange_ts_ns = t.recv_ts_ns;
    return t;
}

struct OptionQuoteSeed {
    uint16_t id;
    double last;
    double bid;
    double ask;
};

static int64_t percentile(std::vector<int64_t>& v, double p) {
    if (v.empty()) return 0;
    size_t idx = static_cast<size_t>(p * (v.size() - 1));
    return v[idx];
}

TEST(LatencyTest, TickToQuoteLatency) {
#ifdef RUNNING_ASAN
    GTEST_SKIP() << "Skipping latency measurement under ASAN (overhead distorts results)";
#endif

    SystemConfig cfg{};
    cfg.product_count = 2;

    std::strncpy(cfg.products[0].underlying_id.data, "cu2501", 31);
    cfg.products[0].params.bid_spread = 50.0;
    cfg.products[0].params.ask_spread = 50.0;
    cfg.products[0].params.quote_volume = 5;
    cfg.products[0].params.max_position = 500;
    cfg.products[0].params.enabled = true;
    cfg.products[0].params.min_quote_interval_ms = 0;
    cfg.products[0].params.follow_weight = 0.0;
    cfg.products[0].params.requote_price_epsilon_ticks = 0.0;
    cfg.products[0].params.underlying_move_widen_threshold_ticks = 0.0;
    cfg.products[0].strategy_core = -1;

    std::strncpy(cfg.products[1].underlying_id.data, "rb2501", 31);
    cfg.products[1].params.bid_spread = 5.0;
    cfg.products[1].params.ask_spread = 5.0;
    cfg.products[1].params.quote_volume = 5;
    cfg.products[1].params.max_position = 500;
    cfg.products[1].params.enabled = true;
    cfg.products[1].params.min_quote_interval_ms = 0;
    cfg.products[1].params.follow_weight = 0.0;
    cfg.products[1].params.requote_price_epsilon_ticks = 0.0;
    cfg.products[1].params.underlying_move_widen_threshold_ticks = 0.0;
    cfg.products[1].strategy_core = -1;

    cfg.pricing.risk_free_rate = 0.025;
    cfg.pricing.fit_interval_seconds = 1;
    cfg.risk.hard.max_volume_per_order = 100;
    cfg.risk.soft.max_net_position = 500;
    cfg.risk.soft.max_delta = 100000;
    cfg.risk.soft.max_gamma = 100000;
    cfg.risk.soft.max_vega = 100000;
    cfg.timer.hedge_check_interval_ms = 60000;
    cfg.timer.quote_refresh_interval_ms = 60000;

    cfg.affinity.feed_core = -1;
    cfg.affinity.pricer_core = -1;
    cfg.affinity.gateway_dispatcher_core = -1;
    cfg.affinity.vol_fitter_core = -1;
    cfg.affinity.risk_monitor_core = -1;
    cfg.affinity.timer_core = -1;

    auto gw = std::make_unique<LatencySimGateway>();
    auto* lat_gw = gw.get();

    SimConfig sim{};
    sim.gateway_ack_latency_ms = 0;
    sim.gateway_cancel_latency_ms = 0;
    sim.gateway_fill_interval_ms = 1;
    sim.gateway_order_fill_probability = 0.0;
    sim.gateway_quote_cross_fill_probability = 0.0;
    sim.gateway_quote_passive_fill_probability = 0.0;
    sim.gateway_partial_fill_probability = 0.0;
    sim.gateway_reject_probability = 0.0;
    gw->set_sim_config(sim);

    gw->add_instrument(make_future(0, "cu2501", 0));
    gw->set_last_price(0, 75000.0);
    gw->add_instrument(make_option(1, 0, "cu2501", 0, 73000.0, OptionType::Call));
    gw->add_instrument(make_option(2, 0, "cu2501", 0, 74000.0, OptionType::Call));
    gw->add_instrument(make_option(3, 0, "cu2501", 0, 75000.0, OptionType::Call));
    gw->add_instrument(make_option(4, 0, "cu2501", 0, 73000.0, OptionType::Put));
    gw->add_instrument(make_option(5, 0, "cu2501", 0, 74000.0, OptionType::Put));
    gw->add_instrument(make_option(6, 0, "cu2501", 0, 75000.0, OptionType::Put));

    gw->set_last_price(1, 2800.0);
    gw->set_last_price(2, 1900.0);
    gw->set_last_price(3, 1200.0);
    gw->set_last_price(4, 400.0);
    gw->set_last_price(5, 900.0);
    gw->set_last_price(6, 1200.0);

    gw->add_instrument(make_future(7, "rb2501", 1));
    gw->set_last_price(7, 3500.0);
    gw->add_instrument(make_option(8, 7, "rb2501", 1, 3400.0, OptionType::Call, 0.5));
    gw->add_instrument(make_option(9, 7, "rb2501", 1, 3500.0, OptionType::Call, 0.5));
    gw->add_instrument(make_option(10, 7, "rb2501", 1, 3600.0, OptionType::Call, 0.5));
    gw->add_instrument(make_option(11, 7, "rb2501", 1, 3400.0, OptionType::Put, 0.5));
    gw->add_instrument(make_option(12, 7, "rb2501", 1, 3500.0, OptionType::Put, 0.5));
    gw->add_instrument(make_option(13, 7, "rb2501", 1, 3600.0, OptionType::Put, 0.5));

    gw->set_last_price(8, 130.0);
    gw->set_last_price(9, 75.0);
    gw->set_last_price(10, 35.0);
    gw->set_last_price(11, 30.0);
    gw->set_last_price(12, 75.0);
    gw->set_last_price(13, 130.0);

    gw->connect(cfg.gateway);

    auto engine = std::make_unique<TradingEngine>(cfg, std::move(gw), nullptr);
    engine->start();

    constexpr OptionQuoteSeed PRODUCT_OPTION_QUOTES[2][6] = {
        {
            {1, 2800.0, 2750.0, 2850.0},
            {2, 1900.0, 1870.0, 1930.0},
            {3, 1200.0, 1180.0, 1220.0},
            {4, 400.0, 390.0, 410.0},
            {5, 900.0, 880.0, 920.0},
            {6, 1200.0, 1180.0, 1220.0},
        },
        {
            {8, 130.0, 127.0, 133.0},
            {9, 75.0, 73.0, 77.0},
            {10, 35.0, 34.0, 36.0},
            {11, 30.0, 29.0, 31.0},
            {12, 75.0, 73.0, 77.0},
            {13, 130.0, 127.0, 133.0},
        }
    };
    constexpr uint16_t FUTURE_IDS[] = {0, 7};
    constexpr double FUTURE_BASE[] = {75000.0, 3500.0};
    constexpr double FUTURE_SWING[] = {40.0, 4.0};
    constexpr int EXPECTED_QUOTES = 6;
    constexpr int N = 5000;

    for (const auto& product_quotes : PRODUCT_OPTION_QUOTES) {
        for (const auto& seed : product_quotes) {
            MarketTick tick = make_tick(seed.id, seed.last, seed.bid, seed.ask);
            while (!engine->tick_buf().try_push(tick)) spin_pause();
        }
    }

    std::this_thread::sleep_for(std::chrono::seconds(3));

    std::vector<int64_t> latencies;
    std::vector<int64_t> latencies_by_product[2];
    size_t quotes_by_product[2]{};
    latencies.reserve(N * EXPECTED_QUOTES);
    latencies_by_product[0].reserve((N / 2 + 1) * EXPECTED_QUOTES);
    latencies_by_product[1].reserve((N / 2 + 1) * EXPECTED_QUOTES);

    for (int i = 0; i < N; ++i) {
        const int slot = i % 2;
        const int product_iter = i / 2;
        const uint16_t fut_id = FUTURE_IDS[slot];
        const double signed_swing = (product_iter & 1) ? FUTURE_SWING[slot] : -FUTURE_SWING[slot];
        const double F = FUTURE_BASE[slot] + signed_swing;

        uint64_t base_counts[EXPECTED_QUOTES]{};
        for (int qi = 0; qi < EXPECTED_QUOTES; ++qi) {
            const auto& seed = PRODUCT_OPTION_QUOTES[slot][qi];
            base_counts[qi] = lat_gw->quote_count(seed.id);
            MarketTick opt_tick = make_tick(seed.id, seed.last, seed.bid, seed.ask);
            while (!engine->tick_buf().try_push(opt_tick)) spin_pause();
        }

        lat_gw->set_last_price(fut_id, F);

        MarketTick tick{};
        tick.instrument_id = fut_id;
        tick.last_price = F;
        tick.bid_price[0] = F - FUTURE_SWING[slot] * 0.5;
        tick.ask_price[0] = F + FUTURE_SWING[slot] * 0.5;
        tick.bid_volume[0] = 100;
        tick.ask_volume[0] = 100;
        tick.exchange_ts_ns = get_monotonic_ns();
        tick.recv_ts_ns = tick.exchange_ts_ns;

        const int64_t t0 = tick.recv_ts_ns;
        while (!engine->tick_buf().try_push(tick)) spin_pause();

        constexpr int64_t TIMEOUT_NS = 5'000'000LL;
        const int64_t deadline = t0 + TIMEOUT_NS;
        bool seen[EXPECTED_QUOTES]{};
        int quotes_collected = 0;

        while (get_monotonic_ns() < deadline && quotes_collected < EXPECTED_QUOTES) {
            for (int qi = 0; qi < EXPECTED_QUOTES; ++qi) {
                if (seen[qi]) continue;
                const uint16_t option_id = PRODUCT_OPTION_QUOTES[slot][qi].id;
                if (lat_gw->quote_count(option_id) <= base_counts[qi]) continue;

                const int64_t latency = lat_gw->get_last_gateway_recv_ts(option_id) - t0;
                if (latency > 0) {
                    latencies.push_back(latency);
                    latencies_by_product[slot].push_back(latency);
                    quotes_by_product[slot]++;
                }
                seen[qi] = true;
                ++quotes_collected;
            }

            if (quotes_collected < EXPECTED_QUOTES) spin_pause();
        }

        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }

    engine->stop();

    std::sort(latencies.begin(), latencies.end());
    const size_t n = latencies.size();
    const size_t expected_total_quotes = static_cast<size_t>(N) * EXPECTED_QUOTES;
    const size_t expected_quotes_by_product[2] = {
        static_cast<size_t>((N + 1) / 2) * EXPECTED_QUOTES,
        static_cast<size_t>(N / 2) * EXPECTED_QUOTES,
    };
    const size_t min_total_quotes = static_cast<size_t>(expected_total_quotes * 0.45);
    const size_t min_quotes_by_product[2] = {
        static_cast<size_t>(expected_quotes_by_product[0] * 0.45),
        static_cast<size_t>(expected_quotes_by_product[1] * 0.45),
    };

    std::cout << "\n[LATENCY] Tick-to-gateway-quote over " << N
              << " future ticks, 2 products (cu/rb), 6 quote-path messages per injected future tick\n"
              << "[LATENCY] Quotes captured: " << n
              << " (" << (100.0 * n / expected_total_quotes) << "%)\n";
    std::cout << "[LATENCY] Product 0 quotes: " << quotes_by_product[0] << "\n"
              << "[LATENCY] Product 1 quotes: " << quotes_by_product[1] << "\n";

    if (n > 0) {
        std::cout << "[LATENCY] min:   " << latencies.front()            << " ns\n"
                  << "[LATENCY] p50:   " << percentile(latencies, 0.50)  << " ns\n"
                  << "[LATENCY] p95:   " << percentile(latencies, 0.95)  << " ns\n"
                  << "[LATENCY] p99:   " << percentile(latencies, 0.99)  << " ns\n"
                  << "[LATENCY] p99.9: " << percentile(latencies, 0.999) << " ns\n"
                  << "[LATENCY] max:   " << latencies.back()             << " ns\n";
    }
    for (int prod = 0; prod < 2; ++prod) {
        auto& prod_lat = latencies_by_product[prod];
        if (prod_lat.empty()) continue;
        std::sort(prod_lat.begin(), prod_lat.end());
        std::cout << "[LATENCY] product " << prod
                  << " p50=" << percentile(prod_lat, 0.50)
                  << " ns p99=" << percentile(prod_lat, 0.99)
                  << " ns count=" << prod_lat.size() << "\n";
    }

    // The synthetic alternating market does not force every option to emit a
    // fresh outbound message on every future tick, but it should still drive a
    // stable, balanced stream across both products.
    EXPECT_GT(n, min_total_quotes)
        << "Expected the benchmark to capture a stable quote-path message stream";
    EXPECT_GT(quotes_by_product[0], min_quotes_by_product[0])
        << "Expected product 0 to receive quotes under fair rotation";
    EXPECT_GT(quotes_by_product[1], min_quotes_by_product[1])
        << "Expected product 1 to receive quotes under fair rotation";
    if (n > 0) {
        EXPECT_GT(latencies.front(), 0LL) << "Latency must be positive";
        EXPECT_LT(percentile(latencies, 0.99), 100'000'000LL)
            << "p99 latency exceeded 100ms, something is very wrong";
    }
}
