#include <gtest/gtest.h>

#include "strategy/option_mm_core.h"
#include "strategy/mm_params.h"
#include "risk/pre_trade_risk.h"
#include "risk/post_trade_risk.h"
#include "common/ring_buffer.h"
#include "common/types.h"
#include "monitoring/topic.h"

#include <cstring>
#include <string>

using namespace omm;

namespace {

const char* exchange_name(Exchange exchange) {
    switch (exchange) {
    case Exchange::SHFE: return "SHFE";
    case Exchange::DCE: return "DCE";
    case Exchange::CZCE: return "CZCE";
    case Exchange::CFFEX: return "CFFEX";
    case Exchange::GFEX: return "GFEX";
    default: return "UNKNOWN";
    }
}

Instrument make_future(uint16_t id,
                       uint8_t product_idx,
                       const char* code,
                       Exchange exchange = Exchange::GFEX) {
    Instrument instr{};
    instr.instrument_id = id;
    instr.product_index = product_idx;
    instr.kind = InstrumentKind::Future;
    instr.exchange = exchange;
    instr.tick_size = 1.0;
    instr.multiplier = 1.0;
    std::strncpy(instr.code.data, code, sizeof(instr.code.data) - 1);
    std::strncpy(instr.underlying_code.data, code, sizeof(instr.underlying_code.data) - 1);
    std::strncpy(instr.exchange_id.data, exchange_name(exchange), sizeof(instr.exchange_id.data) - 1);
    instr.expiry_epoch_ns = get_monotonic_ns() + static_cast<int64_t>(0.25 * 365.0 * 24.0 * 3600.0 * 1e9);
    return instr;
}

Instrument make_option(uint16_t id, uint16_t underlying_id, uint8_t product_idx,
                       OptionType type, double strike,
                       Exchange exchange = Exchange::GFEX) {
    Instrument instr{};
    instr.instrument_id = id;
    instr.underlying_id = underlying_id;
    instr.product_index = product_idx;
    instr.kind = InstrumentKind::Option;
    instr.option_type = type;
    instr.exchange = exchange;
    instr.tick_size = 0.5;
    instr.multiplier = 1.0;
    instr.strike = strike;
    std::strncpy(instr.exchange_id.data, exchange_name(exchange), sizeof(instr.exchange_id.data) - 1);
    instr.expiry_epoch_ns = get_monotonic_ns() + static_cast<int64_t>(0.25 * 365.0 * 24.0 * 3600.0 * 1e9);
    return instr;
}

PricingSignal make_signal(uint16_t instrument_id,
                          double theo_bid,
                          double theo_ask,
                          double delta = 0.4,
                          double vega = 0.2,
                          float underlying_bid = 99.0F,
                          float underlying_ask = 101.0F) {
    PricingSignal sig{};
    sig.instrument_id = instrument_id;
    sig.underlying_id = 0;
    sig.flags = PricingFlagHasUnderlyingRef;
    sig.theo_bid = theo_bid;
    sig.theo_ask = theo_ask;
    sig.delta = static_cast<float>(delta);
    sig.vega = static_cast<float>(vega);
    sig.calc_ts_ns = get_monotonic_ns();
    sig.underlying_ref_bid = underlying_bid;
    sig.underlying_ref_ask = underlying_ask;
    return sig;
}

} // namespace

class OptionMmCoreTest : public ::testing::Test {
protected:
    static constexpr uint8_t PROD = 0;

    HardRiskConfig hard_cfg{};
    SoftRiskConfig soft_cfg{};
    PreTradeRisk pre_risk{hard_cfg};
    PostTradeRisk post_risk{soft_cfg};
    AtomicMMParams params{};
    SPSCRingBuffer<Quote, 512> quote_buf;
    SPSCRingBuffer<Order, 512> order_buf;
    MonitoringTopic<SystemAlert, 256> alert_topic;
    Instrument instruments[MAX_INSTRUMENTS]{};
    SnapshotArray<TopOfBookTick, MAX_INSTRUMENTS> tick_snapshot;
    OptionMMCoreStrategy strat;

    void SetUp() override {
        hard_cfg.max_volume_per_order = 100;
        soft_cfg.max_net_position = 1000;
        soft_cfg.max_delta = 1000.0;
        soft_cfg.max_gamma = 1000.0;
        soft_cfg.max_vega = 1000.0;
        post_risk.set_limits(soft_cfg);

        params.enabled.store(true, std::memory_order_relaxed);
        params.quote_volume.store(5, std::memory_order_relaxed);
        params.max_position.store(20, std::memory_order_relaxed);
        params.warning_position.store(5, std::memory_order_relaxed);
        params.base_half_spread_ticks.store(2.0, std::memory_order_relaxed);
        params.min_half_spread_ticks.store(1.0, std::memory_order_relaxed);
        params.max_half_spread_ticks.store(6.0, std::memory_order_relaxed);
        params.follow_weight.store(0.3, std::memory_order_relaxed);
        params.inventory_skew_per_lot_ticks.store(0.05, std::memory_order_relaxed);
        params.requote_price_epsilon_ticks.store(1.0, std::memory_order_relaxed);
        params.market_width_widen_threshold_ticks.store(4.0, std::memory_order_relaxed);
        params.min_quote_interval_ms.store(0.0, std::memory_order_relaxed);
        params.use_one_sided_at_limits.store(true, std::memory_order_relaxed);

        instruments[0] = make_future(0, PROD, "cu2501");
        instruments[1] = make_option(1, 0, PROD, OptionType::Call, 100.0);
        instruments[2] = make_option(2, 0, PROD, OptionType::Put, 100.0);

        TopOfBookTick future{};
        future.instrument_id = 0;
        future.recv_ts_ns = get_monotonic_ns();
        future.bid_price = 99.0;
        future.ask_price = 101.0;
        future.last_price = 100.0;
        tick_snapshot.publish(0, future);

        TopOfBookTick call{};
        call.instrument_id = 1;
        call.recv_ts_ns = get_monotonic_ns();
        call.bid_price = 9.5;
        call.ask_price = 10.5;
        call.bid_volume = 10;
        call.ask_volume = 10;
        tick_snapshot.publish(1, call);

        TopOfBookTick put{};
        put.instrument_id = 2;
        put.recv_ts_ns = get_monotonic_ns();
        put.bid_price = 7.5;
        put.ask_price = 8.5;
        put.bid_volume = 12;
        put.ask_volume = 12;
        tick_snapshot.publish(2, put);

        strat.init(PROD, &quote_buf, &order_buf, &pre_risk, &params,
                   instruments, &tick_snapshot, &post_risk, &alert_topic);
    }
};

TEST_F(OptionMmCoreTest, GeneratesQuoteFromValidSignal) {
    strat.on_signal(make_signal(1, 9.8, 10.2));

    Quote quote{};
    ASSERT_TRUE(quote_buf.try_pop(quote));
    EXPECT_EQ(quote.instrument_id, 1);
    EXPECT_GT(quote.ask_price, quote.bid_price);
    EXPECT_EQ(quote.bid_volume, 5);
    EXPECT_EQ(quote.ask_volume, 5);
}

TEST_F(OptionMmCoreTest, HedgeFillClearsPreTradeRiskOpenOrder) {
    params.product_delta_threshold.store(1.0, std::memory_order_relaxed);
    strat.on_signal(make_signal(1, 9.8, 10.2, 5.0));

    Quote quote{};
    ASSERT_TRUE(quote_buf.try_pop(quote));

    Trade option_fill{};
    option_fill.instrument_id = 1;
    option_fill.product_index = PROD;
    option_fill.side = Side::Buy;
    option_fill.fill_volume = 1;
    strat.on_fill(option_fill);

    Order hedge{};
    ASSERT_TRUE(order_buf.try_pop(hedge));
    EXPECT_TRUE(hedge.is_hedge);

    strat.on_order_ack(hedge);
    EXPECT_EQ(pre_risk.open_order_count(), 1);

    Trade hedge_fill{};
    hedge_fill.client_order_id = hedge.client_order_id;
    hedge_fill.instrument_id = hedge.instrument_id;
    hedge_fill.product_index = PROD;
    hedge_fill.side = hedge.side;
    hedge_fill.fill_volume = hedge.volume;
    strat.on_fill(hedge_fill);

    EXPECT_EQ(pre_risk.open_order_count(), 0);
}

TEST_F(OptionMmCoreTest, SwitchesToOneSidedQuoteNearWarningPosition) {
    for (int i = 0; i < 5; ++i) {
        Trade trade{};
        trade.instrument_id = 1;
        trade.product_index = PROD;
        trade.side = Side::Buy;
        trade.fill_volume = 1;
        strat.on_fill(trade);
    }

    strat.on_signal(make_signal(1, 9.8, 10.2));

    Quote quote{};
    ASSERT_TRUE(quote_buf.try_pop(quote));
    EXPECT_EQ(quote.bid_volume, 0);
    EXPECT_EQ(quote.ask_volume, 5);
}

TEST_F(OptionMmCoreTest, TapersRiskySideVolumeBeforeWarningPosition) {
    for (int i = 0; i < 3; ++i) {
        Trade trade{};
        trade.instrument_id = 1;
        trade.product_index = PROD;
        trade.side = Side::Buy;
        trade.fill_volume = 1;
        strat.on_fill(trade);
    }

    strat.on_signal(make_signal(1, 9.8, 10.2));

    Quote quote{};
    ASSERT_TRUE(quote_buf.try_pop(quote));
    EXPECT_GT(quote.bid_volume, 0);
    EXPECT_LT(quote.bid_volume, quote.ask_volume);
    EXPECT_EQ(quote.ask_volume, 5);
}

TEST_F(OptionMmCoreTest, LocalFillOnlyTouchesFilledInstrument) {
    params.inventory_skew_per_lot_ticks.store(1.0, std::memory_order_relaxed);
    params.requote_price_epsilon_ticks.store(0.0, std::memory_order_relaxed);

    strat.on_signal(make_signal(1, 9.8, 10.2));
    strat.on_signal(make_signal(2, 7.8, 8.2));

    Quote quote1{};
    Quote quote2{};
    ASSERT_TRUE(quote_buf.try_pop(quote1));
    ASSERT_TRUE(quote_buf.try_pop(quote2));
    strat.on_quote_ack(quote1);
    strat.on_quote_ack(quote2);

    Trade fill{};
    fill.instrument_id = 1;
    fill.product_index = PROD;
    fill.side = Side::Buy;
    fill.fill_volume = 1;
    strat.on_fill(fill);

    Quote cancel{};
    ASSERT_TRUE(quote_buf.try_pop(cancel));
    EXPECT_EQ(cancel.instrument_id, 1);
    EXPECT_EQ(cancel.bid_volume, 0);
    EXPECT_EQ(cancel.ask_volume, 0);

    Quote extra{};
    EXPECT_FALSE(quote_buf.try_pop(extra));
}

TEST_F(OptionMmCoreTest, SuppressesWholeProductAndTriggersHedgeOnDeltaBreach) {
    params.product_delta_threshold.store(1.0, std::memory_order_relaxed);
    params.quote_volume.store(3, std::memory_order_relaxed);

    strat.on_signal(make_signal(1, 9.8, 10.2, 0.5));

    Quote initial{};
    ASSERT_TRUE(quote_buf.try_pop(initial));

    Trade fill{};
    fill.instrument_id = 1;
    fill.product_index = PROD;
    fill.side = Side::Buy;
    fill.fill_volume = 4;
    strat.on_fill(fill);

    Quote cancel{};
    ASSERT_TRUE(quote_buf.try_pop(cancel));
    EXPECT_EQ(cancel.bid_volume, 0);
    EXPECT_EQ(cancel.ask_volume, 0);

    Order hedge{};
    ASSERT_TRUE(order_buf.try_pop(hedge));
    EXPECT_EQ(hedge.instrument_id, 0);
    EXPECT_EQ(hedge.side, Side::Sell);
    EXPECT_TRUE(hedge.is_hedge);
}

TEST_F(OptionMmCoreTest, ExposureRecoveryRequotesWholeProduct) {
    params.product_delta_threshold.store(1.0, std::memory_order_relaxed);
    params.quote_volume.store(3, std::memory_order_relaxed);

    strat.on_signal(make_signal(1, 9.8, 10.2, 0.5));
    strat.on_signal(make_signal(2, 7.8, 8.2, 0.3));

    Quote initial1{};
    Quote initial2{};
    ASSERT_TRUE(quote_buf.try_pop(initial1));
    ASSERT_TRUE(quote_buf.try_pop(initial2));
    strat.on_quote_ack(initial1);
    strat.on_quote_ack(initial2);

    Trade option_fill{};
    option_fill.instrument_id = 1;
    option_fill.product_index = PROD;
    option_fill.side = Side::Buy;
    option_fill.fill_volume = 4;
    strat.on_fill(option_fill);

    Quote cancel1{};
    Quote cancel2{};
    ASSERT_TRUE(quote_buf.try_pop(cancel1));
    ASSERT_TRUE(quote_buf.try_pop(cancel2));
    EXPECT_EQ(cancel1.bid_volume, 0);
    EXPECT_EQ(cancel1.ask_volume, 0);
    EXPECT_EQ(cancel2.bid_volume, 0);
    EXPECT_EQ(cancel2.ask_volume, 0);

    Order hedge{};
    ASSERT_TRUE(order_buf.try_pop(hedge));
    EXPECT_EQ(hedge.instrument_id, 0);
    EXPECT_TRUE(hedge.is_hedge);

    strat.on_quote_cancel(cancel1);
    strat.on_quote_cancel(cancel2);

    Quote none{};
    EXPECT_FALSE(quote_buf.try_pop(none));

    Trade hedge_fill{};
    hedge_fill.client_order_id = hedge.client_order_id;
    hedge_fill.instrument_id = hedge.instrument_id;
    hedge_fill.product_index = PROD;
    hedge_fill.side = hedge.side;
    hedge_fill.fill_volume = hedge.volume;
    strat.on_fill(hedge_fill);

    Quote reprice1{};
    Quote reprice2{};
    ASSERT_TRUE(quote_buf.try_pop(reprice1));
    ASSERT_TRUE(quote_buf.try_pop(reprice2));

    const bool saw_first =
        reprice1.instrument_id == 1 || reprice2.instrument_id == 1;
    const bool saw_second =
        reprice1.instrument_id == 2 || reprice2.instrument_id == 2;
    EXPECT_TRUE(saw_first);
    EXPECT_TRUE(saw_second);
    EXPECT_GT(reprice1.bid_volume + reprice1.ask_volume, 0);
    EXPECT_GT(reprice2.bid_volume + reprice2.ask_volume, 0);
}

TEST_F(OptionMmCoreTest, SuppressesQuotesBrieflyOnUnderlyingShock) {
    params.underlying_move_widen_threshold_ticks.store(1.0, std::memory_order_relaxed);
    params.min_quote_interval_ms.store(0.0, std::memory_order_relaxed);

    strat.on_signal(make_signal(1, 9.8, 10.2, 0.4, 0.2, 99.0F, 101.0F));
    Quote initial{};
    ASSERT_TRUE(quote_buf.try_pop(initial));

    Quote ack = initial;
    strat.on_quote_ack(ack);

    strat.on_signal(make_signal(1, 9.8, 10.2, 0.4, 0.2, 103.0F, 105.0F));

    Quote cancel{};
    ASSERT_TRUE(quote_buf.try_pop(cancel));
    EXPECT_EQ(cancel.bid_volume, 0);
    EXPECT_EQ(cancel.ask_volume, 0);
}

TEST_F(OptionMmCoreTest, DoesNotQuoteThroughTheoWhenFollowingMarketHigher) {
    params.follow_weight.store(1.0, std::memory_order_relaxed);
    TopOfBookTick tick{};
    ASSERT_TRUE(tick_snapshot.read(1, &tick));
    tick.recv_ts_ns = get_monotonic_ns();
    tick.bid_price = 11.0;
    tick.ask_price = 12.0;
    tick.bid_volume = 12;
    tick.ask_volume = 8;
    tick_snapshot.publish(1, tick);

    strat.on_signal(make_signal(1, 9.8, 10.2));

    Quote quote{};
    ASSERT_TRUE(quote_buf.try_pop(quote));
    EXPECT_LE(quote.bid_price, 9.5);
    EXPECT_GE(quote.ask_price, 10.5);
}

TEST_F(OptionMmCoreTest, DoesNotQuoteThroughTheoWhenFollowingMarketLower) {
    params.follow_weight.store(1.0, std::memory_order_relaxed);
    TopOfBookTick tick{};
    ASSERT_TRUE(tick_snapshot.read(1, &tick));
    tick.recv_ts_ns = get_monotonic_ns();
    tick.bid_price = 8.0;
    tick.ask_price = 9.0;
    tick.bid_volume = 8;
    tick.ask_volume = 12;
    tick_snapshot.publish(1, tick);

    strat.on_signal(make_signal(1, 9.8, 10.2));

    Quote quote{};
    ASSERT_TRUE(quote_buf.try_pop(quote));
    EXPECT_LE(quote.bid_price, 9.5);
    EXPECT_GE(quote.ask_price, 10.5);
}

TEST_F(OptionMmCoreTest, CancelsBeforeReplacingLiveQuote) {
    strat.on_signal(make_signal(1, 9.8, 10.2));

    Quote initial{};
    ASSERT_TRUE(quote_buf.try_pop(initial));
    strat.on_quote_ack(initial);

    strat.on_signal(make_signal(1, 11.8, 12.2));

    Quote cancel{};
    ASSERT_TRUE(quote_buf.try_pop(cancel));
    EXPECT_EQ(cancel.bid_volume, 0);
    EXPECT_EQ(cancel.ask_volume, 0);

    Quote cancel_ack = initial;
    cancel_ack.client_quote_id = cancel.client_quote_id;
    cancel_ack.bid_volume = 0;
    cancel_ack.ask_volume = 0;
    strat.on_quote_cancel(cancel_ack);

    Quote replacement{};
    ASSERT_TRUE(quote_buf.try_pop(replacement));
    EXPECT_GT(replacement.bid_volume, 0);
    EXPECT_GT(replacement.ask_volume, 0);
    EXPECT_GT(replacement.bid_price, initial.bid_price);
    EXPECT_GT(replacement.ask_price, initial.ask_price);
}

TEST_F(OptionMmCoreTest, ReplacesLiveQuoteDirectlyOnShfe) {
    instruments[0] = make_future(0, PROD, "cu2501", Exchange::SHFE);
    instruments[1] = make_option(1, 0, PROD, OptionType::Call, 100.0, Exchange::SHFE);
    instruments[2] = make_option(2, 0, PROD, OptionType::Put, 100.0, Exchange::SHFE);
    strat.init(PROD, &quote_buf, &order_buf, &pre_risk, &params,
               instruments, &tick_snapshot, &post_risk, &alert_topic);

    strat.on_signal(make_signal(1, 9.8, 10.2));

    Quote initial{};
    ASSERT_TRUE(quote_buf.try_pop(initial));
    strat.on_quote_ack(initial);

    strat.on_signal(make_signal(1, 11.8, 12.2));

    Quote replacement{};
    ASSERT_TRUE(quote_buf.try_pop(replacement));
    EXPECT_GT(replacement.bid_volume, 0);
    EXPECT_GT(replacement.ask_volume, 0);
    EXPECT_GT(replacement.bid_price, initial.bid_price);
    EXPECT_GT(replacement.ask_price, initial.ask_price);

    Quote extra{};
    EXPECT_FALSE(quote_buf.try_pop(extra));
}

TEST_F(OptionMmCoreTest, ClearsDeferredRequoteWhenStrategyStops) {
    strat.on_signal(make_signal(1, 9.8, 10.2));

    Quote initial{};
    ASSERT_TRUE(quote_buf.try_pop(initial));
    strat.on_quote_ack(initial);

    strat.on_signal(make_signal(1, 11.8, 12.2));

    Quote cancel{};
    ASSERT_TRUE(quote_buf.try_pop(cancel));
    EXPECT_EQ(cancel.bid_volume, 0);
    EXPECT_EQ(cancel.ask_volume, 0);

    params.enabled.store(false, std::memory_order_relaxed);
    TimerEvent refresh{};
    refresh.type = TimerEventType::QuoteRefresh;
    refresh.trigger_ts_ns = get_monotonic_ns();
    strat.on_timer(refresh);

    Quote cancel_ack = initial;
    cancel_ack.client_quote_id = cancel.client_quote_id;
    cancel_ack.bid_volume = 0;
    cancel_ack.ask_volume = 0;
    strat.on_quote_cancel(cancel_ack);

    Quote unexpected{};
    EXPECT_FALSE(quote_buf.try_pop(unexpected));

    params.enabled.store(true, std::memory_order_relaxed);
    EXPECT_FALSE(quote_buf.try_pop(unexpected));
}

TEST_F(OptionMmCoreTest, CancelsLiveQuoteAfterThreeSecondsAlive) {
    strat.on_signal(make_signal(1, 9.8, 10.2));

    Quote initial{};
    ASSERT_TRUE(quote_buf.try_pop(initial));

    Quote ack = initial;
    ack.ack_ts = get_monotonic_ns() - 3'100'000'000LL;
    strat.on_quote_ack(ack);

    TimerEvent refresh{};
    refresh.type = TimerEventType::QuoteRefresh;
    refresh.trigger_ts_ns = get_monotonic_ns();
    strat.on_timer(refresh);

    Quote cancel{};
    ASSERT_TRUE(quote_buf.try_pop(cancel));
    EXPECT_EQ(cancel.client_quote_id, initial.client_quote_id);
    EXPECT_EQ(cancel.bid_volume, 0);
    EXPECT_EQ(cancel.ask_volume, 0);
}

TEST_F(OptionMmCoreTest, RetriesCancelAndAlertsAfterThreeFailures) {
    strat.on_signal(make_signal(1, 9.8, 10.2));

    Quote initial{};
    ASSERT_TRUE(quote_buf.try_pop(initial));

    const int64_t base_ns = get_monotonic_ns();
    Quote ack = initial;
    ack.ack_ts = base_ns - 3'500'000'000LL;
    strat.on_quote_ack(ack);

    TimerEvent refresh{};
    refresh.type = TimerEventType::QuoteRefresh;

    refresh.trigger_ts_ns = base_ns;
    strat.on_timer(refresh);
    Quote cancel1{};
    ASSERT_TRUE(quote_buf.try_pop(cancel1));
    EXPECT_EQ(cancel1.client_quote_id, initial.client_quote_id);

    refresh.trigger_ts_ns = base_ns + 1'100'000'000LL;
    strat.on_timer(refresh);
    Quote cancel2{};
    ASSERT_TRUE(quote_buf.try_pop(cancel2));
    EXPECT_EQ(cancel2.client_quote_id, initial.client_quote_id);

    refresh.trigger_ts_ns = base_ns + 2'200'000'000LL;
    strat.on_timer(refresh);
    Quote cancel3{};
    ASSERT_TRUE(quote_buf.try_pop(cancel3));
    EXPECT_EQ(cancel3.client_quote_id, initial.client_quote_id);

    refresh.trigger_ts_ns = base_ns + 3'300'000'000LL;
    strat.on_timer(refresh);

    Quote extra{};
    EXPECT_FALSE(quote_buf.try_pop(extra));

    uint64_t cursor = 0;
    SystemAlert alert{};
    ASSERT_TRUE(alert_topic.read_next(cursor, alert));
    EXPECT_EQ(alert.type, SystemAlertType::QuoteCancelGiveUp);
    EXPECT_EQ(alert.instrument_id, 1);
    EXPECT_EQ(alert.product_index, PROD);
    EXPECT_NE(std::string(alert.message).find("cancel failed"), std::string::npos);
}

TEST_F(OptionMmCoreTest, StopsCancelRetryOnceQuoteFullyFilled) {
    strat.on_signal(make_signal(1, 9.8, 10.2));

    Quote initial{};
    ASSERT_TRUE(quote_buf.try_pop(initial));

    const int64_t base_ns = get_monotonic_ns();
    Quote ack = initial;
    ack.ack_ts = base_ns - 3'500'000'000LL;
    strat.on_quote_ack(ack);

    TimerEvent refresh{};
    refresh.type = TimerEventType::QuoteRefresh;
    refresh.trigger_ts_ns = base_ns;
    strat.on_timer(refresh);

    Quote cancel{};
    ASSERT_TRUE(quote_buf.try_pop(cancel));
    EXPECT_EQ(cancel.client_quote_id, initial.client_quote_id);

    params.enabled.store(false, std::memory_order_relaxed);

    Trade bid_fill{};
    bid_fill.client_order_id = initial.client_quote_id;
    bid_fill.instrument_id = 1;
    bid_fill.product_index = PROD;
    bid_fill.side = Side::Buy;
    bid_fill.fill_volume = initial.bid_volume;
    strat.on_fill(bid_fill);

    Trade ask_fill{};
    ask_fill.client_order_id = initial.client_quote_id;
    ask_fill.instrument_id = 1;
    ask_fill.product_index = PROD;
    ask_fill.side = Side::Sell;
    ask_fill.fill_volume = initial.ask_volume;
    strat.on_fill(ask_fill);

    refresh.trigger_ts_ns = base_ns + 1'100'000'000LL;
    strat.on_timer(refresh);

    Quote retry{};
    EXPECT_FALSE(quote_buf.try_pop(retry));

    uint64_t cursor = 0;
    SystemAlert alert{};
    EXPECT_FALSE(alert_topic.read_next(cursor, alert));
}
