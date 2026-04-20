#include "sim/femas_simulator.h"

#include "logger/logger.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace omm {

namespace {

constexpr int kBookDepth = 5;
constexpr int kDefaultTopicId = 100;
constexpr char kDefaultParticipantId[] = "SIM";

template <std::size_t N>
void copy_cstr(char (&dst)[N], std::string_view src) noexcept {
    const std::size_t len = std::min(src.size(), N - 1);
    if (len > 0) std::memcpy(dst, src.data(), len);
    dst[len] = '\0';
}

template <std::size_t N>
void copy_cstr(char (&dst)[N], const char* src) noexcept {
    if (!src) {
        dst[0] = '\0';
        return;
    }
    copy_cstr(dst, std::string_view(src));
}

std::string trim(std::string_view text) {
    std::size_t start = 0;
    while (start < text.size()
           && std::isspace(static_cast<unsigned char>(text[start])) != 0) {
        ++start;
    }
    std::size_t end = text.size();
    while (end > start
           && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) {
        --end;
    }
    return std::string(text.substr(start, end - start));
}

std::string percent_decode(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '%' && i + 2 < value.size()) {
            const char hi = value[i + 1];
            const char lo = value[i + 2];
            const auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return 10 + c - 'a';
                if (c >= 'A' && c <= 'F') return 10 + c - 'A';
                return -1;
            };
            const int hi_v = hex(hi);
            const int lo_v = hex(lo);
            if (hi_v >= 0 && lo_v >= 0) {
                out.push_back(static_cast<char>((hi_v << 4) | lo_v));
                i += 2;
                continue;
            }
        }
        if (value[i] == '+') {
            out.push_back(' ');
        } else {
            out.push_back(value[i]);
        }
    }
    return out;
}

bool starts_with_ci(std::string_view text, std::string_view prefix) noexcept {
    if (text.size() < prefix.size()) return false;
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(text[i]))
            != std::tolower(static_cast<unsigned char>(prefix[i]))) {
            return false;
        }
    }
    return true;
}

std::vector<std::string> split_csv_line(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    cur.reserve(line.size());
    bool in_quotes = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (c == '"') {
            if (in_quotes && i + 1 < line.size() && line[i + 1] == '"') {
                cur.push_back('"');
                ++i;
            } else {
                in_quotes = !in_quotes;
            }
            continue;
        }
        if (c == ',' && !in_quotes) {
            out.push_back(cur);
            cur.clear();
            continue;
        }
        cur.push_back(c);
    }
    out.push_back(cur);
    return out;
}

std::string normalize_trading_day(std::string_view raw) {
    std::string out;
    out.reserve(raw.size());
    for (const char c : raw) {
        if (std::isdigit(static_cast<unsigned char>(c)) != 0) {
            out.push_back(c);
        }
    }
    if (out.size() == 8) return out;
    return {};
}

bool parse_int(std::string_view text, int* out) {
    if (!out) return false;
    const std::string value = trim(text);
    if (value.empty()) return false;
    try {
        *out = std::stoi(value);
        return true;
    } catch (...) {
        return false;
    }
}

bool parse_double(std::string_view text, double* out) {
    if (!out) return false;
    const std::string value = trim(text);
    if (value.empty()) return false;
    try {
        *out = std::stod(value);
        return true;
    } catch (...) {
        return false;
    }
}

int64_t parse_time_ns_of_day(std::string_view raw_time, int millisec) {
    int h = 0;
    int m = 0;
    int s = 0;
    int fractional_ns = std::max(0, millisec) * 1'000'000;

    const std::string text = trim(raw_time);
    if (text.empty()) return -1;

    if (std::sscanf(text.c_str(), "%d:%d:%d", &h, &m, &s) != 3) {
        return -1;
    }
    if (const std::size_t dot = text.find('.'); dot != std::string::npos) {
        const std::string frac = text.substr(dot + 1);
        if (!frac.empty()) {
            std::string digits;
            for (const char c : frac) {
                if (std::isdigit(static_cast<unsigned char>(c)) == 0) break;
                digits.push_back(c);
            }
            if (!digits.empty()) {
                while (digits.size() < 9) digits.push_back('0');
                if (digits.size() > 9) digits.resize(9);
                fractional_ns = std::stoi(digits);
            }
        }
    }

    const int64_t total_sec = static_cast<int64_t>(h) * 3600
                            + static_cast<int64_t>(m) * 60
                            + static_cast<int64_t>(s);
    return total_sec * 1'000'000'000LL + fractional_ns;
}

std::string format_numeric_id(uint64_t value) {
    char buffer[32]{};
    std::snprintf(buffer, sizeof(buffer), "%llu",
                  static_cast<unsigned long long>(value));
    return buffer;
}

bool match_product_filter(const FemasSimFrontConfig& cfg,
                          std::string_view product_id,
                          std::string_view instrument_id,
                          std::string_view underlying_id) {
    if (cfg.products.empty()) return true;
    for (const std::string& token : cfg.products) {
        if (token.empty()) continue;
        if (starts_with_ci(product_id, token)
            || starts_with_ci(instrument_id, token)
            || starts_with_ci(underlying_id, token)) {
            return true;
        }
    }
    return false;
}

char parse_options_type(std::string_view text) {
    const std::string value = trim(text);
    if (value.empty()) return USTP_FTDC_OT_NotOptions;
    if (value == "1" || starts_with_ci(value, "call")) return USTP_FTDC_OT_CallOptions;
    if (value == "2" || starts_with_ci(value, "put")) return USTP_FTDC_OT_PutOptions;
    return USTP_FTDC_OT_NotOptions;
}

struct InstrumentRecord {
    CUstpFtdcRspInstrumentField field{};
};

struct ReplayTick {
    CUstpFtdcDepthMarketDataField field{};
    int64_t ns_of_day{0};
    int sequence_no{0};
};

struct BookState {
    CUstpFtdcDepthMarketDataField field{};
    int available_bid[kBookDepth]{};
    int available_ask[kBookDepth]{};
    bool initialized{false};
};

struct MdClient {
    CUstpFtdcMduserSpi* spi{nullptr};
    int topic_id{kDefaultTopicId};
    std::unordered_set<std::string> subscriptions;
};

struct TraderClient {
    CUstpFtdcTraderSpi* spi{nullptr};
};

struct ActiveOrder {
    int client_id{0};
    CUstpFtdcInputOrderField req{};
    std::string order_sys_id;
    int traded_volume{0};
};

struct ActiveQuote {
    int client_id{0};
    CUstpFtdcInputQuoteField req{};
    std::string quote_sys_id;
    std::string bid_order_sys_id;
    std::string ask_order_sys_id;
    int bid_traded{0};
    int ask_traded{0};
};

struct Dataset {
    std::vector<InstrumentRecord> instruments;
    std::vector<ReplayTick> ticks;
};

bool load_dataset_from_exports(const FemasSimFrontConfig& cfg,
                               Dataset* out,
                               std::string* error) {
    if (!out || !error) return false;

    std::filesystem::path export_dir(cfg.ddb);
    if (starts_with_ci(cfg.ddb, "file://")) {
        export_dir = cfg.ddb.substr(7);
    }

    if (!std::filesystem::exists(export_dir)) {
        *error = "DolphinDB export directory not found: " + export_dir.string();
        return false;
    }

    const std::filesystem::path instruments_path = export_dir / "instruments.csv";
    const std::filesystem::path ticks_path = export_dir / "ticks.csv";

    if (!std::filesystem::exists(instruments_path)) {
        *error = "missing instruments.csv in " + export_dir.string();
        return false;
    }
    if (!std::filesystem::exists(ticks_path)) {
        *error = "missing ticks.csv in " + export_dir.string();
        return false;
    }

    const std::string trading_day = normalize_trading_day(cfg.trading_day);
    const int64_t start_ns = parse_time_ns_of_day(cfg.start_time, 0);
    const int64_t end_ns = parse_time_ns_of_day(cfg.end_time, 0);
    if (trading_day.empty() || start_ns < 0 || end_ns < 0 || end_ns < start_ns) {
        *error = "invalid trading day or replay window in sim:// front";
        return false;
    }

    std::ifstream instruments_file(instruments_path);
    if (!instruments_file.is_open()) {
        *error = "failed to open " + instruments_path.string();
        return false;
    }

    std::string line;
    if (!std::getline(instruments_file, line)) {
        *error = "empty " + instruments_path.string();
        return false;
    }
    std::vector<std::string> header = split_csv_line(line);
    std::unordered_map<std::string, std::size_t> columns;
    for (std::size_t i = 0; i < header.size(); ++i) {
        columns.emplace(trim(header[i]), i);
    }

    auto field_text = [&](const std::vector<std::string>& row,
                          const char* name) -> std::string_view {
        const auto it = columns.find(name);
        if (it == columns.end() || it->second >= row.size()) return {};
        return row[it->second];
    };

    std::unordered_set<std::string> allowed_instruments;
    while (std::getline(instruments_file, line)) {
        if (line.empty()) continue;
        std::vector<std::string> row = split_csv_line(line);

        const std::string exchange_id = trim(field_text(row, "exchange_id"));
        const std::string product_id = trim(field_text(row, "product_id"));
        const std::string instrument_id = trim(field_text(row, "instrument_id"));
        const std::string underlying_id = trim(field_text(row, "underlying_instr_id"));

        if (!cfg.exchange.empty() && !starts_with_ci(exchange_id, cfg.exchange)) continue;
        if (!match_product_filter(cfg, product_id, instrument_id, underlying_id)) continue;

        InstrumentRecord record{};
        copy_cstr(record.field.ExchangeID, exchange_id);
        copy_cstr(record.field.ProductID, product_id);
        copy_cstr(record.field.ProductName, field_text(row, "product_name"));
        copy_cstr(record.field.InstrumentID, instrument_id);
        copy_cstr(record.field.InstrumentName, field_text(row, "instrument_name"));
        copy_cstr(record.field.CurrencyID, field_text(row, "currency_id"));
        copy_cstr(record.field.CreateDate, normalize_trading_day(field_text(row, "create_date")));
        copy_cstr(record.field.OpenDate, normalize_trading_day(field_text(row, "open_date")));
        copy_cstr(record.field.ExpireDate, normalize_trading_day(field_text(row, "expire_date")));
        copy_cstr(record.field.UnderlyingInstrID, underlying_id);
        record.field.OptionsType = parse_options_type(field_text(row, "options_type"));

        int delivery_year = 0;
        int delivery_month = 0;
        int volume_multiple = 1;
        double price_tick = 0.2;
        double strike_price = 0.0;

        (void)parse_int(field_text(row, "delivery_year"), &delivery_year);
        (void)parse_int(field_text(row, "delivery_month"), &delivery_month);
        (void)parse_int(field_text(row, "volume_multiple"), &volume_multiple);
        (void)parse_double(field_text(row, "price_tick"), &price_tick);
        (void)parse_double(field_text(row, "strike_price"), &strike_price);

        record.field.DeliveryYear = delivery_year;
        record.field.DeliveryMonth = delivery_month;
        record.field.VolumeMultiple = volume_multiple;
        record.field.PriceTick = price_tick;
        record.field.StrikePrice = strike_price;
        record.field.MaxLimitOrderVolume = 1000;
        record.field.MinLimitOrderVolume = 1;
        record.field.MaxMarketOrderVolume = 1000;
        record.field.MinMarketOrderVolume = 1;
        record.field.IsTrading = 1;

        out->instruments.push_back(record);
        allowed_instruments.insert(instrument_id);
    }

    if (out->instruments.empty()) {
        *error = "instrument bootstrap returned no rows";
        return false;
    }

    std::ifstream ticks_file(ticks_path);
    if (!ticks_file.is_open()) {
        *error = "failed to open " + ticks_path.string();
        return false;
    }
    if (!std::getline(ticks_file, line)) {
        *error = "empty " + ticks_path.string();
        return false;
    }
    header = split_csv_line(line);
    columns.clear();
    for (std::size_t i = 0; i < header.size(); ++i) {
        columns.emplace(trim(header[i]), i);
    }

    while (std::getline(ticks_file, line)) {
        if (line.empty()) continue;
        std::vector<std::string> row = split_csv_line(line);
        const std::string instrument_id = trim(field_text(row, "instrument_id"));
        if (allowed_instruments.find(instrument_id) == allowed_instruments.end()) continue;

        const std::string action_day = normalize_trading_day(field_text(row, "action_day"));
        if (!action_day.empty() && action_day != trading_day) continue;

        int update_millisec = 0;
        (void)parse_int(field_text(row, "update_millisec"), &update_millisec);
        const int64_t tick_ns = parse_time_ns_of_day(field_text(row, "update_time"),
                                                     update_millisec);
        if (tick_ns < start_ns || tick_ns > end_ns) continue;

        ReplayTick tick{};
        tick.ns_of_day = tick_ns;
        copy_cstr(tick.field.TradingDay, trading_day);
        copy_cstr(tick.field.ActionDay, trading_day);
        copy_cstr(tick.field.UpdateTime, field_text(row, "update_time"));
        tick.field.UpdateMillisec = update_millisec;
        copy_cstr(tick.field.InstrumentID, instrument_id);

        (void)parse_double(field_text(row, "last_price"), &tick.field.LastPrice);
        int volume = 0;
        (void)parse_int(field_text(row, "volume"), &volume);
        tick.field.Volume = volume;
        (void)parse_double(field_text(row, "open_interest"), &tick.field.OpenInterest);
        (void)parse_double(field_text(row, "open_price"), &tick.field.OpenPrice);
        (void)parse_double(field_text(row, "highest_price"), &tick.field.HighestPrice);
        (void)parse_double(field_text(row, "lowest_price"), &tick.field.LowestPrice);
        (void)parse_double(field_text(row, "pre_settlement_price"), &tick.field.PreSettlementPrice);
        (void)parse_double(field_text(row, "pre_close_price"), &tick.field.PreClosePrice);

        auto parse_level = [&](int level) {
            const std::string idx = std::to_string(level);
            double bid_price = 0.0;
            double ask_price = 0.0;
            int bid_volume = 0;
            int ask_volume = 0;
            (void)parse_double(field_text(row, ("bid_price" + idx).c_str()), &bid_price);
            (void)parse_int(field_text(row, ("bid_volume" + idx).c_str()), &bid_volume);
            (void)parse_double(field_text(row, ("ask_price" + idx).c_str()), &ask_price);
            (void)parse_int(field_text(row, ("ask_volume" + idx).c_str()), &ask_volume);
            switch (level) {
            case 1:
                tick.field.BidPrice1 = bid_price;
                tick.field.BidVolume1 = bid_volume;
                tick.field.AskPrice1 = ask_price;
                tick.field.AskVolume1 = ask_volume;
                break;
            case 2:
                tick.field.BidPrice2 = bid_price;
                tick.field.BidVolume2 = bid_volume;
                tick.field.AskPrice2 = ask_price;
                tick.field.AskVolume2 = ask_volume;
                break;
            case 3:
                tick.field.BidPrice3 = bid_price;
                tick.field.BidVolume3 = bid_volume;
                tick.field.AskPrice3 = ask_price;
                tick.field.AskVolume3 = ask_volume;
                break;
            case 4:
                tick.field.BidPrice4 = bid_price;
                tick.field.BidVolume4 = bid_volume;
                tick.field.AskPrice4 = ask_price;
                tick.field.AskVolume4 = ask_volume;
                break;
            case 5:
                tick.field.BidPrice5 = bid_price;
                tick.field.BidVolume5 = bid_volume;
                tick.field.AskPrice5 = ask_price;
                tick.field.AskVolume5 = ask_volume;
                break;
            default:
                break;
            }
        };

        for (int level = 1; level <= kBookDepth; ++level) parse_level(level);
        out->ticks.push_back(tick);
    }

    std::sort(out->ticks.begin(), out->ticks.end(),
              [](const ReplayTick& lhs, const ReplayTick& rhs) {
                  if (lhs.ns_of_day != rhs.ns_of_day) return lhs.ns_of_day < rhs.ns_of_day;
                  return std::string_view(lhs.field.InstrumentID)
                       < std::string_view(rhs.field.InstrumentID);
              });

    for (std::size_t i = 0; i < out->ticks.size(); ++i) {
        out->ticks[i].sequence_no = static_cast<int>(i + 1);
    }

    if (out->ticks.empty()) {
        *error = "replay query returned no ticks";
        return false;
    }

    return true;
}

class FemasSimSession : public std::enable_shared_from_this<FemasSimSession> {
public:
    explicit FemasSimSession(FemasSimFrontConfig cfg)
        : cfg_(std::move(cfg)) {
        if (!load_dataset_from_exports(cfg_, &dataset_, &load_error_)) {
            OMM_LOG_ERROR("femas-sim", "dataset load failed: {}", load_error_);
            return;
        }
        for (const InstrumentRecord& record : dataset_.instruments) {
            instruments_by_id_.emplace(record.field.InstrumentID, record);
        }
        worker_ = std::thread([this] { replay_loop(); });
    }

    ~FemasSimSession() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_flag_ = true;
            cv_.notify_all();
        }
        if (worker_.joinable()) worker_.join();
    }

    [[nodiscard]] bool ready() const noexcept {
        return load_error_.empty();
    }

    [[nodiscard]] const std::string& error() const noexcept {
        return load_error_;
    }

    [[nodiscard]] const std::string& trading_day() const noexcept {
        return cfg_.trading_day;
    }

    int attach_md_client(CUstpFtdcMduserSpi* spi, int topic_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        const int id = next_client_id_++;
        md_clients_[id].spi = spi;
        md_clients_[id].topic_id = topic_id;
        return id;
    }

    void detach_md_client(int client_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        md_clients_.erase(client_id);
    }

    void update_md_topic(int client_id, int topic_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = md_clients_.find(client_id);
        if (it == md_clients_.end()) return;
        it->second.topic_id = topic_id;
    }

    struct MdSubscriptionResult {
        std::string instrument_id;
        bool ok{false};
        std::string error;
        bool is_last{false};
    };

    std::vector<MdSubscriptionResult> subscribe_md(int client_id,
                                                   char* instrument_ids[],
                                                   int count) {
        std::vector<MdSubscriptionResult> results;
        std::lock_guard<std::mutex> lock(mutex_);
        auto client_it = md_clients_.find(client_id);
        if (client_it == md_clients_.end()) return results;

        results.reserve(std::max(0, count));
        for (int i = 0; i < count; ++i) {
            const std::string instrument_id = instrument_ids && instrument_ids[i]
                ? std::string(instrument_ids[i]) : std::string{};
            MdSubscriptionResult result{};
            result.instrument_id = instrument_id;
            result.ok = instruments_by_id_.find(instrument_id) != instruments_by_id_.end();
            if (result.ok) {
                client_it->second.subscriptions.insert(instrument_id);
            } else {
                result.error = "unknown instrument";
            }
            results.push_back(result);
        }

        if (!results.empty()) results.back().is_last = true;
        return results;
    }

    int attach_trader_client(CUstpFtdcTraderSpi* spi) {
        std::lock_guard<std::mutex> lock(mutex_);
        const int id = next_client_id_++;
        trader_clients_[id].spi = spi;
        return id;
    }

    void detach_trader_client(int client_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = active_orders_.begin(); it != active_orders_.end();) {
            if (it->second.client_id == client_id) {
                it = active_orders_.erase(it);
            } else {
                ++it;
            }
        }
        for (auto it = active_quotes_.begin(); it != active_quotes_.end();) {
            if (it->second.client_id == client_id) {
                instrument_to_quote_.erase(it->second.req.InstrumentID);
                it = active_quotes_.erase(it);
            } else {
                ++it;
            }
        }
        trader_clients_.erase(client_id);
    }

    std::vector<InstrumentRecord> query_instruments(const CUstpFtdcQryInstrumentField& req) const {
        std::vector<InstrumentRecord> matches;
        for (const InstrumentRecord& record : dataset_.instruments) {
            const bool exchange_ok = req.ExchangeID[0] == '\0'
                || std::strncmp(record.field.ExchangeID, req.ExchangeID,
                                sizeof(req.ExchangeID)) == 0;
            const bool product_ok = req.ProductID[0] == '\0'
                || starts_with_ci(record.field.ProductID, req.ProductID);
            const bool instrument_ok = req.InstrumentID[0] == '\0'
                || std::strncmp(record.field.InstrumentID, req.InstrumentID,
                                sizeof(req.InstrumentID)) == 0;
            if (exchange_ok && product_ok && instrument_ok) {
                matches.push_back(record);
            }
        }
        return matches;
    }

    [[nodiscard]] bool replay_finished() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return replay_finished_;
    }

    void submit_order(int client_id,
                      const CUstpFtdcInputOrderField& req,
                      int request_id) {
        std::vector<std::function<void()>> callbacks;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            CUstpFtdcTraderSpi* spi = find_trader_spi_locked(client_id);
            if (!spi) return;

            const auto book_it = books_.find(req.InstrumentID);
            bool rejected = false;
            if (replay_finished_) {
                append_order_reject_locked(spi, req, request_id,
                                           "replay finished", &callbacks);
                rejected = true;
            }
            if (!rejected
                && instruments_by_id_.find(req.InstrumentID) == instruments_by_id_.end()) {
                append_order_reject_locked(spi, req, request_id,
                                           "unknown instrument", &callbacks);
                rejected = true;
            }
            if (!rejected && req.Volume <= 0) {
                append_order_reject_locked(spi, req, request_id,
                                           "invalid volume", &callbacks);
                rejected = true;
            }
            if (!rejected
                && req.OrderPriceType != USTP_FTDC_OPT_AnyPrice
                && req.LimitPrice <= 0.0) {
                append_order_reject_locked(spi, req, request_id,
                                           "invalid price", &callbacks);
                rejected = true;
            }
            if (!rejected
                && req.OrderPriceType == USTP_FTDC_OPT_AnyPrice
                && (book_it == books_.end() || !book_it->second.initialized)) {
                append_order_reject_locked(spi, req, request_id,
                                           "no market data", &callbacks);
                rejected = true;
            }

            if (!rejected) {
                ActiveOrder order{};
                order.client_id = client_id;
                order.req = req;
                order.order_sys_id = format_numeric_id(next_order_sys_id_++);

                std::vector<std::function<void()>> trade_callbacks;
                BookState* book = book_it == books_.end() ? nullptr : &book_it->second;
                const int filled_now = fill_order_locked(&order, book, &trade_callbacks, spi);

                const int remaining = std::max(0, order.req.Volume - filled_now);
                const char status = remaining == 0
                    ? USTP_FTDC_OS_AllTraded
                    : (filled_now > 0 ? USTP_FTDC_OS_PartTradedQueueing
                                      : USTP_FTDC_OS_NoTradeQueueing);
                append_order_ack_locked(spi, order, status, &callbacks);
                callbacks.insert(callbacks.end(),
                                 trade_callbacks.begin(), trade_callbacks.end());

                if (remaining > 0) {
                    active_orders_[order.req.UserOrderLocalID] = order;
                }
            }
        }
        for (auto& callback : callbacks) callback();
    }

    void cancel_order(int client_id,
                      const CUstpFtdcOrderActionField& req,
                      int request_id) {
        std::vector<std::function<void()>> callbacks;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            CUstpFtdcTraderSpi* spi = find_trader_spi_locked(client_id);
            if (!spi) return;

            auto order_it = active_orders_.find(req.UserOrderLocalID);
            if (order_it == active_orders_.end()
                || order_it->second.client_id != client_id) {
                CUstpFtdcOrderActionField rsp = req;
                CUstpFtdcRspInfoField info{};
                info.ErrorID = 1;
                copy_cstr(info.ErrorMsg, "order not live");
                callbacks.push_back([spi, rsp, info, request_id]() mutable {
                    spi->OnRspOrderAction(&rsp, &info, request_id, true);
                });
            } else {
                append_order_cancel_locked(spi, order_it->second, &callbacks);
                active_orders_.erase(order_it);
            }
        }
        for (auto& callback : callbacks) callback();
    }

    void submit_quote(int client_id,
                      const CUstpFtdcInputQuoteField& req,
                      int request_id) {
        std::vector<std::function<void()>> callbacks;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            CUstpFtdcTraderSpi* spi = find_trader_spi_locked(client_id);
            if (!spi) return;

            const auto book_it = books_.find(req.InstrumentID);
            bool rejected = false;
            if (replay_finished_) {
                append_quote_reject_locked(spi, req, request_id,
                                           "replay finished", &callbacks);
                rejected = true;
            }
            if (!rejected
                && instruments_by_id_.find(req.InstrumentID) == instruments_by_id_.end()) {
                append_quote_reject_locked(spi, req, request_id,
                                           "unknown instrument", &callbacks);
                rejected = true;
            }
            if (!rejected
                && ((req.BidVolume > 0 && req.BidPrice <= 0.0)
                || (req.AskVolume > 0 && req.AskPrice <= 0.0)
                || (req.BidVolume <= 0 && req.AskVolume <= 0)
                || (req.BidVolume > 0 && req.AskVolume > 0 && req.AskPrice <= req.BidPrice))) {
                append_quote_reject_locked(spi, req, request_id,
                                           "invalid quote", &callbacks);
                rejected = true;
            }

            if (!rejected) {
                const std::string instrument_id = req.InstrumentID;
                const auto live_it = instrument_to_quote_.find(instrument_id);
                if (live_it != instrument_to_quote_.end()) {
                    auto quote_it = active_quotes_.find(live_it->second);
                    if (quote_it != active_quotes_.end()) {
                        append_quote_cancel_locked(spi, quote_it->second, &callbacks);
                        active_quotes_.erase(quote_it);
                    }
                    instrument_to_quote_.erase(live_it);
                }

                ActiveQuote quote{};
                quote.client_id = client_id;
                quote.req = req;
                quote.quote_sys_id = format_numeric_id(next_quote_sys_id_++);
                quote.bid_order_sys_id = format_numeric_id(next_order_sys_id_++);
                quote.ask_order_sys_id = format_numeric_id(next_order_sys_id_++);

                append_quote_ack_locked(spi, quote, &callbacks);

                BookState* book = book_it == books_.end() ? nullptr : &book_it->second;
                fill_quote_locked(&quote, book, &callbacks, spi);

                if (quote.req.BidVolume > quote.bid_traded
                    || quote.req.AskVolume > quote.ask_traded) {
                    active_quotes_[quote.req.UserQuoteLocalID] = quote;
                    instrument_to_quote_[instrument_id] = quote.req.UserQuoteLocalID;
                }
            }
        }
        for (auto& callback : callbacks) callback();
    }

    void cancel_quote(int client_id,
                      const CUstpFtdcQuoteActionField& req,
                      int request_id) {
        std::vector<std::function<void()>> callbacks;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            CUstpFtdcTraderSpi* spi = find_trader_spi_locked(client_id);
            if (!spi) return;

            auto quote_it = active_quotes_.find(req.UserQuoteLocalID);
            if (quote_it == active_quotes_.end()
                || quote_it->second.client_id != client_id) {
                CUstpFtdcQuoteActionField rsp = req;
                CUstpFtdcRspInfoField info{};
                info.ErrorID = 1;
                copy_cstr(info.ErrorMsg, "quote not live");
                callbacks.push_back([spi, rsp, info, request_id]() mutable {
                    spi->OnRspQuoteAction(&rsp, &info, request_id, true);
                });
            } else {
                append_quote_cancel_locked(spi, quote_it->second, &callbacks);
                instrument_to_quote_.erase(quote_it->second.req.InstrumentID);
                active_quotes_.erase(quote_it);
            }
        }
        for (auto& callback : callbacks) callback();
    }

private:
    void replay_loop() {
        if (!ready()) return;

        const int64_t first_ns = dataset_.ticks.front().ns_of_day;
        const auto steady_start = std::chrono::steady_clock::now();

        for (const ReplayTick& tick : dataset_.ticks) {
            {
                std::unique_lock<std::mutex> lock(mutex_);
                if (stop_flag_) return;
                if (!cfg_.max_speed) {
                    const int64_t replay_delta_ns = tick.ns_of_day - first_ns;
                    const int64_t due_ns = static_cast<int64_t>(
                        replay_delta_ns / std::max(0.0001, cfg_.replay_speed));
                    const auto due_time = steady_start + std::chrono::nanoseconds(due_ns);
                    cv_.wait_until(lock, due_time, [this] { return stop_flag_; });
                    if (stop_flag_) return;
                }
            }

            std::vector<std::function<void()>> callbacks;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                BookState& book = books_[tick.field.InstrumentID];
                book.field = tick.field;
                book.initialized = true;
                last_update_time_ = tick.field.UpdateTime;
                book.available_bid[0] = tick.field.BidVolume1;
                book.available_bid[1] = tick.field.BidVolume2;
                book.available_bid[2] = tick.field.BidVolume3;
                book.available_bid[3] = tick.field.BidVolume4;
                book.available_bid[4] = tick.field.BidVolume5;
                book.available_ask[0] = tick.field.AskVolume1;
                book.available_ask[1] = tick.field.AskVolume2;
                book.available_ask[2] = tick.field.AskVolume3;
                book.available_ask[3] = tick.field.AskVolume4;
                book.available_ask[4] = tick.field.AskVolume5;

                for (const auto& entry : md_clients_) {
                    const MdClient& client = entry.second;
                    if (!client.spi) continue;
                    if (!client.subscriptions.empty()
                        && client.subscriptions.find(tick.field.InstrumentID)
                            == client.subscriptions.end()) {
                        continue;
                    }
                    CUstpFtdcDepthMarketDataField md = tick.field;
                    const int topic_id = client.topic_id;
                    const int sequence_no = tick.sequence_no;
                    CUstpFtdcMduserSpi* spi = client.spi;
                    callbacks.push_back([spi, topic_id, sequence_no, md]() mutable {
                        spi->OnPackageStart(topic_id, sequence_no);
                        spi->OnRtnDepthMarketData(&md);
                        spi->OnPackageEnd(topic_id, sequence_no);
                    });
                }

                process_resting_orders_locked(tick.field.InstrumentID, &book, &callbacks);
                process_resting_quotes_locked(tick.field.InstrumentID, &book, &callbacks);
            }

            for (auto& callback : callbacks) callback();
        }

        std::lock_guard<std::mutex> lock(mutex_);
        replay_finished_ = true;
    }

    CUstpFtdcTraderSpi* find_trader_spi_locked(int client_id) {
        const auto it = trader_clients_.find(client_id);
        if (it == trader_clients_.end()) return nullptr;
        return it->second.spi;
    }

    static double ask_price_at(const BookState& book, int level) noexcept {
        switch (level) {
        case 0: return book.field.AskPrice1;
        case 1: return book.field.AskPrice2;
        case 2: return book.field.AskPrice3;
        case 3: return book.field.AskPrice4;
        case 4: return book.field.AskPrice5;
        default: return 0.0;
        }
    }

    static double bid_price_at(const BookState& book, int level) noexcept {
        switch (level) {
        case 0: return book.field.BidPrice1;
        case 1: return book.field.BidPrice2;
        case 2: return book.field.BidPrice3;
        case 3: return book.field.BidPrice4;
        case 4: return book.field.BidPrice5;
        default: return 0.0;
        }
    }

    void append_order_reject_locked(CUstpFtdcTraderSpi* spi,
                                    const CUstpFtdcInputOrderField& req,
                                    int request_id,
                                    std::string_view error,
                                    std::vector<std::function<void()>>* callbacks) const {
        CUstpFtdcInputOrderField rsp = req;
        CUstpFtdcRspInfoField info{};
        info.ErrorID = 1;
        copy_cstr(info.ErrorMsg, error);
        callbacks->push_back([spi, rsp, info, request_id]() mutable {
            spi->OnRspOrderInsert(&rsp, &info, request_id, true);
        });
    }

    void append_quote_reject_locked(CUstpFtdcTraderSpi* spi,
                                    const CUstpFtdcInputQuoteField& req,
                                    int request_id,
                                    std::string_view error,
                                    std::vector<std::function<void()>>* callbacks) const {
        CUstpFtdcInputQuoteField rsp = req;
        CUstpFtdcRspInfoField info{};
        info.ErrorID = 1;
        copy_cstr(info.ErrorMsg, error);
        callbacks->push_back([spi, rsp, info, request_id]() mutable {
            spi->OnRspQuoteInsert(&rsp, &info, request_id, true);
        });
    }

    void append_order_ack_locked(CUstpFtdcTraderSpi* spi,
                                 const ActiveOrder& order,
                                 char status,
                                 std::vector<std::function<void()>>* callbacks) const {
        CUstpFtdcOrderField rtn{};
        copy_cstr(rtn.BrokerID, order.req.BrokerID);
        copy_cstr(rtn.ExchangeID, order.req.ExchangeID);
        copy_cstr(rtn.OrderSysID, order.order_sys_id);
        copy_cstr(rtn.InvestorID, order.req.InvestorID);
        copy_cstr(rtn.UserID, order.req.UserID);
        copy_cstr(rtn.InstrumentID, order.req.InstrumentID);
        copy_cstr(rtn.UserOrderLocalID, order.req.UserOrderLocalID);
        copy_cstr(rtn.TradingDay, cfg_.trading_day);
        copy_cstr(rtn.OrderUserID, order.req.UserID);
        copy_cstr(rtn.ParticipantID, kDefaultParticipantId);
        rtn.Direction = order.req.Direction;
        rtn.OffsetFlag = order.req.OffsetFlag;
        rtn.HedgeFlag = order.req.HedgeFlag;
        rtn.LimitPrice = order.req.LimitPrice;
        rtn.Volume = order.req.Volume;
        rtn.OrderPriceType = order.req.OrderPriceType;
        rtn.TimeCondition = order.req.TimeCondition;
        rtn.VolumeCondition = order.req.VolumeCondition;
        rtn.OrderStatus = status;
        rtn.VolumeTraded = order.traded_volume;
        rtn.VolumeRemain = std::max(0, order.req.Volume - order.traded_volume);
        copy_cstr(rtn.InsertTime, current_tick_time_locked());
        callbacks->push_back([spi, rtn]() mutable {
            spi->OnRtnOrder(&rtn);
        });
    }

    void append_order_cancel_locked(CUstpFtdcTraderSpi* spi,
                                    const ActiveOrder& order,
                                    std::vector<std::function<void()>>* callbacks) const {
        CUstpFtdcOrderField rtn{};
        copy_cstr(rtn.BrokerID, order.req.BrokerID);
        copy_cstr(rtn.ExchangeID, order.req.ExchangeID);
        copy_cstr(rtn.OrderSysID, order.order_sys_id);
        copy_cstr(rtn.InvestorID, order.req.InvestorID);
        copy_cstr(rtn.UserID, order.req.UserID);
        copy_cstr(rtn.InstrumentID, order.req.InstrumentID);
        copy_cstr(rtn.UserOrderLocalID, order.req.UserOrderLocalID);
        rtn.Direction = order.req.Direction;
        rtn.OffsetFlag = order.req.OffsetFlag;
        rtn.HedgeFlag = order.req.HedgeFlag;
        rtn.LimitPrice = order.req.LimitPrice;
        rtn.Volume = order.req.Volume;
        rtn.OrderStatus = USTP_FTDC_OS_Canceled;
        rtn.VolumeTraded = order.traded_volume;
        rtn.VolumeRemain = std::max(0, order.req.Volume - order.traded_volume);
        copy_cstr(rtn.CancelTime, current_tick_time_locked());
        callbacks->push_back([spi, rtn]() mutable {
            spi->OnRtnOrder(&rtn);
        });
    }

    void append_trade_locked(CUstpFtdcTraderSpi* spi,
                             const CUstpFtdcInputOrderField& req,
                             std::string_view order_sys_id,
                             int fill_volume,
                             double fill_price,
                             std::vector<std::function<void()>>* callbacks) const {
        CUstpFtdcTradeField trade{};
        copy_cstr(trade.BrokerID, req.BrokerID);
        copy_cstr(trade.ExchangeID, req.ExchangeID);
        copy_cstr(trade.TradingDay, cfg_.trading_day);
        copy_cstr(trade.ParticipantID, kDefaultParticipantId);
        copy_cstr(trade.InvestorID, req.InvestorID);
        copy_cstr(trade.UserID, req.UserID);
        copy_cstr(trade.OrderUserID, req.UserID);
        copy_cstr(trade.OrderSysID, order_sys_id);
        copy_cstr(trade.UserOrderLocalID, req.UserOrderLocalID);
        copy_cstr(trade.InstrumentID, req.InstrumentID);
        copy_cstr(trade.TradeID, format_numeric_id(next_trade_id_++));
        copy_cstr(trade.TradeTime, current_tick_time_locked());
        trade.Direction = req.Direction;
        trade.OffsetFlag = req.OffsetFlag;
        trade.HedgeFlag = req.HedgeFlag;
        trade.TradePrice = fill_price;
        trade.TradeVolume = fill_volume;
        callbacks->push_back([spi, trade]() mutable {
            spi->OnRtnTrade(&trade);
        });
    }

    void append_quote_ack_locked(CUstpFtdcTraderSpi* spi,
                                 const ActiveQuote& quote,
                                 std::vector<std::function<void()>>* callbacks) const {
        CUstpFtdcRtnQuoteField rtn{};
        copy_cstr(rtn.BrokerID, quote.req.BrokerID);
        copy_cstr(rtn.ExchangeID, quote.req.ExchangeID);
        copy_cstr(rtn.InvestorID, quote.req.InvestorID);
        copy_cstr(rtn.UserID, quote.req.UserID);
        copy_cstr(rtn.InstrumentID, quote.req.InstrumentID);
        copy_cstr(rtn.QuoteSysID, quote.quote_sys_id);
        copy_cstr(rtn.UserQuoteLocalID, quote.req.UserQuoteLocalID);
        copy_cstr(rtn.BidUserOrderLocalID, quote.req.BidUserOrderLocalID);
        copy_cstr(rtn.AskUserOrderLocalID, quote.req.AskUserOrderLocalID);
        copy_cstr(rtn.BidOrderSysID, quote.bid_order_sys_id);
        copy_cstr(rtn.AskOrderSysID, quote.ask_order_sys_id);
        copy_cstr(rtn.InsertTime, current_tick_time_locked());
        copy_cstr(rtn.QuoteUserID, quote.req.QuoteUserID);
        rtn.BidVolume = quote.req.BidVolume;
        rtn.AskVolume = quote.req.AskVolume;
        rtn.BidOffsetFlag = quote.req.BidOffsetFlag;
        rtn.AskOffsetFlag = quote.req.AskOffsetFlag;
        rtn.BidHedgeFlag = quote.req.BidHedgeFlag;
        rtn.AskHedgeFlag = quote.req.AskHedgeFlag;
        rtn.BidPrice = quote.req.BidPrice;
        rtn.AskPrice = quote.req.AskPrice;
        callbacks->push_back([spi, rtn]() mutable {
            spi->OnRtnQuote(&rtn);
        });
    }

    void append_quote_cancel_locked(CUstpFtdcTraderSpi* spi,
                                    const ActiveQuote& quote,
                                    std::vector<std::function<void()>>* callbacks) const {
        CUstpFtdcRtnQuoteField rtn{};
        copy_cstr(rtn.BrokerID, quote.req.BrokerID);
        copy_cstr(rtn.ExchangeID, quote.req.ExchangeID);
        copy_cstr(rtn.InvestorID, quote.req.InvestorID);
        copy_cstr(rtn.UserID, quote.req.UserID);
        copy_cstr(rtn.InstrumentID, quote.req.InstrumentID);
        copy_cstr(rtn.QuoteSysID, quote.quote_sys_id);
        copy_cstr(rtn.UserQuoteLocalID, quote.req.UserQuoteLocalID);
        copy_cstr(rtn.BidUserOrderLocalID, quote.req.BidUserOrderLocalID);
        copy_cstr(rtn.AskUserOrderLocalID, quote.req.AskUserOrderLocalID);
        copy_cstr(rtn.BidOrderSysID, quote.bid_order_sys_id);
        copy_cstr(rtn.AskOrderSysID, quote.ask_order_sys_id);
        copy_cstr(rtn.CancelTime, current_tick_time_locked());
        rtn.BidVolume = 0;
        rtn.AskVolume = 0;
        rtn.BidPrice = quote.req.BidPrice;
        rtn.AskPrice = quote.req.AskPrice;
        callbacks->push_back([spi, rtn]() mutable {
            spi->OnRtnQuote(&rtn);
        });
    }

    int fill_order_locked(ActiveOrder* order,
                          BookState* book,
                          std::vector<std::function<void()>>* callbacks,
                          CUstpFtdcTraderSpi* spi) {
        if (!order || !book || !book->initialized) return 0;

        int remaining = std::max(0, order->req.Volume - order->traded_volume);
        const bool is_buy = order->req.Direction == USTP_FTDC_D_Buy;
        const bool market_order = order->req.OrderPriceType == USTP_FTDC_OPT_AnyPrice;

        for (int level = 0; level < kBookDepth && remaining > 0; ++level) {
            const double level_price = is_buy ? ask_price_at(*book, level)
                                              : bid_price_at(*book, level);
            int& level_volume = is_buy ? book->available_ask[level]
                                       : book->available_bid[level];
            if (level_price <= 0.0 || level_volume <= 0) continue;

            const bool crosses = market_order
                || (is_buy && order->req.LimitPrice >= level_price)
                || (!is_buy && order->req.LimitPrice <= level_price);
            if (!crosses) break;

            const int fill_volume = std::min(remaining, level_volume);
            if (fill_volume <= 0) continue;

            level_volume -= fill_volume;
            remaining -= fill_volume;
            order->traded_volume += fill_volume;
            append_trade_locked(spi, order->req, order->order_sys_id,
                                fill_volume, level_price, callbacks);
        }

        return order->traded_volume;
    }

    void fill_quote_locked(ActiveQuote* quote,
                           BookState* book,
                           std::vector<std::function<void()>>* callbacks,
                           CUstpFtdcTraderSpi* spi) {
        if (!quote || !book || !book->initialized) return;

        auto consume_leg = [&](bool bid_side) {
            int* traded = bid_side ? &quote->bid_traded : &quote->ask_traded;
            const int total = bid_side ? quote->req.BidVolume : quote->req.AskVolume;
            const bool is_buy = bid_side;
            const double limit_price = bid_side ? quote->req.BidPrice : quote->req.AskPrice;
            const char direction = bid_side ? USTP_FTDC_D_Buy : USTP_FTDC_D_Sell;
            const char offset = bid_side ? quote->req.BidOffsetFlag : quote->req.AskOffsetFlag;
            const char hedge = bid_side ? quote->req.BidHedgeFlag : quote->req.AskHedgeFlag;
            const std::string& order_sys_id = bid_side ? quote->bid_order_sys_id
                                                       : quote->ask_order_sys_id;
            const char* user_local_id = bid_side ? quote->req.BidUserOrderLocalID
                                                 : quote->req.AskUserOrderLocalID;
            int remaining = std::max(0, total - *traded);
            for (int level = 0; level < kBookDepth && remaining > 0; ++level) {
                const double level_price = is_buy ? ask_price_at(*book, level)
                                                  : bid_price_at(*book, level);
                int& level_volume = is_buy ? book->available_ask[level]
                                           : book->available_bid[level];
                if (level_price <= 0.0 || level_volume <= 0) continue;

                const bool crosses = is_buy ? (limit_price >= level_price)
                                            : (limit_price <= level_price);
                if (!crosses) break;

                const int fill_volume = std::min(remaining, level_volume);
                if (fill_volume <= 0) continue;

                level_volume -= fill_volume;
                remaining -= fill_volume;
                *traded += fill_volume;

                CUstpFtdcInputOrderField synthetic{};
                copy_cstr(synthetic.BrokerID, quote->req.BrokerID);
                copy_cstr(synthetic.ExchangeID, quote->req.ExchangeID);
                copy_cstr(synthetic.InvestorID, quote->req.InvestorID);
                copy_cstr(synthetic.UserID, quote->req.UserID);
                copy_cstr(synthetic.InstrumentID, quote->req.InstrumentID);
                copy_cstr(synthetic.UserOrderLocalID, user_local_id);
                synthetic.Direction = direction;
                synthetic.OffsetFlag = offset;
                synthetic.HedgeFlag = hedge;
                synthetic.LimitPrice = limit_price;
                synthetic.Volume = total;
                append_trade_locked(spi, synthetic, order_sys_id,
                                    fill_volume, level_price, callbacks);
            }
        };

        consume_leg(true);
        consume_leg(false);
    }

    void process_resting_orders_locked(const std::string& instrument_id,
                                       BookState* book,
                                       std::vector<std::function<void()>>* callbacks) {
        if (!book) return;
        for (auto it = active_orders_.begin(); it != active_orders_.end();) {
            ActiveOrder& order = it->second;
            if (order.req.InstrumentID != instrument_id) {
                ++it;
                continue;
            }
            CUstpFtdcTraderSpi* spi = find_trader_spi_locked(order.client_id);
            if (!spi) {
                it = active_orders_.erase(it);
                continue;
            }
            const int before = order.traded_volume;
            fill_order_locked(&order, book, callbacks, spi);
            if (order.traded_volume >= order.req.Volume) {
                it = active_orders_.erase(it);
                continue;
            }
            if (order.traded_volume != before) {
                append_order_ack_locked(spi, order, USTP_FTDC_OS_PartTradedQueueing,
                                        callbacks);
            }
            ++it;
        }
    }

    void process_resting_quotes_locked(const std::string& instrument_id,
                                       BookState* book,
                                       std::vector<std::function<void()>>* callbacks) {
        if (!book) return;
        for (auto it = active_quotes_.begin(); it != active_quotes_.end();) {
            ActiveQuote& quote = it->second;
            if (quote.req.InstrumentID != instrument_id) {
                ++it;
                continue;
            }
            CUstpFtdcTraderSpi* spi = find_trader_spi_locked(quote.client_id);
            if (!spi) {
                instrument_to_quote_.erase(quote.req.InstrumentID);
                it = active_quotes_.erase(it);
                continue;
            }
            fill_quote_locked(&quote, book, callbacks, spi);
            if (quote.bid_traded >= quote.req.BidVolume
                && quote.ask_traded >= quote.req.AskVolume) {
                instrument_to_quote_.erase(quote.req.InstrumentID);
                it = active_quotes_.erase(it);
                continue;
            }
            ++it;
        }
    }

    [[nodiscard]] std::string current_tick_time_locked() const {
        return last_update_time_.empty() ? cfg_.start_time : last_update_time_;
    }

    FemasSimFrontConfig cfg_;
    Dataset dataset_;
    std::string load_error_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool stop_flag_{false};
    bool replay_finished_{false};
    int next_client_id_{1};
    uint64_t next_order_sys_id_{1};
    uint64_t next_quote_sys_id_{1};
    mutable uint64_t next_trade_id_{1};

    std::unordered_map<std::string, InstrumentRecord> instruments_by_id_;
    std::unordered_map<int, MdClient> md_clients_;
    std::unordered_map<int, TraderClient> trader_clients_;
    std::unordered_map<std::string, BookState> books_;
    std::unordered_map<std::string, ActiveOrder> active_orders_;
    std::unordered_map<std::string, ActiveQuote> active_quotes_;
    std::unordered_map<std::string, std::string> instrument_to_quote_;
    std::string last_update_time_;
    std::thread worker_;
};

class SessionRegistry {
public:
    std::shared_ptr<FemasSimSession> acquire(const FemasSimFrontConfig& cfg) {
        std::lock_guard<std::mutex> lock(mutex_);
        const std::string key = build_key(cfg);
        auto it = sessions_.find(key);
        if (it != sessions_.end()) {
            if (auto existing = it->second.lock()) return existing;
        }
        auto created = std::make_shared<FemasSimSession>(cfg);
        sessions_[key] = created;
        return created;
    }

private:
    static std::string build_key(const FemasSimFrontConfig& cfg) {
        std::ostringstream oss;
        oss << cfg.session_name << "|" << cfg.exchange << "|" << cfg.trading_day
            << "|" << cfg.start_time << "|" << cfg.end_time << "|" << cfg.ddb << "|";
        for (const std::string& product : cfg.products) {
            oss << product << ",";
        }
        return oss.str();
    }

    std::mutex mutex_;
    std::unordered_map<std::string, std::weak_ptr<FemasSimSession>> sessions_;
};

SessionRegistry& session_registry() {
    static SessionRegistry registry;
    return registry;
}

class SimFemasMdApi final : public IFemasMdApi {
public:
    explicit SimFemasMdApi(std::string_view front_addr)
        : front_addr_(front_addr) {}

    void Release() override {
        if (session_ && client_id_ != 0) session_->detach_md_client(client_id_);
        delete this;
    }

    void Init() override {
        ensure_session();
        if (session_ && client_id_ == 0) {
            client_id_ = session_->attach_md_client(spi_, topic_id_);
        }
        if (spi_) spi_->OnFrontConnected();
    }

    void RegisterSpi(CUstpFtdcMduserSpi* spi) override {
        spi_ = spi;
    }

    void SetHeartbeatTimeout(unsigned int timeout) override {
        heartbeat_timeout_sec_ = timeout;
    }

    void SubscribeMarketDataTopic(int topic_id,
                                  USTP_TE_RESUME_TYPE) override {
        topic_id_ = topic_id;
        if (session_ && client_id_ != 0) session_->update_md_topic(client_id_, topic_id_);
    }

    void RegisterFront(char* front_addr) override {
        front_addr_ = front_addr ? front_addr : "";
    }

    int ReqUserLogin(CUstpFtdcReqUserLoginField* req,
                     int request_id) override {
        if (!spi_) return -1;
        ensure_session();

        CUstpFtdcRspUserLoginField login{};
        CUstpFtdcRspInfoField info{};
        if (!session_ || !session_->ready()) {
            info.ErrorID = 1;
            copy_cstr(info.ErrorMsg, session_ ? session_->error() : load_error_);
        } else {
            copy_cstr(login.TradingDay, session_->trading_day());
            copy_cstr(login.BrokerID, req ? req->BrokerID : "");
            copy_cstr(login.UserID, req ? req->UserID : "");
            copy_cstr(login.LoginTime, cfg_.start_time);
        }
        spi_->OnRspUserLogin(&login, info.ErrorID == 0 ? nullptr : &info,
                             request_id, true);
        return info.ErrorID == 0 ? 0 : -1;
    }

    int SubMarketData(char* instrument_ids[], int count) override {
        if (!session_ || client_id_ == 0 || !spi_) return -1;
        auto results = session_->subscribe_md(client_id_, instrument_ids, count);
        for (const auto& result : results) {
            CUstpFtdcSpecificInstrumentField instrument{};
            CUstpFtdcRspInfoField info{};
            copy_cstr(instrument.InstrumentID, result.instrument_id);
            CUstpFtdcRspInfoField* info_ptr = nullptr;
            if (!result.ok) {
                info.ErrorID = 1;
                copy_cstr(info.ErrorMsg, result.error);
                info_ptr = &info;
            }
            spi_->OnRspSubMarketData(&instrument, info_ptr, 0, result.is_last);
        }
        return 0;
    }

private:
    void ensure_session() {
        if (session_ || !load_error_.empty()) return;
        if (!parse_femas_sim_front(front_addr_, &cfg_, &load_error_)) return;
        session_ = session_registry().acquire(cfg_);
    }

    std::string front_addr_;
    std::string load_error_;
    FemasSimFrontConfig cfg_{};
    std::shared_ptr<FemasSimSession> session_;
    CUstpFtdcMduserSpi* spi_{nullptr};
    int client_id_{0};
    int topic_id_{kDefaultTopicId};
    unsigned int heartbeat_timeout_sec_{30};
};

class SimFemasTraderApi final : public IFemasTraderApi {
public:
    explicit SimFemasTraderApi(std::string_view front_addr)
        : front_addr_(front_addr) {}

    void Release() override {
        if (session_ && client_id_ != 0) session_->detach_trader_client(client_id_);
        delete this;
    }

    void Init() override {
        ensure_session();
        if (session_ && client_id_ == 0) {
            client_id_ = session_->attach_trader_client(spi_);
        }
        if (spi_) spi_->OnFrontConnected();
    }

    void RegisterSpi(CUstpFtdcTraderSpi* spi) override {
        spi_ = spi;
    }

    void SubscribePrivateTopic(USTP_TE_RESUME_TYPE) override {
    }

    void SubscribePublicTopic(USTP_TE_RESUME_TYPE) override {
    }

    void RegisterFront(char* front_addr) override {
        front_addr_ = front_addr ? front_addr : "";
    }

    int ReqUserLogin(CUstpFtdcReqUserLoginField* req,
                     int request_id) override {
        if (!spi_) return -1;
        ensure_session();

        CUstpFtdcRspUserLoginField login{};
        CUstpFtdcRspInfoField info{};
        if (!session_ || !session_->ready()) {
            info.ErrorID = 1;
            copy_cstr(info.ErrorMsg, session_ ? session_->error() : load_error_);
        } else {
            copy_cstr(login.TradingDay, session_->trading_day());
            copy_cstr(login.BrokerID, req ? req->BrokerID : "");
            copy_cstr(login.UserID, req ? req->UserID : "");
            copy_cstr(login.LoginTime, cfg_.start_time);
            copy_cstr(login.MaxOrderLocalID, "000000000000");
        }
        spi_->OnRspUserLogin(&login, info.ErrorID == 0 ? nullptr : &info,
                             request_id, true);
        return info.ErrorID == 0 ? 0 : -1;
    }

    int ReqOrderInsert(CUstpFtdcInputOrderField* req,
                       int request_id) override {
        if (!session_ || !req || client_id_ == 0) return -1;
        session_->submit_order(client_id_, *req, request_id);
        return 0;
    }

    int ReqOrderAction(CUstpFtdcOrderActionField* req,
                       int request_id) override {
        if (!session_ || !req || client_id_ == 0) return -1;
        session_->cancel_order(client_id_, *req, request_id);
        return 0;
    }

    int ReqQuoteInsert(CUstpFtdcInputQuoteField* req,
                       int request_id) override {
        if (!session_ || !req || client_id_ == 0) return -1;
        session_->submit_quote(client_id_, *req, request_id);
        return 0;
    }

    int ReqQuoteAction(CUstpFtdcQuoteActionField* req,
                       int request_id) override {
        if (!session_ || !req || client_id_ == 0) return -1;
        session_->cancel_quote(client_id_, *req, request_id);
        return 0;
    }

    int ReqQryInstrument(CUstpFtdcQryInstrumentField* req,
                         int request_id) override {
        if (!session_ || !spi_) return -1;
        const CUstpFtdcQryInstrumentField query = req ? *req : CUstpFtdcQryInstrumentField{};
        std::vector<InstrumentRecord> matches = session_->query_instruments(query);
        if (matches.empty()) {
            spi_->OnRspQryInstrument(nullptr, nullptr, request_id, true);
            return 0;
        }
        for (std::size_t i = 0; i < matches.size(); ++i) {
            CUstpFtdcRspInstrumentField field = matches[i].field;
            spi_->OnRspQryInstrument(&field, nullptr, request_id,
                                     i + 1 == matches.size());
        }
        return 0;
    }

private:
    void ensure_session() {
        if (session_ || !load_error_.empty()) return;
        if (!parse_femas_sim_front(front_addr_, &cfg_, &load_error_)) return;
        session_ = session_registry().acquire(cfg_);
    }

    std::string front_addr_;
    std::string load_error_;
    FemasSimFrontConfig cfg_{};
    std::shared_ptr<FemasSimSession> session_;
    CUstpFtdcTraderSpi* spi_{nullptr};
    int client_id_{0};
};

} // namespace

bool parse_femas_sim_front(std::string_view front_addr,
                           FemasSimFrontConfig* out,
                           std::string* error) {
    if (!out || !error) return false;
    *out = FemasSimFrontConfig{};
    error->clear();

    if (front_addr.rfind("sim://", 0) != 0) {
        *error = "front address must start with sim://";
        return false;
    }

    const std::size_t scheme_end = front_addr.find("://");
    std::string_view body = front_addr.substr(scheme_end + 3);
    const std::size_t query_pos = body.find('?');
    out->session_name = query_pos == std::string_view::npos
        ? std::string(body) : std::string(body.substr(0, query_pos));
    if (out->session_name.empty()) out->session_name = "session";

    if (query_pos != std::string_view::npos) {
        std::string_view query = body.substr(query_pos + 1);
        while (!query.empty()) {
            const std::size_t amp = query.find('&');
            const std::string_view pair = query.substr(0, amp);
            const std::size_t eq = pair.find('=');
            const std::string key = trim(pair.substr(0, eq));
            const std::string value = eq == std::string_view::npos
                ? std::string{} : percent_decode(pair.substr(eq + 1));

            if (key == "exchange") out->exchange = value;
            else if (key == "products") {
                std::stringstream ss(value);
                std::string token;
                while (std::getline(ss, token, ',')) {
                    token = trim(token);
                    if (!token.empty()) out->products.push_back(token);
                }
            } else if (key == "date") out->trading_day = normalize_trading_day(value);
            else if (key == "start") out->start_time = value;
            else if (key == "end") out->end_time = value;
            else if (key == "ddb") out->ddb = value;
            else if (key == "speed") {
                const std::string normalized = trim(value);
                if (starts_with_ci(normalized, "max")) {
                    out->max_speed = true;
                    out->replay_speed = 0.0;
                } else {
                    std::string speed = normalized;
                    if (!speed.empty() && (speed.back() == 'x' || speed.back() == 'X')) {
                        speed.pop_back();
                    }
                    if (!parse_double(speed, &out->replay_speed) || out->replay_speed <= 0.0) {
                        *error = "invalid speed parameter";
                        return false;
                    }
                }
            }

            if (amp == std::string_view::npos) break;
            query.remove_prefix(amp + 1);
        }
    }

    if (out->exchange.empty()) {
        *error = "missing exchange parameter";
        return false;
    }
    if (out->trading_day.empty()) {
        *error = "missing date parameter";
        return false;
    }
    if (out->start_time.empty() || out->end_time.empty()) {
        *error = "missing replay window parameters";
        return false;
    }
    if (out->ddb.empty()) {
        *error = "missing ddb parameter";
        return false;
    }
    if (!out->max_speed && out->replay_speed <= 0.0) {
        out->replay_speed = 1.0;
    }
    return true;
}

IFemasMdApi* create_sim_femas_md_api(std::string_view front_addr) {
    return new SimFemasMdApi(front_addr);
}

IFemasTraderApi* create_sim_femas_trader_api(std::string_view front_addr) {
    return new SimFemasTraderApi(front_addr);
}

} // namespace omm
