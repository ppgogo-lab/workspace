#include <gtest/gtest.h>

#include "engine/trading_engine.h"
#include "gateway/sim_gateway.h"
#include "common/types.h"
#include "common/config.h"
#include "common/thread_utils.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

using namespace omm;

// ─── ASAN detection ───────────────────────────────────────────────────────────
#if defined(__SANITIZE_ADDRESS__)
#  define RUNNING_ASAN 1
#elif defined(__has_feature)
#  if __has_feature(address_sanitizer)
#    define RUNNING_ASAN 1
#  endif
#endif

// ─── LatencySimGateway ────────────────────────────────────────────────────────
// Extends SimGateway to record send_ts per instrument for latency measurement.
class LatencySimGateway : public SimGateway {
public:
    bool send_quote(const Quote& q) noexcept override {
        last_send_ts_[q.instrument_id].store(q.send_ts,
                                              std::memory_order_release);
        return SimGateway::send_quote(q);
    }

    int64_t get_last_send_ts(uint16_t id) const noexcept {
        return last_send_ts_[id].load(std::memory_order_acquire);
    }

private:
    std::atomic<int64_t> last_send_ts_[MAX_INSTRUMENTS]{};
};

// ─── Instrument helpers ───────────────────────────────────────────────────────

static Instrument make_future(uint16_t id, const char* code,
                               uint8_t product_idx) {
    Instrument f{};
    f.instrument_id = id;
    f.underlying_id = id;
    f.product_index = product_idx;
    f.kind          = InstrumentKind::Future;
    f.multiplier    = 1.0;
    f.tick_size     = 10.0;
    std::strncpy(f.code.data, code, sizeof(f.code.data) - 1);
    std::strncpy(f.underlying_code.data, code, sizeof(f.underlying_code.data) - 1);
    f.exchange      = Exchange::SHFE;
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
    o.instrument_id   = id;
    o.underlying_id   = underlying_id;
    o.product_index   = product_idx;
    o.kind            = InstrumentKind::Option;
    o.option_type     = otype;
    o.strike          = strike;
    o.tick_size       = tick_size;
    o.multiplier      = 1.0;
    o.exchange        = Exchange::SHFE;
    std::strncpy(o.underlying_code.data, underlying_code,
                 sizeof(o.underlying_code.data) - 1);
    o.expiry_epoch_ns = get_monotonic_ns()
                      + static_cast<int64_t>(0.25 * 365.0 * 24.0 * 3600.0 * 1e9);
    return o;
}

static MarketTick make_tick(uint16_t id, double last,
                             double bid, double ask) {
    MarketTick t{};
    t.instrument_id  = id;
    t.last_price     = last;
    t.bid_price[0]   = bid;
    t.ask_price[0]   = ask;
    t.bid_volume[0]  = 10;
    t.ask_volume[0]  = 10;
    t.recv_ts_ns     = get_monotonic_ns();
    t.exchange_ts_ns = t.recv_ts_ns;
    return t;
}

// ─── Percentile helper ────────────────────────────────────────────────────────
static int64_t percentile(std::vector<int64_t>& v, double p) {
    if (v.empty()) return 0;
    size_t idx = static_cast<size_t>(p * (v.size() - 1));
    return v[idx];
}

// ─── Latency test ─────────────────────────────────────────────────────────────
TEST(LatencyTest, TickToQuoteLatency) {
#ifdef RUNNING_ASAN
    GTEST_SKIP() << "Skipping latency measurement under ASAN (overhead distorts results)";
#endif

    // ── SystemConfig ──────────────────────────────────────────────────────────
    SystemConfig cfg{};
    cfg.product_count = 2;

    // Product 0: cu (copper, SHFE, ATM ~75000)
    std::strncpy(cfg.products[0].underlying_id.data, "cu2501", 31);
    cfg.products[0].params.bid_spread            = 50.0;   // 50 CNY half-spread
    cfg.products[0].params.ask_spread            = 50.0;
    cfg.products[0].params.quote_volume          = 5;
    cfg.products[0].params.max_position          = 500;
    cfg.products[0].params.enabled               = true;
    cfg.products[0].params.min_quote_interval_ms = 0;      // no rate limit in test
    cfg.products[0].strategy_core                = -1;     // no affinity

    // Product 1: rb (rebar, SHFE, ATM ~3500)
    std::strncpy(cfg.products[1].underlying_id.data, "rb2501", 31);
    cfg.products[1].params.bid_spread            = 5.0;    // 5 CNY half-spread
    cfg.products[1].params.ask_spread            = 5.0;
    cfg.products[1].params.quote_volume          = 5;
    cfg.products[1].params.max_position          = 500;
    cfg.products[1].params.enabled               = true;
    cfg.products[1].params.min_quote_interval_ms = 0;
    cfg.products[1].strategy_core                = -1;

    cfg.pricing.risk_free_rate       = 0.025;
    cfg.pricing.fit_interval_seconds = 1;   // fitter fires within 1s
    cfg.risk.hard.max_volume_per_order = 100;
    cfg.risk.soft.max_net_position     = 500;
    cfg.risk.soft.max_delta            = 100000;
    cfg.risk.soft.max_gamma            = 100000;
    cfg.risk.soft.max_vega             = 100000;
    cfg.timer.hedge_check_interval_ms  = 60000; // suppress hedge orders
    cfg.timer.quote_refresh_interval_ms = 60000;

    // All thread affinities off
    cfg.affinity.feed_core               = -1;
    cfg.affinity.pricer_core             = -1;
    cfg.affinity.gateway_dispatcher_core = -1;
    cfg.affinity.vol_fitter_core         = -1;
    cfg.affinity.risk_monitor_core       = -1;
    cfg.affinity.timer_core              = -1;

    // ── Instruments ───────────────────────────────────────────────────────────
    // cu: future id=0, options id=1..6
    // rb: future id=7, options id=8..13
    auto gw = std::make_unique<LatencySimGateway>();

    // cu future
    gw->add_instrument(make_future(0, "cu2501", 0));
    gw->set_last_price(0, 75000.0);

    // cu options (T=0.25, sigma~0.20, F=75000)
    // Approximate Black-76 mid prices for realistic bid/ask
    gw->add_instrument(make_option(1, 0, "cu2501", 0, 73000.0, OptionType::Call));
    gw->add_instrument(make_option(2, 0, "cu2501", 0, 74000.0, OptionType::Call));
    gw->add_instrument(make_option(3, 0, "cu2501", 0, 75000.0, OptionType::Call));
    gw->add_instrument(make_option(4, 0, "cu2501", 0, 73000.0, OptionType::Put));
    gw->add_instrument(make_option(5, 0, "cu2501", 0, 74000.0, OptionType::Put));
    gw->add_instrument(make_option(6, 0, "cu2501", 0, 75000.0, OptionType::Put));

    // Realistic cu option mid-prices (Black-76 approximations)
    gw->set_last_price(1, 2800.0);  // cu C73000 ITM call
    gw->set_last_price(2, 1900.0);  // cu C74000 call
    gw->set_last_price(3, 1200.0);  // cu C75000 ATM call
    gw->set_last_price(4,  400.0);  // cu P73000 OTM put
    gw->set_last_price(5,  900.0);  // cu P74000 put
    gw->set_last_price(6, 1200.0);  // cu P75000 ATM put

    // rb future
    gw->add_instrument(make_future(7, "rb2501", 1));
    gw->set_last_price(7, 3500.0);

    // rb options (T=0.25, sigma~0.18, F=3500)
    gw->add_instrument(make_option(8,  7, "rb2501", 1, 3400.0, OptionType::Call, 0.5));
    gw->add_instrument(make_option(9,  7, "rb2501", 1, 3500.0, OptionType::Call, 0.5));
    gw->add_instrument(make_option(10, 7, "rb2501", 1, 3600.0, OptionType::Call, 0.5));
    gw->add_instrument(make_option(11, 7, "rb2501", 1, 3400.0, OptionType::Put,  0.5));
    gw->add_instrument(make_option(12, 7, "rb2501", 1, 3500.0, OptionType::Put,  0.5));
    gw->add_instrument(make_option(13, 7, "rb2501", 1, 3600.0, OptionType::Put,  0.5));

    gw->set_last_price(8,  130.0);  // rb C3400 ITM call
    gw->set_last_price(9,   75.0);  // rb C3500 ATM call
    gw->set_last_price(10,  35.0);  // rb C3600 OTM call
    gw->set_last_price(11,  30.0);  // rb P3400 OTM put
    gw->set_last_price(12,  75.0);  // rb P3500 ATM put
    gw->set_last_price(13, 130.0);  // rb P3600 ITM put

    gw->connect(cfg.gateway);

    auto engine = std::make_unique<TradingEngine>(cfg, std::move(gw), nullptr);
    engine->start();

    // ── Pre-warm: inject option ticks for vol fitter, then future ticks ──────
    // Push option ticks with realistic bid/ask so vol fitter can invert to IV
    struct { uint16_t id; double last; double bid; double ask; } prewarm[] = {
        {1, 2800.0, 2750.0, 2850.0},
        {2, 1900.0, 1870.0, 1930.0},
        {3, 1200.0, 1180.0, 1220.0},
        {4,  400.0,  390.0,  410.0},
        {5,  900.0,  880.0,  920.0},
        {6, 1200.0, 1180.0, 1220.0},
        {8,  130.0,  127.0,  133.0},
        {9,   75.0,   73.0,   77.0},
        {10,  35.0,   34.0,   36.0},
        {11,  30.0,   29.0,   31.0},
        {12,  75.0,   73.0,   77.0},
        {13, 130.0,  127.0,  133.0},
    };

    for (auto& pw : prewarm) {
        auto tick = make_tick(pw.id, pw.last, pw.bid, pw.ask);
        (void)engine->tick_buf().try_push(tick);
    }

    // Wait for vol fitter to complete one cycle (fit_interval_seconds=1, wait 3s)
    std::this_thread::sleep_for(std::chrono::seconds(3));

    // ── Latency measurement loop ───────────────────────────────────────────────
    // Push FUTURE ticks (id=0 for cu, id=7 for rb) — each future tick reprices
    // all options for that product and emits N PricingSignals (one per option).
    // We alternate between cu and rb futures in round-robin.

    constexpr uint16_t FUTURE_IDS[] = {0, 7};       // cu2501, rb2501
    constexpr uint8_t  FUTURE_PROD[] = {0, 1};      // product indices
    constexpr double   FUTURE_MID[] = {75000.0, 3500.0};  // ATM forward prices

    constexpr int N = 5000;
    std::vector<int64_t> latencies;
    std::vector<int64_t> latencies_by_product[2];
    size_t quotes_by_product[2]{};
    latencies.reserve(N * 12);  // each future tick → 6 option quotes per product
    latencies_by_product[0].reserve(N * 6);
    latencies_by_product[1].reserve(N * 6);

    for (int i = 0; i < N; ++i) {
        int slot = i % 2;  // alternate cu/rb
        uint16_t fut_id = FUTURE_IDS[slot];
        uint8_t  prod   = FUTURE_PROD[slot];
        double   F      = FUTURE_MID[slot];

        MarketTick tick{};
        tick.instrument_id  = fut_id;
        tick.last_price     = F;
        tick.bid_price[0]   = F - 10.0;
        tick.ask_price[0]   = F + 10.0;
        tick.bid_volume[0]  = 100;
        tick.ask_volume[0]  = 100;
        tick.exchange_ts_ns = get_monotonic_ns();
        tick.recv_ts_ns     = tick.exchange_ts_ns;

        int64_t t0 = tick.recv_ts_ns;
        (void)engine->tick_buf().try_push(tick);

        // Spin-wait up to 5ms for quotes to appear (one future tick → 6 option quotes)
        constexpr int64_t TIMEOUT_NS = 5'000'000LL;
        int64_t deadline = t0 + TIMEOUT_NS;
        int quotes_collected = 0;
        constexpr int EXPECTED_QUOTES = 6;  // 6 options per product

        while (get_monotonic_ns() < deadline && quotes_collected < EXPECTED_QUOTES) {
            Quote q{};
            if (engine->quote_buf(prod).try_pop(q)) {
                int64_t latency = q.send_ts - t0;
                if (latency > 0) {
                    latencies.push_back(latency);
                    latencies_by_product[prod].push_back(latency);
                    quotes_by_product[prod]++;
                }
                quotes_collected++;
            }
        }

        // 200µs inter-tick sleep — 5kHz rate, avoids ring buffer saturation
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }

    engine->stop();

    // ── Report ────────────────────────────────────────────────────────────────
    std::sort(latencies.begin(), latencies.end());
    size_t n = latencies.size();
    size_t expected_total_quotes = N * 6; // 6 options per future tick

    std::cout << "\n[LATENCY] Tick-to-quote over " << N
              << " future ticks, 2 products (cu/rb), 12 option quotes per cycle\n"
              << "[LATENCY] Quotes captured: " << n
              << " (" << (100.0 * n / expected_total_quotes) << "%)\n";
    std::cout << "[LATENCY] Product 0 quotes: " << quotes_by_product[0] << "\n"
              << "[LATENCY] Product 1 quotes: " << quotes_by_product[1] << "\n";

    if (n > 0) {
        std::cout << "[LATENCY] min:   " << latencies.front()          << " ns\n"
                  << "[LATENCY] p50:   " << percentile(latencies, 0.50) << " ns\n"
                  << "[LATENCY] p95:   " << percentile(latencies, 0.95) << " ns\n"
                  << "[LATENCY] p99:   " << percentile(latencies, 0.99) << " ns\n"
                  << "[LATENCY] p99.9: " << percentile(latencies, 0.999)<< " ns\n"
                  << "[LATENCY] max:   " << latencies.back()            << " ns\n";
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

    // Correctness assertions (not SLA — WSL/dev environment has high jitter)
    EXPECT_GT(n, static_cast<size_t>(expected_total_quotes * 0.80))
        << "Expected at least 80% of option quotes to be captured";
    EXPECT_GT(quotes_by_product[0], static_cast<size_t>(N * 4))
        << "Expected product 0 to receive quotes under fair rotation";
    EXPECT_GT(quotes_by_product[1], static_cast<size_t>(N * 4))
        << "Expected product 1 to receive quotes under fair rotation";
    if (n > 0) {
        EXPECT_GT(latencies.front(), 0LL) << "Latency must be positive";
        // Sanity: p99 < 100ms (even under heavy WSL load)
        EXPECT_LT(percentile(latencies, 0.99), 100'000'000LL)
            << "p99 latency exceeded 100ms — something is very wrong";
    }
}
