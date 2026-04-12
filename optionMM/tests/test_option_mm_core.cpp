#include <gtest/gtest.h>

#include "strategy/option_mm_core.h"
#include "strategy/mm_params.h"
#include "risk/pre_trade_risk.h"
#include "risk/post_trade_risk.h"
#include "common/ring_buffer.h"
#include "common/types.h"

#include <cstring>

using namespace omm;

namespace {

Instrument make_future(uint16_t id, uint8_t product_idx, const char* code) {
    Instrument instr{};
    instr.instrument_id = id;
    instr.product_index = product_idx;
    instr.kind = InstrumentKind::Future;
    instr.tick_size = 1.0;
    instr.multiplier = 1.0;
    std::strncpy(instr.code.data, code, sizeof(instr.code.data) - 1);
    std::strncpy(instr.underlying_code.data, code, sizeof(instr.underlying_code.data) - 1);
    instr.expiry_epoch_ns = get_monotonic_ns() + static_cast<int64_t>(0.25 * 365.0 * 24.0 * 3600.0 * 1e9);
    return instr;
}

Instrument make_option(uint16_t id, uint16_t underlying_id, uint8_t product_idx,
                       OptionType type, double strike) {
    Instrument instr{};
    instr.instrument_id = id;
    instr.underlying_id = underlying_id;
    instr.product_index = product_idx;
    instr.kind = InstrumentKind::Option;
    instr.option_type = type;
    instr.tick_size = 0.5;
    instr.multiplier = 1.0;
    instr.strike = strike;
    instr.expiry_epoch_ns = get_monotonic_ns() + static_cast<int64_t>(0.25 * 365.0 * 24.0 * 3600.0 * 1e9);
    return instr;
}

PricingSignal make_signal(uint16_t instrument_id, double theo, double delta = 0.4) {
    PricingSignal sig{};
    sig.instrument_id = instrument_id;
    sig.underlying_id = 0;
    sig.flags = PricingFlagHasUnderlyingRef;
    sig.theo_bid = theo;
    sig.theo_ask = theo;
    sig.delta = static_cast<float>(delta);
    sig.vega = 0.2F;
    sig.calc_ts_ns = get_monotonic_ns();
    sig.underlying_ref_bid = 99.0F;
    sig.underlying_ref_ask = 101.0F;
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
    Instrument instruments[MAX_INSTRUMENTS]{};
    MarketTick tick_snapshot[MAX_INSTRUMENTS]{};
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

        tick_snapshot[1].instrument_id = 1;
        tick_snapshot[1].recv_ts_ns = get_monotonic_ns();
        tick_snapshot[1].bid_price[0] = 9.5;
        tick_snapshot[1].ask_price[0] = 10.5;
        tick_snapshot[1].bid_volume[0] = 10;
        tick_snapshot[1].ask_volume[0] = 10;

        strat.init(PROD, &quote_buf, &order_buf, &pre_risk, &params,
                   instruments, tick_snapshot, &post_risk);
    }
};

TEST_F(OptionMmCoreTest, GeneratesQuoteFromValidSignal) {
    strat.on_signal(make_signal(1, 10.0));

    Quote quote{};
    ASSERT_TRUE(quote_buf.try_pop(quote));
    EXPECT_EQ(quote.instrument_id, 1);
    EXPECT_GT(quote.ask_price, quote.bid_price);
    EXPECT_EQ(quote.bid_volume, 5);
    EXPECT_EQ(quote.ask_volume, 5);
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

    strat.on_signal(make_signal(1, 10.0));

    Quote quote{};
    ASSERT_TRUE(quote_buf.try_pop(quote));
    EXPECT_EQ(quote.bid_volume, 0);
    EXPECT_EQ(quote.ask_volume, 5);
}
