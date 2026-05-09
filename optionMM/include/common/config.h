#pragma once

#include "common/types.h"
#include <cstdint>
#include <string_view>
#include <stdexcept>

namespace omm {

// Feed source selected by feed.type.
enum class FeedType : uint8_t { Multicast, FPGA, FEMAS, Sim };

struct MulticastConfig {
    char      interface[32]{"eth0"};   // OS network interface used for multicast receive.
    char      groups[8][64]{};         // Multicast group addresses to join.
    int       group_count{0};          // Number of valid entries in groups.
    uint16_t  port{9001};              // UDP port shared by configured multicast groups.
    int       rcvbuf_mb{8};            // Socket receive buffer size in megabytes.
};

struct FpgaConfig {
    char device_path[64]{"/dev/fpga0"}; // Device node used by the FPGA feed adapter.
};

struct FemasMdConfig {
    char front_addr[128]{};            // FEMAS market-data front address, e.g. tcp://host:port.
    char broker_id[16]{};              // FEMAS broker id for market-data login.
    char user_id[32]{};                // FEMAS market-data user id.
    char password[32]{};               // FEMAS market-data password.
    char exchange_id[16]{"CFFEX"};     // Exchange code used for subscription context.
    int  topic_id{100};                // FEMAS market-data topic id.
    int  heartbeat_timeout_sec{30};    // FEMAS heartbeat timeout in seconds.
};

struct FeedConfig {
    FeedType        type{FeedType::Multicast}; // Active feed implementation.
    MulticastConfig multicast;                 // Multicast feed settings.
    FpgaConfig      fpga;                      // FPGA feed settings.
    FemasMdConfig   femas;                     // FEMAS market-data settings.
};

// Gateway implementation selected by gateway.type.
enum class GatewayType : uint8_t { FEMAS, Sim };

struct CtpConfig {
    char front_addr[128]{};            // CTP front address. Kept for legacy config compatibility.
    char broker_id[16]{};              // CTP broker id.
    char user_id[32]{};                // CTP user id.
    char password[32]{};               // CTP password.
    char app_id[32]{};                 // CTP app id.
    char auth_code[64]{};              // CTP auth code.
};

struct FemasConfig {
    char front_addr[128]{};            // FEMAS trader front address, e.g. tcp://host:port.
    char broker_id[16]{};              // FEMAS broker id for order entry.
    char user_id[32]{};                // FEMAS trader user id.
    char password[32]{};               // FEMAS trader password.
};

struct GatewayConfig {
    GatewayType type{GatewayType::FEMAS}; // Active gateway implementation.
    FemasConfig femas;                    // FEMAS order-entry settings.
};

struct SimConfig {
    char     profile[16]{"desk"};     // Built-in simulator profile: desk, calm, or fast.
    char     scenario[16]{"normal"};  // Scenario label for simulator behavior selection.
    uint32_t random_seed{42};         // Deterministic seed for simulator random events.
    int      tick_interval_ms{100};   // Delay between generated simulator ticks.
    int      strikes_per_side{4};     // Number of strikes above and below ATM per expiry.
    int      expiry_count{2};         // Number of simulated option expiries.
    double   future_wave_bps{12.0};   // Deterministic futures wave amplitude in basis points.
    double   future_noise_bps{4.0};   // Random futures noise amplitude in basis points.
    double   option_spread_bps{30.0}; // Simulated option top-of-book spread in basis points.
    Volume   top_level_volume{20};    // Simulated top-level book volume.
    double   au_reference_price{580.0};  // Reference price for AU simulator products.
    double   ag_reference_price{7800.0}; // Reference price for AG simulator products.
    int      gateway_ack_latency_ms{0};  // Artificial submit acknowledgement latency.
    int      gateway_cancel_latency_ms{0}; // Artificial cancel acknowledgement latency.
    int      gateway_fill_interval_ms{25}; // Interval for simulated fill attempts.
    double   gateway_order_fill_probability{1.0}; // Probability a normal order fills.
    double   gateway_quote_cross_fill_probability{1.0}; // Probability a crossed quote fills.
    double   gateway_quote_passive_fill_probability{0.0}; // Probability a passive quote fills.
    double   gateway_partial_fill_probability{0.0}; // Probability a fill is partial.
    double   gateway_reject_probability{0.0}; // Probability a submit request is rejected.
    Volume   gateway_max_fill_size{0}; // Maximum simulated fill size; 0 means no cap.
    int      gateway_slippage_ticks{0}; // Simulated fill slippage in ticks.
    double   gateway_quote_near_touch_ticks{0.5}; // Passive-fill distance from touch.
    bool     gateway_benchmark_mode{false}; // Reduces simulator overhead for latency tests.
};

// Volatility surface model selected by pricing.vol_surface.method.
enum class VolMethod : uint8_t { SVI, SABR, CubicSpline, Wing, OrcWing };
// How product-level base offset is interpreted.
enum class BaseOffsetType : uint8_t { Tick, Price, Percentage };
// Whether future-tick repricing writes full Greeks snapshots on the hot path.
enum class HotPathGreeksMode : uint8_t { Full, Compact, Off };

struct PricingConfig {
    double    risk_free_rate{0.025};   // Annualized risk-free rate used by Black-76.
    VolMethod vol_method{VolMethod::SVI}; // Volatility model used by pricer and fitter.
    int       fit_interval_seconds{60}; // Vol surface fit cadence in seconds.
    double    sabr_beta{1.0};          // SABR beta; usually fixed at 0.5 or 1.0.
    double    signal_emit_price_epsilon_ticks{0.0}; // Theo bid/ask change threshold.
    double    signal_emit_underlying_epsilon_ticks{0.0}; // Underlying ref change threshold.
    double    signal_emit_delta_epsilon{0.0}; // Delta change threshold for signal suppression.
    double    signal_emit_vega_epsilon{0.0};  // Vega change threshold for signal suppression.
    HotPathGreeksMode hot_path_greeks_mode{HotPathGreeksMode::Full}; // Hot Greeks policy.
    int       cold_greeks_interval_ms{1000}; // Cold full-Greeks refresh interval.
    int       cold_greeks_batch_size{64};    // Max options refreshed per cold Greeks batch.
};

struct ProductPricingConfig {
    BaseOffsetType base_offset_type{BaseOffsetType::Price}; // Unit of base_offset_value.
    double         base_offset_value{0.0};                  // Product-level future price offset.
};

[[nodiscard]] inline double apply_base_offset(double future_price,
                                              double underlying_tick_size,
                                              const ProductPricingConfig& cfg) noexcept {
    switch (cfg.base_offset_type) {
        case BaseOffsetType::Tick:
            return future_price + cfg.base_offset_value * underlying_tick_size;
        case BaseOffsetType::Price:
            return future_price + cfg.base_offset_value;
        case BaseOffsetType::Percentage:
            return future_price * (1.0 + cfg.base_offset_value);
    }
    return future_price;
}

struct HardRiskConfig {
    Volume max_volume_per_order{100}; // Hard pre-trade max size for a single order or quote leg.
};

struct SoftRiskConfig {
    int32_t max_net_position{500}; // Soft max absolute net position per product/book context.
    double  max_delta{1000.0};     // Soft max portfolio delta.
    double  max_gamma{500.0};      // Soft max portfolio gamma.
    double  max_vega{10000.0};     // Soft max portfolio vega.
};

struct RiskConfig {
    HardRiskConfig hard; // Blocking pre-trade risk limits.
    SoftRiskConfig soft; // Monitoring and strategy gating risk limits.
};

// Per-product market-making parameters. Runtime updates use AtomicMMParams.
struct MMParamsConfig {
    double  bid_spread{0.5};        // Legacy bid spread parameter in ticks.
    double  ask_spread{0.5};        // Legacy ask spread parameter in ticks.
    Volume  quote_volume{10};       // Default quote size per side.
    double  product_delta_threshold{50.0}; // Product delta threshold for hedging/gating.
    double  product_vega_threshold{1000.0}; // Product vega threshold for hedging/gating.
    double  min_quote_interval_ms{100.0}; // Minimum interval between quote updates.
    int32_t max_position{500};      // Hard strategy position limit.
    int32_t warning_position{250};  // Position level where strategy starts defensive quoting.
    double  base_half_spread_ticks{1.0}; // Normal half-spread in ticks.
    double  min_half_spread_ticks{1.0};  // Tightest allowed half-spread in ticks.
    double  max_half_spread_ticks{8.0};  // Widest allowed half-spread in ticks.
    double  inventory_skew_per_lot_ticks{0.01}; // Quote skew per lot of inventory.
    double  follow_weight{0.35};    // Weight used when following theoretical price changes.
    double  requote_price_epsilon_ticks{1.0}; // Min quote price move before requote.
    double  market_width_widen_threshold_ticks{6.0}; // Widen quotes when market is this wide.
    double  underlying_move_widen_threshold_ticks{2.0}; // Widen after large underlying move.
    bool    use_one_sided_at_limits{true}; // Quote only the reducing side at position limits.
    bool    enabled{true};          // Enables this product's market-making strategy.
    uint8_t _pad[2];                // Padding for stable struct layout.
};

struct ArbParamsConfig {
    double  min_edge_ticks{2.0};    // Minimum arbitrage edge required to trade.
    double  cooldown_ms{25.0};      // Minimum time between arbitrage attempts.
    double  scan_interval_ms{1.0};  // Timer scan cadence for non-event scans.
    double  cleanup_timeout_ms{25.0}; // Time before stale arb orders are cleaned up.
    int32_t max_order_volume{1};    // Max volume per arbitrage order.
    int32_t max_live_orders{8};     // Max live arbitrage orders per strategy.
    bool    cleanup_on_partial{true}; // Cancel residual legs after partial fill.
    bool    enabled{false};         // Enables this arbitrage strategy.
    uint8_t _pad[2];                // Padding for stable struct layout.
};

struct ArbitrageStrategyConfig {
    ArbitrageStrategyType type{ArbitrageStrategyType::None}; // Arbitrage strategy kind.
    ArbParamsConfig       params;                            // Parameters for this arb slot.
    BookId                book_id{INVALID_BOOK_ID};          // Book used for arb orders.
};

struct BookBootstrapConfig {
    BookId book_id{INVALID_BOOK_ID}; // Stable internal book id.
    char   book_code[32]{};          // Operator-facing unique book code.
    char   display_name[64]{};       // Human-readable book name.
    bool   active{true};             // Whether the book is active at startup.
    char   description[128]{};       // Optional book description.
};

struct UserBootstrapConfig {
    UserId user_id{INVALID_USER_ID}; // Stable internal user id.
    char   username[32]{};           // Login/user key.
    char   display_name[64]{};       // Human-readable user name.
    char   password[128]{};          // Configured password or password hash.
    bool   active{true};             // Whether the user is active at startup.
    BookId default_book_id{INVALID_BOOK_ID}; // Default book for manual orders.
};

// One entry per underlying option series. Specific option strikes come from
// gateway instrument query or simulator instrument generation.
struct ProductConfig {
    InstrumentCode underlying_id;    // Underlying futures contract code.
    ExchangeId     exchange_id;      // Exchange code for this product.
    int            strategy_core{-1}; // CPU core for this product strategy thread.
    int            arbitrage_core{-1}; // Optional CPU core for arbitrage sidecar.
    char           strategy_type[32]{"simple_mm"}; // Strategy registered in init_strategies.
    BookId         mm_book_id{INVALID_BOOK_ID}; // Book used for market-making orders.
    ProductPricingConfig pricing;    // Product-specific pricing adjustment.
    MMParamsConfig params;           // Product-specific MM strategy parameters.
    ArbitrageStrategyConfig arbitrage_strategies[MAX_ARBITRAGE_STRATEGIES_PER_PRODUCT]{}; // Arb slots.
    int            arbitrage_strategy_count{0}; // Number of configured arb slots.
};

struct SessionWindow {
    char open_time[9]{};             // Local session open time, HH:MM:SS.
    char close_time[9]{};            // Local session close time, HH:MM:SS.
};

struct TimerConfig {
    int           hedge_check_interval_ms{1000}; // Hedge timer interval.
    int           quote_refresh_interval_ms{500}; // Full-book quote refresh timer interval.
    SessionWindow sessions[4]{};     // Optional simple intraday session windows.
    int           session_count{0};  // Number of valid entries in sessions.
};

struct ExchangeCalendarDayConfig {
    int32_t date{0};                 // Calendar date in YYYYMMDD format.
    bool    is_trading_day{false};   // Whether date is a trading day.
    uint8_t _pad[3]{};               // Padding for stable struct layout.
};

struct ExchangeTradingSessionConfig {
    int8_t start_day_offset{0};      // Start date offset from trading day.
    int8_t end_day_offset{0};        // End date offset from trading day.
    char   start_time[9]{};          // Session start time, HH:MM:SS.
    char   end_time[9]{};            // Session end time, HH:MM:SS.
};

struct ExchangeCalendarConfig {
    ExchangeId exchange_id;          // Exchange this calendar applies to.
    ExchangeCalendarDayConfig days[MAX_EXCHANGE_CALENDAR_DAYS]{}; // Trading-day table.
    int day_count{0};                // Number of valid entries in days.
};

struct ExchangeTradingTimeConfig {
    ExchangeId exchange_id;          // Exchange this trading-time table applies to.
    ExchangeTradingSessionConfig sessions[MAX_TRADING_SESSIONS_PER_EXCHANGE]{}; // Sessions.
    int session_count{0};            // Number of valid entries in sessions.
};

struct ThreadAffinityConfig {
    int feed_core{2};                // CPU core for feed thread.
    int pricer_core{3};              // CPU core for pricer thread.
    int gateway_dispatcher_core{12}; // CPU core for gateway dispatcher thread.
    int vol_fitter_core{13};         // CPU core for vol fitter thread.
    int risk_monitor_core{14};       // CPU core for post-trade risk thread.
    int timer_core{11};              // CPU core for timer thread.
    int grpc_server_core{15};        // CPU core for gRPC monitoring server thread.
};

struct ThreadSchedulingConfig {
    bool enable_realtime{false};     // Apply realtime scheduler priorities when supported.
    bool low_latency_spin{false};    // Spin while idle instead of yielding/sleeping.
    uint8_t _pad0[2]{};              // Padding for stable struct layout.
    int pricer_priority{80};         // Realtime priority for pricer thread.
    int strategy_priority{70};       // Realtime priority for strategy threads.
    int arbitrage_priority{60};      // Realtime priority for arbitrage threads.
    int gateway_dispatcher_priority{75}; // Realtime priority for gateway dispatcher.
    int vol_fitter_priority{20};     // Realtime priority for vol fitter.
    int risk_monitor_priority{30};   // Realtime priority for risk monitor.
    int timer_priority{40};          // Realtime priority for timer thread.
};

// Hot-path monitoring publication policy.
enum class MonitoringPublishMode : uint8_t { Full, Deferred, Off };

struct MonitoringConfig {
    char grpc_listen_addr[64]{"0.0.0.0:50051"}; // gRPC bind address.
    MonitoringPublishMode hot_path_publish_mode{MonitoringPublishMode::Full}; // Monitor event policy.
};

struct PersistenceConfig {
    bool enabled{false};             // Enables SQLite persistence writer.
    char data_path[260]{"data/optionmm.sqlite"}; // SQLite database path.
    int  batch_max_rows{256};        // Max rows per persistence batch write.
    int  flush_interval_ms{10};      // Max delay before flushing persistence batch.
    int  snapshot_interval_ms{1000}; // Periodic state snapshot interval.
    int  busy_timeout_ms{1000};      // SQLite busy timeout in milliseconds.
};

struct ExecutionConfig {
    bool low_latency_mode{false};    // Skips optional hot-path side effects for live trading.
    uint8_t _pad[3]{};               // Padding for stable struct layout.
};

struct InstanceConfig {
    ExchangeId exchange_id;          // Primary instance exchange id.
    AccountId  account_id;           // Trading account id for this process.
};

struct SystemConfig {
    InstanceConfig      instance;    // Instance identity.
    FeedConfig          feed;        // Market-data feed configuration.
    GatewayConfig       gateway;     // Order-entry gateway configuration.
    SimConfig           sim;         // Simulator configuration.
    PricingConfig       pricing;     // Global pricing configuration.
    RiskConfig          risk;        // Global risk configuration.
    ProductConfig       products[MAX_PRODUCTS]; // Product strategy configurations.
    int                 product_count{0}; // Number of configured products.
    TimerConfig         timer;       // Strategy timer configuration.
    MonitoringConfig    monitoring;  // Monitoring/gRPC configuration.
    PersistenceConfig   persistence; // Persistence configuration.
    ExecutionConfig     execution;   // Execution hot-path policy.
    ThreadAffinityConfig affinity;   // Thread CPU pinning configuration.
    ThreadSchedulingConfig scheduling; // Thread scheduling configuration.
    BookBootstrapConfig books[MAX_BOOKS]{}; // Startup books.
    int                 book_count{0}; // Number of configured books.
    UserBootstrapConfig users[MAX_USERS]{}; // Startup users.
    int                 user_count{0}; // Number of configured users.
    ExchangeCalendarConfig exchange_calendars[MAX_EXCHANGE_CALENDARS]{}; // Calendars.
    int                 exchange_calendar_count{0}; // Number of configured calendars.
    ExchangeTradingTimeConfig exchange_trading_times[MAX_EXCHANGE_CALENDARS]{}; // Sessions.
    int                 exchange_trading_time_count{0}; // Number of trading-time tables.
};

// Throws std::runtime_error on parse failure, with the offending YAML key path
// included in the message. yaml-cpp types are intentionally hidden in config.cpp.
[[nodiscard]] SystemConfig load_config(std::string_view path);

} // namespace omm
