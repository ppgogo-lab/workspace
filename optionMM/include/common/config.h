#pragma once

#include "common/types.h"
#include <cstdint>
#include <string_view>
#include <stdexcept>

namespace omm {

// ─── Feed configuration ───────────────────────────────────────────────────────
enum class FeedType : uint8_t { Multicast, FPGA, FEMAS, Sim };

struct MulticastConfig {
    char      interface[32]{"eth0"};   // network interface name, e.g. "enp65s0f0"
    char      groups[8][64]{};         // multicast group addresses
    int       group_count{0};
    uint16_t  port{9001};
    int       rcvbuf_mb{8};            // SO_RCVBUF in MB
};

struct FpgaConfig {
    char device_path[64]{"/dev/fpga0"};
};

struct FemasMdConfig {
    char front_addr[128]{};
    char broker_id[16]{};
    char user_id[32]{};
    char password[32]{};
    char exchange_id[16]{"CFFEX"};
    int  topic_id{100};
    int  heartbeat_timeout_sec{30};
};

struct FeedConfig {
    FeedType       type{FeedType::Multicast};
    MulticastConfig multicast;
    FpgaConfig      fpga;
    FemasMdConfig   femas;
};

// ─── Gateway configuration ────────────────────────────────────────────────────
enum class GatewayType : uint8_t { CTP, FEMAS, Sim };

struct CtpConfig {
    char front_addr[128]{};    // e.g. "tcp://180.168.146.187:10201"
    char broker_id[16]{};
    char user_id[32]{};
    char password[32]{};
    char app_id[32]{};
    char auth_code[64]{};
};

struct FemasConfig {
    char front_addr[128]{};
    char broker_id[16]{};
    char user_id[32]{};
    char password[32]{};
};

struct GatewayConfig {
    GatewayType type{GatewayType::CTP};
    CtpConfig   ctp;
    FemasConfig femas;
};

struct SimConfig {
    char     profile[16]{"desk"};
    char     scenario[16]{"normal"};
    uint32_t random_seed{42};
    int      tick_interval_ms{100};
    int      strikes_per_side{4};
    int      expiry_count{2};
    double   future_wave_bps{12.0};
    double   future_noise_bps{4.0};
    double   option_spread_bps{30.0};
    Volume   top_level_volume{20};
    double   au_reference_price{580.0};
    double   ag_reference_price{7800.0};
    int      gateway_ack_latency_ms{0};
    int      gateway_cancel_latency_ms{0};
    int      gateway_fill_interval_ms{25};
    double   gateway_order_fill_probability{1.0};
    double   gateway_quote_cross_fill_probability{1.0};
    double   gateway_quote_passive_fill_probability{0.0};
    double   gateway_partial_fill_probability{0.0};
    double   gateway_reject_probability{0.0};
    Volume   gateway_max_fill_size{0};
    int      gateway_slippage_ticks{0};
    double   gateway_quote_near_touch_ticks{0.5};
};

// ─── Pricing configuration ────────────────────────────────────────────────────
enum class VolMethod : uint8_t { SVI, SABR, CubicSpline, Wing, OrcWing };

struct PricingConfig {
    double    risk_free_rate{0.025};   // annualised, e.g. 2.5%
    VolMethod vol_method{VolMethod::SVI};
    int       fit_interval_seconds{60};
    double    sabr_beta{1.0};          // SABR beta (fixed, typically 0.5 or 1.0)
    double    signal_emit_price_epsilon_ticks{0.0};
    double    signal_emit_underlying_epsilon_ticks{0.0};
    double    signal_emit_delta_epsilon{0.0};
    double    signal_emit_vega_epsilon{0.0};
};

// ─── Risk configuration ───────────────────────────────────────────────────────
struct HardRiskConfig {
    Volume max_volume_per_order{100};
};

struct SoftRiskConfig {
    int32_t max_net_position{500};
    double  max_delta{1000.0};
    double  max_gamma{500.0};
    double  max_vega{10000.0};
};

struct RiskConfig {
    HardRiskConfig hard;
    SoftRiskConfig soft;
};

// ─── Per-product strategy parameters ─────────────────────────────────────────
// Stored as plain doubles — atomic updates use AtomicMMParams wrapper (mm_params.h).
struct MMParamsConfig {
    double  bid_spread{0.5};
    double  ask_spread{0.5};
    Volume  quote_volume{10};
    double  product_delta_threshold{50.0};
    double  product_vega_threshold{1000.0};
    double  min_quote_interval_ms{100.0};  // minimum time between quote updates
    int32_t max_position{500};
    int32_t warning_position{250};
    double  base_half_spread_ticks{1.0};
    double  min_half_spread_ticks{1.0};
    double  max_half_spread_ticks{8.0};
    double  inventory_skew_per_lot_ticks{0.01};
    double  follow_weight{0.35};
    double  requote_price_epsilon_ticks{1.0};
    double  market_width_widen_threshold_ticks{6.0};
    double  underlying_move_widen_threshold_ticks{2.0};
    bool    use_one_sided_at_limits{true};
    bool    enabled{true};
    uint8_t _pad[2];
};

// ─── Per-product configuration ────────────────────────────────────────────────
// One entry per underlying option series. Strategy thread is per product.
// Specific option strikes are NOT configured here — they come from the gateway
// instrument query at startup (option contracts change daily).
struct ProductConfig {
    InstrumentCode underlying_id;    // futures contract code, e.g. "cu2501"
    ExchangeId     exchange_id;      // which exchange, e.g. "SHFE"
    int            strategy_core{-1}; // CPU core for this product's strategy thread
    char           strategy_type[32]{"simple_mm"};
    MMParamsConfig params;
};

// ─── Timer configuration ─────────────────────────────────────────────────────
struct SessionWindow {
    char open_time[9]{};    // "HH:MM:SS"
    char close_time[9]{};   // "HH:MM:SS"
};

struct TimerConfig {
    int           hedge_check_interval_ms{1000};
    int           quote_refresh_interval_ms{500};
    SessionWindow sessions[4]{};
    int           session_count{0};
};

// ─── Thread affinity configuration ───────────────────────────────────────────
struct ThreadAffinityConfig {
    int feed_core{2};
    int pricer_core{3};
    int gateway_dispatcher_core{12};
    int vol_fitter_core{13};
    int risk_monitor_core{14};
    int timer_core{14};        // shared with risk_monitor by default
    int grpc_server_core{15};
};

// ─── Monitoring configuration ─────────────────────────────────────────────────
enum class MonitoringPublishMode : uint8_t { Full, Deferred, Off };

struct MonitoringConfig {
    char grpc_listen_addr[64]{"0.0.0.0:50051"};
    MonitoringPublishMode hot_path_publish_mode{MonitoringPublishMode::Full};
};

// ─── Instance configuration ───────────────────────────────────────────────────
struct InstanceConfig {
    ExchangeId exchange_id;
    AccountId  account_id;
};

// ─── Root system configuration ────────────────────────────────────────────────
struct SystemConfig {
    InstanceConfig      instance;
    FeedConfig          feed;
    GatewayConfig       gateway;
    SimConfig           sim;
    PricingConfig       pricing;
    RiskConfig          risk;
    ProductConfig       products[MAX_PRODUCTS];
    int                 product_count{0};
    TimerConfig         timer;
    MonitoringConfig    monitoring;
    ThreadAffinityConfig affinity;
};

// ─── Loader ───────────────────────────────────────────────────────────────────
// Throws std::runtime_error on parse failure, with the offending YAML key path
// included in the message so operators know exactly what to fix.
// yaml-cpp types are not exposed in this header — they stay in config.cpp.
[[nodiscard]] SystemConfig load_config(std::string_view path);

} // namespace omm
