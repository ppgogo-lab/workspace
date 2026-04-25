#include <gtest/gtest.h>

#include "common/thread_utils.h"
#include "strategy/pcp_arbitrage.h"

#include <array>
#include <cstring>

using namespace omm;

namespace {

Instrument make_future(uint16_t id, uint8_t product_idx, const char* code) {
    Instrument instr{};
    instr.instrument_id = id;
    instr.underlying_id = id;
    instr.product_index = product_idx;
    instr.kind = InstrumentKind::Future;
    instr.tick_size = 1.0;
    instr.multiplier = 1.0;
    instr.expiry_epoch_ns = get_monotonic_ns()
        + static_cast<int64_t>(0.25 * 365.0 * 24.0 * 3600.0 * 1e9);
    std::strncpy(instr.code.data, code, sizeof(instr.code.data) - 1);
    std::strncpy(instr.underlying_code.data, code, sizeof(instr.underlying_code.data) - 1);
    std::strncpy(instr.exchange_id.data, "SHFE", sizeof(instr.exchange_id.data) - 1);
    return instr;
}

Instrument make_option(uint16_t id,
                       uint16_t underlying_id,
                       uint8_t product_idx,
                       OptionType type,
                       double strike) {
    Instrument instr{};
    instr.instrument_id = id;
    instr.underlying_id = underlying_id;
    instr.product_index = product_idx;
    instr.kind = InstrumentKind::Option;
    instr.option_type = type;
    instr.strike = strike;
    instr.tick_size = 0.5;
    instr.multiplier = 1.0;
    instr.expiry_date = 20251226;
    instr.expiry_epoch_ns = get_monotonic_ns()
        + static_cast<int64_t>(0.25 * 365.0 * 24.0 * 3600.0 * 1e9);
    std::strncpy(instr.exchange_id.data, "SHFE", sizeof(instr.exchange_id.data) - 1);
    return instr;
}

TopOfBookTick make_tick(uint16_t instrument_id,
                        double bid,
                        double ask,
                        int32_t bid_volume = 5,
                        int32_t ask_volume = 5) {
    TopOfBookTick tick{};
    tick.instrument_id = instrument_id;
    tick.recv_ts_ns = get_monotonic_ns();
    tick.exchange_ts_ns = tick.recv_ts_ns;
    tick.bid_price[0] = bid;
    tick.ask_price[0] = ask;
    tick.bid_volume[0] = bid_volume;
    tick.ask_volume[0] = ask_volume;
    tick.last_price = 0.5 * (bid + ask);
    return tick;
}

Trade make_fill(const Order& order) {
    Trade trade{};
    trade.client_order_id = order.client_order_id;
    trade.instrument_id = order.instrument_id;
    trade.product_index = order.product_index;
    trade.side = order.side;
    trade.offset = order.offset;
    trade.fill_price = order.price;
    trade.fill_volume = order.volume;
    trade.fill_ts = get_monotonic_ns();
    trade.exchange_id = order.exchange_id;
    return trade;
}

} // namespace

class PCPArbitrageTest : public ::testing::Test {
protected:
    static constexpr uint8_t kProduct = 0;

    SPSCRingBuffer<ArbIntent, 256> intent_buf_;
    AtomicArbParams params_;
    std::array<Instrument, MAX_INSTRUMENTS> instruments_{};
    SnapshotArray<TopOfBookTick, MAX_INSTRUMENTS> ticks_;
    SnapshotArray<Greeks, MAX_INSTRUMENTS> greeks_;
    HardRiskConfig hard_risk_{};
    AccountId account_{};
    PCPArbitrageStrategy strategy_;

    void SetUp() override {
        hard_risk_.max_volume_per_order = 10;
        std::strncpy(account_.data, "TEST001", sizeof(account_.data) - 1);

        params_.enabled.store(true, std::memory_order_relaxed);
        params_.min_edge_ticks.store(1.0, std::memory_order_relaxed);
        params_.cooldown_ms.store(50.0, std::memory_order_relaxed);
        params_.scan_interval_ms.store(0.0, std::memory_order_relaxed);
        params_.cleanup_timeout_ms.store(1000.0, std::memory_order_relaxed);
        params_.max_order_volume.store(1, std::memory_order_relaxed);
        params_.max_live_orders.store(8, std::memory_order_relaxed);
        params_.cleanup_on_partial.store(true, std::memory_order_relaxed);

        instruments_[0] = make_future(0, kProduct, "cu2501");
        instruments_[1] = make_option(1, 0, kProduct, OptionType::Call, 100.0);
        instruments_[2] = make_option(2, 0, kProduct, OptionType::Put, 100.0);

        ticks_.publish(0, make_tick(0, 105.0, 106.0));
        ticks_.publish(1, make_tick(1, 0.5, 1.0));
        ticks_.publish(2, make_tick(2, 0.1, 0.2));

        strategy_.init(kProduct,
                       &intent_buf_,
                       &params_,
                       instruments_.data(),
                       &ticks_,
                       &greeks_,
                       0.0,
                       hard_risk_,
                       account_);
    }

    std::array<ArbIntent, 8> pop_intents(int* count) {
        std::array<ArbIntent, 8> intents{};
        *count = 0;
        while (*count < static_cast<int>(intents.size()) && intent_buf_.try_pop(intents[*count])) {
            ++(*count);
        }
        return intents;
    }
};

TEST_F(PCPArbitrageTest, EmitsThreeOrdersForPositiveParityEdge) {
    strategy_.evaluate(get_monotonic_ns());

    int count = 0;
    auto intents = pop_intents(&count);
    ASSERT_EQ(count, 3);

    int buy_count = 0;
    int sell_count = 0;
    for (int i = 0; i < count; ++i) {
        EXPECT_EQ(intents[i].kind, ArbIntentKind::SubmitOrder);
        EXPECT_FALSE(intents[i].cleanup);
        if (intents[i].order.side == Side::Buy) {
            ++buy_count;
        } else {
            ++sell_count;
        }
    }
    EXPECT_EQ(buy_count, 1);
    EXPECT_EQ(sell_count, 2);
}

TEST_F(PCPArbitrageTest, PublishesOpportunityMonitorRows) {
    strategy_.evaluate(get_monotonic_ns());

    std::array<PCPPairMonitorState, 4> rows{};
    const int count = strategy_.read_pcp_monitor_states(rows.data(), static_cast<int>(rows.size()));
    ASSERT_EQ(count, 1);

    const auto& row = rows[0];
    EXPECT_EQ(row.product_index, kProduct);
    EXPECT_EQ(row.strategy_type, ArbitrageStrategyType::PCP);
    EXPECT_EQ(row.call_id, 1);
    EXPECT_EQ(row.put_id, 2);
    EXPECT_EQ(row.future_id, 0);
    EXPECT_TRUE(row.market_valid);
    EXPECT_TRUE(row.selected);
    EXPECT_DOUBLE_EQ(row.discount_factor, 1.0);
    EXPECT_DOUBLE_EQ(row.synthetic_bid, 100.3);
    EXPECT_DOUBLE_EQ(row.synthetic_ask, 100.9);
    EXPECT_DOUBLE_EQ(row.future_bid, 105.0);
    EXPECT_DOUBLE_EQ(row.future_ask, 106.0);
    EXPECT_DOUBLE_EQ(row.long_synth_edge_ticks, 4.1);
    EXPECT_DOUBLE_EQ(row.short_synth_edge_ticks, -5.7);
    EXPECT_EQ(row.best_direction, PCPMonitorDirection::LongSyntheticShortFuture);
    EXPECT_DOUBLE_EQ(row.best_edge_ticks, 4.1);
    EXPECT_EQ(row.best_volume, 1);
}

TEST_F(PCPArbitrageTest, FullBasketFillDoesNotTriggerCleanup) {
    strategy_.evaluate(get_monotonic_ns());

    int count = 0;
    auto intents = pop_intents(&count);
    ASSERT_EQ(count, 3);

    for (int i = 0; i < count; ++i) {
        strategy_.on_order_ack(intents[i].order);
        strategy_.on_fill(make_fill(intents[i].order));
    }

    strategy_.evaluate(get_monotonic_ns());

    ArbIntent intent{};
    EXPECT_FALSE(intent_buf_.try_pop(intent));

    ArbStrategyMonitorState state{};
    ASSERT_TRUE(strategy_.read_monitor_state(&state));
    EXPECT_FALSE(state.cleanup_active);
    EXPECT_EQ(state.live_orders, 0);
}

TEST_F(PCPArbitrageTest, PartialBasketTriggersCleanupOrders) {
    strategy_.evaluate(get_monotonic_ns());

    int count = 0;
    auto intents = pop_intents(&count);
    ASSERT_EQ(count, 3);

    for (int i = 0; i < count; ++i) {
        strategy_.on_order_ack(intents[i].order);
    }

    strategy_.on_fill(make_fill(intents[0].order));
    strategy_.on_fill(make_fill(intents[2].order));
    strategy_.on_order_cancel(intents[1].order.client_order_id);

    strategy_.evaluate(get_monotonic_ns());

    int cleanup_count = 0;
    auto cleanup_intents = pop_intents(&cleanup_count);
    ASSERT_EQ(cleanup_count, 2);
    for (int i = 0; i < cleanup_count; ++i) {
        EXPECT_TRUE(cleanup_intents[i].cleanup);
        EXPECT_EQ(cleanup_intents[i].kind, ArbIntentKind::SubmitOrder);
    }

    ArbStrategyMonitorState state{};
    ASSERT_TRUE(strategy_.read_monitor_state(&state));
    EXPECT_TRUE(state.cleanup_active);
}
