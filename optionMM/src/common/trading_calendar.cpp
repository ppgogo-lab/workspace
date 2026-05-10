#include "common/trading_calendar.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>

namespace omm {

namespace {

constexpr double kTradingDaysPerYear = 252.0;

std::time_t normalize_tm(std::tm* tm) noexcept {
    tm->tm_isdst = -1;
    return std::mktime(tm);
}

bool is_trading_day(const ExchangeTradingCalendar& cal, int32_t date) noexcept {
    const auto it = std::lower_bound(
        cal.days.begin(), cal.days.end(), date,
        [](const TradeCalendarDay& lhs, int32_t rhs) { return lhs.date < rhs; });
    return it != cal.days.end() && it->date == date && it->is_trading_day;
}

int compare_date(int32_t lhs, int32_t rhs) noexcept {
    if (lhs < rhs) return -1;
    if (lhs > rhs) return 1;
    return 0;
}

} // namespace

bool parse_hhmmss(std::string_view text, int32_t* out_seconds) noexcept {
    if (out_seconds == nullptr || text.size() != 8) return false;
    int hour = 0;
    int minute = 0;
    int second = 0;
    char c1 = 0;
    char c2 = 0;
    if (std::sscanf(std::string(text).c_str(), "%2d%c%2d%c%2d",
                    &hour, &c1, &minute, &c2, &second) != 5) {
        return false;
    }
    if (c1 != ':' || c2 != ':' || hour < 0 || hour > 23
        || minute < 0 || minute > 59 || second < 0 || second > 59) {
        return false;
    }
    *out_seconds = hour * 3600 + minute * 60 + second;
    return true;
}

int32_t yyyymmdd_from_time(std::time_t t) noexcept {
    std::tm local_tm{};
#if defined(_WIN32)
    localtime_s(&local_tm, &t);
#else
    localtime_r(&t, &local_tm);
#endif
    return (local_tm.tm_year + 1900) * 10000
         + (local_tm.tm_mon + 1) * 100
         + local_tm.tm_mday;
}

std::time_t time_from_yyyymmdd_seconds(int32_t date, int32_t seconds) noexcept {
    const int year = date / 10000;
    const int month = (date / 100) % 100;
    const int day = date % 100;
    std::tm tm{};
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = seconds / 3600;
    tm.tm_min = (seconds % 3600) / 60;
    tm.tm_sec = seconds % 60;
    return normalize_tm(&tm);
}

int32_t add_days_yyyymmdd(int32_t date, int days) noexcept {
    std::time_t t = time_from_yyyymmdd_seconds(date, 12 * 3600);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    tm.tm_mday += days;
    t = normalize_tm(&tm);
    return yyyymmdd_from_time(t);
}

/**
 * @brief Implements Load from config.
 * @param cfg Parameter supplied by the caller.
 * @return Return value produced by the operation.
 */
bool TradingCalendarService::load_from_config(const SystemConfig& cfg) {
    std::vector<ExchangeTradingCalendar> calendars;
    calendars.reserve(static_cast<std::size_t>(cfg.exchange_calendar_count));

    for (int i = 0; i < cfg.exchange_calendar_count; ++i) {
        ExchangeTradingCalendar cal{};
        cal.exchange_id = std::string(cfg.exchange_calendars[i].exchange_id.view());
        cal.days.reserve(static_cast<std::size_t>(cfg.exchange_calendars[i].day_count));
        for (int d = 0; d < cfg.exchange_calendars[i].day_count; ++d) {
            cal.days.push_back({
                cfg.exchange_calendars[i].days[d].date,
                cfg.exchange_calendars[i].days[d].is_trading_day,
            });
        }

        for (int j = 0; j < cfg.exchange_trading_time_count; ++j) {
            if (!(cfg.exchange_trading_times[j].exchange_id == cfg.exchange_calendars[i].exchange_id)) {
                continue;
            }
            cal.sessions.reserve(static_cast<std::size_t>(cfg.exchange_trading_times[j].session_count));
            for (int s = 0; s < cfg.exchange_trading_times[j].session_count; ++s) {
                const auto& src = cfg.exchange_trading_times[j].sessions[s];
                TradingSessionWindow session{};
                session.start_day_offset = src.start_day_offset;
                session.end_day_offset = src.end_day_offset;
                if (!parse_hhmmss(src.start_time, &session.start_seconds)
                    || !parse_hhmmss(src.end_time, &session.end_seconds)) {
                    return false;
                }
                cal.sessions.push_back(session);
            }
            break;
        }
        calendars.push_back(std::move(cal));
    }
    return load(std::move(calendars));
}

/**
 * @brief Implements Load.
 * @param calendars Parameter supplied by the caller.
 * @return Return value produced by the operation.
 */
bool TradingCalendarService::load(std::vector<ExchangeTradingCalendar> calendars) {
    calendars_.clear();
    index_.clear();
    for (auto& cal : calendars) {
        if (cal.exchange_id.empty() || cal.days.empty() || cal.sessions.empty()) return false;
        std::map<int32_t, bool> days_by_date;
        for (const auto& day : cal.days) {
            days_by_date[day.date] = day.is_trading_day;
        }
        cal.days.clear();
        cal.days.reserve(days_by_date.size());
        for (const auto& [date, trading] : days_by_date) {
            cal.days.push_back({date, trading});
        }
        cal.total_session_seconds = 0;
        for (const auto& session : cal.sessions) {
            const int32_t anchor = 20260115;
            const std::time_t start = time_from_yyyymmdd_seconds(
                add_days_yyyymmdd(anchor, session.start_day_offset),
                session.start_seconds);
            const std::time_t end = time_from_yyyymmdd_seconds(
                add_days_yyyymmdd(anchor, session.end_day_offset),
                session.end_seconds);
            if (end <= start) return false;
            cal.total_session_seconds += static_cast<int64_t>(end - start);
        }
        if (cal.total_session_seconds <= 0) return false;
        index_[cal.exchange_id] = calendars_.size();
        calendars_.push_back(std::move(cal));
    }
    return true;
}

/**
 * @brief Implements Has exchange.
 * @param exchange_id Parameter supplied by the caller.
 * @return Return value produced by the operation.
 * @note Noexcept API preserves hot-path failure and latency invariants.
 */
bool TradingCalendarService::has_exchange(std::string_view exchange_id) const noexcept {
    return find(exchange_id) != nullptr;
}

/**
 * @brief Implements Find.
 * @param exchange_id Parameter supplied by the caller.
 * @return Return value produced by the operation.
 * @note Noexcept API preserves hot-path failure and latency invariants.
 */
const ExchangeTradingCalendar* TradingCalendarService::find(std::string_view exchange_id) const noexcept {
    const auto it = index_.find(std::string(exchange_id));
    if (it == index_.end() || it->second >= calendars_.size()) return nullptr;
    return &calendars_[it->second];
}

/**
 * @brief Implements Validate products.
 * @param cfg Parameter supplied by the caller.
 * @param error Parameter supplied by the caller.
 * @return Return value produced by the operation.
 */
bool TradingCalendarService::validate_products(const SystemConfig& cfg, std::string* error) const {
    for (int i = 0; i < cfg.product_count && i < MAX_PRODUCTS; ++i) {
        const std::string exchange(cfg.products[i].exchange_id.view());
        if (!has_exchange(exchange)) {
            if (error != nullptr) {
                *error = "missing exchange calendar/trading sessions for " + exchange;
            }
            return false;
        }
    }
    return true;
}

/**
 * @brief Implements Time to expiry years.
 * @param exchange_id Parameter supplied by the caller.
 * @param now_wall Parameter supplied by the caller.
 * @param expiry_date Parameter supplied by the caller.
 * @return Return value produced by the operation.
 * @note Noexcept API preserves hot-path failure and latency invariants.
 */
double TradingCalendarService::time_to_expiry_years(std::string_view exchange_id,
                                                    std::time_t now_wall,
                                                    int32_t expiry_date) const noexcept {
    const ExchangeTradingCalendar* cal = find(exchange_id);
    if (cal == nullptr || cal->total_session_seconds <= 0 || expiry_date <= 0) {
        return 1e-4;
    }

    const int32_t now_date = yyyymmdd_from_time(now_wall);
    int32_t trading_day = add_days_yyyymmdd(now_date, -1);
    int64_t remaining_seconds = 0;
    int guard = 0;
    while (compare_date(trading_day, expiry_date) <= 0 && guard++ < MAX_EXCHANGE_CALENDAR_DAYS + 8) {
        if (is_trading_day(*cal, trading_day)) {
            for (const TradingSessionWindow& session : cal->sessions) {
                const int32_t start_date = add_days_yyyymmdd(trading_day, session.start_day_offset);
                const int32_t end_date = add_days_yyyymmdd(trading_day, session.end_day_offset);
                const std::time_t start = time_from_yyyymmdd_seconds(start_date, session.start_seconds);
                const std::time_t end = time_from_yyyymmdd_seconds(end_date, session.end_seconds);
                if (end <= now_wall) continue;
                const std::time_t effective_start = std::max(start, now_wall);
                if (end > effective_start) {
                    remaining_seconds += static_cast<int64_t>(end - effective_start);
                }
            }
        }
        trading_day = add_days_yyyymmdd(trading_day, 1);
    }

    const double trading_days =
        static_cast<double>(remaining_seconds) / static_cast<double>(cal->total_session_seconds);
    const double years = trading_days / kTradingDaysPerYear;
    return years > 1e-4 ? years : 1e-4;
}

} // namespace omm
