#pragma once

#include "common/config.h"
#include "common/ring_buffer.h"
#include "common/types.h"
#include "gateway/gateway.h"
#include "pricing/orc_wing.h"
#include "pricing/svi.h"
#include "pricing/vol_surface.h"
#include "pricing/wing.h"

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
    GatewayOrderRecoveryHandle recovery{};
};

struct QuotePersistenceEvent {
    QuotePersistenceEventType  type{QuotePersistenceEventType::Submit};
    uint8_t                    _pad0[7]{};
    Quote                      quote{};
    GatewayQuoteRecoveryHandle recovery{};
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

    std::vector<GatewayRecoveredOrder> live_orders;
    std::vector<GatewayRecoveredQuote> live_quotes;
    std::vector<Position> positions;
    std::array<ProductParamsState, MAX_PRODUCTS> mm_params{};
    std::vector<ArbParamsState> arb_params;
    SoftRiskConfig risk_params{};
    bool has_risk_params{false};
};

class DataRepository {
public:
    explicit DataRepository(const PersistenceConfig& cfg,
                            GatewayType gateway_type,
                            VolMethod vol_method);
    ~DataRepository();

    DataRepository(const DataRepository&) = delete;
    DataRepository& operator=(const DataRepository&) = delete;

    void set_instruments(const Instrument* instruments,
                         uint16_t n_instruments) noexcept;

    bool open();
    void start();
    void stop() noexcept;

    [[nodiscard]] bool is_enabled() const noexcept { return cfg_.enabled; }

    bool persist_instruments();
    bool sync_identity_state(const SystemConfig& cfg, IdentityState* out);
    bool load_recovery_state(RecoveryState* out);
    bool load_trade_history(std::vector<Trade>* out);
    bool persist_end_of_day_snapshot(const EndOfDaySnapshot& snapshot);

    bool enqueue_order_event(const OrderPersistenceEvent& event) noexcept;
    bool enqueue_quote_event(const QuotePersistenceEvent& event) noexcept;
    bool enqueue_trade(const Trade& trade) noexcept;
    bool enqueue_mm_params(const MMParamsPersistenceEvent& event) noexcept;
    bool enqueue_arb_params(const ArbParamsPersistenceEvent& event) noexcept;
    bool enqueue_risk_params(const RiskParamsPersistenceEvent& event) noexcept;
    bool enqueue_positions_snapshot(const PositionSnapshotEvent& event) noexcept;

private:
    struct LiveOrderRecord {
        Order order{};
        GatewayOrderRecoveryHandle recovery{};
        bool terminal{false};
    };

    struct LiveQuoteRecord {
        Quote quote{};
        GatewayQuoteRecoveryHandle recovery{};
        Volume remaining_bid{0};
        Volume remaining_ask{0};
    };

    void writer_loop() noexcept;
    bool ensure_schema_locked();
    bool prepare_statements_locked();
    void finalize_statements_locked() noexcept;
    void close_locked() noexcept;
    bool load_identity_locked(IdentityState* out);
    bool seed_identity_locked(const SystemConfig& cfg);
    bool migrate_identity_schema_locked();
    bool migrate_book_columns_locked();

    bool flush_once_locked(int max_rows);
    bool write_order_event_locked(const OrderPersistenceEvent& event);
    bool write_quote_event_locked(const QuotePersistenceEvent& event);
    bool write_trade_locked(const Trade& trade);
    bool write_mm_params_locked(const MMParamsPersistenceEvent& event);
    bool write_arb_params_locked(const ArbParamsPersistenceEvent& event);
    bool write_risk_params_locked(const RiskParamsPersistenceEvent& event);
    bool write_positions_snapshot_locked(const PositionSnapshotEvent& event);
    bool write_instruments_locked(int32_t trading_day,
                                  const char* table_name,
                                  const Instrument* instruments,
                                  uint16_t n_instruments);
    bool write_eod_snapshot_locked(const EndOfDaySnapshot& snapshot);

    void prime_caches_locked(const RecoveryState& state);
    void mark_live_order_deleted_locked(OrderId id);
    void mark_live_quote_deleted_locked(QuoteId id);

    [[nodiscard]] const Instrument* instrument_by_id(uint16_t instrument_id) const noexcept;
    [[nodiscard]] uint16_t find_instrument_id_by_code(const char* code) const noexcept;
    [[nodiscard]] int32_t current_trading_day() const noexcept;
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
