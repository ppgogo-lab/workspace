#include <gtest/gtest.h>
#include "risk/pre_trade_risk.h"
#include "risk/post_trade_risk.h"

using namespace omm;

// ─── Helpers ──────────────────────────────────────────────────────────────────

static Order make_order(OrderId id, uint16_t instr, Side side,
                         double price, Volume vol) {
    Order o{};
    o.client_order_id = id;
    o.instrument_id   = instr;
    o.side            = side;
    o.price           = price;
    o.volume          = vol;
    return o;
}

static Trade make_trade(uint16_t instr, Side side, double price,
                         Volume vol, uint8_t product = 0) {
    Trade t{};
    t.instrument_id = instr;
    t.product_index = product;
    t.side          = side;
    t.fill_price    = price;
    t.fill_volume   = vol;
    return t;
}

// ─── PreTradeRisk ─────────────────────────────────────────────────────────────

class PreTradeRiskTest : public ::testing::Test {
protected:
    HardRiskConfig cfg{};
    PreTradeRisk* risk{nullptr};

    void SetUp() override {
        cfg.max_volume_per_order = 100;
        risk = new PreTradeRisk(cfg);
    }
    void TearDown() override { delete risk; }
};

TEST_F(PreTradeRiskTest, NormalOrderPasses) {
    auto order = make_order(1, 0, Side::Buy, 100.0, 10);
    EXPECT_EQ(risk->check_order(order), PreTradeRisk::RejectReason::OK);
}

TEST_F(PreTradeRiskTest, MaxVolumeRejected) {
    auto order = make_order(1, 0, Side::Buy, 100.0, 101);
    EXPECT_EQ(risk->check_order(order), PreTradeRisk::RejectReason::MAX_VOLUME);
}

TEST_F(PreTradeRiskTest, ExactMaxVolumeAllowed) {
    auto order = make_order(1, 0, Side::Buy, 100.0, 100);
    EXPECT_EQ(risk->check_order(order), PreTradeRisk::RejectReason::OK);
}

TEST_F(PreTradeRiskTest, OpenOrderTracking) {
    EXPECT_EQ(risk->open_order_count(), 0);
    auto order = make_order(1, 0, Side::Buy, 100.0, 10);
    ASSERT_EQ(risk->check_order(order), PreTradeRisk::RejectReason::OK);
    risk->on_order_ack(order);
    EXPECT_EQ(risk->open_order_count(), 1);
    risk->on_order_cancel(order.client_order_id);
    EXPECT_EQ(risk->open_order_count(), 0);
}

TEST_F(PreTradeRiskTest, FillReducesOpenCount) {
    auto order = make_order(1, 0, Side::Buy, 100.0, 10);
    risk->on_order_ack(order);
    EXPECT_EQ(risk->open_order_count(), 1);
    risk->on_order_fill(1, 10, true);
    EXPECT_EQ(risk->open_order_count(), 0);
}

TEST_F(PreTradeRiskTest, PartialFillKeepsOpen) {
    auto order = make_order(1, 0, Side::Buy, 100.0, 10);
    risk->on_order_ack(order);
    risk->on_order_fill(1, 5, false);
    EXPECT_EQ(risk->open_order_count(), 1);
    risk->on_order_fill(1, 5, true);
    EXPECT_EQ(risk->open_order_count(), 0);
}

TEST_F(PreTradeRiskTest, FullFillRecomputesBestAskForSelfTradeCheck) {
    auto best_ask = make_order(1, 0, Side::Sell, 100.0, 5);
    auto next_ask = make_order(2, 0, Side::Sell, 105.0, 5);
    risk->on_order_ack(best_ask);
    risk->on_order_ack(next_ask);

    risk->on_order_fill(best_ask.client_order_id, best_ask.volume, true);

    auto buy_below_remaining_ask = make_order(3, 0, Side::Buy, 102.0, 1);
    EXPECT_EQ(risk->check_order(buy_below_remaining_ask), PreTradeRisk::RejectReason::OK);

    auto buy_crossing_remaining_ask = make_order(4, 0, Side::Buy, 105.0, 1);
    EXPECT_EQ(risk->check_order(buy_crossing_remaining_ask),
              PreTradeRisk::RejectReason::SELF_TRADE);
}

TEST_F(PreTradeRiskTest, CancelRecomputesBestBidForSelfTradeCheck) {
    auto best_bid = make_order(1, 0, Side::Buy, 100.0, 5);
    auto next_bid = make_order(2, 0, Side::Buy, 95.0, 5);
    risk->on_order_ack(best_bid);
    risk->on_order_ack(next_bid);

    risk->on_order_cancel(best_bid.client_order_id);

    auto sell_above_remaining_bid = make_order(3, 0, Side::Sell, 97.0, 1);
    EXPECT_EQ(risk->check_order(sell_above_remaining_bid), PreTradeRisk::RejectReason::OK);

    auto sell_crossing_remaining_bid = make_order(4, 0, Side::Sell, 95.0, 1);
    EXPECT_EQ(risk->check_order(sell_crossing_remaining_bid),
              PreTradeRisk::RejectReason::SELF_TRADE);
}

TEST_F(PreTradeRiskTest, SelfTradeBuyCrossesOwnAsk) {
    // Post own sell at 100 → new buy at 100 should be rejected
    auto sell = make_order(1, 0, Side::Sell, 100.0, 5);
    risk->on_order_ack(sell);  // resting sell at 100

    auto buy = make_order(2, 0, Side::Buy, 100.0, 5);
    EXPECT_EQ(risk->check_order(buy), PreTradeRisk::RejectReason::SELF_TRADE);
}

TEST_F(PreTradeRiskTest, SelfTradeSellCrossesOwnBid) {
    // Post own buy at 100 → new sell at 99 should be rejected
    auto buy = make_order(1, 0, Side::Buy, 100.0, 5);
    risk->on_order_ack(buy);

    auto sell = make_order(2, 0, Side::Sell, 99.0, 5);
    EXPECT_EQ(risk->check_order(sell), PreTradeRisk::RejectReason::SELF_TRADE);
}

TEST_F(PreTradeRiskTest, NoSelfTradeOnDifferentInstrument) {
    // Resting order on instrument 0, new order on instrument 1 — no self-trade
    auto sell = make_order(1, 0, Side::Sell, 100.0, 5);
    risk->on_order_ack(sell);

    auto buy = make_order(2, 1, Side::Buy, 100.0, 5);
    EXPECT_EQ(risk->check_order(buy), PreTradeRisk::RejectReason::OK);
}

TEST_F(PreTradeRiskTest, NoSelfTradePriceDoesNotCross) {
    // Buy at 99 with resting sell at 101 — no crossing
    auto sell = make_order(1, 0, Side::Sell, 101.0, 5);
    risk->on_order_ack(sell);

    auto buy = make_order(2, 0, Side::Buy, 99.0, 5);
    EXPECT_EQ(risk->check_order(buy), PreTradeRisk::RejectReason::OK);
}

TEST_F(PreTradeRiskTest, TooManyOpenOrders) {
    // Fill up all slots
    for (int i = 0; i < MAX_OPEN_ORDERS; ++i) {
        auto order = make_order(i + 1, 0, Side::Buy, 100.0 + i, 1);
        ASSERT_EQ(risk->check_order(order), PreTradeRisk::RejectReason::OK);
        risk->on_order_ack(order);
    }
    EXPECT_EQ(risk->open_order_count(), MAX_OPEN_ORDERS);
    // One more should be rejected
    auto extra = make_order(MAX_OPEN_ORDERS + 1, 0, Side::Buy, 200.0, 1);
    EXPECT_EQ(risk->check_order(extra), PreTradeRisk::RejectReason::TOO_MANY_OPEN_ORDERS);
}

TEST_F(PreTradeRiskTest, ResetClearsState) {
    auto order = make_order(1, 0, Side::Buy, 100.0, 10);
    risk->on_order_ack(order);
    EXPECT_EQ(risk->open_order_count(), 1);
    risk->reset();
    EXPECT_EQ(risk->open_order_count(), 0);
}

// ─── Quote check ──────────────────────────────────────────────────────────────

TEST_F(PreTradeRiskTest, QuotePassesBothLegs) {
    Quote q{};
    q.instrument_id = 0;
    q.bid_price = 99.0; q.bid_volume = 10;
    q.ask_price = 101.0; q.ask_volume = 10;
    EXPECT_EQ(risk->check_quote(q), PreTradeRisk::RejectReason::OK);
}

TEST_F(PreTradeRiskTest, QuoteRejectsOversizedLeg) {
    Quote q{};
    q.instrument_id = 0;
    q.bid_price = 99.0; q.bid_volume = 200;  // exceeds max
    q.ask_price = 101.0; q.ask_volume = 10;
    EXPECT_EQ(risk->check_quote(q), PreTradeRisk::RejectReason::MAX_VOLUME);
}

// ─── PostTradeRisk ────────────────────────────────────────────────────────────

class PostTradeRiskTest : public ::testing::Test {
protected:
    SoftRiskConfig cfg{};
    PostTradeRisk* risk{nullptr};

    void SetUp() override {
        cfg.max_net_position = 100;
        cfg.max_delta = 500.0;
        cfg.max_gamma = 200.0;
        cfg.max_vega  = 5000.0;
        risk = new PostTradeRisk(cfg);
    }
    void TearDown() override { delete risk; }
};

TEST_F(PostTradeRiskTest, NoBreachInitially) {
    EXPECT_FALSE(risk->any_breach());
}

TEST_F(PostTradeRiskTest, BuyUpdatePosition) {
    risk->on_fill(make_trade(0, Side::Buy, 100.0, 10));
    EXPECT_EQ(risk->get_position(0).net_position, 10);
    EXPECT_EQ(risk->get_position(0).long_position, 10);
}

TEST_F(PostTradeRiskTest, SellUpdatePosition) {
    risk->on_fill(make_trade(0, Side::Buy,  100.0, 20));
    risk->on_fill(make_trade(0, Side::Sell, 105.0, 10));
    EXPECT_EQ(risk->get_position(0).net_position, 10);
}

TEST_F(PostTradeRiskTest, PositionBreachTriggered) {
    // Fill beyond max_net_position (100)
    risk->on_fill(make_trade(0, Side::Buy, 100.0, 101));
    EXPECT_TRUE(risk->position_breach());
}

TEST_F(PostTradeRiskTest, RealizedPnL) {
    risk->on_fill(make_trade(0, Side::Buy,  100.0, 10));
    risk->on_fill(make_trade(0, Side::Sell, 110.0, 10));
    // PnL = (110 - 100) * 10 = 100
    EXPECT_NEAR(risk->get_position(0).realized_pnl, 100.0, 1e-10);
}

TEST_F(PostTradeRiskTest, CheckLimitsDelta) {
    // Build a Greeks table and check that delta breach fires correctly
    Greeks greeks[MAX_INSTRUMENTS]{};
    greeks[0].delta = 10.0;  // each unit has delta = 10
    greeks[0].theo_price = 5.0;

    risk->on_fill(make_trade(0, Side::Buy, 5.0, 60));  // net = 60
    // Portfolio delta = 60 * 10 = 600 > max_delta (500) → breach
    risk->check_limits(greeks, 1);
    EXPECT_TRUE(risk->delta_breach());
}

TEST_F(PostTradeRiskTest, CheckLimitsNoBreachSmallPosition) {
    Greeks greeks[MAX_INSTRUMENTS]{};
    greeks[0].delta = 1.0;
    greeks[0].theo_price = 5.0;

    risk->on_fill(make_trade(0, Side::Buy, 5.0, 10));  // delta = 10 << 500
    risk->check_limits(greeks, 1);
    EXPECT_FALSE(risk->delta_breach());
    EXPECT_FALSE(risk->gamma_breach());
    EXPECT_FALSE(risk->vega_breach());
}

TEST_F(PostTradeRiskTest, ResetClearsBreaches) {
    risk->on_fill(make_trade(0, Side::Buy, 100.0, 101));
    EXPECT_TRUE(risk->position_breach());
    risk->reset();
    EXPECT_FALSE(risk->any_breach());
    EXPECT_EQ(risk->get_position(0).net_position, 0);
}
