#pragma once

#include "common/config.h"

#include <cstdint>
#include <ctime>
#include <string>
#include <unordered_map>
#include <vector>

namespace omm {

struct TradeCalendarDay {
    int32_t date{0};
    bool is_trading_day{false};
};

struct TradingSessionWindow {
    int8_t start_day_offset{0};
    int8_t end_day_offset{0};
    int32_t start_seconds{0};
    int32_t end_seconds{0};
};

struct ExchangeTradingCalendar {
    std::string exchange_id;
    std::vector<TradeCalendarDay> days;
    std::vector<TradingSessionWindow> sessions;
    int64_t total_session_seconds{0};
};

class TradingCalendarService {
public:
    /**
     * @brief Load from config.
     * @param cfg Parameter supplied by the caller.
     * @return Return value produced by the operation.
     */
    bool load_from_config(const SystemConfig& cfg);
    /**
     * @brief Load.
     * @param calendars Parameter supplied by the caller.
     * @return Return value produced by the operation.
     */
    bool load(std::vector<ExchangeTradingCalendar> calendars);

    /**
     * @brief Has exchange.
     * @param exchange_id Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] bool has_exchange(std::string_view exchange_id) const noexcept;
    /**
     * @brief Validate products.
     * @param cfg Parameter supplied by the caller.
     * @param error Parameter supplied by the caller.
     * @return Return value produced by the operation.
     */
    [[nodiscard]] bool validate_products(const SystemConfig& cfg, std::string* error) const;
    /**
     * @brief Time to expiry years.
     * @param exchange_id Parameter supplied by the caller.
     * @param now_wall Parameter supplied by the caller.
     * @param expiry_date Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] double time_to_expiry_years(std::string_view exchange_id,
                                              std::time_t now_wall,
                                              int32_t expiry_date) const noexcept;

    /**
     * @brief Find.
     * @param exchange_id Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] const ExchangeTradingCalendar* find(std::string_view exchange_id) const noexcept;

private:
    std::vector<ExchangeTradingCalendar> calendars_;
    std::unordered_map<std::string, std::size_t> index_;
};

[[nodiscard]] bool parse_hhmmss(std::string_view text, int32_t* out_seconds) noexcept;
[[nodiscard]] int32_t yyyymmdd_from_time(std::time_t t) noexcept;
[[nodiscard]] std::time_t time_from_yyyymmdd_seconds(int32_t date,
                                                     int32_t seconds) noexcept;
[[nodiscard]] int32_t add_days_yyyymmdd(int32_t date, int days) noexcept;

} // namespace omm
