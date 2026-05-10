#pragma once

#include "common/config.h"
#include "common/ring_buffer.h"
#include "common/types.h"
#include "gateway/gateway.h"
#include "pricing/orc_wing.h"
#include "pricing/svi.h"
#include "pricing/vol_surface.h"
#include "pricing/wing.h"
#include "common/trading_calendar.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <memory>
#include <thread>
#include <unordered_map>
#include <vector>

struct sqlite3;
struct sqlite3_stmt;

namespace omm {

enum class OrderPersistenceEventType : uint8_t {
    Submit = 0,
    Ack = 1,
    Cancel = 2,
    Reject = 3,
};

enum class QuotePersistenceEventType : uint8_t {
    Submit = 0,
    Ack = 1,
    Cancel = 2,
    Reject = 3,
};

struct OrderPersistenceEvent {
    OrderPersistenceEventType  type{OrderPersistenceEventType::Submit};
    uint8_t                    _pad0[7]{};
    Order                      order{};
};

struct QuotePersistenceEvent {
    QuotePersistenceEventType  type{QuotePersistenceEventType::Submit};
    uint8_t                    _pad0[7]{};
    Quote                      quote{};
};

struct MMParamsPersistenceEvent {
    uint8_t       product_index{0xFF};
    uint8_t       _pad0[7]{};
    MMParamsConfig params{};
    Timestamp     update_ts{0};
};

struct ArbParamsPersistenceEvent {
    uint8_t               product_index{0xFF};
    ArbitrageStrategyType strategy_type{ArbitrageStrategyType::None};
    uint8_t               _pad0[6]{};
    ArbParamsConfig       params{};
    Timestamp             update_ts{0};
};

struct RiskParamsPersistenceEvent {
    SoftRiskConfig params{};
    Timestamp      update_ts{0};
};

struct PositionSnapshotEvent {
    Timestamp snapshot_ts{0};
    uint16_t  n_instruments{0};
    uint8_t   _pad0[6]{};
    Position  positions[MAX_INSTRUMENTS]{};
};

struct PersistedVolModelSlice {
    uint8_t model_type{0};
    uint8_t product_index{0};
    uint8_t _pad0[2]{};
    int32_t trading_day{0};
    double  expiry_T{0.0};
    double  a{0.0};
    double  b{0.0};
    double  rho{0.0};
    double  m{0.0};
    double  sigma{0.0};
    double  atm_vol{0.0};
    double  slope_call{0.0};
    double  slope_put{0.0};
    double  curve_call{0.0};
    double  curve_put{0.0};
    double  ref_price{0.0};
    double  atm_forward{0.0};
    double  ssr{0.0};
    double  vol_ref{0.0};
    double  slope_ref{0.0};
    double  vcr{0.0};
    double  scr{0.0};
    double  put_curv{0.0};
    double  call_curv{0.0};
    double  down_cutoff{0.0};
    double  up_cutoff{0.0};
    double  down_smoothing{0.0};
    double  up_smoothing{0.0};
};

struct EndOfDaySnapshot {
    int32_t trading_day{0};
    uint16_t n_instruments{0};
    uint16_t n_model_slices{0};
    Greeks greeks[MAX_INSTRUMENTS]{};
    Instrument instruments[MAX_INSTRUMENTS]{};
    PersistedVolModelSlice model_slices[MAX_PRODUCTS * MAX_EXPIRIES]{};
};

struct PersistedBook {
    BookId book_id{INVALID_BOOK_ID};
    char   book_code[32]{};
    char   display_name[64]{};
    bool   active{true};
    char   description[128]{};
};

struct PersistedUser {
    UserId user_id{INVALID_USER_ID};
    char   username[32]{};
    char   display_name[64]{};
    char   password_hash[256]{};
    bool   active{true};
    BookId default_book_id{INVALID_BOOK_ID};
};

struct PersistedArbBookBinding {
    uint8_t               product_index{0xFF};
    ArbitrageStrategyType strategy_type{ArbitrageStrategyType::None};
    uint8_t               _pad0[2]{};
    BookId                book_id{INVALID_BOOK_ID};
};

struct IdentityState {
    std::vector<PersistedUser> users;
    std::vector<PersistedBook> books;
    std::array<BookId, MAX_PRODUCTS> mm_book_ids{};
    std::vector<PersistedArbBookBinding> arb_book_bindings;
};

struct RecoveryState {
    struct ProductParamsState {
        bool valid{false};
        MMParamsConfig params{};
    };

    struct ArbParamsState {
        bool valid{false};
        uint8_t product_index{0xFF};
        ArbitrageStrategyType strategy_type{ArbitrageStrategyType::None};
        ArbParamsConfig params{};
    };

    std::vector<Order> live_orders;
    std::vector<Quote> live_quotes;
    std::vector<Position> positions;
    std::array<ProductParamsState, MAX_PRODUCTS> mm_params{};
    std::vector<ArbParamsState> arb_params;
    SoftRiskConfig risk_params{};
    bool has_risk_params{false};
};

class DataRepository {
public:
    /**
     * @brief DataRepository.
     * @param cfg Parameter supplied by the caller.
     * @param gateway_type Parameter supplied by the caller.
     * @param vol_method Parameter supplied by the caller.
     * @return None.
     */
    explicit DataRepository(const PersistenceConfig& cfg,
                            GatewayType gateway_type,
                            VolMethod vol_method);
    /**
     * @brief DataRepository.
     * @return None.
     */
    ~DataRepository();

    /**
     * @brief DataRepository.
     * @param DataRepository Parameter supplied by the caller.
     * @return None.
     */
    DataRepository(const DataRepository&) = delete;
    DataRepository& operator=(const DataRepository&) = delete;

    /**
     * @brief Set instruments.
     * @param instruments Parameter supplied by the caller.
     * @param n_instruments Parameter supplied by the caller.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    void set_instruments(const Instrument* instruments,
                         uint16_t n_instruments) noexcept;

    /**
     * @brief Open.
     * @return Return value produced by the operation.
     */
    bool open();
    /**
     * @brief Start.
     * @return None.
     */
    void start();
    /**
     * @brief Stop.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    void stop() noexcept;

    /**
     * @brief Is enabled.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] bool is_enabled() const noexcept { return cfg_.enabled; }

    /**
     * @brief Persist instruments.
     * @return Return value produced by the operation.
     */
    bool persist_instruments();
    /**
     * @brief Sync identity state.
     * @param cfg Parameter supplied by the caller.
     * @param out Parameter supplied by the caller.
     * @return Return value produced by the operation.
     */
    bool sync_identity_state(const SystemConfig& cfg, IdentityState* out);
    /**
     * @brief Load recovery state.
     * @param out Parameter supplied by the caller.
     * @return Return value produced by the operation.
     */
    bool load_recovery_state(RecoveryState* out);
    /**
     * @brief Load trade history.
     * @param out Parameter supplied by the caller.
     * @return Return value produced by the operation.
     */
    bool load_trade_history(std::vector<Trade>* out);
    /**
     * @brief Persist end of day snapshot.
     * @param snapshot Parameter supplied by the caller.
     * @return Return value produced by the operation.
     */
    bool persist_end_of_day_snapshot(const EndOfDaySnapshot& snapshot);
    /**
     * @brief Seed exchange calendar.
     * @param cfg Parameter supplied by the caller.
     * @return Return value produced by the operation.
     */
    bool seed_exchange_calendar(const SystemConfig& cfg);
    /**
     * @brief Load exchange calendars.
     * @param out Parameter supplied by the caller.
     * @return Return value produced by the operation.
     */
    bool load_exchange_calendars(std::vector<ExchangeTradingCalendar>* out);

    /**
     * @brief Enqueue order event.
     * @param event Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    bool enqueue_order_event(const OrderPersistenceEvent& event) noexcept;
    /**
     * @brief Enqueue quote event.
     * @param event Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    bool enqueue_quote_event(const QuotePersistenceEvent& event) noexcept;
    /**
     * @brief Enqueue trade.
     * @param trade Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    bool enqueue_trade(const Trade& trade) noexcept;
    /**
     * @brief Enqueue mm params.
     * @param event Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    bool enqueue_mm_params(const MMParamsPersistenceEvent& event) noexcept;
    /**
     * @brief Enqueue arb params.
     * @param event Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    bool enqueue_arb_params(const ArbParamsPersistenceEvent& event) noexcept;
    /**
     * @brief Enqueue risk params.
     * @param event Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    bool enqueue_risk_params(const RiskParamsPersistenceEvent& event) noexcept;
    /**
     * @brief Enqueue positions snapshot.
     * @param event Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    bool enqueue_positions_snapshot(const PositionSnapshotEvent& event) noexcept;

    // Batch enqueue methods for improved throughput
    /**
     * @brief Enqueue order events batch.
     * @param events Parameter supplied by the caller.
     * @param count Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    int enqueue_order_events_batch(const OrderPersistenceEvent* events, int count) noexcept;
    /**
     * @brief Enqueue quote events batch.
     * @param events Parameter supplied by the caller.
     * @param count Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    int enqueue_quote_events_batch(const QuotePersistenceEvent* events, int count) noexcept;
    /**
     * @brief Enqueue trades batch.
     * @param trades Parameter supplied by the caller.
     * @param count Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    int enqueue_trades_batch(const Trade* trades, int count) noexcept;

private:
    struct LiveOrderRecord {
        Order order{};
        bool terminal{false};
    };

    struct LiveQuoteRecord {
        Quote quote{};
        Volume remaining_bid{0};
        Volume remaining_ask{0};
    };

    /**
     * @brief Writer loop.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    void writer_loop() noexcept;
    /**
     * @brief Ensure schema locked.
     * @return Return value produced by the operation.
     */
    bool ensure_schema_locked();
    /**
     * @brief Prepare statements locked.
     * @return Return value produced by the operation.
     */
    bool prepare_statements_locked();
    /**
     * @brief Finalize statements locked.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    void finalize_statements_locked() noexcept;
    /**
     * @brief Close locked.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    void close_locked() noexcept;
    /**
     * @brief Load identity locked.
     * @param out Parameter supplied by the caller.
     * @return Return value produced by the operation.
     */
    bool load_identity_locked(IdentityState* out);
    /**
     * @brief Seed identity locked.
     * @param cfg Parameter supplied by the caller.
     * @return Return value produced by the operation.
     */
    bool seed_identity_locked(const SystemConfig& cfg);
    /**
     * @brief Migrate identity schema locked.
     * @return Return value produced by the operation.
     */
    bool migrate_identity_schema_locked();
    /**
     * @brief Migrate book columns locked.
     * @return Return value produced by the operation.
     */
    bool migrate_book_columns_locked();
    /**
     * @brief Migrate order semantics columns locked.
     * @return Return value produced by the operation.
     */
    bool migrate_order_semantics_columns_locked();

    /**
     * @brief Flush once locked.
     * @param max_rows Parameter supplied by the caller.
     * @return Return value produced by the operation.
     */
    bool flush_once_locked(int max_rows);
    /**
     * @brief Write order event locked.
     * @param event Parameter supplied by the caller.
     * @return Return value produced by the operation.
     */
    bool write_order_event_locked(const OrderPersistenceEvent& event);
    /**
     * @brief Write quote event locked.
     * @param event Parameter supplied by the caller.
     * @return Return value produced by the operation.
     */
    bool write_quote_event_locked(const QuotePersistenceEvent& event);
    /**
     * @brief Write trade locked.
     * @param trade Parameter supplied by the caller.
     * @return Return value produced by the operation.
     */
    bool write_trade_locked(const Trade& trade);
    /**
     * @brief Write mm params locked.
     * @param event Parameter supplied by the caller.
     * @return Return value produced by the operation.
     */
    bool write_mm_params_locked(const MMParamsPersistenceEvent& event);
    /**
     * @brief Write arb params locked.
     * @param event Parameter supplied by the caller.
     * @return Return value produced by the operation.
     */
    bool write_arb_params_locked(const ArbParamsPersistenceEvent& event);
    /**
     * @brief Write risk params locked.
     * @param event Parameter supplied by the caller.
     * @return Return value produced by the operation.
     */
    bool write_risk_params_locked(const RiskParamsPersistenceEvent& event);
    /**
     * @brief Write positions snapshot locked.
     * @param event Parameter supplied by the caller.
     * @return Return value produced by the operation.
     */
    bool write_positions_snapshot_locked(const PositionSnapshotEvent& event);
    /**
     * @brief Write instruments locked.
     * @param trading_day Parameter supplied by the caller.
     * @param table_name Parameter supplied by the caller.
     * @param instruments Parameter supplied by the caller.
     * @param n_instruments Parameter supplied by the caller.
     * @return Return value produced by the operation.
     */
    bool write_instruments_locked(int32_t trading_day,
                                  const char* table_name,
                                  const Instrument* instruments,
                                  uint16_t n_instruments);
    /**
     * @brief Write eod snapshot locked.
     * @param snapshot Parameter supplied by the caller.
     * @return Return value produced by the operation.
     */
    bool write_eod_snapshot_locked(const EndOfDaySnapshot& snapshot);

    /**
     * @brief Prime caches locked.
     * @param state Parameter supplied by the caller.
     * @return None.
     */
    void prime_caches_locked(const RecoveryState& state);
    /**
     * @brief Mark live order deleted locked.
     * @param id Parameter supplied by the caller.
     * @return None.
     */
    void mark_live_order_deleted_locked(OrderId id);
    /**
     * @brief Mark live quote deleted locked.
     * @param id Parameter supplied by the caller.
     * @return None.
     */
    void mark_live_quote_deleted_locked(QuoteId id);

    /**
     * @brief Instrument by id.
     * @param instrument_id Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] const Instrument* instrument_by_id(uint16_t instrument_id) const noexcept;
    /**
     * @brief Find instrument id by code.
     * @param code Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] uint16_t find_instrument_id_by_code(const char* code) const noexcept;
    /**
     * @brief Current trading day.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] int32_t current_trading_day() const noexcept;
    /**
     * @brief Is ctp ask leg.
     * @param id Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] static bool is_ctp_ask_leg(OrderId id) noexcept;

    PersistenceConfig cfg_{};
    GatewayType gateway_type_{GatewayType::Sim};
    VolMethod vol_method_{VolMethod::SVI};
    const Instrument* instruments_{nullptr};
    uint16_t n_instruments_{0};

    std::atomic<bool> stop_flag_{false};
    std::atomic<bool> writer_running_{false};
    std::thread writer_thread_;

    SPSCRingBuffer<OrderPersistenceEvent, 4096> order_events_;
    SPSCRingBuffer<QuotePersistenceEvent, 2048> quote_events_;
    SPSCRingBuffer<Trade, 4096> trades_;
    SPSCRingBuffer<MMParamsPersistenceEvent, 128> mm_param_events_;
    SPSCRingBuffer<ArbParamsPersistenceEvent, 128> arb_param_events_;
    SPSCRingBuffer<RiskParamsPersistenceEvent, 32> risk_param_events_;
    SPSCRingBuffer<PositionSnapshotEvent, 8> position_snapshots_;

    std::unordered_map<OrderId, LiveOrderRecord> live_orders_;
    std::unordered_map<QuoteId, LiveQuoteRecord> live_quotes_;

    mutable std::mutex db_mutex_;
    sqlite3* db_{nullptr};
    sqlite3_stmt* stmt_upsert_live_order_{nullptr};
    sqlite3_stmt* stmt_delete_live_order_{nullptr};
    sqlite3_stmt* stmt_upsert_live_quote_{nullptr};
    sqlite3_stmt* stmt_delete_live_quote_{nullptr};
    sqlite3_stmt* stmt_insert_trade_{nullptr};
    sqlite3_stmt* stmt_insert_position_{nullptr};
    sqlite3_stmt* stmt_clear_positions_{nullptr};
    sqlite3_stmt* stmt_upsert_mm_params_{nullptr};
    sqlite3_stmt* stmt_upsert_arb_params_{nullptr};
    sqlite3_stmt* stmt_replace_risk_params_{nullptr};
    sqlite3_stmt* stmt_replace_instrument_{nullptr};
    sqlite3_stmt* stmt_delete_instruments_{nullptr};
    sqlite3_stmt* stmt_replace_eod_greeks_{nullptr};
    sqlite3_stmt* stmt_replace_eod_model_params_{nullptr};
    sqlite3_stmt* stmt_replace_eod_instrument_{nullptr};
    sqlite3_stmt* stmt_delete_eod_greeks_for_day_{nullptr};
    sqlite3_stmt* stmt_delete_eod_model_params_for_day_{nullptr};
    sqlite3_stmt* stmt_delete_eod_instruments_for_day_{nullptr};
    sqlite3_stmt* stmt_delete_books_{nullptr};
    sqlite3_stmt* stmt_delete_users_{nullptr};
    sqlite3_stmt* stmt_delete_strategy_bindings_{nullptr};
    sqlite3_stmt* stmt_upsert_book_{nullptr};
    sqlite3_stmt* stmt_upsert_user_{nullptr};
    sqlite3_stmt* stmt_upsert_strategy_binding_{nullptr};
};

/**
 * @brief Build end of day snapshot.
 * @param trading_day Parameter supplied by the caller.
 * @param instruments Parameter supplied by the caller.
 * @param n_instruments Parameter supplied by the caller.
 * @param greeks_snapshot Parameter supplied by the caller.
 * @param model Parameter supplied by the caller.
 * @param svi_surfaces Parameter supplied by the caller.
 * @param wing_surfaces Parameter supplied by the caller.
 * @param orc_wing_surfaces Parameter supplied by the caller.
 * @param product_count Parameter supplied by the caller.
 * @return Return value produced by the operation.
 * @note Noexcept API preserves hot-path failure and latency invariants.
 */
EndOfDaySnapshot build_end_of_day_snapshot(
        int32_t trading_day,
        const Instrument* instruments,
        uint16_t n_instruments,
        const Greeks* greeks_snapshot,
        VolMethod model,
        const std::array<VolSurfaceManager<SVIVolSurface>, MAX_PRODUCTS>& svi_surfaces,
        const std::array<VolSurfaceManager<WingVolSurface>, MAX_PRODUCTS>& wing_surfaces,
        const std::array<VolSurfaceManager<OrcWingVolSurface>, MAX_PRODUCTS>& orc_wing_surfaces,
        int product_count) noexcept;

} // namespace omm
