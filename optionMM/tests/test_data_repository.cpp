#include <gtest/gtest.h>

#include <sqlite3.h>

#include "common/auth.h"
#include "persistence/data_repository.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

using namespace omm;

namespace {

std::filesystem::path make_test_db_path(const char* suffix) {
    const auto base = std::filesystem::temp_directory_path()
        / ("optionmm_repo_" + std::string(suffix) + ".sqlite");
    return base;
}

void remove_db_artifacts(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
    std::filesystem::remove(path.string() + "-wal", ec);
    std::filesystem::remove(path.string() + "-shm", ec);
}

void copy_cstr(char* dst, std::size_t n, const char* src) {
    if (dst == nullptr || n == 0) return;
    std::strncpy(dst, src ? src : "", n - 1);
    dst[n - 1] = '\0';
}

uint16_t init_test_instruments(Instrument* instruments) {
    instruments[0] = Instrument{};
    instruments[0].code = InstrumentCode("rb2505");
    instruments[0].underlying_code = InstrumentCode("rb2505");
    instruments[0].exchange_id = ExchangeId("SHFE");
    instruments[0].exchange = Exchange::SHFE;
    instruments[0].kind = InstrumentKind::Future;
    instruments[0].option_type = OptionType::Call;
    instruments[0].strike = 0.0;
    instruments[0].multiplier = 10.0;
    instruments[0].tick_size = 1.0;
    instruments[0].expiry_date = 20260520;
    instruments[0].expiry_epoch_ns = 1'000'000;
    instruments[0].instrument_id = 0;
    instruments[0].underlying_id = 0;
    instruments[0].product_index = 0;

    instruments[1] = Instrument{};
    instruments[1].code = InstrumentCode("rb2505-C-3500");
    instruments[1].underlying_code = InstrumentCode("rb2505");
    instruments[1].exchange_id = ExchangeId("SHFE");
    instruments[1].exchange = Exchange::SHFE;
    instruments[1].kind = InstrumentKind::Option;
    instruments[1].option_type = OptionType::Call;
    instruments[1].strike = 3500.0;
    instruments[1].multiplier = 10.0;
    instruments[1].tick_size = 0.5;
    instruments[1].expiry_date = 20260520;
    instruments[1].expiry_epoch_ns = 2'000'000;
    instruments[1].instrument_id = 1;
    instruments[1].underlying_id = 0;
    instruments[1].product_index = 0;

    return 2;
}

int query_single_int(sqlite3* db, const char* sql) {
    sqlite3_stmt* stmt = nullptr;
    EXPECT_EQ(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr), SQLITE_OK);
    int value = -1;
    if (stmt != nullptr && sqlite3_step(stmt) == SQLITE_ROW) {
        value = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return value;
}

double query_single_double(sqlite3* db, const char* sql) {
    sqlite3_stmt* stmt = nullptr;
    EXPECT_EQ(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr), SQLITE_OK);
    double value = -1.0;
    if (stmt != nullptr && sqlite3_step(stmt) == SQLITE_ROW) {
        value = sqlite3_column_double(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return value;
}

} // namespace

TEST(DataRepositoryTest, PersistsAndRecoversLiveState) {
    const auto db_path = make_test_db_path("recovery");
    remove_db_artifacts(db_path);

    PersistenceConfig cfg{};
    cfg.enabled = true;
    cfg.batch_max_rows = 64;
    cfg.flush_interval_ms = 1;
    cfg.snapshot_interval_ms = 10;
    copy_cstr(cfg.data_path, sizeof(cfg.data_path), db_path.string().c_str());

    Instrument instruments[2]{};
    const uint16_t n_instruments = init_test_instruments(instruments);

    {
        DataRepository repo(cfg, GatewayType::Sim, VolMethod::Wing);
        repo.set_instruments(instruments, n_instruments);
        ASSERT_TRUE(repo.open());
        ASSERT_TRUE(repo.persist_instruments());
        repo.start();

        MMParamsPersistenceEvent mm{};
        mm.product_index = 0;
        mm.params.bid_spread = 1.25;
        mm.params.ask_spread = 1.5;
        mm.params.quote_volume = 12;
        mm.params.product_delta_threshold = 25.0;
        mm.params.product_vega_threshold = 80.0;
        mm.params.min_quote_interval_ms = 15.0;
        mm.params.max_position = 30;
        mm.params.warning_position = 20;
        mm.params.base_half_spread_ticks = 1.2;
        mm.params.min_half_spread_ticks = 0.8;
        mm.params.max_half_spread_ticks = 3.5;
        mm.params.inventory_skew_per_lot_ticks = 0.3;
        mm.params.follow_weight = 0.6;
        mm.params.requote_price_epsilon_ticks = 1.1;
        mm.params.market_width_widen_threshold_ticks = 4.0;
        mm.params.underlying_move_widen_threshold_ticks = 2.0;
        mm.params.use_one_sided_at_limits = true;
        mm.params.enabled = false;
        mm.update_ts = 101;
        EXPECT_TRUE(repo.enqueue_mm_params(mm));

        ArbParamsPersistenceEvent arb{};
        arb.product_index = 0;
        arb.strategy_type = ArbitrageStrategyType::PCP;
        arb.params.min_edge_ticks = 2.5;
        arb.params.cooldown_ms = 30.0;
        arb.params.scan_interval_ms = 4.0;
        arb.params.cleanup_timeout_ms = 40.0;
        arb.params.max_order_volume = 2;
        arb.params.max_live_orders = 6;
        arb.params.cleanup_on_partial = true;
        arb.params.enabled = true;
        arb.update_ts = 102;
        EXPECT_TRUE(repo.enqueue_arb_params(arb));

        RiskParamsPersistenceEvent risk{};
        risk.params.max_net_position = 200;
        risk.params.max_delta = 500.0;
        risk.params.max_gamma = 250.0;
        risk.params.max_vega = 900.0;
        risk.update_ts = 103;
        EXPECT_TRUE(repo.enqueue_risk_params(risk));

        PositionSnapshotEvent positions{};
        positions.snapshot_ts = 104;
        positions.n_instruments = n_instruments;
        positions.positions[1].instrument_id = 1;
        positions.positions[1].product_index = 0;
        positions.positions[1].net_position = 4;
        positions.positions[1].long_position = 4;
        positions.positions[1].long_today = 2;
        positions.positions[1].avg_long_price = 18.5;
        positions.positions[1].realized_pnl = 12.75;
        EXPECT_TRUE(repo.enqueue_positions_snapshot(positions));

        QuotePersistenceEvent quote_submit{};
        quote_submit.type = QuotePersistenceEventType::Submit;
        quote_submit.quote.client_quote_id = 501;
        quote_submit.quote.instrument_id = 1;
        quote_submit.quote.product_index = 0;
        quote_submit.quote.account_id = AccountId("acct-1");
        quote_submit.quote.exchange_id = ExchangeId("SHFE");
        quote_submit.quote.bid_offset = OffsetFlag::Open;
        quote_submit.quote.ask_offset = OffsetFlag::Open;
        quote_submit.quote.bid_price = 12.0;
        quote_submit.quote.ask_price = 12.5;
        quote_submit.quote.bid_volume = 5;
        quote_submit.quote.ask_volume = 4;
        quote_submit.quote.book_id = 201;
        quote_submit.quote.send_ts = 105;
        EXPECT_TRUE(repo.enqueue_quote_event(quote_submit));

        Trade quote_fill{};
        quote_fill.client_order_id = 501;
        quote_fill.trade_id = 7001;
        quote_fill.instrument_id = 1;
        quote_fill.product_index = 0;
        quote_fill.account_id = AccountId("acct-1");
        quote_fill.exchange_id = ExchangeId("SHFE");
        quote_fill.side = Side::Buy;
        quote_fill.offset = OffsetFlag::Open;
        quote_fill.fill_price = 12.0;
        quote_fill.fill_volume = 2;
        quote_fill.book_id = 201;
        quote_fill.fill_ts = 106;
        EXPECT_TRUE(repo.enqueue_trade(quote_fill));

        OrderPersistenceEvent order_submit{};
        order_submit.type = OrderPersistenceEventType::Submit;
        order_submit.order.client_order_id = 601;
        order_submit.order.instrument_id = 1;
        order_submit.order.product_index = 0;
        order_submit.order.account_id = AccountId("acct-1");
        order_submit.order.exchange_id = ExchangeId("SHFE");
        order_submit.order.side = Side::Sell;
        order_submit.order.offset = OffsetFlag::Open;
        order_submit.order.order_type = OrderType::Limit;
        order_submit.order.status = OrderStatus::New;
        order_submit.order.price = 12.8;
        order_submit.order.volume = 7;
        order_submit.order.book_id = 202;
        order_submit.order.send_ts = 107;
        EXPECT_TRUE(repo.enqueue_order_event(order_submit));

        Trade order_fill{};
        order_fill.client_order_id = 601;
        order_fill.trade_id = 7002;
        order_fill.instrument_id = 1;
        order_fill.product_index = 0;
        order_fill.account_id = AccountId("acct-1");
        order_fill.exchange_id = ExchangeId("SHFE");
        order_fill.side = Side::Sell;
        order_fill.offset = OffsetFlag::Open;
        order_fill.fill_price = 12.8;
        order_fill.fill_volume = 3;
        order_fill.book_id = 202;
        order_fill.fill_ts = 108;
        EXPECT_TRUE(repo.enqueue_trade(order_fill));

        repo.stop();
    }

    {
        DataRepository repo(cfg, GatewayType::Sim, VolMethod::Wing);
        repo.set_instruments(instruments, n_instruments);
        ASSERT_TRUE(repo.open());

        RecoveryState state{};
        ASSERT_TRUE(repo.load_recovery_state(&state));

        ASSERT_TRUE(state.mm_params[0].valid);
        EXPECT_DOUBLE_EQ(state.mm_params[0].params.bid_spread, 1.25);
        EXPECT_EQ(state.mm_params[0].params.quote_volume, 12);
        EXPECT_FALSE(state.mm_params[0].params.enabled);

        ASSERT_EQ(state.arb_params.size(), 1u);
        EXPECT_EQ(state.arb_params[0].product_index, 0);
        EXPECT_EQ(state.arb_params[0].strategy_type, ArbitrageStrategyType::PCP);
        EXPECT_DOUBLE_EQ(state.arb_params[0].params.min_edge_ticks, 2.5);
        EXPECT_TRUE(state.arb_params[0].params.enabled);

        ASSERT_TRUE(state.has_risk_params);
        EXPECT_EQ(state.risk_params.max_net_position, 200);
        EXPECT_DOUBLE_EQ(state.risk_params.max_vega, 900.0);

        ASSERT_EQ(state.positions.size(), 1u);
        EXPECT_EQ(state.positions[0].instrument_id, 1);
        EXPECT_EQ(state.positions[0].net_position, 4);
        EXPECT_DOUBLE_EQ(state.positions[0].realized_pnl, 12.75);

        ASSERT_EQ(state.live_quotes.size(), 1u);
        EXPECT_EQ(state.live_quotes[0].client_quote_id, 501u);
        EXPECT_EQ(state.live_quotes[0].book_id, 201u);
        EXPECT_EQ(state.live_quotes[0].bid_volume, 3);
        EXPECT_EQ(state.live_quotes[0].ask_volume, 4);

        ASSERT_EQ(state.live_orders.size(), 1u);
        EXPECT_EQ(state.live_orders[0].client_order_id, 601u);
        EXPECT_EQ(state.live_orders[0].book_id, 202u);
        EXPECT_EQ(state.live_orders[0].volume, 7);
        EXPECT_EQ(state.live_orders[0].filled_volume, 3);
        EXPECT_EQ(state.live_orders[0].status, OrderStatus::PartialFilled);
        EXPECT_DOUBLE_EQ(state.live_orders[0].avg_fill_price, 12.8);

        std::vector<Trade> trades;
        ASSERT_TRUE(repo.load_trade_history(&trades));
        ASSERT_EQ(trades.size(), 2u);
        EXPECT_EQ(trades[0].book_id, 201u);
        EXPECT_EQ(trades[1].book_id, 202u);
    }

    remove_db_artifacts(db_path);
}

TEST(DataRepositoryTest, PersistsInstrumentsAndEndOfDaySnapshots) {
    const auto db_path = make_test_db_path("eod");
    remove_db_artifacts(db_path);

    PersistenceConfig cfg{};
    cfg.enabled = true;
    copy_cstr(cfg.data_path, sizeof(cfg.data_path), db_path.string().c_str());

    Instrument instruments[2]{};
    const uint16_t n_instruments = init_test_instruments(instruments);

    {
        DataRepository repo(cfg, GatewayType::Sim, VolMethod::Wing);
        repo.set_instruments(instruments, n_instruments);
        ASSERT_TRUE(repo.open());
        ASSERT_TRUE(repo.persist_instruments());

        EndOfDaySnapshot snapshot{};
        snapshot.trading_day = 20260421;
        snapshot.n_instruments = n_instruments;
        snapshot.n_model_slices = 1;
        snapshot.instruments[0] = instruments[0];
        snapshot.instruments[1] = instruments[1];
        snapshot.greeks[0].instrument_id = 0;
        snapshot.greeks[0].theo_price = 3550.0;
        snapshot.greeks[0].delta = 1.0;
        snapshot.greeks[0].gamma = 0.0;
        snapshot.greeks[0].vega = 0.0;
        snapshot.greeks[0].theta = -0.1;
        snapshot.greeks[0].rho = 0.2;
        snapshot.greeks[0].iv = 0.18;
        snapshot.greeks[0].T = 0.08;
        snapshot.greeks[0].calc_ts_ns = 201;
        snapshot.greeks[1].instrument_id = 1;
        snapshot.greeks[1].theo_price = 42.5;
        snapshot.greeks[1].delta = 0.55;
        snapshot.greeks[1].gamma = 0.03;
        snapshot.greeks[1].vega = 8.5;
        snapshot.greeks[1].theta = -0.6;
        snapshot.greeks[1].rho = 0.4;
        snapshot.greeks[1].iv = 0.22;
        snapshot.greeks[1].T = 0.08;
        snapshot.greeks[1].calc_ts_ns = 202;

        snapshot.model_slices[0].trading_day = snapshot.trading_day;
        snapshot.model_slices[0].model_type = static_cast<uint8_t>(VolMethod::Wing);
        snapshot.model_slices[0].product_index = 0;
        snapshot.model_slices[0].expiry_T = 0.08;
        snapshot.model_slices[0].atm_vol = 0.22;
        snapshot.model_slices[0].slope_call = -0.12;
        snapshot.model_slices[0].slope_put = 0.14;
        snapshot.model_slices[0].curve_call = 0.03;
        snapshot.model_slices[0].curve_put = 0.04;

        ASSERT_TRUE(repo.persist_end_of_day_snapshot(snapshot));
    }

    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(db_path.string().c_str(), &db), SQLITE_OK);

    EXPECT_EQ(query_single_int(db, "SELECT COUNT(*) FROM instruments"), 2);
    EXPECT_EQ(query_single_int(db, "SELECT COUNT(*) FROM eod_instruments WHERE trading_day = 20260421"), 2);
    EXPECT_EQ(query_single_int(db, "SELECT COUNT(*) FROM eod_greeks WHERE trading_day = 20260421"), 2);
    EXPECT_EQ(query_single_int(db, "SELECT COUNT(*) FROM eod_vol_model_params WHERE trading_day = 20260421"), 1);
    EXPECT_DOUBLE_EQ(
        query_single_double(db,
                            "SELECT atm_vol FROM eod_vol_model_params "
                            "WHERE trading_day = 20260421 AND product_index = 0"),
        0.22);

    sqlite3_close(db);
    remove_db_artifacts(db_path);
}

TEST(DataRepositoryTest, SeedsAndLoadsExchangeCalendars) {
    const auto db_path = make_test_db_path("calendar");
    remove_db_artifacts(db_path);

    PersistenceConfig cfg{};
    cfg.enabled = true;
    copy_cstr(cfg.data_path, sizeof(cfg.data_path), db_path.string().c_str());

    SystemConfig system_cfg{};
    system_cfg.exchange_calendar_count = 1;
    system_cfg.exchange_calendars[0].exchange_id = ExchangeId("SHFE");
    system_cfg.exchange_calendars[0].days[0] = {20260424, true};
    system_cfg.exchange_calendars[0].days[1] = {20260425, false};
    system_cfg.exchange_calendars[0].day_count = 2;
    system_cfg.exchange_trading_time_count = 1;
    system_cfg.exchange_trading_times[0].exchange_id = ExchangeId("SHFE");
    auto& night = system_cfg.exchange_trading_times[0].sessions[0];
    night.start_day_offset = -1;
    night.end_day_offset = 0;
    copy_cstr(night.start_time, sizeof(night.start_time), "21:00:00");
    copy_cstr(night.end_time, sizeof(night.end_time), "02:00:00");
    auto& day = system_cfg.exchange_trading_times[0].sessions[1];
    copy_cstr(day.start_time, sizeof(day.start_time), "09:00:00");
    copy_cstr(day.end_time, sizeof(day.end_time), "15:00:00");
    system_cfg.exchange_trading_times[0].session_count = 2;

    DataRepository repo(cfg, GatewayType::Sim, VolMethod::Wing);
    ASSERT_TRUE(repo.open());
    ASSERT_TRUE(repo.seed_exchange_calendar(system_cfg));

    std::vector<ExchangeTradingCalendar> calendars;
    ASSERT_TRUE(repo.load_exchange_calendars(&calendars));
    ASSERT_EQ(calendars.size(), 1u);
    EXPECT_EQ(calendars[0].exchange_id, "SHFE");
    ASSERT_EQ(calendars[0].days.size(), 2u);
    EXPECT_TRUE(calendars[0].days[0].is_trading_day);
    ASSERT_EQ(calendars[0].sessions.size(), 2u);
    EXPECT_EQ(calendars[0].sessions[0].start_day_offset, -1);

    remove_db_artifacts(db_path);
}

TEST(DataRepositoryTest, SeedsAndLoadsIdentityState) {
    const auto db_path = make_test_db_path("identity");
    remove_db_artifacts(db_path);

    PersistenceConfig persistence_cfg{};
    persistence_cfg.enabled = true;
    copy_cstr(persistence_cfg.data_path, sizeof(persistence_cfg.data_path), db_path.string().c_str());

    SystemConfig system_cfg{};
    system_cfg.persistence.enabled = true;
    system_cfg.book_count = 3;
    system_cfg.user_count = 1;
    system_cfg.product_count = 1;

    system_cfg.books[0].book_id = 101;
    copy_cstr(system_cfg.books[0].book_code, sizeof(system_cfg.books[0].book_code), "MM-RB");
    copy_cstr(system_cfg.books[0].display_name, sizeof(system_cfg.books[0].display_name), "MM RB");
    copy_cstr(system_cfg.books[0].description, sizeof(system_cfg.books[0].description), "Market making");

    system_cfg.books[1].book_id = 102;
    copy_cstr(system_cfg.books[1].book_code, sizeof(system_cfg.books[1].book_code), "ARB-RB");
    copy_cstr(system_cfg.books[1].display_name, sizeof(system_cfg.books[1].display_name), "ARB RB");
    copy_cstr(system_cfg.books[1].description, sizeof(system_cfg.books[1].description), "Arbitrage");

    system_cfg.books[2].book_id = 900;
    copy_cstr(system_cfg.books[2].book_code, sizeof(system_cfg.books[2].book_code), "MANUAL");
    copy_cstr(system_cfg.books[2].display_name, sizeof(system_cfg.books[2].display_name), "Manual");
    copy_cstr(system_cfg.books[2].description, sizeof(system_cfg.books[2].description), "Manual trading");

    system_cfg.users[0].user_id = 7;
    copy_cstr(system_cfg.users[0].username, sizeof(system_cfg.users[0].username), "alice");
    copy_cstr(system_cfg.users[0].display_name, sizeof(system_cfg.users[0].display_name), "Alice");
    copy_cstr(system_cfg.users[0].password, sizeof(system_cfg.users[0].password), "secret-1");
    system_cfg.users[0].default_book_id = 900;
    system_cfg.users[0].active = true;

    system_cfg.products[0].mm_book_id = 101;
    system_cfg.products[0].arbitrage_strategy_count = 1;
    system_cfg.products[0].arbitrage_strategies[0].type = ArbitrageStrategyType::PCP;
    system_cfg.products[0].arbitrage_strategies[0].book_id = 102;

    {
        DataRepository repo(persistence_cfg, GatewayType::Sim, VolMethod::Wing);
        ASSERT_TRUE(repo.open());

        IdentityState state{};
        ASSERT_TRUE(repo.sync_identity_state(system_cfg, &state));

        ASSERT_EQ(state.books.size(), 3u);
        ASSERT_EQ(state.users.size(), 1u);
        EXPECT_EQ(state.mm_book_ids[0], 101u);
        ASSERT_EQ(state.arb_book_bindings.size(), 1u);
        EXPECT_EQ(state.arb_book_bindings[0].book_id, 102u);

        EXPECT_EQ(state.users[0].user_id, 7u);
        EXPECT_EQ(std::string(state.users[0].username), "alice");
        EXPECT_EQ(state.users[0].default_book_id, 900u);
        EXPECT_TRUE(password_hash_encoded(state.users[0].password_hash));
        EXPECT_TRUE(verify_password("secret-1", state.users[0].password_hash));
    }

    remove_db_artifacts(db_path);
}
