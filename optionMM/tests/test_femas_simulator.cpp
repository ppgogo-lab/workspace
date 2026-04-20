#include <gtest/gtest.h>

#include "sim/femas_simulator.h"

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace omm;

template <std::size_t N>
void copy_cstr(char (&dst)[N], std::string_view src) {
    const std::size_t len = std::min(src.size(), N - 1);
    if (len > 0) std::memcpy(dst, src.data(), len);
    dst[len] = '\0';
}

class MdSpy final : public CUstpFtdcMduserSpi {
public:
    void OnRspUserLogin(CUstpFtdcRspUserLoginField*,
                        CUstpFtdcRspInfoField* pRspInfo,
                        int, bool) override {
        std::lock_guard<std::mutex> lock(mutex_);
        login_ok_ = (pRspInfo == nullptr || pRspInfo->ErrorID == 0);
        cv_.notify_all();
    }

    void OnRspSubMarketData(CUstpFtdcSpecificInstrumentField* pSpecificInstrument,
                            CUstpFtdcRspInfoField* pRspInfo,
                            int, bool bIsLast) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pSpecificInstrument) subscriptions_.push_back(pSpecificInstrument->InstrumentID);
        if (pRspInfo && pRspInfo->ErrorID != 0) subscription_errors_.push_back(pRspInfo->ErrorMsg);
        if (bIsLast) subscribed_ = true;
        cv_.notify_all();
    }

    void OnRtnDepthMarketData(CUstpFtdcDepthMarketDataField* pDepthMarketData) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pDepthMarketData) ticks_.push_back(*pDepthMarketData);
        cv_.notify_all();
    }

    bool wait_for_login() {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, std::chrono::milliseconds(200),
                            [this] { return login_ok_; });
    }

    bool wait_for_subscription() {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, std::chrono::milliseconds(200),
                            [this] { return subscribed_; });
    }

    bool wait_for_ticks(std::size_t count) {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, std::chrono::milliseconds(400),
                            [this, count] { return ticks_.size() >= count; });
    }

    CUstpFtdcDepthMarketDataField tick(std::size_t index) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return ticks_.at(index);
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool login_ok_{false};
    bool subscribed_{false};
    std::vector<std::string> subscriptions_;
    std::vector<std::string> subscription_errors_;
    std::vector<CUstpFtdcDepthMarketDataField> ticks_;
};

class TraderSpy final : public CUstpFtdcTraderSpi {
public:
    void OnRspUserLogin(CUstpFtdcRspUserLoginField*,
                        CUstpFtdcRspInfoField* pRspInfo,
                        int, bool) override {
        std::lock_guard<std::mutex> lock(mutex_);
        login_ok_ = (pRspInfo == nullptr || pRspInfo->ErrorID == 0);
        cv_.notify_all();
    }

    void OnRspQryInstrument(CUstpFtdcRspInstrumentField* pRspInstrument,
                            CUstpFtdcRspInfoField*,
                            int, bool bIsLast) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pRspInstrument) instruments_.push_back(*pRspInstrument);
        if (bIsLast) query_done_ = true;
        cv_.notify_all();
    }

    void OnRtnOrder(CUstpFtdcOrderField* pOrder) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pOrder) orders_.push_back(*pOrder);
        cv_.notify_all();
    }

    void OnRtnTrade(CUstpFtdcTradeField* pTrade) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pTrade) trades_.push_back(*pTrade);
        cv_.notify_all();
    }

    void OnRtnQuote(CUstpFtdcRtnQuoteField* pQuote) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pQuote) quotes_.push_back(*pQuote);
        cv_.notify_all();
    }

    bool wait_for_login() {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, std::chrono::milliseconds(200),
                            [this] { return login_ok_; });
    }

    bool wait_for_instruments(std::size_t count) {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, std::chrono::milliseconds(200),
                            [this, count] { return query_done_ && instruments_.size() >= count; });
    }

    bool wait_for_trades(std::size_t count) {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, std::chrono::milliseconds(400),
                            [this, count] { return trades_.size() >= count; });
    }

    bool wait_for_quotes(std::size_t count) {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, std::chrono::milliseconds(400),
                            [this, count] { return quotes_.size() >= count; });
    }

    std::vector<CUstpFtdcRspInstrumentField> instruments() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return instruments_;
    }

    std::vector<CUstpFtdcOrderField> orders() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return orders_;
    }

    std::vector<CUstpFtdcTradeField> trades() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return trades_;
    }

    std::vector<CUstpFtdcRtnQuoteField> quotes() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return quotes_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool login_ok_{false};
    bool query_done_{false};
    std::vector<CUstpFtdcRspInstrumentField> instruments_;
    std::vector<CUstpFtdcOrderField> orders_;
    std::vector<CUstpFtdcTradeField> trades_;
    std::vector<CUstpFtdcRtnQuoteField> quotes_;
};

std::filesystem::path write_export_fixture() {
    const auto base = std::filesystem::temp_directory_path() / "optionmm_femas_sim_test";
    std::filesystem::remove_all(base);
    std::filesystem::create_directories(base);

    {
        std::ofstream instruments(base / "instruments.csv");
        instruments
            << "exchange_id,product_id,product_name,instrument_id,instrument_name,delivery_year,delivery_month,volume_multiple,price_tick,create_date,open_date,expire_date,underlying_instr_id,strike_price,options_type,currency,currency_id\n"
            << "CFFEX,IF,IndexFuture,IF2506,IF Main,2025,6,300,0.2,20250101,20250101,20250628,,0,0,CNY,CNY\n"
            << "CFFEX,IO,IndexOption,IO2506-C-4000,IO Call,2025,6,100,0.2,20250101,20250101,20250628,IF2506,4000,1,CNY,CNY\n";
    }

    {
        std::ofstream ticks(base / "ticks.csv");
        ticks
            << "action_day,update_time,update_millisec,instrument_id,last_price,volume,open_interest,open_price,highest_price,lowest_price,pre_settlement_price,pre_close_price,"
               "bid_price1,bid_volume1,ask_price1,ask_volume1,bid_price2,bid_volume2,ask_price2,ask_volume2,bid_price3,bid_volume3,ask_price3,ask_volume3,bid_price4,bid_volume4,ask_price4,ask_volume4,bid_price5,bid_volume5,ask_price5,ask_volume5\n"
            << "20250420,09:30:00,0,IO2506-C-4000,10.2,100,200,10.0,11.0,9.0,9.8,9.7,"
               "9.5,2,10.0,3,9.0,3,10.5,4,8.5,4,11.0,5,8.0,5,11.5,6,7.5,6,12.0,7\n"
            << "20250420,09:30:01,0,IO2506-C-4000,9.3,120,210,9.6,10.0,9.0,9.8,9.7,"
               "9.0,2,9.2,2,8.8,3,9.4,3,8.6,4,9.6,4,8.4,5,9.8,5,8.2,6,10.0,6\n";
    }

    return base;
}

} // namespace

TEST(FemasSimulator, ParsesSimFrontUri) {
    FemasSimFrontConfig cfg{};
    std::string error;
    ASSERT_TRUE(parse_femas_sim_front(
        "sim://session?exchange=CFFEX&products=IF,IO&date=2025-04-20&start=09:30:00&end=11:30:00&speed=10x&ddb=file:///tmp/fake",
        &cfg, &error));
    EXPECT_EQ(cfg.session_name, "session");
    EXPECT_EQ(cfg.exchange, "CFFEX");
    ASSERT_EQ(cfg.products.size(), 2u);
    EXPECT_EQ(cfg.products[0], "IF");
    EXPECT_EQ(cfg.products[1], "IO");
    EXPECT_EQ(cfg.trading_day, "20250420");
    EXPECT_EQ(cfg.start_time, "09:30:00");
    EXPECT_EQ(cfg.end_time, "11:30:00");
    EXPECT_DOUBLE_EQ(cfg.replay_speed, 10.0);
    EXPECT_FALSE(cfg.max_speed);
}

TEST(FemasSimulator, ReplaysFiveDepthAndMatchesOrdersAndQuotes) {
    const std::filesystem::path fixture = write_export_fixture();
    const std::string uri =
        "sim://session?exchange=CFFEX&products=IF,IO&date=2025-04-20&start=09:30:00&end=09:30:01&speed=10x&ddb=file:///"
        + fixture.generic_string();

    MdSpy md_spi;
    TraderSpy trader_spi;

    IFemasMdApi* md_api = create_sim_femas_md_api(uri);
    ASSERT_NE(md_api, nullptr);
    md_api->RegisterSpi(&md_spi);
    md_api->RegisterFront(const_cast<char*>(uri.c_str()));
    md_api->SubscribeMarketDataTopic(100, USTP_TERT_QUICK);
    md_api->Init();

    CUstpFtdcReqUserLoginField md_login{};
    copy_cstr(md_login.BrokerID, "9999");
    copy_cstr(md_login.UserID, "sim-user");
    copy_cstr(md_login.Password, "pass");
    ASSERT_EQ(md_api->ReqUserLogin(&md_login, 1), 0);
    ASSERT_TRUE(md_spi.wait_for_login());

    char* subscriptions[] = {
        const_cast<char*>("IO2506-C-4000"),
    };
    ASSERT_EQ(md_api->SubMarketData(subscriptions, 1), 0);
    ASSERT_TRUE(md_spi.wait_for_subscription());

    IFemasTraderApi* trader_api = create_sim_femas_trader_api(uri);
    ASSERT_NE(trader_api, nullptr);
    trader_api->RegisterSpi(&trader_spi);
    trader_api->RegisterFront(const_cast<char*>(uri.c_str()));
    trader_api->SubscribePrivateTopic(USTP_TERT_QUICK);
    trader_api->SubscribePublicTopic(USTP_TERT_QUICK);
    trader_api->Init();

    CUstpFtdcReqUserLoginField trader_login{};
    copy_cstr(trader_login.BrokerID, "9999");
    copy_cstr(trader_login.UserID, "sim-user");
    copy_cstr(trader_login.Password, "pass");
    ASSERT_EQ(trader_api->ReqUserLogin(&trader_login, 1), 0);
    ASSERT_TRUE(trader_spi.wait_for_login());

    CUstpFtdcQryInstrumentField qry{};
    copy_cstr(qry.ExchangeID, "CFFEX");
    ASSERT_EQ(trader_api->ReqQryInstrument(&qry, 2), 0);
    ASSERT_TRUE(trader_spi.wait_for_instruments(2));
    const auto instruments = trader_spi.instruments();
    ASSERT_EQ(instruments.size(), 2u);
    EXPECT_STREQ(instruments[1].UnderlyingInstrID, "IF2506");

    ASSERT_TRUE(md_spi.wait_for_ticks(1));
    const CUstpFtdcDepthMarketDataField first_tick = md_spi.tick(0);
    EXPECT_DOUBLE_EQ(first_tick.BidPrice5, 7.5);
    EXPECT_EQ(first_tick.BidVolume5, 6);
    EXPECT_DOUBLE_EQ(first_tick.AskPrice5, 12.0);
    EXPECT_EQ(first_tick.AskVolume5, 7);

    CUstpFtdcInputOrderField order{};
    copy_cstr(order.BrokerID, "9999");
    copy_cstr(order.ExchangeID, "CFFEX");
    copy_cstr(order.InvestorID, "sim-user");
    copy_cstr(order.UserID, "sim-user");
    copy_cstr(order.InstrumentID, "IO2506-C-4000");
    copy_cstr(order.UserOrderLocalID, "1001");
    order.OrderPriceType = USTP_FTDC_OPT_LimitPrice;
    order.Direction = USTP_FTDC_D_Buy;
    order.OffsetFlag = USTP_FTDC_OF_Open;
    order.HedgeFlag = USTP_FTDC_CHF_Speculation;
    order.LimitPrice = 10.5;
    order.Volume = 6;
    order.TimeCondition = USTP_FTDC_TC_GFD;
    order.VolumeCondition = USTP_FTDC_VC_AV;
    ASSERT_EQ(trader_api->ReqOrderInsert(&order, 3), 0);

    CUstpFtdcInputQuoteField quote1{};
    copy_cstr(quote1.BrokerID, "9999");
    copy_cstr(quote1.ExchangeID, "CFFEX");
    copy_cstr(quote1.InvestorID, "sim-user");
    copy_cstr(quote1.UserID, "sim-user");
    copy_cstr(quote1.QuoteUserID, "sim-user");
    copy_cstr(quote1.InstrumentID, "IO2506-C-4000");
    copy_cstr(quote1.UserQuoteLocalID, "Q1");
    copy_cstr(quote1.BidUserOrderLocalID, "Q1B");
    copy_cstr(quote1.AskUserOrderLocalID, "Q1A");
    quote1.BidVolume = 2;
    quote1.AskVolume = 2;
    quote1.BidOffsetFlag = USTP_FTDC_OF_Open;
    quote1.AskOffsetFlag = USTP_FTDC_OF_Open;
    quote1.BidHedgeFlag = USTP_FTDC_CHF_MarketMaker;
    quote1.AskHedgeFlag = USTP_FTDC_CHF_MarketMaker;
    quote1.BidPrice = 8.8;
    quote1.AskPrice = 11.2;
    ASSERT_EQ(trader_api->ReqQuoteInsert(&quote1, 4), 0);

    CUstpFtdcInputQuoteField quote2 = quote1;
    copy_cstr(quote2.UserQuoteLocalID, "Q2");
    copy_cstr(quote2.BidUserOrderLocalID, "Q2B");
    copy_cstr(quote2.AskUserOrderLocalID, "Q2A");
    quote2.BidPrice = 9.2;
    quote2.AskPrice = 11.0;
    ASSERT_EQ(trader_api->ReqQuoteInsert(&quote2, 5), 0);

    ASSERT_TRUE(trader_spi.wait_for_quotes(3));
    ASSERT_TRUE(trader_spi.wait_for_trades(3));

    const auto orders = trader_spi.orders();
    ASSERT_FALSE(orders.empty());
    EXPECT_STREQ(orders.front().UserOrderLocalID, "1001");
    EXPECT_EQ(orders.front().VolumeTraded, 6);

    const auto trades = trader_spi.trades();
    ASSERT_GE(trades.size(), 3u);
    EXPECT_STREQ(trades[0].UserOrderLocalID, "1001");
    EXPECT_DOUBLE_EQ(trades[0].TradePrice, 10.0);
    EXPECT_EQ(trades[0].TradeVolume, 3);
    EXPECT_STREQ(trades[1].UserOrderLocalID, "1001");
    EXPECT_DOUBLE_EQ(trades[1].TradePrice, 10.5);
    EXPECT_EQ(trades[1].TradeVolume, 3);
    EXPECT_STREQ(trades[2].UserOrderLocalID, "Q2B");
    EXPECT_DOUBLE_EQ(trades[2].TradePrice, 9.2);
    EXPECT_EQ(trades[2].TradeVolume, 2);

    const auto quotes = trader_spi.quotes();
    ASSERT_GE(quotes.size(), 3u);
    EXPECT_STREQ(quotes[0].UserQuoteLocalID, "Q1");
    EXPECT_EQ(quotes[0].CancelTime[0], '\0');
    EXPECT_STREQ(quotes[1].UserQuoteLocalID, "Q1");
    EXPECT_NE(quotes[1].CancelTime[0], '\0');
    EXPECT_STREQ(quotes[2].UserQuoteLocalID, "Q2");
    EXPECT_EQ(quotes[2].CancelTime[0], '\0');

    md_api->Release();
    trader_api->Release();
    std::filesystem::remove_all(fixture);
}
