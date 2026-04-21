#include "persistence/data_repository.h"

#include "logger/logger.h"

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>

namespace omm {

namespace {

constexpr uint64_t kCtpSyntheticAskLegBit = 1ULL << 47;

template<typename T>
int bind_integer(sqlite3_stmt* stmt, int index, T value) {
    return sqlite3_bind_int64(stmt, index, static_cast<sqlite3_int64>(value));
}

int bind_text(sqlite3_stmt* stmt, int index, const char* text) {
    return sqlite3_bind_text(stmt, index, text ? text : "", -1, SQLITE_TRANSIENT);
}

void reset_stmt(sqlite3_stmt* stmt) noexcept {
    if (stmt == nullptr) return;
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
}

bool step_done(sqlite3* db, sqlite3_stmt* stmt, const char* what) {
    const int rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        OMM_LOG_ERROR("repo", "{} failed rc={} err={}", what, rc, sqlite3_errmsg(db));
        reset_stmt(stmt);
        return false;
    }
    reset_stmt(stmt);
    return true;
}

bool exec_sql(sqlite3* db, const char* sql) {
    char* err = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        OMM_LOG_ERROR("repo", "sql exec failed rc={} err={}", rc, err ? err : "");
        sqlite3_free(err);
        return false;
    }
    return true;
}

bool any_quote_handle(const GatewayQuoteRecoveryHandle& handle) noexcept {
    return handle.valid
        || handle.bid_order_id != 0
        || handle.ask_order_id != 0
        || handle.quote_local_id[0] != '\0'
        || handle.quote_sys_id[0] != '\0'
        || handle.bid_local_id[0] != '\0'
        || handle.ask_local_id[0] != '\0'
        || handle.bid_order_sys_id[0] != '\0'
        || handle.ask_order_sys_id[0] != '\0';
}

bool any_order_handle(const GatewayOrderRecoveryHandle& handle) noexcept {
    return handle.valid
        || handle.is_quote_leg
        || handle.client_quote_id != 0
        || handle.exchange_local_id[0] != '\0'
        || handle.order_sys_id[0] != '\0';
}

const char* safe_code(const Instrument* instrument) noexcept {
    return instrument ? instrument->code.data : "";
}

} // namespace

DataRepository::DataRepository(const PersistenceConfig& cfg,
                               GatewayType gateway_type,
                               VolMethod vol_method)
    : cfg_(cfg)
    , gateway_type_(gateway_type)
    , vol_method_(vol_method) {}

DataRepository::~DataRepository() {
    stop();
    std::lock_guard<std::mutex> lock(db_mutex_);
    close_locked();
}

void DataRepository::set_instruments(const Instrument* instruments,
                                     uint16_t n_instruments) noexcept {
    instruments_ = instruments;
    n_instruments_ = n_instruments;
}

bool DataRepository::open() {
    if (!cfg_.enabled) return true;

    std::lock_guard<std::mutex> lock(db_mutex_);
    if (db_ != nullptr) return true;

    try {
        const std::filesystem::path path(cfg_.data_path);
        if (path.has_parent_path()) {
            std::filesystem::create_directories(path.parent_path());
        }
    } catch (const std::exception& e) {
        OMM_LOG_ERROR("repo", "failed to create data dir path={} err={}", cfg_.data_path, e.what());
        return false;
    }

    const int rc = sqlite3_open(cfg_.data_path, &db_);
    if (rc != SQLITE_OK || db_ == nullptr) {
        OMM_LOG_ERROR("repo", "sqlite open failed path={} rc={}", cfg_.data_path, rc);
        close_locked();
        return false;
    }

    sqlite3_busy_timeout(db_, cfg_.busy_timeout_ms);
    if (!exec_sql(db_, "PRAGMA journal_mode=WAL;")
        || !exec_sql(db_, "PRAGMA synchronous=NORMAL;")
        || !exec_sql(db_, "PRAGMA temp_store=MEMORY;")
        || !exec_sql(db_, "PRAGMA foreign_keys=OFF;")) {
        close_locked();
        return false;
    }

    if (!ensure_schema_locked() || !prepare_statements_locked()) {
        close_locked();
        return false;
    }
    return true;
}

void DataRepository::start() {
    if (!cfg_.enabled || writer_running_.load(std::memory_order_acquire)) return;
    stop_flag_.store(false, std::memory_order_release);
    writer_running_.store(true, std::memory_order_release);
    writer_thread_ = std::thread([this] { writer_loop(); });
}

void DataRepository::stop() noexcept {
    if (!writer_running_.exchange(false, std::memory_order_acq_rel)) return;
    stop_flag_.store(true, std::memory_order_release);
    if (writer_thread_.joinable()) writer_thread_.join();
}

bool DataRepository::persist_instruments() {
    if (!cfg_.enabled) return true;
    std::lock_guard<std::mutex> lock(db_mutex_);
    if (db_ == nullptr) return false;
    if (!exec_sql(db_, "BEGIN IMMEDIATE TRANSACTION;")) return false;
    reset_stmt(stmt_delete_instruments_);
    if (!step_done(db_, stmt_delete_instruments_, "delete instruments")) {
        (void)exec_sql(db_, "ROLLBACK;");
        return false;
    }
    const bool ok = write_instruments_locked(current_trading_day(), "instruments", instruments_, n_instruments_);
    if (!ok) {
        (void)exec_sql(db_, "ROLLBACK;");
        return false;
    }
    return exec_sql(db_, "COMMIT;");
}

bool DataRepository::load_recovery_state(RecoveryState* out) {
    if (out == nullptr) return false;
    *out = RecoveryState{};
    if (!cfg_.enabled) return true;

    std::lock_guard<std::mutex> lock(db_mutex_);
    if (db_ == nullptr) return false;

    auto load_positions = [&]() {
        sqlite3_stmt* stmt = nullptr;
        const char* sql =
            "SELECT instrument_code, product_index, net_position, long_position, short_position,"
            " long_today, short_today, avg_long_price, avg_short_price, realized_pnl"
            " FROM positions_snapshot";
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const auto* code = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            const uint16_t instrument_id = find_instrument_id_by_code(code);
            if (instrument_id == INVALID_INSTRUMENT_ID) continue;
            Position pos{};
            pos.instrument_id = instrument_id;
            pos.product_index = static_cast<uint8_t>(sqlite3_column_int(stmt, 1));
            pos.net_position = sqlite3_column_int(stmt, 2);
            pos.long_position = sqlite3_column_int(stmt, 3);
            pos.short_position = sqlite3_column_int(stmt, 4);
            pos.long_today = sqlite3_column_int(stmt, 5);
            pos.short_today = sqlite3_column_int(stmt, 6);
            pos.avg_long_price = sqlite3_column_double(stmt, 7);
            pos.avg_short_price = sqlite3_column_double(stmt, 8);
            pos.realized_pnl = sqlite3_column_double(stmt, 9);
            out->positions.push_back(pos);
        }
        sqlite3_finalize(stmt);
        return true;
    };

    auto load_mm_params = [&]() {
        sqlite3_stmt* stmt = nullptr;
        const char* sql =
            "SELECT product_index, bid_spread, ask_spread, quote_volume, product_delta_threshold,"
            " product_vega_threshold, min_quote_interval_ms, max_position, warning_position,"
            " base_half_spread_ticks, min_half_spread_ticks, max_half_spread_ticks,"
            " inventory_skew_per_lot_ticks, follow_weight, requote_price_epsilon_ticks,"
            " market_width_widen_threshold_ticks, underlying_move_widen_threshold_ticks,"
            " use_one_sided_at_limits, enabled FROM strategy_params";
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const int product_index = sqlite3_column_int(stmt, 0);
            if (product_index < 0 || product_index >= MAX_PRODUCTS) continue;
            auto& entry = out->mm_params[product_index];
            entry.valid = true;
            entry.params.bid_spread = sqlite3_column_double(stmt, 1);
            entry.params.ask_spread = sqlite3_column_double(stmt, 2);
            entry.params.quote_volume = sqlite3_column_int(stmt, 3);
            entry.params.product_delta_threshold = sqlite3_column_double(stmt, 4);
            entry.params.product_vega_threshold = sqlite3_column_double(stmt, 5);
            entry.params.min_quote_interval_ms = sqlite3_column_double(stmt, 6);
            entry.params.max_position = sqlite3_column_int(stmt, 7);
            entry.params.warning_position = sqlite3_column_int(stmt, 8);
            entry.params.base_half_spread_ticks = sqlite3_column_double(stmt, 9);
            entry.params.min_half_spread_ticks = sqlite3_column_double(stmt, 10);
            entry.params.max_half_spread_ticks = sqlite3_column_double(stmt, 11);
            entry.params.inventory_skew_per_lot_ticks = sqlite3_column_double(stmt, 12);
            entry.params.follow_weight = sqlite3_column_double(stmt, 13);
            entry.params.requote_price_epsilon_ticks = sqlite3_column_double(stmt, 14);
            entry.params.market_width_widen_threshold_ticks = sqlite3_column_double(stmt, 15);
            entry.params.underlying_move_widen_threshold_ticks = sqlite3_column_double(stmt, 16);
            entry.params.use_one_sided_at_limits = sqlite3_column_int(stmt, 17) != 0;
            entry.params.enabled = sqlite3_column_int(stmt, 18) != 0;
        }
        sqlite3_finalize(stmt);
        return true;
    };

    auto load_arb_params = [&]() {
        sqlite3_stmt* stmt = nullptr;
        const char* sql =
            "SELECT product_index, strategy_type, min_edge_ticks, cooldown_ms, scan_interval_ms,"
            " cleanup_timeout_ms, max_order_volume, max_live_orders, cleanup_on_partial, enabled"
            " FROM arb_strategy_params";
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            RecoveryState::ArbParamsState entry{};
            entry.valid = true;
            entry.product_index = static_cast<uint8_t>(sqlite3_column_int(stmt, 0));
            entry.strategy_type =
                static_cast<ArbitrageStrategyType>(sqlite3_column_int(stmt, 1));
            entry.params.min_edge_ticks = sqlite3_column_double(stmt, 2);
            entry.params.cooldown_ms = sqlite3_column_double(stmt, 3);
            entry.params.scan_interval_ms = sqlite3_column_double(stmt, 4);
            entry.params.cleanup_timeout_ms = sqlite3_column_double(stmt, 5);
            entry.params.max_order_volume = sqlite3_column_int(stmt, 6);
            entry.params.max_live_orders = sqlite3_column_int(stmt, 7);
            entry.params.cleanup_on_partial = sqlite3_column_int(stmt, 8) != 0;
            entry.params.enabled = sqlite3_column_int(stmt, 9) != 0;
            out->arb_params.push_back(entry);
        }
        sqlite3_finalize(stmt);
        return true;
    };

    auto load_risk_params = [&]() {
        sqlite3_stmt* stmt = nullptr;
        const char* sql =
            "SELECT max_net_position, max_delta, max_gamma, max_vega"
            " FROM risk_params WHERE id = 1";
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            out->has_risk_params = true;
            out->risk_params.max_net_position = sqlite3_column_int(stmt, 0);
            out->risk_params.max_delta = sqlite3_column_double(stmt, 1);
            out->risk_params.max_gamma = sqlite3_column_double(stmt, 2);
            out->risk_params.max_vega = sqlite3_column_double(stmt, 3);
        }
        sqlite3_finalize(stmt);
        return true;
    };

    auto load_live_orders = [&]() {
        sqlite3_stmt* stmt = nullptr;
        const char* sql =
            "SELECT client_order_id, instrument_code, product_index, account_id, exchange_id,"
            " side, offset_flag, order_type, status, price, volume, filled_volume, avg_fill_price,"
            " send_ts, ack_ts, is_manual, is_hedge, exchange_order_id, is_quote_leg,"
            " client_quote_id, exchange_local_id, order_sys_id"
            " FROM live_orders";
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const auto* code = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            const uint16_t instrument_id = find_instrument_id_by_code(code);
            if (instrument_id == INVALID_INSTRUMENT_ID) continue;

            GatewayRecoveredOrder entry{};
            entry.order.client_order_id = static_cast<OrderId>(sqlite3_column_int64(stmt, 0));
            entry.order.instrument_id = instrument_id;
            entry.order.product_index = static_cast<uint8_t>(sqlite3_column_int(stmt, 2));
            std::strncpy(entry.order.account_id.data,
                         reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3)),
                         sizeof(entry.order.account_id.data) - 1);
            std::strncpy(entry.order.exchange_id.data,
                         reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4)),
                         sizeof(entry.order.exchange_id.data) - 1);
            entry.order.side = static_cast<Side>(sqlite3_column_int(stmt, 5));
            entry.order.offset = static_cast<OffsetFlag>(sqlite3_column_int(stmt, 6));
            entry.order.order_type = static_cast<OrderType>(sqlite3_column_int(stmt, 7));
            entry.order.status = static_cast<OrderStatus>(sqlite3_column_int(stmt, 8));
            entry.order.price = sqlite3_column_double(stmt, 9);
            entry.order.volume = sqlite3_column_int(stmt, 10);
            entry.order.filled_volume = sqlite3_column_int(stmt, 11);
            entry.order.avg_fill_price = sqlite3_column_double(stmt, 12);
            entry.order.send_ts = sqlite3_column_int64(stmt, 13);
            entry.order.ack_ts = sqlite3_column_int64(stmt, 14);
            entry.order.is_manual = sqlite3_column_int(stmt, 15) != 0;
            entry.order.is_hedge = sqlite3_column_int(stmt, 16) != 0;
            entry.order.exchange_order_id = static_cast<OrderId>(sqlite3_column_int64(stmt, 17));

            entry.recovery.valid = true;
            entry.recovery.is_quote_leg = sqlite3_column_int(stmt, 18) != 0;
            entry.recovery.client_quote_id = static_cast<QuoteId>(sqlite3_column_int64(stmt, 19));
            std::strncpy(entry.recovery.exchange_local_id,
                         reinterpret_cast<const char*>(sqlite3_column_text(stmt, 20)),
                         sizeof(entry.recovery.exchange_local_id) - 1);
            std::strncpy(entry.recovery.order_sys_id,
                         reinterpret_cast<const char*>(sqlite3_column_text(stmt, 21)),
                         sizeof(entry.recovery.order_sys_id) - 1);
            out->live_orders.push_back(entry);
        }
        sqlite3_finalize(stmt);
        return true;
    };

    auto load_live_quotes = [&]() {
        sqlite3_stmt* stmt = nullptr;
        const char* sql =
            "SELECT client_quote_id, instrument_code, product_index, account_id, exchange_id,"
            " bid_offset, ask_offset, bid_price, ask_price, bid_volume, ask_volume,"
            " bid_status, ask_status, send_ts, ack_ts, exchange_quote_id,"
            " remaining_bid, remaining_ask, bid_order_id, ask_order_id, quote_local_id,"
            " quote_sys_id, bid_local_id, ask_local_id, bid_order_sys_id, ask_order_sys_id"
            " FROM live_quotes";
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const auto* code = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            const uint16_t instrument_id = find_instrument_id_by_code(code);
            if (instrument_id == INVALID_INSTRUMENT_ID) continue;

            GatewayRecoveredQuote entry{};
            entry.quote.client_quote_id = static_cast<QuoteId>(sqlite3_column_int64(stmt, 0));
            entry.quote.instrument_id = instrument_id;
            entry.quote.product_index = static_cast<uint8_t>(sqlite3_column_int(stmt, 2));
            std::strncpy(entry.quote.account_id.data,
                         reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3)),
                         sizeof(entry.quote.account_id.data) - 1);
            std::strncpy(entry.quote.exchange_id.data,
                         reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4)),
                         sizeof(entry.quote.exchange_id.data) - 1);
            entry.quote.bid_offset = static_cast<OffsetFlag>(sqlite3_column_int(stmt, 5));
            entry.quote.ask_offset = static_cast<OffsetFlag>(sqlite3_column_int(stmt, 6));
            entry.quote.bid_price = sqlite3_column_double(stmt, 7);
            entry.quote.ask_price = sqlite3_column_double(stmt, 8);
            entry.quote.bid_volume = sqlite3_column_int(stmt, 16);
            entry.quote.ask_volume = sqlite3_column_int(stmt, 17);
            entry.quote.bid_status = static_cast<OrderStatus>(sqlite3_column_int(stmt, 11));
            entry.quote.ask_status = static_cast<OrderStatus>(sqlite3_column_int(stmt, 12));
            entry.quote.send_ts = sqlite3_column_int64(stmt, 13);
            entry.quote.ack_ts = sqlite3_column_int64(stmt, 14);
            entry.quote.exchange_quote_id = static_cast<QuoteId>(sqlite3_column_int64(stmt, 15));

            entry.recovery.valid = true;
            entry.recovery.bid_order_id = static_cast<OrderId>(sqlite3_column_int64(stmt, 18));
            entry.recovery.ask_order_id = static_cast<OrderId>(sqlite3_column_int64(stmt, 19));
            std::strncpy(entry.recovery.quote_local_id,
                         reinterpret_cast<const char*>(sqlite3_column_text(stmt, 20)),
                         sizeof(entry.recovery.quote_local_id) - 1);
            std::strncpy(entry.recovery.quote_sys_id,
                         reinterpret_cast<const char*>(sqlite3_column_text(stmt, 21)),
                         sizeof(entry.recovery.quote_sys_id) - 1);
            std::strncpy(entry.recovery.bid_local_id,
                         reinterpret_cast<const char*>(sqlite3_column_text(stmt, 22)),
                         sizeof(entry.recovery.bid_local_id) - 1);
            std::strncpy(entry.recovery.ask_local_id,
                         reinterpret_cast<const char*>(sqlite3_column_text(stmt, 23)),
                         sizeof(entry.recovery.ask_local_id) - 1);
            std::strncpy(entry.recovery.bid_order_sys_id,
                         reinterpret_cast<const char*>(sqlite3_column_text(stmt, 24)),
                         sizeof(entry.recovery.bid_order_sys_id) - 1);
            std::strncpy(entry.recovery.ask_order_sys_id,
                         reinterpret_cast<const char*>(sqlite3_column_text(stmt, 25)),
                         sizeof(entry.recovery.ask_order_sys_id) - 1);
            out->live_quotes.push_back(entry);
        }
        sqlite3_finalize(stmt);
        return true;
    };

    const bool ok = load_positions()
        && load_mm_params()
        && load_arb_params()
        && load_risk_params()
        && load_live_orders()
        && load_live_quotes();
    if (ok) {
        prime_caches_locked(*out);
    }
    return ok;
}

bool DataRepository::persist_end_of_day_snapshot(const EndOfDaySnapshot& snapshot) {
    if (!cfg_.enabled) return true;
    std::lock_guard<std::mutex> lock(db_mutex_);
    if (db_ == nullptr) return false;
    return write_eod_snapshot_locked(snapshot);
}

bool DataRepository::enqueue_order_event(const OrderPersistenceEvent& event) noexcept {
    if (!cfg_.enabled) return true;
    return order_events_.try_push(event);
}

bool DataRepository::enqueue_quote_event(const QuotePersistenceEvent& event) noexcept {
    if (!cfg_.enabled) return true;
    return quote_events_.try_push(event);
}

bool DataRepository::enqueue_trade(const Trade& trade) noexcept {
    if (!cfg_.enabled) return true;
    return trades_.try_push(trade);
}

bool DataRepository::enqueue_mm_params(const MMParamsPersistenceEvent& event) noexcept {
    if (!cfg_.enabled) return true;
    return mm_param_events_.try_push(event);
}

bool DataRepository::enqueue_arb_params(const ArbParamsPersistenceEvent& event) noexcept {
    if (!cfg_.enabled) return true;
    return arb_param_events_.try_push(event);
}

bool DataRepository::enqueue_risk_params(const RiskParamsPersistenceEvent& event) noexcept {
    if (!cfg_.enabled) return true;
    return risk_param_events_.try_push(event);
}

bool DataRepository::enqueue_positions_snapshot(const PositionSnapshotEvent& event) noexcept {
    if (!cfg_.enabled) return true;
    return position_snapshots_.try_push(event);
}

void DataRepository::writer_loop() noexcept {
    while (!stop_flag_.load(std::memory_order_acquire)
           || !order_events_.empty_approx()
           || !quote_events_.empty_approx()
           || !trades_.empty_approx()
           || !mm_param_events_.empty_approx()
           || !arb_param_events_.empty_approx()
           || !risk_param_events_.empty_approx()
           || !position_snapshots_.empty_approx()) {
        bool did_work = false;
        {
            std::lock_guard<std::mutex> lock(db_mutex_);
            if (db_ != nullptr) {
                did_work = flush_once_locked(cfg_.batch_max_rows);
            }
        }
        if (!did_work) {
            std::this_thread::sleep_for(std::chrono::milliseconds(
                std::max(1, cfg_.flush_interval_ms)));
        }
    }
}

bool DataRepository::ensure_schema_locked() {
    static const char* kSchemaSql =
        "PRAGMA user_version = 1;"
        "CREATE TABLE IF NOT EXISTS instruments ("
        " instrument_code TEXT PRIMARY KEY,"
        " trading_day INTEGER NOT NULL,"
        " underlying_code TEXT NOT NULL,"
        " exchange_id TEXT NOT NULL,"
        " exchange INTEGER NOT NULL,"
        " kind INTEGER NOT NULL,"
        " option_type INTEGER NOT NULL,"
        " strike REAL NOT NULL,"
        " multiplier REAL NOT NULL,"
        " tick_size REAL NOT NULL,"
        " expiry_date INTEGER NOT NULL,"
        " expiry_epoch_ns INTEGER NOT NULL,"
        " product_index INTEGER NOT NULL,"
        " underlying_instrument_code TEXT NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS trades ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " client_order_id INTEGER NOT NULL,"
        " trade_id INTEGER NOT NULL,"
        " instrument_code TEXT NOT NULL,"
        " product_index INTEGER NOT NULL,"
        " account_id TEXT NOT NULL,"
        " exchange_id TEXT NOT NULL,"
        " side INTEGER NOT NULL,"
        " offset_flag INTEGER NOT NULL,"
        " fill_price REAL NOT NULL,"
        " fill_volume INTEGER NOT NULL,"
        " fill_ts INTEGER NOT NULL,"
        " UNIQUE(client_order_id, trade_id, instrument_code, fill_ts, side, fill_volume)"
        ");"
        "CREATE TABLE IF NOT EXISTS live_orders ("
        " client_order_id INTEGER PRIMARY KEY,"
        " instrument_code TEXT NOT NULL,"
        " product_index INTEGER NOT NULL,"
        " account_id TEXT NOT NULL,"
        " exchange_id TEXT NOT NULL,"
        " side INTEGER NOT NULL,"
        " offset_flag INTEGER NOT NULL,"
        " order_type INTEGER NOT NULL,"
        " status INTEGER NOT NULL,"
        " price REAL NOT NULL,"
        " volume INTEGER NOT NULL,"
        " filled_volume INTEGER NOT NULL,"
        " avg_fill_price REAL NOT NULL,"
        " send_ts INTEGER NOT NULL,"
        " ack_ts INTEGER NOT NULL,"
        " is_manual INTEGER NOT NULL,"
        " is_hedge INTEGER NOT NULL,"
        " exchange_order_id INTEGER NOT NULL,"
        " is_quote_leg INTEGER NOT NULL,"
        " client_quote_id INTEGER NOT NULL,"
        " exchange_local_id TEXT NOT NULL,"
        " order_sys_id TEXT NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS live_quotes ("
        " client_quote_id INTEGER PRIMARY KEY,"
        " instrument_code TEXT NOT NULL,"
        " product_index INTEGER NOT NULL,"
        " account_id TEXT NOT NULL,"
        " exchange_id TEXT NOT NULL,"
        " bid_offset INTEGER NOT NULL,"
        " ask_offset INTEGER NOT NULL,"
        " bid_price REAL NOT NULL,"
        " ask_price REAL NOT NULL,"
        " bid_volume INTEGER NOT NULL,"
        " ask_volume INTEGER NOT NULL,"
        " bid_status INTEGER NOT NULL,"
        " ask_status INTEGER NOT NULL,"
        " send_ts INTEGER NOT NULL,"
        " ack_ts INTEGER NOT NULL,"
        " exchange_quote_id INTEGER NOT NULL,"
        " remaining_bid INTEGER NOT NULL,"
        " remaining_ask INTEGER NOT NULL,"
        " bid_order_id INTEGER NOT NULL,"
        " ask_order_id INTEGER NOT NULL,"
        " quote_local_id TEXT NOT NULL,"
        " quote_sys_id TEXT NOT NULL,"
        " bid_local_id TEXT NOT NULL,"
        " ask_local_id TEXT NOT NULL,"
        " bid_order_sys_id TEXT NOT NULL,"
        " ask_order_sys_id TEXT NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS positions_snapshot ("
        " instrument_code TEXT PRIMARY KEY,"
        " product_index INTEGER NOT NULL,"
        " net_position INTEGER NOT NULL,"
        " long_position INTEGER NOT NULL,"
        " short_position INTEGER NOT NULL,"
        " long_today INTEGER NOT NULL,"
        " short_today INTEGER NOT NULL,"
        " avg_long_price REAL NOT NULL,"
        " avg_short_price REAL NOT NULL,"
        " realized_pnl REAL NOT NULL,"
        " snapshot_ts INTEGER NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS strategy_params ("
        " product_index INTEGER PRIMARY KEY,"
        " bid_spread REAL NOT NULL,"
        " ask_spread REAL NOT NULL,"
        " quote_volume INTEGER NOT NULL,"
        " product_delta_threshold REAL NOT NULL,"
        " product_vega_threshold REAL NOT NULL,"
        " min_quote_interval_ms REAL NOT NULL,"
        " max_position INTEGER NOT NULL,"
        " warning_position INTEGER NOT NULL,"
        " base_half_spread_ticks REAL NOT NULL,"
        " min_half_spread_ticks REAL NOT NULL,"
        " max_half_spread_ticks REAL NOT NULL,"
        " inventory_skew_per_lot_ticks REAL NOT NULL,"
        " follow_weight REAL NOT NULL,"
        " requote_price_epsilon_ticks REAL NOT NULL,"
        " market_width_widen_threshold_ticks REAL NOT NULL,"
        " underlying_move_widen_threshold_ticks REAL NOT NULL,"
        " use_one_sided_at_limits INTEGER NOT NULL,"
        " enabled INTEGER NOT NULL,"
        " update_ts INTEGER NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS arb_strategy_params ("
        " product_index INTEGER NOT NULL,"
        " strategy_type INTEGER NOT NULL,"
        " min_edge_ticks REAL NOT NULL,"
        " cooldown_ms REAL NOT NULL,"
        " scan_interval_ms REAL NOT NULL,"
        " cleanup_timeout_ms REAL NOT NULL,"
        " max_order_volume INTEGER NOT NULL,"
        " max_live_orders INTEGER NOT NULL,"
        " cleanup_on_partial INTEGER NOT NULL,"
        " enabled INTEGER NOT NULL,"
        " update_ts INTEGER NOT NULL,"
        " PRIMARY KEY(product_index, strategy_type)"
        ");"
        "CREATE TABLE IF NOT EXISTS risk_params ("
        " id INTEGER PRIMARY KEY CHECK(id = 1),"
        " max_net_position INTEGER NOT NULL,"
        " max_delta REAL NOT NULL,"
        " max_gamma REAL NOT NULL,"
        " max_vega REAL NOT NULL,"
        " update_ts INTEGER NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS eod_greeks ("
        " trading_day INTEGER NOT NULL,"
        " instrument_code TEXT NOT NULL,"
        " theo_price REAL NOT NULL,"
        " delta REAL NOT NULL,"
        " gamma REAL NOT NULL,"
        " vega REAL NOT NULL,"
        " theta REAL NOT NULL,"
        " rho REAL NOT NULL,"
        " iv REAL NOT NULL,"
        " time_to_expiry REAL NOT NULL,"
        " calc_ts INTEGER NOT NULL,"
        " PRIMARY KEY(trading_day, instrument_code)"
        ");"
        "CREATE TABLE IF NOT EXISTS eod_vol_model_params ("
        " trading_day INTEGER NOT NULL,"
        " product_index INTEGER NOT NULL,"
        " model_type INTEGER NOT NULL,"
        " expiry_t REAL NOT NULL,"
        " a REAL NOT NULL,"
        " b REAL NOT NULL,"
        " rho REAL NOT NULL,"
        " m REAL NOT NULL,"
        " sigma REAL NOT NULL,"
        " atm_vol REAL NOT NULL,"
        " slope_call REAL NOT NULL,"
        " slope_put REAL NOT NULL,"
        " curve_call REAL NOT NULL,"
        " curve_put REAL NOT NULL,"
        " ref_price REAL NOT NULL,"
        " atm_forward REAL NOT NULL,"
        " ssr REAL NOT NULL,"
        " vol_ref REAL NOT NULL,"
        " slope_ref REAL NOT NULL,"
        " vcr REAL NOT NULL,"
        " scr REAL NOT NULL,"
        " put_curv REAL NOT NULL,"
        " call_curv REAL NOT NULL,"
        " down_cutoff REAL NOT NULL,"
        " up_cutoff REAL NOT NULL,"
        " down_smoothing REAL NOT NULL,"
        " up_smoothing REAL NOT NULL,"
        " PRIMARY KEY(trading_day, product_index, model_type, expiry_t)"
        ");"
        "CREATE TABLE IF NOT EXISTS eod_instruments ("
        " trading_day INTEGER NOT NULL,"
        " instrument_code TEXT NOT NULL,"
        " underlying_code TEXT NOT NULL,"
        " exchange_id TEXT NOT NULL,"
        " exchange INTEGER NOT NULL,"
        " kind INTEGER NOT NULL,"
        " option_type INTEGER NOT NULL,"
        " strike REAL NOT NULL,"
        " multiplier REAL NOT NULL,"
        " tick_size REAL NOT NULL,"
        " expiry_date INTEGER NOT NULL,"
        " expiry_epoch_ns INTEGER NOT NULL,"
        " product_index INTEGER NOT NULL,"
        " underlying_instrument_code TEXT NOT NULL,"
        " PRIMARY KEY(trading_day, instrument_code)"
        ");";
    return exec_sql(db_, kSchemaSql);
}

bool DataRepository::prepare_statements_locked() {
    auto prepare = [&](sqlite3_stmt** stmt, const char* sql) {
        return sqlite3_prepare_v2(db_, sql, -1, stmt, nullptr) == SQLITE_OK;
    };

    return prepare(&stmt_upsert_live_order_,
                   "INSERT INTO live_orders ("
                   " client_order_id, instrument_code, product_index, account_id, exchange_id,"
                   " side, offset_flag, order_type, status, price, volume, filled_volume,"
                   " avg_fill_price, send_ts, ack_ts, is_manual, is_hedge, exchange_order_id,"
                   " is_quote_leg, client_quote_id, exchange_local_id, order_sys_id)"
                   " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"
                   " ON CONFLICT(client_order_id) DO UPDATE SET"
                   " instrument_code=excluded.instrument_code,"
                   " product_index=excluded.product_index,"
                   " account_id=excluded.account_id,"
                   " exchange_id=excluded.exchange_id,"
                   " side=excluded.side,"
                   " offset_flag=excluded.offset_flag,"
                   " order_type=excluded.order_type,"
                   " status=excluded.status,"
                   " price=excluded.price,"
                   " volume=excluded.volume,"
                   " filled_volume=excluded.filled_volume,"
                   " avg_fill_price=excluded.avg_fill_price,"
                   " send_ts=excluded.send_ts,"
                   " ack_ts=excluded.ack_ts,"
                   " is_manual=excluded.is_manual,"
                   " is_hedge=excluded.is_hedge,"
                   " exchange_order_id=excluded.exchange_order_id,"
                   " is_quote_leg=excluded.is_quote_leg,"
                   " client_quote_id=excluded.client_quote_id,"
                   " exchange_local_id=excluded.exchange_local_id,"
                   " order_sys_id=excluded.order_sys_id")
        && prepare(&stmt_delete_live_order_, "DELETE FROM live_orders WHERE client_order_id = ?")
        && prepare(&stmt_upsert_live_quote_,
                   "INSERT INTO live_quotes ("
                   " client_quote_id, instrument_code, product_index, account_id, exchange_id,"
                   " bid_offset, ask_offset, bid_price, ask_price, bid_volume, ask_volume,"
                   " bid_status, ask_status, send_ts, ack_ts, exchange_quote_id, remaining_bid,"
                   " remaining_ask, bid_order_id, ask_order_id, quote_local_id, quote_sys_id,"
                   " bid_local_id, ask_local_id, bid_order_sys_id, ask_order_sys_id)"
                   " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"
                   " ON CONFLICT(client_quote_id) DO UPDATE SET"
                   " instrument_code=excluded.instrument_code,"
                   " product_index=excluded.product_index,"
                   " account_id=excluded.account_id,"
                   " exchange_id=excluded.exchange_id,"
                   " bid_offset=excluded.bid_offset,"
                   " ask_offset=excluded.ask_offset,"
                   " bid_price=excluded.bid_price,"
                   " ask_price=excluded.ask_price,"
                   " bid_volume=excluded.bid_volume,"
                   " ask_volume=excluded.ask_volume,"
                   " bid_status=excluded.bid_status,"
                   " ask_status=excluded.ask_status,"
                   " send_ts=excluded.send_ts,"
                   " ack_ts=excluded.ack_ts,"
                   " exchange_quote_id=excluded.exchange_quote_id,"
                   " remaining_bid=excluded.remaining_bid,"
                   " remaining_ask=excluded.remaining_ask,"
                   " bid_order_id=excluded.bid_order_id,"
                   " ask_order_id=excluded.ask_order_id,"
                   " quote_local_id=excluded.quote_local_id,"
                   " quote_sys_id=excluded.quote_sys_id,"
                   " bid_local_id=excluded.bid_local_id,"
                   " ask_local_id=excluded.ask_local_id,"
                   " bid_order_sys_id=excluded.bid_order_sys_id,"
                   " ask_order_sys_id=excluded.ask_order_sys_id")
        && prepare(&stmt_delete_live_quote_, "DELETE FROM live_quotes WHERE client_quote_id = ?")
        && prepare(&stmt_insert_trade_,
                   "INSERT OR IGNORE INTO trades ("
                   " client_order_id, trade_id, instrument_code, product_index, account_id,"
                   " exchange_id, side, offset_flag, fill_price, fill_volume, fill_ts)"
                   " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)")
        && prepare(&stmt_insert_position_,
                   "INSERT OR REPLACE INTO positions_snapshot ("
                   " instrument_code, product_index, net_position, long_position, short_position,"
                   " long_today, short_today, avg_long_price, avg_short_price, realized_pnl, snapshot_ts)"
                   " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)")
        && prepare(&stmt_clear_positions_, "DELETE FROM positions_snapshot")
        && prepare(&stmt_upsert_mm_params_,
                   "INSERT OR REPLACE INTO strategy_params ("
                   " product_index, bid_spread, ask_spread, quote_volume, product_delta_threshold,"
                   " product_vega_threshold, min_quote_interval_ms, max_position, warning_position,"
                   " base_half_spread_ticks, min_half_spread_ticks, max_half_spread_ticks,"
                   " inventory_skew_per_lot_ticks, follow_weight, requote_price_epsilon_ticks,"
                   " market_width_widen_threshold_ticks, underlying_move_widen_threshold_ticks,"
                   " use_one_sided_at_limits, enabled, update_ts)"
                   " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)")
        && prepare(&stmt_upsert_arb_params_,
                   "INSERT OR REPLACE INTO arb_strategy_params ("
                   " product_index, strategy_type, min_edge_ticks, cooldown_ms, scan_interval_ms,"
                   " cleanup_timeout_ms, max_order_volume, max_live_orders, cleanup_on_partial,"
                   " enabled, update_ts)"
                   " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)")
        && prepare(&stmt_replace_risk_params_,
                   "INSERT OR REPLACE INTO risk_params ("
                   " id, max_net_position, max_delta, max_gamma, max_vega, update_ts)"
                   " VALUES (1, ?, ?, ?, ?, ?)")
        && prepare(&stmt_replace_instrument_,
                   "INSERT OR REPLACE INTO instruments ("
                   " instrument_code, trading_day, underlying_code, exchange_id, exchange, kind,"
                   " option_type, strike, multiplier, tick_size, expiry_date, expiry_epoch_ns,"
                   " product_index, underlying_instrument_code)"
                   " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)")
        && prepare(&stmt_delete_instruments_, "DELETE FROM instruments")
        && prepare(&stmt_replace_eod_greeks_,
                   "INSERT OR REPLACE INTO eod_greeks ("
                   " trading_day, instrument_code, theo_price, delta, gamma, vega, theta, rho,"
                   " iv, time_to_expiry, calc_ts)"
                   " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)")
        && prepare(&stmt_replace_eod_model_params_,
                   "INSERT OR REPLACE INTO eod_vol_model_params ("
                   " trading_day, product_index, model_type, expiry_t, a, b, rho, m, sigma,"
                   " atm_vol, slope_call, slope_put, curve_call, curve_put, ref_price,"
                   " atm_forward, ssr, vol_ref, slope_ref, vcr, scr, put_curv, call_curv,"
                   " down_cutoff, up_cutoff, down_smoothing, up_smoothing)"
                   " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)")
        && prepare(&stmt_replace_eod_instrument_,
                   "INSERT OR REPLACE INTO eod_instruments ("
                   " instrument_code, trading_day, underlying_code, exchange_id, exchange, kind,"
                   " option_type, strike, multiplier, tick_size, expiry_date, expiry_epoch_ns,"
                   " product_index, underlying_instrument_code)"
                   " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)")
        && prepare(&stmt_delete_eod_greeks_for_day_,
                   "DELETE FROM eod_greeks WHERE trading_day = ?")
        && prepare(&stmt_delete_eod_model_params_for_day_,
                   "DELETE FROM eod_vol_model_params WHERE trading_day = ?")
        && prepare(&stmt_delete_eod_instruments_for_day_,
                   "DELETE FROM eod_instruments WHERE trading_day = ?");
}

void DataRepository::finalize_statements_locked() noexcept {
    auto finalize = [](sqlite3_stmt*& stmt) {
        if (stmt != nullptr) {
            sqlite3_finalize(stmt);
            stmt = nullptr;
        }
    };
    finalize(stmt_upsert_live_order_);
    finalize(stmt_delete_live_order_);
    finalize(stmt_upsert_live_quote_);
    finalize(stmt_delete_live_quote_);
    finalize(stmt_insert_trade_);
    finalize(stmt_insert_position_);
    finalize(stmt_clear_positions_);
    finalize(stmt_upsert_mm_params_);
    finalize(stmt_upsert_arb_params_);
    finalize(stmt_replace_risk_params_);
    finalize(stmt_replace_instrument_);
    finalize(stmt_delete_instruments_);
    finalize(stmt_replace_eod_greeks_);
    finalize(stmt_replace_eod_model_params_);
    finalize(stmt_replace_eod_instrument_);
    finalize(stmt_delete_eod_greeks_for_day_);
    finalize(stmt_delete_eod_model_params_for_day_);
    finalize(stmt_delete_eod_instruments_for_day_);
}

void DataRepository::close_locked() noexcept {
    finalize_statements_locked();
    if (db_ != nullptr) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

bool DataRepository::flush_once_locked(int max_rows) {
    if (db_ == nullptr) return false;

    int rows = 0;
    if (order_events_.empty_approx()
        && quote_events_.empty_approx()
        && trades_.empty_approx()
        && mm_param_events_.empty_approx()
        && arb_param_events_.empty_approx()
        && risk_param_events_.empty_approx()
        && position_snapshots_.empty_approx()) {
        return false;
    }

    if (!exec_sql(db_, "BEGIN IMMEDIATE TRANSACTION;")) return false;

    OrderPersistenceEvent order_event{};
    while (rows < max_rows && order_events_.try_pop(order_event)) {
        if (!write_order_event_locked(order_event)) {
            (void)exec_sql(db_, "ROLLBACK;");
            return false;
        }
        ++rows;
    }

    QuotePersistenceEvent quote_event{};
    while (rows < max_rows && quote_events_.try_pop(quote_event)) {
        if (!write_quote_event_locked(quote_event)) {
            (void)exec_sql(db_, "ROLLBACK;");
            return false;
        }
        ++rows;
    }

    Trade trade{};
    while (rows < max_rows && trades_.try_pop(trade)) {
        if (!write_trade_locked(trade)) {
            (void)exec_sql(db_, "ROLLBACK;");
            return false;
        }
        ++rows;
    }

    MMParamsPersistenceEvent mm_event{};
    while (rows < max_rows && mm_param_events_.try_pop(mm_event)) {
        if (!write_mm_params_locked(mm_event)) {
            (void)exec_sql(db_, "ROLLBACK;");
            return false;
        }
        ++rows;
    }

    ArbParamsPersistenceEvent arb_event{};
    while (rows < max_rows && arb_param_events_.try_pop(arb_event)) {
        if (!write_arb_params_locked(arb_event)) {
            (void)exec_sql(db_, "ROLLBACK;");
            return false;
        }
        ++rows;
    }

    RiskParamsPersistenceEvent risk_event{};
    while (rows < max_rows && risk_param_events_.try_pop(risk_event)) {
        if (!write_risk_params_locked(risk_event)) {
            (void)exec_sql(db_, "ROLLBACK;");
            return false;
        }
        ++rows;
    }

    PositionSnapshotEvent positions_event{};
    while (rows < max_rows && position_snapshots_.try_pop(positions_event)) {
        if (!write_positions_snapshot_locked(positions_event)) {
            (void)exec_sql(db_, "ROLLBACK;");
            return false;
        }
        ++rows;
    }

    if (rows == 0) {
        (void)exec_sql(db_, "ROLLBACK;");
        return false;
    }
    return exec_sql(db_, "COMMIT;");
}

bool DataRepository::write_order_event_locked(const OrderPersistenceEvent& event) {
    const bool is_ctp_quote_leg = gateway_type_ == GatewayType::CTP
        && (is_ctp_ask_leg(event.order.client_order_id)
            || live_quotes_.find(event.order.client_order_id) != live_quotes_.end());
    const bool is_quote_leg = event.recovery.is_quote_leg || is_ctp_quote_leg;

    if (is_quote_leg) {
        QuoteId quote_id = event.recovery.client_quote_id;
        if (quote_id == 0) {
            quote_id = is_ctp_ask_leg(event.order.client_order_id)
                ? (event.order.client_order_id & ~kCtpSyntheticAskLegBit)
                : event.order.client_order_id;
        }
        auto it = live_quotes_.find(quote_id);
        if (it == live_quotes_.end()) return true;
        LiveQuoteRecord& record = it->second;
        if (event.order.side == Side::Buy) {
            record.quote.bid_status = event.order.status;
        } else {
            record.quote.ask_status = event.order.status;
        }
        if (event.type == OrderPersistenceEventType::Cancel
            || event.type == OrderPersistenceEventType::Reject) {
            if (event.order.side == Side::Buy) record.remaining_bid = 0;
            else record.remaining_ask = 0;
        }
        if (any_order_handle(event.recovery)) {
            if (event.order.side == Side::Buy) {
                if (event.recovery.order_sys_id[0]) {
                    std::strncpy(record.recovery.bid_order_sys_id,
                                 event.recovery.order_sys_id,
                                 sizeof(record.recovery.bid_order_sys_id) - 1);
                }
                if (event.recovery.exchange_local_id[0]) {
                    std::strncpy(record.recovery.bid_local_id,
                                 event.recovery.exchange_local_id,
                                 sizeof(record.recovery.bid_local_id) - 1);
                }
            } else {
                if (event.recovery.order_sys_id[0]) {
                    std::strncpy(record.recovery.ask_order_sys_id,
                                 event.recovery.order_sys_id,
                                 sizeof(record.recovery.ask_order_sys_id) - 1);
                }
                if (event.recovery.exchange_local_id[0]) {
                    std::strncpy(record.recovery.ask_local_id,
                                 event.recovery.exchange_local_id,
                                 sizeof(record.recovery.ask_local_id) - 1);
                }
            }
            record.recovery.valid = record.recovery.valid || any_order_handle(event.recovery);
        }
        QuotePersistenceEvent quote_event{};
        quote_event.type = QuotePersistenceEventType::Ack;
        quote_event.quote = record.quote;
        quote_event.recovery = record.recovery;
        return write_quote_event_locked(quote_event);
    }

    LiveOrderRecord& record = live_orders_[event.order.client_order_id];
    if (event.type == OrderPersistenceEventType::Submit || record.order.client_order_id == 0) {
        record.order = event.order;
    } else {
        Order merged = record.order;
        merged.exchange_order_id = event.order.exchange_order_id != 0
            ? event.order.exchange_order_id : merged.exchange_order_id;
        merged.status = event.order.status;
        merged.filled_volume = std::max(merged.filled_volume, event.order.filled_volume);
        merged.avg_fill_price = event.order.avg_fill_price > 0.0
            ? event.order.avg_fill_price : merged.avg_fill_price;
        merged.ack_ts = event.order.ack_ts != 0 ? event.order.ack_ts : merged.ack_ts;
        record.order = merged;
    }
    if (any_order_handle(event.recovery)) {
        record.recovery = event.recovery;
    }
    if (event.type == OrderPersistenceEventType::Cancel
        || event.type == OrderPersistenceEventType::Reject) {
        mark_live_order_deleted_locked(event.order.client_order_id);
        return true;
    }

    reset_stmt(stmt_upsert_live_order_);
    bind_integer(stmt_upsert_live_order_, 1, record.order.client_order_id);
    bind_text(stmt_upsert_live_order_, 2, safe_code(instrument_by_id(record.order.instrument_id)));
    bind_integer(stmt_upsert_live_order_, 3, record.order.product_index);
    bind_text(stmt_upsert_live_order_, 4, record.order.account_id.data);
    bind_text(stmt_upsert_live_order_, 5, record.order.exchange_id.data);
    bind_integer(stmt_upsert_live_order_, 6, static_cast<int>(record.order.side));
    bind_integer(stmt_upsert_live_order_, 7, static_cast<int>(record.order.offset));
    bind_integer(stmt_upsert_live_order_, 8, static_cast<int>(record.order.order_type));
    bind_integer(stmt_upsert_live_order_, 9, static_cast<int>(record.order.status));
    sqlite3_bind_double(stmt_upsert_live_order_, 10, record.order.price);
    bind_integer(stmt_upsert_live_order_, 11, record.order.volume);
    bind_integer(stmt_upsert_live_order_, 12, record.order.filled_volume);
    sqlite3_bind_double(stmt_upsert_live_order_, 13, record.order.avg_fill_price);
    bind_integer(stmt_upsert_live_order_, 14, record.order.send_ts);
    bind_integer(stmt_upsert_live_order_, 15, record.order.ack_ts);
    bind_integer(stmt_upsert_live_order_, 16, record.order.is_manual ? 1 : 0);
    bind_integer(stmt_upsert_live_order_, 17, record.order.is_hedge ? 1 : 0);
    bind_integer(stmt_upsert_live_order_, 18, record.order.exchange_order_id);
    bind_integer(stmt_upsert_live_order_, 19, record.recovery.is_quote_leg ? 1 : 0);
    bind_integer(stmt_upsert_live_order_, 20, record.recovery.client_quote_id);
    bind_text(stmt_upsert_live_order_, 21, record.recovery.exchange_local_id);
    bind_text(stmt_upsert_live_order_, 22, record.recovery.order_sys_id);
    return step_done(db_, stmt_upsert_live_order_, "upsert live_order");
}

bool DataRepository::write_quote_event_locked(const QuotePersistenceEvent& event) {
    if (event.type == QuotePersistenceEventType::Submit
        && event.quote.bid_volume == 0
        && event.quote.ask_volume == 0) {
        return true;
    }

    if (event.type == QuotePersistenceEventType::Cancel
        || event.type == QuotePersistenceEventType::Reject) {
        mark_live_quote_deleted_locked(event.quote.client_quote_id);
        return true;
    }

    LiveQuoteRecord& record = live_quotes_[event.quote.client_quote_id];
    if (event.type == QuotePersistenceEventType::Submit || record.quote.client_quote_id == 0) {
        record.quote = event.quote;
        record.remaining_bid = event.quote.bid_volume;
        record.remaining_ask = event.quote.ask_volume;
        record.recovery = event.recovery;
        if (gateway_type_ == GatewayType::CTP) {
            if (record.recovery.bid_order_id == 0) {
                record.recovery.bid_order_id = event.quote.client_quote_id;
            }
            if (record.recovery.ask_order_id == 0) {
                record.recovery.ask_order_id = event.quote.client_quote_id | kCtpSyntheticAskLegBit;
            }
            record.recovery.valid = true;
        }
    } else {
        Quote merged = record.quote;
        merged.exchange_quote_id = event.quote.exchange_quote_id != 0
            ? event.quote.exchange_quote_id : merged.exchange_quote_id;
        merged.bid_status = event.quote.bid_status;
        merged.ask_status = event.quote.ask_status;
        merged.ack_ts = event.quote.ack_ts != 0 ? event.quote.ack_ts : merged.ack_ts;
        record.quote = merged;
        if (any_quote_handle(event.recovery)) {
            if (event.recovery.bid_order_id != 0) record.recovery.bid_order_id = event.recovery.bid_order_id;
            if (event.recovery.ask_order_id != 0) record.recovery.ask_order_id = event.recovery.ask_order_id;
            if (event.recovery.quote_local_id[0]) {
                std::strncpy(record.recovery.quote_local_id,
                             event.recovery.quote_local_id,
                             sizeof(record.recovery.quote_local_id) - 1);
            }
            if (event.recovery.quote_sys_id[0]) {
                std::strncpy(record.recovery.quote_sys_id,
                             event.recovery.quote_sys_id,
                             sizeof(record.recovery.quote_sys_id) - 1);
            }
            if (event.recovery.bid_local_id[0]) {
                std::strncpy(record.recovery.bid_local_id,
                             event.recovery.bid_local_id,
                             sizeof(record.recovery.bid_local_id) - 1);
            }
            if (event.recovery.ask_local_id[0]) {
                std::strncpy(record.recovery.ask_local_id,
                             event.recovery.ask_local_id,
                             sizeof(record.recovery.ask_local_id) - 1);
            }
            if (event.recovery.bid_order_sys_id[0]) {
                std::strncpy(record.recovery.bid_order_sys_id,
                             event.recovery.bid_order_sys_id,
                             sizeof(record.recovery.bid_order_sys_id) - 1);
            }
            if (event.recovery.ask_order_sys_id[0]) {
                std::strncpy(record.recovery.ask_order_sys_id,
                             event.recovery.ask_order_sys_id,
                             sizeof(record.recovery.ask_order_sys_id) - 1);
            }
            record.recovery.valid = record.recovery.valid || any_quote_handle(event.recovery);
        }
    }

    reset_stmt(stmt_upsert_live_quote_);
    bind_integer(stmt_upsert_live_quote_, 1, record.quote.client_quote_id);
    bind_text(stmt_upsert_live_quote_, 2, safe_code(instrument_by_id(record.quote.instrument_id)));
    bind_integer(stmt_upsert_live_quote_, 3, record.quote.product_index);
    bind_text(stmt_upsert_live_quote_, 4, record.quote.account_id.data);
    bind_text(stmt_upsert_live_quote_, 5, record.quote.exchange_id.data);
    bind_integer(stmt_upsert_live_quote_, 6, static_cast<int>(record.quote.bid_offset));
    bind_integer(stmt_upsert_live_quote_, 7, static_cast<int>(record.quote.ask_offset));
    sqlite3_bind_double(stmt_upsert_live_quote_, 8, record.quote.bid_price);
    sqlite3_bind_double(stmt_upsert_live_quote_, 9, record.quote.ask_price);
    bind_integer(stmt_upsert_live_quote_, 10, record.quote.bid_volume);
    bind_integer(stmt_upsert_live_quote_, 11, record.quote.ask_volume);
    bind_integer(stmt_upsert_live_quote_, 12, static_cast<int>(record.quote.bid_status));
    bind_integer(stmt_upsert_live_quote_, 13, static_cast<int>(record.quote.ask_status));
    bind_integer(stmt_upsert_live_quote_, 14, record.quote.send_ts);
    bind_integer(stmt_upsert_live_quote_, 15, record.quote.ack_ts);
    bind_integer(stmt_upsert_live_quote_, 16, record.quote.exchange_quote_id);
    bind_integer(stmt_upsert_live_quote_, 17, record.remaining_bid);
    bind_integer(stmt_upsert_live_quote_, 18, record.remaining_ask);
    bind_integer(stmt_upsert_live_quote_, 19, record.recovery.bid_order_id);
    bind_integer(stmt_upsert_live_quote_, 20, record.recovery.ask_order_id);
    bind_text(stmt_upsert_live_quote_, 21, record.recovery.quote_local_id);
    bind_text(stmt_upsert_live_quote_, 22, record.recovery.quote_sys_id);
    bind_text(stmt_upsert_live_quote_, 23, record.recovery.bid_local_id);
    bind_text(stmt_upsert_live_quote_, 24, record.recovery.ask_local_id);
    bind_text(stmt_upsert_live_quote_, 25, record.recovery.bid_order_sys_id);
    bind_text(stmt_upsert_live_quote_, 26, record.recovery.ask_order_sys_id);
    return step_done(db_, stmt_upsert_live_quote_, "upsert live_quote");
}

bool DataRepository::write_trade_locked(const Trade& trade) {
    const Instrument* instrument = instrument_by_id(trade.instrument_id);
    if (instrument == nullptr) return true;

    reset_stmt(stmt_insert_trade_);
    bind_integer(stmt_insert_trade_, 1, trade.client_order_id);
    bind_integer(stmt_insert_trade_, 2, trade.trade_id);
    bind_text(stmt_insert_trade_, 3, instrument->code.data);
    bind_integer(stmt_insert_trade_, 4, trade.product_index);
    bind_text(stmt_insert_trade_, 5, trade.account_id.data);
    bind_text(stmt_insert_trade_, 6, trade.exchange_id.data);
    bind_integer(stmt_insert_trade_, 7, static_cast<int>(trade.side));
    bind_integer(stmt_insert_trade_, 8, static_cast<int>(trade.offset));
    sqlite3_bind_double(stmt_insert_trade_, 9, trade.fill_price);
    bind_integer(stmt_insert_trade_, 10, trade.fill_volume);
    bind_integer(stmt_insert_trade_, 11, trade.fill_ts);
    if (!step_done(db_, stmt_insert_trade_, "insert trade")) return false;

    auto order_it = live_orders_.find(trade.client_order_id);
    if (order_it != live_orders_.end()) {
        LiveOrderRecord& order = order_it->second;
        order.order.filled_volume += trade.fill_volume;
        const double total_notional =
            order.order.avg_fill_price
                * static_cast<double>(std::max<Volume>(0, order.order.filled_volume - trade.fill_volume))
            + trade.fill_price * static_cast<double>(trade.fill_volume);
        if (order.order.filled_volume > 0) {
            order.order.avg_fill_price = total_notional / order.order.filled_volume;
        }
        order.order.status = (order.order.filled_volume >= order.order.volume)
            ? OrderStatus::Filled
            : OrderStatus::PartialFilled;
        order.order.ack_ts = trade.fill_ts;
        if (order.order.filled_volume >= order.order.volume) {
            mark_live_order_deleted_locked(trade.client_order_id);
        } else {
            OrderPersistenceEvent event{};
            event.type = OrderPersistenceEventType::Ack;
            event.order = order.order;
            event.recovery = order.recovery;
            return write_order_event_locked(event);
        }
        return true;
    }

    QuoteId quote_id = trade.client_order_id;
    if (is_ctp_ask_leg(trade.client_order_id)) {
        quote_id = trade.client_order_id & ~kCtpSyntheticAskLegBit;
    }
    auto quote_it = live_quotes_.find(quote_id);
    if (quote_it == live_quotes_.end()) return true;

    LiveQuoteRecord& quote = quote_it->second;
    if (trade.side == Side::Buy) {
        quote.remaining_bid = std::max<Volume>(0, quote.remaining_bid - trade.fill_volume);
    } else {
        quote.remaining_ask = std::max<Volume>(0, quote.remaining_ask - trade.fill_volume);
    }
    if (quote.remaining_bid <= 0 && quote.remaining_ask <= 0) {
        mark_live_quote_deleted_locked(quote_id);
        return true;
    }
    QuotePersistenceEvent event{};
    event.type = QuotePersistenceEventType::Ack;
    event.quote = quote.quote;
    event.quote.bid_volume = quote.remaining_bid;
    event.quote.ask_volume = quote.remaining_ask;
    event.recovery = quote.recovery;
    return write_quote_event_locked(event);
}

bool DataRepository::write_mm_params_locked(const MMParamsPersistenceEvent& event) {
    reset_stmt(stmt_upsert_mm_params_);
    bind_integer(stmt_upsert_mm_params_, 1, event.product_index);
    sqlite3_bind_double(stmt_upsert_mm_params_, 2, event.params.bid_spread);
    sqlite3_bind_double(stmt_upsert_mm_params_, 3, event.params.ask_spread);
    bind_integer(stmt_upsert_mm_params_, 4, event.params.quote_volume);
    sqlite3_bind_double(stmt_upsert_mm_params_, 5, event.params.product_delta_threshold);
    sqlite3_bind_double(stmt_upsert_mm_params_, 6, event.params.product_vega_threshold);
    sqlite3_bind_double(stmt_upsert_mm_params_, 7, event.params.min_quote_interval_ms);
    bind_integer(stmt_upsert_mm_params_, 8, event.params.max_position);
    bind_integer(stmt_upsert_mm_params_, 9, event.params.warning_position);
    sqlite3_bind_double(stmt_upsert_mm_params_, 10, event.params.base_half_spread_ticks);
    sqlite3_bind_double(stmt_upsert_mm_params_, 11, event.params.min_half_spread_ticks);
    sqlite3_bind_double(stmt_upsert_mm_params_, 12, event.params.max_half_spread_ticks);
    sqlite3_bind_double(stmt_upsert_mm_params_, 13, event.params.inventory_skew_per_lot_ticks);
    sqlite3_bind_double(stmt_upsert_mm_params_, 14, event.params.follow_weight);
    sqlite3_bind_double(stmt_upsert_mm_params_, 15, event.params.requote_price_epsilon_ticks);
    sqlite3_bind_double(stmt_upsert_mm_params_, 16, event.params.market_width_widen_threshold_ticks);
    sqlite3_bind_double(stmt_upsert_mm_params_, 17, event.params.underlying_move_widen_threshold_ticks);
    bind_integer(stmt_upsert_mm_params_, 18, event.params.use_one_sided_at_limits ? 1 : 0);
    bind_integer(stmt_upsert_mm_params_, 19, event.params.enabled ? 1 : 0);
    bind_integer(stmt_upsert_mm_params_, 20, event.update_ts);
    return step_done(db_, stmt_upsert_mm_params_, "upsert strategy_params");
}

bool DataRepository::write_arb_params_locked(const ArbParamsPersistenceEvent& event) {
    reset_stmt(stmt_upsert_arb_params_);
    bind_integer(stmt_upsert_arb_params_, 1, event.product_index);
    bind_integer(stmt_upsert_arb_params_, 2, static_cast<int>(event.strategy_type));
    sqlite3_bind_double(stmt_upsert_arb_params_, 3, event.params.min_edge_ticks);
    sqlite3_bind_double(stmt_upsert_arb_params_, 4, event.params.cooldown_ms);
    sqlite3_bind_double(stmt_upsert_arb_params_, 5, event.params.scan_interval_ms);
    sqlite3_bind_double(stmt_upsert_arb_params_, 6, event.params.cleanup_timeout_ms);
    bind_integer(stmt_upsert_arb_params_, 7, event.params.max_order_volume);
    bind_integer(stmt_upsert_arb_params_, 8, event.params.max_live_orders);
    bind_integer(stmt_upsert_arb_params_, 9, event.params.cleanup_on_partial ? 1 : 0);
    bind_integer(stmt_upsert_arb_params_, 10, event.params.enabled ? 1 : 0);
    bind_integer(stmt_upsert_arb_params_, 11, event.update_ts);
    return step_done(db_, stmt_upsert_arb_params_, "upsert arb_strategy_params");
}

bool DataRepository::write_risk_params_locked(const RiskParamsPersistenceEvent& event) {
    reset_stmt(stmt_replace_risk_params_);
    bind_integer(stmt_replace_risk_params_, 1, event.params.max_net_position);
    sqlite3_bind_double(stmt_replace_risk_params_, 2, event.params.max_delta);
    sqlite3_bind_double(stmt_replace_risk_params_, 3, event.params.max_gamma);
    sqlite3_bind_double(stmt_replace_risk_params_, 4, event.params.max_vega);
    bind_integer(stmt_replace_risk_params_, 5, event.update_ts);
    return step_done(db_, stmt_replace_risk_params_, "replace risk_params");
}

bool DataRepository::write_positions_snapshot_locked(const PositionSnapshotEvent& event) {
    reset_stmt(stmt_clear_positions_);
    if (!step_done(db_, stmt_clear_positions_, "clear positions_snapshot")) return false;
    for (uint16_t i = 0; i < event.n_instruments; ++i) {
        const Position& pos = event.positions[i];
        if (pos.instrument_id >= MAX_INSTRUMENTS) continue;
        if (pos.net_position == 0
            && pos.long_position == 0
            && pos.short_position == 0
            && pos.realized_pnl == 0.0) {
            continue;
        }
        const Instrument* instrument = instrument_by_id(pos.instrument_id);
        if (instrument == nullptr) continue;
        reset_stmt(stmt_insert_position_);
        bind_text(stmt_insert_position_, 1, instrument->code.data);
        bind_integer(stmt_insert_position_, 2, pos.product_index);
        bind_integer(stmt_insert_position_, 3, pos.net_position);
        bind_integer(stmt_insert_position_, 4, pos.long_position);
        bind_integer(stmt_insert_position_, 5, pos.short_position);
        bind_integer(stmt_insert_position_, 6, pos.long_today);
        bind_integer(stmt_insert_position_, 7, pos.short_today);
        sqlite3_bind_double(stmt_insert_position_, 8, pos.avg_long_price);
        sqlite3_bind_double(stmt_insert_position_, 9, pos.avg_short_price);
        sqlite3_bind_double(stmt_insert_position_, 10, pos.realized_pnl);
        bind_integer(stmt_insert_position_, 11, event.snapshot_ts);
        if (!step_done(db_, stmt_insert_position_, "insert positions_snapshot")) return false;
    }
    return true;
}

bool DataRepository::write_instruments_locked(int32_t trading_day,
                                              const char* table_name,
                                              const Instrument* instruments,
                                              uint16_t n_instruments) {
    if (instruments == nullptr) return true;
    sqlite3_stmt* stmt = std::strcmp(table_name, "eod_instruments") == 0
        ? stmt_replace_eod_instrument_
        : stmt_replace_instrument_;
    for (uint16_t i = 0; i < n_instruments; ++i) {
        const Instrument& instrument = instruments[i];
        if (instrument.instrument_id == INVALID_INSTRUMENT_ID) continue;
        reset_stmt(stmt);
        bind_text(stmt, 1, instrument.code.data);
        bind_integer(stmt, 2, trading_day);
        bind_text(stmt, 3, instrument.underlying_code.data);
        bind_text(stmt, 4, instrument.exchange_id.data);
        bind_integer(stmt, 5, static_cast<int>(instrument.exchange));
        bind_integer(stmt, 6, static_cast<int>(instrument.kind));
        bind_integer(stmt, 7, static_cast<int>(instrument.option_type));
        sqlite3_bind_double(stmt, 8, instrument.strike);
        sqlite3_bind_double(stmt, 9, instrument.multiplier);
        sqlite3_bind_double(stmt, 10, instrument.tick_size);
        bind_integer(stmt, 11, instrument.expiry_date);
        bind_integer(stmt, 12, instrument.expiry_epoch_ns);
        bind_integer(stmt, 13, instrument.product_index);
        const char* underlying_code = "";
        if (instrument.underlying_id < n_instruments_) {
            const Instrument* underlying = instrument_by_id(instrument.underlying_id);
            underlying_code = safe_code(underlying);
        }
        bind_text(stmt, 14, underlying_code);
        if (!step_done(db_, stmt, "replace instrument")) return false;
    }
    return true;
}

bool DataRepository::write_eod_snapshot_locked(const EndOfDaySnapshot& snapshot) {
    if (!exec_sql(db_, "BEGIN IMMEDIATE TRANSACTION;")) return false;

    reset_stmt(stmt_delete_eod_greeks_for_day_);
    bind_integer(stmt_delete_eod_greeks_for_day_, 1, snapshot.trading_day);
    if (!step_done(db_, stmt_delete_eod_greeks_for_day_, "clear eod_greeks")) {
        (void)exec_sql(db_, "ROLLBACK;");
        return false;
    }
    reset_stmt(stmt_delete_eod_model_params_for_day_);
    bind_integer(stmt_delete_eod_model_params_for_day_, 1, snapshot.trading_day);
    if (!step_done(db_, stmt_delete_eod_model_params_for_day_, "clear eod_vol_model_params")) {
        (void)exec_sql(db_, "ROLLBACK;");
        return false;
    }
    reset_stmt(stmt_delete_eod_instruments_for_day_);
    bind_integer(stmt_delete_eod_instruments_for_day_, 1, snapshot.trading_day);
    if (!step_done(db_, stmt_delete_eod_instruments_for_day_, "clear eod_instruments")) {
        (void)exec_sql(db_, "ROLLBACK;");
        return false;
    }

    if (!write_instruments_locked(snapshot.trading_day, "eod_instruments",
                                  snapshot.instruments, snapshot.n_instruments)) {
        (void)exec_sql(db_, "ROLLBACK;");
        return false;
    }

    for (uint16_t i = 0; i < snapshot.n_instruments; ++i) {
        const Greeks& greek = snapshot.greeks[i];
        const Instrument& instrument = snapshot.instruments[i];
        if (instrument.instrument_id == INVALID_INSTRUMENT_ID) continue;
        if (greek.instrument_id == INVALID_INSTRUMENT_ID) continue;
        reset_stmt(stmt_replace_eod_greeks_);
        bind_integer(stmt_replace_eod_greeks_, 1, snapshot.trading_day);
        bind_text(stmt_replace_eod_greeks_, 2, instrument.code.data);
        sqlite3_bind_double(stmt_replace_eod_greeks_, 3, greek.theo_price);
        sqlite3_bind_double(stmt_replace_eod_greeks_, 4, greek.delta);
        sqlite3_bind_double(stmt_replace_eod_greeks_, 5, greek.gamma);
        sqlite3_bind_double(stmt_replace_eod_greeks_, 6, greek.vega);
        sqlite3_bind_double(stmt_replace_eod_greeks_, 7, greek.theta);
        sqlite3_bind_double(stmt_replace_eod_greeks_, 8, greek.rho);
        sqlite3_bind_double(stmt_replace_eod_greeks_, 9, greek.iv);
        sqlite3_bind_double(stmt_replace_eod_greeks_, 10, greek.T);
        bind_integer(stmt_replace_eod_greeks_, 11, greek.calc_ts_ns);
        if (!step_done(db_, stmt_replace_eod_greeks_, "replace eod_greeks")) {
            (void)exec_sql(db_, "ROLLBACK;");
            return false;
        }
    }

    for (uint16_t i = 0; i < snapshot.n_model_slices; ++i) {
        const PersistedVolModelSlice& slice = snapshot.model_slices[i];
        reset_stmt(stmt_replace_eod_model_params_);
        bind_integer(stmt_replace_eod_model_params_, 1, slice.trading_day);
        bind_integer(stmt_replace_eod_model_params_, 2, slice.product_index);
        bind_integer(stmt_replace_eod_model_params_, 3, slice.model_type);
        sqlite3_bind_double(stmt_replace_eod_model_params_, 4, slice.expiry_T);
        sqlite3_bind_double(stmt_replace_eod_model_params_, 5, slice.a);
        sqlite3_bind_double(stmt_replace_eod_model_params_, 6, slice.b);
        sqlite3_bind_double(stmt_replace_eod_model_params_, 7, slice.rho);
        sqlite3_bind_double(stmt_replace_eod_model_params_, 8, slice.m);
        sqlite3_bind_double(stmt_replace_eod_model_params_, 9, slice.sigma);
        sqlite3_bind_double(stmt_replace_eod_model_params_, 10, slice.atm_vol);
        sqlite3_bind_double(stmt_replace_eod_model_params_, 11, slice.slope_call);
        sqlite3_bind_double(stmt_replace_eod_model_params_, 12, slice.slope_put);
        sqlite3_bind_double(stmt_replace_eod_model_params_, 13, slice.curve_call);
        sqlite3_bind_double(stmt_replace_eod_model_params_, 14, slice.curve_put);
        sqlite3_bind_double(stmt_replace_eod_model_params_, 15, slice.ref_price);
        sqlite3_bind_double(stmt_replace_eod_model_params_, 16, slice.atm_forward);
        sqlite3_bind_double(stmt_replace_eod_model_params_, 17, slice.ssr);
        sqlite3_bind_double(stmt_replace_eod_model_params_, 18, slice.vol_ref);
        sqlite3_bind_double(stmt_replace_eod_model_params_, 19, slice.slope_ref);
        sqlite3_bind_double(stmt_replace_eod_model_params_, 20, slice.vcr);
        sqlite3_bind_double(stmt_replace_eod_model_params_, 21, slice.scr);
        sqlite3_bind_double(stmt_replace_eod_model_params_, 22, slice.put_curv);
        sqlite3_bind_double(stmt_replace_eod_model_params_, 23, slice.call_curv);
        sqlite3_bind_double(stmt_replace_eod_model_params_, 24, slice.down_cutoff);
        sqlite3_bind_double(stmt_replace_eod_model_params_, 25, slice.up_cutoff);
        sqlite3_bind_double(stmt_replace_eod_model_params_, 26, slice.down_smoothing);
        sqlite3_bind_double(stmt_replace_eod_model_params_, 27, slice.up_smoothing);
        if (!step_done(db_, stmt_replace_eod_model_params_, "replace eod_vol_model_params")) {
            (void)exec_sql(db_, "ROLLBACK;");
            return false;
        }
    }

    return exec_sql(db_, "COMMIT;");
}

void DataRepository::prime_caches_locked(const RecoveryState& state) {
    live_orders_.clear();
    live_quotes_.clear();
    for (const auto& order : state.live_orders) {
        LiveOrderRecord record{};
        record.order = order.order;
        record.recovery = order.recovery;
        live_orders_.emplace(order.order.client_order_id, record);
    }
    for (const auto& quote : state.live_quotes) {
        LiveQuoteRecord record{};
        record.quote = quote.quote;
        record.recovery = quote.recovery;
        record.remaining_bid = quote.quote.bid_volume;
        record.remaining_ask = quote.quote.ask_volume;
        live_quotes_.emplace(quote.quote.client_quote_id, record);
    }
}

void DataRepository::mark_live_order_deleted_locked(OrderId id) {
    live_orders_.erase(id);
    reset_stmt(stmt_delete_live_order_);
    bind_integer(stmt_delete_live_order_, 1, id);
    (void)step_done(db_, stmt_delete_live_order_, "delete live_order");
}

void DataRepository::mark_live_quote_deleted_locked(QuoteId id) {
    live_quotes_.erase(id);
    reset_stmt(stmt_delete_live_quote_);
    bind_integer(stmt_delete_live_quote_, 1, id);
    (void)step_done(db_, stmt_delete_live_quote_, "delete live_quote");
}

const Instrument* DataRepository::instrument_by_id(uint16_t instrument_id) const noexcept {
    if (instruments_ == nullptr || instrument_id >= n_instruments_) return nullptr;
    return &instruments_[instrument_id];
}

uint16_t DataRepository::find_instrument_id_by_code(const char* code) const noexcept {
    if (code == nullptr || code[0] == '\0' || instruments_ == nullptr) {
        return INVALID_INSTRUMENT_ID;
    }
    for (uint16_t i = 0; i < n_instruments_; ++i) {
        if (std::strncmp(instruments_[i].code.data, code, sizeof(instruments_[i].code.data)) == 0) {
            return i;
        }
    }
    return INVALID_INSTRUMENT_ID;
}

int32_t DataRepository::current_trading_day() const noexcept {
    const std::time_t now = std::time(nullptr);
    std::tm local_tm{};
#if defined(_WIN32)
    localtime_s(&local_tm, &now);
#else
    localtime_r(&now, &local_tm);
#endif
    return (local_tm.tm_year + 1900) * 10000
         + (local_tm.tm_mon + 1) * 100
         + local_tm.tm_mday;
}

bool DataRepository::is_ctp_ask_leg(OrderId id) noexcept {
    return (id & kCtpSyntheticAskLegBit) != 0;
}

EndOfDaySnapshot build_end_of_day_snapshot(
        int32_t trading_day,
        const Instrument* instruments,
        uint16_t n_instruments,
        const Greeks* greeks_snapshot,
        VolMethod model,
        const std::array<VolSurfaceManager<SVIVolSurface>, MAX_PRODUCTS>& svi_surfaces,
        const std::array<VolSurfaceManager<WingVolSurface>, MAX_PRODUCTS>& wing_surfaces,
        const std::array<VolSurfaceManager<OrcWingVolSurface>, MAX_PRODUCTS>& orc_wing_surfaces,
        int product_count) noexcept {
    EndOfDaySnapshot snapshot{};
    snapshot.trading_day = trading_day;
    snapshot.n_instruments = n_instruments;
    for (uint16_t i = 0; i < n_instruments; ++i) {
        snapshot.instruments[i] = instruments[i];
        snapshot.greeks[i] = greeks_snapshot[i];
    }

    uint16_t slice_count = 0;
    for (int product = 0; product < product_count && product < MAX_PRODUCTS; ++product) {
        if (model == VolMethod::Wing) {
            const auto* surface = static_cast<const WingVolSurface*>(wing_surfaces[product].get());
            for (int i = 0; i < surface->n_slices && slice_count < MAX_PRODUCTS * MAX_EXPIRIES; ++i) {
                PersistedVolModelSlice& slice = snapshot.model_slices[slice_count++];
                slice.model_type = static_cast<uint8_t>(VolMethod::Wing);
                slice.product_index = static_cast<uint8_t>(product);
                slice.trading_day = trading_day;
                slice.expiry_T = surface->slices[i].expiry_T;
                slice.atm_vol = surface->slices[i].ATM_vol;
                slice.slope_call = surface->slices[i].slope_call;
                slice.slope_put = surface->slices[i].slope_put;
                slice.curve_call = surface->slices[i].curve_call;
                slice.curve_put = surface->slices[i].curve_put;
            }
        } else if (model == VolMethod::OrcWing) {
            const auto* surface = static_cast<const OrcWingVolSurface*>(orc_wing_surfaces[product].get());
            for (int i = 0; i < surface->n_slices && slice_count < MAX_PRODUCTS * MAX_EXPIRIES; ++i) {
                PersistedVolModelSlice& slice = snapshot.model_slices[slice_count++];
                slice.model_type = static_cast<uint8_t>(VolMethod::OrcWing);
                slice.product_index = static_cast<uint8_t>(product);
                slice.trading_day = trading_day;
                slice.expiry_T = surface->slices[i].expiry_T;
                slice.ref_price = surface->slices[i].ref_price;
                slice.atm_forward = surface->slices[i].atm_forward;
                slice.ssr = surface->slices[i].ssr;
                slice.vol_ref = surface->slices[i].vol_ref;
                slice.slope_ref = surface->slices[i].slope_ref;
                slice.vcr = surface->slices[i].vcr;
                slice.scr = surface->slices[i].scr;
                slice.put_curv = surface->slices[i].put_curv;
                slice.call_curv = surface->slices[i].call_curv;
                slice.down_cutoff = surface->slices[i].down_cutoff;
                slice.up_cutoff = surface->slices[i].up_cutoff;
                slice.down_smoothing = surface->slices[i].down_smoothing;
                slice.up_smoothing = surface->slices[i].up_smoothing;
            }
        } else {
            const auto* surface = static_cast<const SVIVolSurface*>(svi_surfaces[product].get());
            for (int i = 0; i < surface->n_slices && slice_count < MAX_PRODUCTS * MAX_EXPIRIES; ++i) {
                PersistedVolModelSlice& slice = snapshot.model_slices[slice_count++];
                slice.model_type = static_cast<uint8_t>(VolMethod::SVI);
                slice.product_index = static_cast<uint8_t>(product);
                slice.trading_day = trading_day;
                slice.expiry_T = surface->slices[i].expiry_T;
                slice.a = surface->slices[i].a;
                slice.b = surface->slices[i].b;
                slice.rho = surface->slices[i].rho;
                slice.m = surface->slices[i].m;
                slice.sigma = surface->slices[i].sigma;
            }
        }
    }
    snapshot.n_model_slices = slice_count;
    return snapshot;
}

} // namespace omm
