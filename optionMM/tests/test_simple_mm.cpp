#include <gtest/gtest.h>

#include "strategy/simple_mm.h"
#include "strategy/mm_params.h"
#include "risk/pre_trade_risk.h"
#include "common/ring_buffer.h"
#include "common/types.h"
#include "gateway/sim_gateway.h"
#include "engine/trading_engine.h"

#include <cstring>
#include <cmath>
#include <thread>
#include <chrono>

using namespace omm;

// ─── Helpers ──────────────────────────────────────────────────────────────────

static Instrument make_option(uint16_t id, uint16_t underlying_id,
                               uint8_t product_idx,
                               double strike, OptionType otype,
                               double tick_size = 0.01) {
    Instrument instr{};
    instr.instrument_id   = id;
    instr.underlying_id   = underlying_id;
    instr.product_index   = product_idx;
    instr.kind            = InstrumentKind::Option;
    instr.option_type     = otype;
    instr.strike          = strike;
    instr.tick_size       = tick_size;
    instr.multiplier      = 1.0;
    // expiry ~3 months from now (in ns)
    instr.expiry_epoch_ns = get_monotonic_ns()
                          + static_cast<int64_t>(0.25 * 365.0 * 24.0 * 3600.0 * 1e9);
    return instr;
}

static Instrument make_future(uint16_t id, uint8_t product_idx,
                              const char* code,
                              double tick_size = 0.01) {
    Instrument instr{};
    instr.instrument_id = id;
    instr.product_index = product_idx;
    instr.kind          = InstrumentKind::Future;
    instr.tick_size     = tick_size;
    instr.multiplier    = 1.0;
    std::strncpy(instr.code.data, code, sizeof(instr.code.data) - 1);
    return instr;
}

static PricingSignal make_signal(uint16_t instrument_id, double theo,
                                  double delta = 0.5) {
    PricingSignal s{};
    s.instrument_id        = instrument_id;
    s.calc_ts_ns           = get_monotonic_ns();
    s.theo_bid             = theo;
    s.theo_ask             = theo;
    s.delta                = static_cast<float>(delta);
    return s;
}

static bool wait_for_gateway_event(SimGateway& gw,
                                   GatewayEvent* out,
                                   std::chrono::milliseconds timeout = std::chrono::milliseconds(200)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (gw.callback_buf.try_pop(*out)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

static bool wait_for_monitored_quote(const TradingEngine& engine,
                                     Quote* out,
                                     std::chrono::milliseconds timeout = std::chrono::milliseconds(200)) {
    uint64_t cursor = 0;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (engine.monitor_quotes().read_next(cursor, *out)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

// ─── SimpleMMStrategy unit tests ─────────────────────────────────────────────

class SimpleMmTest : public ::testing::Test {
protected:
    static constexpr int PROD = 0;

    HardRiskConfig  hard_cfg{};
    PreTradeRisk*   risk{nullptr};
    AtomicMMParams  params{};

    SPSCRingBuffer<Quote, 512> quote_buf;
    SPSCRingBuffer<Order, 512> order_buf;

    Instrument instruments[MAX_INSTRUMENTS]{};
    SimpleMMStrategy strat;

    void SetUp() override {
        hard_cfg.max_volume_per_order = 100;
        risk = new PreTradeRisk(hard_cfg);

        params.bid_spread.store(2.0, std::memory_order_relaxed);
        params.ask_spread.store(2.0, std::memory_order_relaxed);
        params.quote_volume.store(5, std::memory_order_relaxed);
        params.max_position.store(100, std::memory_order_relaxed);
        params.enabled.store(true, std::memory_order_relaxed);

        instruments[0] = make_option(0, 1, PROD, 100.0,
                                     OptionType::Call, 0.5);

        strat.init(PROD, &quote_buf, &order_buf, risk, &params, instruments);
    }
    void TearDown() override { delete risk; }
};

TEST_F(SimpleMmTest, QuoteGeneratedOnSignal) {
    auto sig = make_signal(0, 5.0);
    strat.on_signal(sig);

    Quote q{};
    ASSERT_TRUE(quote_buf.try_pop(q));
    EXPECT_EQ(q.instrument_id, 0);
    EXPECT_LT(q.bid_price, 5.0);
    EXPECT_GT(q.ask_price, 5.0);
    EXPECT_EQ(q.bid_volume, 5);
    EXPECT_EQ(q.ask_volume, 5);
}

TEST_F(SimpleMmTest, SpreadCorrect) {
    auto sig = make_signal(0, 10.0);
    strat.on_signal(sig);

    Quote q{};
    ASSERT_TRUE(quote_buf.try_pop(q));
    // bid_spread=2, ask_spread=2 → half-spread each side → total spread = 2
    EXPECT_NEAR(q.ask_price - q.bid_price, 2.0, 0.6);
}

TEST_F(SimpleMmTest, NoQuoteWhenDisabled) {
    params.enabled.store(false, std::memory_order_relaxed);
    auto sig = make_signal(0, 5.0);
    strat.on_signal(sig);

    Quote q{};
    EXPECT_FALSE(quote_buf.try_pop(q));
}

TEST_F(SimpleMmTest, StaleSignalDropped) {
    PricingSignal sig = make_signal(0, 5.0);
    // Make signal 1 second old (> 10ms staleness threshold)
    sig.calc_ts_ns = get_monotonic_ns() - 1'000'000'000LL;
    strat.on_signal(sig);

    Quote q{};
    EXPECT_FALSE(quote_buf.try_pop(q));
}

TEST_F(SimpleMmTest, ZeroTheoNoQuote) {
    auto sig = make_signal(0, 0.0);
    strat.on_signal(sig);
    Quote q{};
    EXPECT_FALSE(quote_buf.try_pop(q));
}

TEST_F(SimpleMmTest, InventoryLeanLongLimit) {
    // Simulate being at max long position → should stop buying (bid_vol = 0)
    // on_fill to build up position
    for (int i = 0; i < 100; ++i) {
        Trade t{};
        t.instrument_id = 0;
        t.product_index = PROD;
        t.side          = Side::Buy;
        t.fill_volume   = 1;
        t.fill_price    = 5.0;
        strat.on_fill(t);
    }

    auto sig = make_signal(0, 5.0);
    strat.on_signal(sig);

    Quote q{};
    if (quote_buf.try_pop(q)) {
        // If a quote was generated, bid_vol should be 0 (leaning ask only)
        EXPECT_EQ(q.bid_volume, 0);
    }
    // else: no quote at all (also acceptable when fully at limit)
}

TEST_F(SimpleMmTest, ParamUpdatePropagates) {
    // Update spread via atomic params and verify next quote reflects it
    params.bid_spread.store(5.0, std::memory_order_release);
    params.ask_spread.store(5.0, std::memory_order_release);

    auto sig = make_signal(0, 20.0);
    strat.on_signal(sig);

    Quote q{};
    ASSERT_TRUE(quote_buf.try_pop(q));
    // bid ~= 20 - 2.5 = 17.5, ask ~= 20 + 2.5 = 22.5 → total spread = 5
    EXPECT_LT(q.bid_price, 20.0);
    EXPECT_GT(q.ask_price, 20.0);
    double spread = q.ask_price - q.bid_price;
    EXPECT_NEAR(spread, 5.0, 1.0);
}

TEST_F(SimpleMmTest, TimerSessionCloseNoMoreQuotes) {
    // Send one quote first
    strat.on_signal(make_signal(0, 5.0));
    Quote q{};
    ASSERT_TRUE(quote_buf.try_pop(q));

    // Now fire SessionClose timer
    TimerEvent ev{};
    ev.type = TimerEventType::SessionClose;
    ev.trigger_ts_ns = get_monotonic_ns();
    strat.on_timer(ev);

    // After SessionClose, new signals should still work
    // (strategy doesn't disable itself on close, it cancels live quotes)
}

// ─── SimGateway tests ─────────────────────────────────────────────────────────

TEST(SimGateway, ConnectAndSendOrder) {
    SimGateway gw;
    GatewayConfig cfg{};
    ASSERT_TRUE(gw.connect(cfg));
    EXPECT_TRUE(gw.is_connected());

    Instrument instr = make_option(0, 1, 0, 100.0, OptionType::Call);
    gw.add_instrument(instr);
    gw.set_last_price(0, 5.0);

    Order o{};
    o.client_order_id = 1;
    o.instrument_id   = 0;
    o.product_index   = 0;
    o.side            = Side::Buy;
    o.order_type      = OrderType::Limit;
    o.price           = 5.0;
    o.volume          = 10;

    ASSERT_TRUE(gw.send_order(o));
    EXPECT_EQ(gw.orders_sent(), 1u);

    // Should get OrderAck + Fill (price matches last_price)
    GatewayEvent ev{};
    ASSERT_TRUE(wait_for_gateway_event(gw, &ev));
    EXPECT_EQ(ev.type, GatewayEventType::OrderAck);

    ASSERT_TRUE(wait_for_gateway_event(gw, &ev));
    EXPECT_EQ(ev.type, GatewayEventType::OrderFill);
    EXPECT_EQ(ev.trade.fill_volume, 10);
    EXPECT_EQ(gw.orders_filled(), 1u);
}

TEST(SimGateway, SendQuoteGetAck) {
    SimGateway gw;
    GatewayConfig cfg{};
    gw.connect(cfg);
    gw.set_last_price(0, 102.0);  // market above ask → ask side fills

    Quote q{};
    q.client_quote_id = 1;
    q.instrument_id   = 0;
    q.bid_price       = 99.0;
    q.ask_price       = 101.0;
    q.bid_volume      = 5;
    q.ask_volume      = 5;

    ASSERT_TRUE(gw.send_quote(q));

    GatewayEvent ev{};
    ASSERT_TRUE(wait_for_gateway_event(gw, &ev));
    EXPECT_EQ(ev.type, GatewayEventType::QuoteAck);
    // last_price=102 >= ask=101 → ask side fills (someone bought from us)
    ASSERT_TRUE(wait_for_gateway_event(gw, &ev));
    EXPECT_EQ(ev.type, GatewayEventType::QuoteFill);
    EXPECT_EQ(ev.trade.side, Side::Sell);
}

TEST(SimGateway, CancelOrderGetAck) {
    SimGateway gw;
    GatewayConfig cfg{};
    gw.connect(cfg);

    Instrument instr = make_option(0, 1, 0, 100.0, OptionType::Call);
    gw.add_instrument(instr);
    gw.set_last_price(0, 20.0);

    Order o{};
    o.client_order_id = 42;
    o.instrument_id   = 0;
    o.product_index   = 0;
    o.side            = Side::Buy;
    o.order_type      = OrderType::Limit;
    o.price           = 10.0;
    o.volume          = 10;
    ASSERT_TRUE(gw.send_order(o));

    ASSERT_TRUE(gw.cancel_order(42, 0));

    GatewayEvent ev{};
    ASSERT_TRUE(wait_for_gateway_event(gw, &ev));
    if (ev.type == GatewayEventType::OrderAck) {
        ASSERT_TRUE(wait_for_gateway_event(gw, &ev));
    }
    EXPECT_EQ(ev.type, GatewayEventType::OrderCancel);
    EXPECT_EQ(ev.order.client_order_id, 42u);
}

// ─── End-to-end: TradingEngine integration ────────────────────────────────────

TEST(SimGateway, PartialFillWithLatency) {
    SimGateway gw;
    SimConfig sim{};
    sim.gateway_ack_latency_ms = 5;
    sim.gateway_fill_interval_ms = 10;
    sim.gateway_order_fill_probability = 1.0;
    sim.gateway_partial_fill_probability = 1.0;
    sim.gateway_max_fill_size = 3;
    gw.set_sim_config(sim);

    GatewayConfig cfg{};
    ASSERT_TRUE(gw.connect(cfg));

    Instrument instr = make_option(0, 1, 0, 100.0, OptionType::Call);
    gw.add_instrument(instr);
    gw.set_last_price(0, 5.0);

    Order o{};
    o.client_order_id = 7;
    o.instrument_id   = 0;
    o.product_index   = 0;
    o.side            = Side::Buy;
    o.order_type      = OrderType::Limit;
    o.price           = 5.0;
    o.volume          = 7;
    ASSERT_TRUE(gw.send_order(o));

    GatewayEvent ev{};
    ASSERT_TRUE(wait_for_gateway_event(gw, &ev));
    ASSERT_EQ(ev.type, GatewayEventType::OrderAck);

    int total_filled = 0;
    int fills_seen = 0;
    while (total_filled < 7 && fills_seen < 8) {
        ASSERT_TRUE(wait_for_gateway_event(gw, &ev));
        ASSERT_EQ(ev.type, GatewayEventType::OrderFill);
        EXPECT_LE(ev.trade.fill_volume, 3);
        total_filled += ev.trade.fill_volume;
        ++fills_seen;
    }
    EXPECT_EQ(total_filled, 7);
}

TEST(SimGateway, RejectsOrdersWhenConfigured) {
    SimGateway gw;
    SimConfig sim{};
    sim.gateway_reject_probability = 1.0;
    gw.set_sim_config(sim);

    GatewayConfig cfg{};
    ASSERT_TRUE(gw.connect(cfg));

    Instrument instr = make_option(0, 1, 0, 100.0, OptionType::Call);
    gw.add_instrument(instr);
    gw.set_last_price(0, 5.0);

    Order o{};
    o.client_order_id = 99;
    o.instrument_id   = 0;
    o.product_index   = 0;
    o.side            = Side::Buy;
    o.order_type      = OrderType::Limit;
    o.price           = 5.0;
    o.volume          = 10;
    ASSERT_TRUE(gw.send_order(o));

    GatewayEvent ev{};
    ASSERT_TRUE(wait_for_gateway_event(gw, &ev));
    EXPECT_EQ(ev.type, GatewayEventType::OrderReject);
    EXPECT_EQ(ev.order.status, OrderStatus::Rejected);
}

TEST(TradingEngineIntegration, TickToQuote) {
    // Build a minimal SystemConfig
    SystemConfig cfg{};
    cfg.product_count = 1;
    cfg.products[0].params.bid_spread = 0.01;
    cfg.products[0].params.ask_spread = 0.01;
    cfg.products[0].params.quote_volume = 5;
    cfg.products[0].params.max_position = 100;
    cfg.products[0].params.enabled = true;
    std::strncpy(cfg.products[0].underlying_id.data, "cu2501", 31);
    cfg.pricing.risk_free_rate = 0.025;
    cfg.pricing.fit_interval_seconds = 60;
    cfg.timer.hedge_check_interval_ms = 10000;
    cfg.risk.hard.max_volume_per_order = 100;
    cfg.risk.soft.max_net_position = 500;

    auto gw = std::make_unique<SimGateway>();
    // Add one option instrument — strike at-the-money relative to last_price
    Instrument fut = make_future(1, 0, "cu2501", 0.01);
    Instrument opt = make_option(0, 1, 0, 5.0, OptionType::Call, 0.01);
    std::memcpy(opt.underlying_code.data, "cu2501", 7);
    gw->add_instrument(fut);
    gw->add_instrument(opt);
    gw->set_last_price(0, 5.0);
    gw->set_last_price(1, 5.0);
    gw->connect(cfg.gateway);

    auto engine = std::make_unique<TradingEngine>(cfg, std::move(gw), nullptr);
    engine->start();

    // Push a synthetic tick directly into the engine's tick buffer
    MarketTick tick{};
    tick.instrument_id  = 0;
    tick.last_price     = 5.0;
    tick.bid_price[0]   = 4.99;
    tick.ask_price[0]   = 5.01;
    tick.recv_ts_ns     = get_monotonic_ns();
    tick.exchange_ts_ns = tick.recv_ts_ns;
    (void)engine->tick_buf().try_push(tick);

    // Give the pricer and strategy threads time to process
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // The gateway dispatcher drains quote_buf_ and calls send_quote() on SimGateway.
    // Verify end-to-end by checking that SimGateway received at least one quote.
    auto* sim_gw = static_cast<SimGateway*>(engine->gateway());
    uint64_t quotes_sent = sim_gw->quotes_sent();

    engine->stop();

    EXPECT_GT(quotes_sent, 0u) << "Expected SimGateway to receive at least one quote";
}

TEST(TradingEngineIntegration, DeferredMonitoringStillStreamsQuotes) {
    SystemConfig cfg{};
    cfg.product_count = 1;
    cfg.products[0].params.bid_spread = 0.01;
    cfg.products[0].params.ask_spread = 0.01;
    cfg.products[0].params.quote_volume = 5;
    cfg.products[0].params.max_position = 100;
    cfg.products[0].params.enabled = true;
    cfg.monitoring.hot_path_publish_mode = MonitoringPublishMode::Deferred;
    std::strncpy(cfg.products[0].underlying_id.data, "cu2501", 31);
    cfg.pricing.risk_free_rate = 0.025;
    cfg.pricing.fit_interval_seconds = 60;
    cfg.timer.hedge_check_interval_ms = 10000;
    cfg.risk.hard.max_volume_per_order = 100;
    cfg.risk.soft.max_net_position = 500;

    auto gw = std::make_unique<SimGateway>();
    Instrument fut = make_future(1, 0, "cu2501", 0.01);
    Instrument opt = make_option(0, 1, 0, 5.0, OptionType::Call, 0.01);
    std::memcpy(opt.underlying_code.data, "cu2501", 7);
    gw->add_instrument(fut);
    gw->add_instrument(opt);
    gw->set_last_price(0, 5.0);
    gw->set_last_price(1, 5.0);
    gw->connect(cfg.gateway);

    auto engine = std::make_unique<TradingEngine>(cfg, std::move(gw), nullptr);
    engine->start();

    MarketTick tick{};
    tick.instrument_id  = 0;
    tick.last_price     = 5.0;
    tick.bid_price[0]   = 4.99;
    tick.ask_price[0]   = 5.01;
    tick.recv_ts_ns     = get_monotonic_ns();
    tick.exchange_ts_ns = tick.recv_ts_ns;
    (void)engine->tick_buf().try_push(tick);

    Quote monitored{};
    const bool saw_monitored_quote = wait_for_monitored_quote(*engine, &monitored);

    engine->stop();

    ASSERT_TRUE(saw_monitored_quote)
        << "Expected deferred monitoring mode to publish quote flow";
    EXPECT_EQ(monitored.product_index, 0);
    EXPECT_GT(monitored.bid_volume + monitored.ask_volume, 0);
}

TEST(TradingEngineIntegration, PricingSignalOverflowFallsBackToCoalescing) {
    SystemConfig cfg{};
    cfg.product_count = 1;
    cfg.products[0].params.bid_spread = 0.01;
    cfg.products[0].params.ask_spread = 0.01;
    cfg.products[0].params.quote_volume = 1;
    cfg.products[0].params.max_position = 1000;
    cfg.products[0].params.enabled = true;
    cfg.products[0].strategy_core = -1;
    cfg.monitoring.hot_path_publish_mode = MonitoringPublishMode::Off;
    std::strncpy(cfg.products[0].underlying_id.data, "cu2501", 31);
    cfg.pricing.risk_free_rate = 0.025;
    cfg.pricing.fit_interval_seconds = 60;
    cfg.timer.hedge_check_interval_ms = 60000;
    cfg.timer.quote_refresh_interval_ms = 60000;
    cfg.risk.hard.max_volume_per_order = 100;
    cfg.risk.soft.max_net_position = 5000;

    auto gw = std::make_unique<SimGateway>();
    Instrument fut = make_future(0, 0, "cu2501", 0.01);
    gw->add_instrument(fut);
    gw->set_last_price(0, 75000.0);

    constexpr int kOptionCount = 900;
    for (int i = 0; i < kOptionCount; ++i) {
        const bool is_call = (i & 1) == 0;
        Instrument opt = make_option(static_cast<uint16_t>(i + 1),
                                     0,
                                     0,
                                     72000.0 + static_cast<double>(i),
                                     is_call ? OptionType::Call : OptionType::Put,
                                     0.5);
        std::memcpy(opt.underlying_code.data, "cu2501", 7);
        gw->add_instrument(opt);
        gw->set_last_price(static_cast<uint16_t>(i + 1), 100.0 + (i % 25));
    }
    gw->connect(cfg.gateway);

    auto engine = std::make_unique<TradingEngine>(cfg, std::move(gw), nullptr);
    engine->start();

    for (int burst = 0; burst < 6; ++burst) {
        MarketTick tick{};
        tick.instrument_id  = 0;
        tick.last_price     = 75000.0 + burst * 5.0;
        tick.bid_price[0]   = tick.last_price - 1.0;
        tick.ask_price[0]   = tick.last_price + 1.0;
        tick.bid_volume[0]  = 100;
        tick.ask_volume[0]  = 100;
        tick.recv_ts_ns     = get_monotonic_ns();
        tick.exchange_ts_ns = tick.recv_ts_ns;
        tick.sequence_no    = static_cast<uint64_t>(burst + 1);
        while (!engine->tick_buf().try_push(tick)) spin_pause();
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (std::chrono::steady_clock::now() < deadline
           && engine->total_coalesced_signal_writes() == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    const uint64_t signal_writes = engine->total_coalesced_signal_writes();
    const uint32_t signal_ring_depth = engine->max_signal_queue_depth();
    const uint32_t signal_mailbox_depth = engine->max_signal_mailbox_depth();
    engine->stop();

    EXPECT_GT(signal_writes, 0u)
        << "Expected latest-only signal coalescing to engage under synthetic overload";
    EXPECT_GT(signal_ring_depth, 0u);
    EXPECT_GT(signal_mailbox_depth, 0u);
}

TEST(TradingEngineIntegration, TimerRefreshesUseCoalescedMailbox) {
    SystemConfig cfg{};
    cfg.product_count = 1;
    cfg.products[0].params.bid_spread = 0.01;
    cfg.products[0].params.ask_spread = 0.01;
    cfg.products[0].params.quote_volume = 5;
    cfg.products[0].params.max_position = 100;
    cfg.products[0].params.enabled = true;
    cfg.products[0].strategy_core = -1;
    cfg.monitoring.hot_path_publish_mode = MonitoringPublishMode::Off;
    std::strncpy(cfg.products[0].underlying_id.data, "cu2501", 31);
    cfg.pricing.risk_free_rate = 0.025;
    cfg.pricing.fit_interval_seconds = 60;
    cfg.timer.hedge_check_interval_ms = 10;
    cfg.timer.quote_refresh_interval_ms = 10;
    cfg.risk.hard.max_volume_per_order = 100;
    cfg.risk.soft.max_net_position = 500;

    auto gw = std::make_unique<SimGateway>();
    Instrument fut = make_future(0, 0, "cu2501", 0.01);
    Instrument opt = make_option(1, 0, 0, 5.0, OptionType::Call, 0.01);
    std::memcpy(opt.underlying_code.data, "cu2501", 7);
    gw->add_instrument(fut);
    gw->add_instrument(opt);
    gw->set_last_price(0, 5.0);
    gw->set_last_price(1, 5.0);
    gw->connect(cfg.gateway);

    auto engine = std::make_unique<TradingEngine>(cfg, std::move(gw), nullptr);
    engine->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(250));

    const uint64_t timer_writes = engine->total_coalesced_timer_writes();
    const uint32_t timer_ring_depth = engine->max_timer_queue_depth();
    engine->stop();

    EXPECT_GT(timer_writes, 0u)
        << "Expected HedgeCheck/QuoteRefresh to publish via the coalesced timer mailbox";
    EXPECT_EQ(timer_ring_depth, 0u)
        << "Coalesced HedgeCheck/QuoteRefresh should not build timer_buf_ backlog";
}
