#include <gtest/gtest.h>

#include "common/trading_calendar.h"

#include <cmath>
#include <cstring>

using namespace omm;

namespace {

void copy_cstr(char* dst, std::size_t n, const char* src) {
    std::strncpy(dst, src, n - 1);
    dst[n - 1] = '\0';
}

std::time_t wall_time(int32_t date, const char* hhmmss) {
    int32_t seconds = 0;
    EXPECT_TRUE(parse_hhmmss(hhmmss, &seconds));
    return time_from_yyyymmdd_seconds(date, seconds);
}

ExchangeTradingCalendar make_shfe_calendar() {
    ExchangeTradingCalendar cal{};
    cal.exchange_id = "SHFE";
    cal.days = {
        {20260423, true},
        {20260424, true},
        {20260425, false},
        {20260426, false},
        {20260427, true},
        {20260428, true},
    };
    auto add = [&](int8_t start_offset, const char* start,
                   int8_t end_offset, const char* end) {
        TradingSessionWindow session{};
        session.start_day_offset = start_offset;
        session.end_day_offset = end_offset;
        EXPECT_TRUE(parse_hhmmss(start, &session.start_seconds));
        EXPECT_TRUE(parse_hhmmss(end, &session.end_seconds));
        cal.sessions.push_back(session);
    };
    add(-1, "21:00:00", 0, "02:00:00");
    add(0, "09:00:00", 0, "10:15:00");
    add(0, "10:30:00", 0, "11:30:00");
    add(0, "13:00:00", 0, "15:00:00");
    return cal;
}

} // namespace

TEST(TradingCalendarServiceTest, ShfeIntradayRemainingUsesTradingDayFraction) {
    TradingCalendarService service;
    ASSERT_TRUE(service.load({make_shfe_calendar()}));

    const double T = service.time_to_expiry_years(
        "SHFE", wall_time(20260424, "14:00:00"), 20260428);
    const double expected = (1.0 / 9.25 + 2.0) / 252.0;
    EXPECT_NEAR(T, expected, 1e-9);
}

TEST(TradingCalendarServiceTest, NightSessionMapsToTradingDay) {
    TradingCalendarService service;
    ASSERT_TRUE(service.load({make_shfe_calendar()}));

    const double T = service.time_to_expiry_years(
        "SHFE", wall_time(20260423, "22:00:00"), 20260424);
    const double expected = (8.25 / 9.25) / 252.0;
    EXPECT_NEAR(T, expected, 1e-9);
}

TEST(TradingCalendarServiceTest, DifferentExchangesAreIndependent) {
    ExchangeTradingCalendar shfe = make_shfe_calendar();
    ExchangeTradingCalendar cffex{};
    cffex.exchange_id = "CFFEX";
    cffex.days = shfe.days;
    TradingSessionWindow day{};
    ASSERT_TRUE(parse_hhmmss("09:30:00", &day.start_seconds));
    ASSERT_TRUE(parse_hhmmss("15:00:00", &day.end_seconds));
    cffex.sessions.push_back(day);

    TradingCalendarService service;
    ASSERT_TRUE(service.load({shfe, cffex}));

    const double shfe_t = service.time_to_expiry_years(
        "SHFE", wall_time(20260424, "14:00:00"), 20260428);
    const double cffex_t = service.time_to_expiry_years(
        "CFFEX", wall_time(20260424, "14:00:00"), 20260428);
    EXPECT_NE(shfe_t, cffex_t);
}

TEST(TradingCalendarServiceTest, LoadsFromConfig) {
    SystemConfig cfg{};
    cfg.product_count = 1;
    cfg.products[0].exchange_id = ExchangeId("SHFE");
    cfg.exchange_calendar_count = 1;
    cfg.exchange_calendars[0].exchange_id = ExchangeId("SHFE");
    cfg.exchange_calendars[0].days[0] = {20260425, false};
    cfg.exchange_calendars[0].day_count = 1;
    cfg.exchange_trading_time_count = 1;
    cfg.exchange_trading_times[0].exchange_id = ExchangeId("SHFE");
    auto& s = cfg.exchange_trading_times[0].sessions[0];
    copy_cstr(s.start_time, sizeof(s.start_time), "09:00:00");
    copy_cstr(s.end_time, sizeof(s.end_time), "15:00:00");
    cfg.exchange_trading_times[0].session_count = 1;

    TradingCalendarService service;
    ASSERT_TRUE(service.load_from_config(cfg));
    std::string error;
    EXPECT_TRUE(service.validate_products(cfg, &error));
}
