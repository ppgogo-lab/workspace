#include "common/config.h"

#include <yaml-cpp/yaml.h>
#include <cstring>
#include <stdexcept>
#include <string>
#include <algorithm>

namespace omm {

// ─── Helpers ──────────────────────────────────────────────────────────────────
static void require(const YAML::Node& node, const char* path) {
    if (!node || node.IsNull())
        throw std::runtime_error(std::string("config: required key missing: ") + path);
}

template<typename T>
static T get(const YAML::Node& node, const char* path, T default_val) {
    if (!node || !node.IsDefined()) return default_val;
    try {
        return node.as<T>();
    } catch (const YAML::Exception& e) {
        throw std::runtime_error(std::string("config: bad value at '") + path + "': " + e.what());
    }
}

static void str_copy(char* dst, std::size_t dst_size, const YAML::Node& node,
                     const char* path, const char* default_val = "") {
    std::string val = get<std::string>(node, path, default_val);
    if (val.size() >= dst_size)
        throw std::runtime_error(std::string("config: value too long at '") + path + "'");
    std::strncpy(dst, val.c_str(), dst_size - 1);
    dst[dst_size - 1] = '\0';
}

// ─── Section parsers ──────────────────────────────────────────────────────────
static InstanceConfig parse_instance(const YAML::Node& n) {
    require(n, "instance");
    InstanceConfig c;
    str_copy(c.exchange_id.data, sizeof(c.exchange_id.data),
             n["exchange_id"], "instance.exchange_id");
    str_copy(c.account_id.data, sizeof(c.account_id.data),
             n["account_id"], "instance.account_id");
    return c;
}

static FeedConfig parse_feed(const YAML::Node& n) {
    require(n, "feed");
    FeedConfig c;
    std::string type = get<std::string>(n["type"], "feed.type", "multicast");
    if (type == "fpga")      c.type = FeedType::FPGA;
    else if (type == "multicast") c.type = FeedType::Multicast;
    else if (type == "femas") c.type = FeedType::FEMAS;
    else if (type == "sim") c.type = FeedType::Sim;
    else throw std::runtime_error("config: feed.type must be 'multicast', 'fpga', 'femas', or 'sim', got: " + type);

    if (auto mc = n["multicast"]) {
        str_copy(c.multicast.interface, sizeof(c.multicast.interface),
                 mc["interface"], "feed.multicast.interface", "eth0");
        c.multicast.port    = get<uint16_t>(mc["port"], "feed.multicast.port", 9001);
        c.multicast.rcvbuf_mb = get<int>(mc["rcvbuf_mb"], "feed.multicast.rcvbuf_mb", 8);
        if (auto groups = mc["groups"]) {
            int i = 0;
            for (auto g : groups) {
                if (i >= 8) throw std::runtime_error("config: too many multicast groups (max 8)");
                str_copy(c.multicast.groups[i++], 64, g, "feed.multicast.groups[]", "");
            }
            c.multicast.group_count = i;
        }
    }
    if (auto fp = n["fpga"])
        str_copy(c.fpga.device_path, sizeof(c.fpga.device_path),
                 fp["device"], "feed.fpga.device", "/dev/fpga0");
    if (auto fm = n["femas"]) {
        str_copy(c.femas.front_addr, sizeof(c.femas.front_addr),
                 fm["front_addr"], "feed.femas.front_addr", "");
        str_copy(c.femas.broker_id, sizeof(c.femas.broker_id),
                 fm["broker_id"], "feed.femas.broker_id", "");
        str_copy(c.femas.user_id, sizeof(c.femas.user_id),
                 fm["user_id"], "feed.femas.user_id", "");
        str_copy(c.femas.password, sizeof(c.femas.password),
                 fm["password"], "feed.femas.password", "");
        str_copy(c.femas.exchange_id, sizeof(c.femas.exchange_id),
                 fm["exchange_id"], "feed.femas.exchange_id", "CFFEX");
        c.femas.topic_id = get<int>(fm["topic_id"], "feed.femas.topic_id", 100);
        c.femas.heartbeat_timeout_sec =
            get<int>(fm["heartbeat_timeout_sec"], "feed.femas.heartbeat_timeout_sec", 30);
    }
    return c;
}

static GatewayConfig parse_gateway(const YAML::Node& n) {
    require(n, "gateway");
    GatewayConfig c;
    std::string type = get<std::string>(n["type"], "gateway.type", "ctp");
    if (type == "ctp")       c.type = GatewayType::CTP;
    else if (type == "femas") c.type = GatewayType::FEMAS;
    else if (type == "sim") c.type = GatewayType::Sim;
    else throw std::runtime_error("config: gateway.type must be 'ctp', 'femas', or 'sim', got: " + type);

    if (auto ctp = n["ctp"]) {
        str_copy(c.ctp.front_addr, sizeof(c.ctp.front_addr), ctp["front_addr"], "gateway.ctp.front_addr");
        str_copy(c.ctp.broker_id,  sizeof(c.ctp.broker_id),  ctp["broker_id"],  "gateway.ctp.broker_id");
        str_copy(c.ctp.user_id,    sizeof(c.ctp.user_id),    ctp["user_id"],    "gateway.ctp.user_id");
        str_copy(c.ctp.password,   sizeof(c.ctp.password),   ctp["password"],   "gateway.ctp.password");
        str_copy(c.ctp.app_id,     sizeof(c.ctp.app_id),     ctp["app_id"],     "gateway.ctp.app_id", "");
        str_copy(c.ctp.auth_code,  sizeof(c.ctp.auth_code),  ctp["auth_code"],  "gateway.ctp.auth_code", "");
    }
    if (auto fm = n["femas"]) {
        str_copy(c.femas.front_addr, sizeof(c.femas.front_addr), fm["front_addr"], "gateway.femas.front_addr");
        str_copy(c.femas.broker_id,  sizeof(c.femas.broker_id),  fm["broker_id"],  "gateway.femas.broker_id");
        str_copy(c.femas.user_id,    sizeof(c.femas.user_id),    fm["user_id"],    "gateway.femas.user_id");
        str_copy(c.femas.password,   sizeof(c.femas.password),   fm["password"],   "gateway.femas.password");
    }
    return c;
}

static SimConfig parse_sim(const YAML::Node& n) {
    SimConfig c;
    if (!n) return c;

    const std::string profile = get<std::string>(n["profile"], "sim.profile", "desk");
    const std::string scenario = get<std::string>(n["scenario"], "sim.scenario", "normal");
    str_copy(c.profile, sizeof(c.profile), n["profile"], "sim.profile", profile.c_str());
    str_copy(c.scenario, sizeof(c.scenario), n["scenario"], "sim.scenario", scenario.c_str());

    if (profile == "calm") {
        c.tick_interval_ms = 160;
        c.future_wave_bps = 8.0;
        c.future_noise_bps = 1.6;
        c.option_spread_bps = 22.0;
        c.gateway_ack_latency_ms = 30;
        c.gateway_cancel_latency_ms = 40;
        c.gateway_fill_interval_ms = 180;
        c.gateway_order_fill_probability = 0.28;
        c.gateway_quote_cross_fill_probability = 0.10;
        c.gateway_quote_passive_fill_probability = 0.015;
        c.gateway_partial_fill_probability = 0.70;
        c.gateway_reject_probability = 0.005;
        c.gateway_max_fill_size = 1;
        c.gateway_quote_near_touch_ticks = 0.3;
    } else if (profile == "fast") {
        c.tick_interval_ms = 70;
        c.future_wave_bps = 24.0;
        c.future_noise_bps = 7.0;
        c.option_spread_bps = 34.0;
        c.gateway_ack_latency_ms = 10;
        c.gateway_cancel_latency_ms = 15;
        c.gateway_fill_interval_ms = 80;
        c.gateway_order_fill_probability = 0.65;
        c.gateway_quote_cross_fill_probability = 0.26;
        c.gateway_quote_passive_fill_probability = 0.05;
        c.gateway_partial_fill_probability = 0.55;
        c.gateway_reject_probability = 0.015;
        c.gateway_max_fill_size = 3;
        c.gateway_quote_near_touch_ticks = 0.8;
    }

    c.random_seed        = get<uint32_t>(n["random_seed"], "sim.random_seed", 42);
    c.tick_interval_ms   = get<int>(n["tick_interval_ms"], "sim.tick_interval_ms", 100);
    c.strikes_per_side   = get<int>(n["strikes_per_side"], "sim.strikes_per_side", 4);
    c.expiry_count       = get<int>(n["expiry_count"], "sim.expiry_count", 2);
    c.future_wave_bps    = get<double>(n["future_wave_bps"], "sim.future_wave_bps", 12.0);
    c.future_noise_bps   = get<double>(n["future_noise_bps"], "sim.future_noise_bps", 4.0);
    c.option_spread_bps  = get<double>(n["option_spread_bps"], "sim.option_spread_bps", 30.0);
    c.top_level_volume   = get<int>(n["top_level_volume"], "sim.top_level_volume", 20);
    c.au_reference_price = get<double>(n["au_reference_price"], "sim.au_reference_price", 580.0);
    c.ag_reference_price = get<double>(n["ag_reference_price"], "sim.ag_reference_price", 7800.0);
    c.gateway_ack_latency_ms = get<int>(n["gateway_ack_latency_ms"], "sim.gateway_ack_latency_ms", 0);
    c.gateway_cancel_latency_ms = get<int>(n["gateway_cancel_latency_ms"], "sim.gateway_cancel_latency_ms", 0);
    c.gateway_fill_interval_ms = get<int>(n["gateway_fill_interval_ms"], "sim.gateway_fill_interval_ms", 25);
    c.gateway_order_fill_probability = get<double>(n["gateway_order_fill_probability"],
                                                   "sim.gateway_order_fill_probability", 1.0);
    c.gateway_quote_cross_fill_probability = get<double>(n["gateway_quote_cross_fill_probability"],
                                                         "sim.gateway_quote_cross_fill_probability", 1.0);
    c.gateway_quote_passive_fill_probability = get<double>(n["gateway_quote_passive_fill_probability"],
                                                           "sim.gateway_quote_passive_fill_probability", 0.0);
    c.gateway_partial_fill_probability = get<double>(n["gateway_partial_fill_probability"],
                                                     "sim.gateway_partial_fill_probability", 0.0);
    c.gateway_reject_probability = get<double>(n["gateway_reject_probability"],
                                               "sim.gateway_reject_probability", 0.0);
    c.gateway_max_fill_size = get<int>(n["gateway_max_fill_size"], "sim.gateway_max_fill_size", 0);
    c.gateway_slippage_ticks = get<int>(n["gateway_slippage_ticks"], "sim.gateway_slippage_ticks", 0);
    c.gateway_quote_near_touch_ticks = get<double>(n["gateway_quote_near_touch_ticks"],
                                                   "sim.gateway_quote_near_touch_ticks", 0.5);
    return c;
}

static PricingConfig parse_pricing(const YAML::Node& n) {
    PricingConfig c;
    if (!n) return c;
    c.risk_free_rate      = get<double>(n["risk_free_rate"], "pricing.risk_free_rate", 0.025);
    c.fit_interval_seconds = get<int>(n["vol_surface"]["fit_interval_seconds"],
                                      "pricing.vol_surface.fit_interval_seconds", 60);
    c.sabr_beta           = get<double>(n["sabr_beta"], "pricing.sabr_beta", 1.0);
    c.signal_emit_price_epsilon_ticks = get<double>(
        n["signal_emit_price_epsilon_ticks"],
        "pricing.signal_emit_price_epsilon_ticks",
        0.0);
    c.signal_emit_underlying_epsilon_ticks = get<double>(
        n["signal_emit_underlying_epsilon_ticks"],
        "pricing.signal_emit_underlying_epsilon_ticks",
        0.0);
    c.signal_emit_delta_epsilon = get<double>(
        n["signal_emit_delta_epsilon"],
        "pricing.signal_emit_delta_epsilon",
        0.0);
    c.signal_emit_vega_epsilon = get<double>(
        n["signal_emit_vega_epsilon"],
        "pricing.signal_emit_vega_epsilon",
        0.0);

    std::string method = get<std::string>(n["vol_surface"]["method"],
                                          "pricing.vol_surface.method", "svi");
    if      (method == "svi")          c.vol_method = VolMethod::SVI;
    else if (method == "sabr")         c.vol_method = VolMethod::SABR;
    else if (method == "cubic_spline") c.vol_method = VolMethod::CubicSpline;
    else if (method == "wing")         c.vol_method = VolMethod::Wing;
    else if (method == "orcWing")      c.vol_method = VolMethod::OrcWing;
    else throw std::runtime_error("config: pricing.vol_surface.method must be svi/sabr/cubic_spline/wing/orcWing");
    return c;
}

static RiskConfig parse_risk(const YAML::Node& n) {
    RiskConfig c;
    if (!n) return c;
    if (auto h = n["hard"]) {
        c.hard.max_volume_per_order = get<int>(h["max_volume_per_order"],
                                               "risk.hard.max_volume_per_order", 100);
    }
    if (auto s = n["soft"]) {
        c.soft.max_net_position = get<int>(s["max_net_position"], "risk.soft.max_net_position", 500);
        c.soft.max_delta        = get<double>(s["max_delta"], "risk.soft.max_delta", 1000.0);
        c.soft.max_gamma        = get<double>(s["max_gamma"], "risk.soft.max_gamma", 500.0);
        c.soft.max_vega         = get<double>(s["max_vega"],  "risk.soft.max_vega",  10000.0);
    }
    return c;
}

static MMParamsConfig parse_mm_params(const YAML::Node& n, std::string path_prefix) {
    MMParamsConfig p;
    if (!n) return p;
    p.bid_spread             = get<double>(n["bid_spread"], (path_prefix+".bid_spread").c_str(), 0.5);
    p.ask_spread             = get<double>(n["ask_spread"], (path_prefix+".ask_spread").c_str(), 0.5);
    p.quote_volume           = get<int>(n["quote_volume"], (path_prefix+".quote_volume").c_str(), 10);
    p.product_delta_threshold = get<double>(n["product_delta_threshold"],
                                            (path_prefix+".product_delta_threshold").c_str(), 50.0);
    p.product_vega_threshold = get<double>(n["product_vega_threshold"],
                                           (path_prefix+".product_vega_threshold").c_str(), 1000.0);
    p.min_quote_interval_ms  = get<double>(n["min_quote_interval_ms"],
                                           (path_prefix+".min_quote_interval_ms").c_str(), 100.0);
    p.max_position           = get<int>(n["max_position"], (path_prefix+".max_position").c_str(), 500);
    p.warning_position       = get<int>(n["warning_position"], (path_prefix+".warning_position").c_str(),
                                        std::max(1, p.max_position / 2));
    p.base_half_spread_ticks = get<double>(n["base_half_spread_ticks"],
                                           (path_prefix+".base_half_spread_ticks").c_str(),
                                           std::max(0.5, 0.5 * (p.bid_spread + p.ask_spread) * 0.5));
    p.min_half_spread_ticks  = get<double>(n["min_half_spread_ticks"],
                                           (path_prefix+".min_half_spread_ticks").c_str(),
                                           p.base_half_spread_ticks);
    p.max_half_spread_ticks  = get<double>(n["max_half_spread_ticks"],
                                           (path_prefix+".max_half_spread_ticks").c_str(),
                                           std::max(p.min_half_spread_ticks, p.base_half_spread_ticks * 4.0));
    p.inventory_skew_per_lot_ticks = get<double>(n["inventory_skew_per_lot_ticks"],
                                                 (path_prefix+".inventory_skew_per_lot_ticks").c_str(),
                                                 0.01);
    p.follow_weight          = get<double>(n["follow_weight"], (path_prefix+".follow_weight").c_str(), 0.35);
    p.requote_price_epsilon_ticks = get<double>(n["requote_price_epsilon_ticks"],
                                                (path_prefix+".requote_price_epsilon_ticks").c_str(), 1.0);
    p.market_width_widen_threshold_ticks = get<double>(n["market_width_widen_threshold_ticks"],
                                                       (path_prefix+".market_width_widen_threshold_ticks").c_str(),
                                                       6.0);
    p.underlying_move_widen_threshold_ticks = get<double>(n["underlying_move_widen_threshold_ticks"],
                                                          (path_prefix+".underlying_move_widen_threshold_ticks").c_str(),
                                                          2.0);
    p.use_one_sided_at_limits = get<bool>(n["use_one_sided_at_limits"],
                                          (path_prefix+".use_one_sided_at_limits").c_str(), true);
    p.enabled                = get<bool>(n["enabled"], (path_prefix+".enabled").c_str(), true);
    return p;
}

static ArbitrageStrategyType parse_arb_strategy_type(const YAML::Node& n,
                                                     const char* path) {
    const std::string type = get<std::string>(n, path, "");
    if (type == "pcp" || type == "PCP") return ArbitrageStrategyType::PCP;
    if (type.empty()) return ArbitrageStrategyType::None;
    throw std::runtime_error(
        std::string("config: ") + path + " must be 'pcp', got: " + type);
}

static ArbParamsConfig parse_arb_params(const YAML::Node& n, std::string path_prefix) {
    ArbParamsConfig p;
    if (!n) return p;
    p.min_edge_ticks = get<double>(n["min_edge_ticks"],
                                   (path_prefix + ".min_edge_ticks").c_str(),
                                   2.0);
    p.cooldown_ms = get<double>(n["cooldown_ms"],
                                (path_prefix + ".cooldown_ms").c_str(),
                                25.0);
    p.scan_interval_ms = get<double>(n["scan_interval_ms"],
                                     (path_prefix + ".scan_interval_ms").c_str(),
                                     1.0);
    p.cleanup_timeout_ms = get<double>(n["cleanup_timeout_ms"],
                                       (path_prefix + ".cleanup_timeout_ms").c_str(),
                                       25.0);
    p.max_order_volume = get<int>(n["max_order_volume"],
                                  (path_prefix + ".max_order_volume").c_str(),
                                  1);
    p.max_live_orders = get<int>(n["max_live_orders"],
                                 (path_prefix + ".max_live_orders").c_str(),
                                 8);
    p.cleanup_on_partial = get<bool>(n["cleanup_on_partial"],
                                     (path_prefix + ".cleanup_on_partial").c_str(),
                                     true);
    p.enabled = get<bool>(n["enabled"],
                          (path_prefix + ".enabled").c_str(),
                          false);
    return p;
}

static TimerConfig parse_timer(const YAML::Node& n) {
    TimerConfig c;
    if (!n) return c;
    c.hedge_check_interval_ms   = get<int>(n["hedge_check_interval_ms"],
                                            "timer.hedge_check_interval_ms", 1000);
    c.quote_refresh_interval_ms = get<int>(n["quote_refresh_interval_ms"],
                                            "timer.quote_refresh_interval_ms", 500);
    if (auto sched = n["session_schedule"]) {
        for (auto sess : sched) {
            if (c.session_count >= 4)
                throw std::runtime_error("config: too many session windows (max 4)");
            auto& sw = c.sessions[c.session_count++];
            str_copy(sw.open_time,  sizeof(sw.open_time),  sess["open"],  "timer.session_schedule.open");
            str_copy(sw.close_time, sizeof(sw.close_time), sess["close"], "timer.session_schedule.close");
        }
    }
    return c;
}

static ThreadAffinityConfig parse_affinity(const YAML::Node& n) {
    ThreadAffinityConfig c;
    if (!n) return c;
    c.feed_core               = get<int>(n["feed_core"],               "affinity.feed_core", 2);
    c.pricer_core             = get<int>(n["pricer_core"],             "affinity.pricer_core", 3);
    c.gateway_dispatcher_core = get<int>(n["gateway_dispatcher_core"], "affinity.gateway_dispatcher_core", 12);
    c.vol_fitter_core         = get<int>(n["vol_fitter_core"],         "affinity.vol_fitter_core", 13);
    c.risk_monitor_core       = get<int>(n["risk_monitor_core"],       "affinity.risk_monitor_core", 14);
    c.timer_core              = get<int>(n["timer_core"],              "affinity.timer_core", 14);
    c.grpc_server_core        = get<int>(n["grpc_server_core"],        "affinity.grpc_server_core", 15);
    return c;
}

static MonitoringConfig parse_monitoring(const YAML::Node& n) {
    MonitoringConfig c;
    if (!n) return c;

    str_copy(c.grpc_listen_addr,
             sizeof(c.grpc_listen_addr),
             n["grpc_listen_addr"], "monitoring.grpc_listen_addr", "0.0.0.0:50051");

    const std::string mode = get<std::string>(
        n["hot_path_publish_mode"], "monitoring.hot_path_publish_mode", "full");
    if (mode == "full") {
        c.hot_path_publish_mode = MonitoringPublishMode::Full;
    } else if (mode == "deferred") {
        c.hot_path_publish_mode = MonitoringPublishMode::Deferred;
    } else if (mode == "off") {
        c.hot_path_publish_mode = MonitoringPublishMode::Off;
    } else {
        throw std::runtime_error(
            "config: monitoring.hot_path_publish_mode must be full/deferred/off");
    }
    return c;
}

// ─── Main loader ──────────────────────────────────────────────────────────────
SystemConfig load_config(std::string_view path) {
    YAML::Node root;
    try {
        root = YAML::LoadFile(std::string(path));
    } catch (const YAML::Exception& e) {
        throw std::runtime_error(std::string("config: failed to load '")
                                 + std::string(path) + "': " + e.what());
    }

    SystemConfig cfg;
    cfg.instance  = parse_instance(root["instance"]);
    cfg.feed      = parse_feed(root["feed"]);
    cfg.gateway   = parse_gateway(root["gateway"]);
    cfg.sim       = parse_sim(root["sim"]);
    cfg.pricing   = parse_pricing(root["pricing"]);
    cfg.risk      = parse_risk(root["risk"]);
    cfg.timer     = parse_timer(root["timer"]);
    cfg.monitoring = parse_monitoring(root["monitoring"]);
    cfg.affinity  = parse_affinity(root["thread_affinity"]);

    // Products (underlying option series)
    auto products_node = root["products"];
    if (!products_node || !products_node.IsSequence())
        throw std::runtime_error("config: 'products' must be a non-empty sequence");

    cfg.product_count = 0;
    for (auto pn : products_node) {
        if (cfg.product_count >= MAX_PRODUCTS)
            throw std::runtime_error("config: too many products (max 32)");

        ProductConfig& p = cfg.products[cfg.product_count];
        str_copy(p.underlying_id.data, sizeof(p.underlying_id.data),
                 pn["underlying_id"], "products[].underlying_id");
        str_copy(p.exchange_id.data, sizeof(p.exchange_id.data),
                 pn["exchange_id"], "products[].exchange_id");
        p.strategy_core = get<int>(pn["strategy_core"], "products[].strategy_core", -1);
        if (p.strategy_core < 0)
            throw std::runtime_error("config: products[].strategy_core must be specified");
        p.arbitrage_core = get<int>(pn["arbitrage_core"], "products[].arbitrage_core", -1);

        str_copy(p.strategy_type, sizeof(p.strategy_type),
                 pn["strategy_type"], "products[].strategy_type", "simple_mm");
        p.params = parse_mm_params(pn["params"],
                                   "products[" + std::to_string(cfg.product_count) + "].params");
        p.arbitrage_strategy_count = 0;
        if (auto arb_node = pn["arbitrage_strategies"]) {
            if (!arb_node.IsSequence()) {
                throw std::runtime_error("config: products[].arbitrage_strategies must be a sequence");
            }
            for (auto an : arb_node) {
                if (p.arbitrage_strategy_count >= MAX_ARBITRAGE_STRATEGIES_PER_PRODUCT) {
                    throw std::runtime_error("config: too many arbitrage strategies per product");
                }
                ArbitrageStrategyConfig& arb =
                    p.arbitrage_strategies[p.arbitrage_strategy_count];
                const std::string base =
                    "products[" + std::to_string(cfg.product_count)
                    + "].arbitrage_strategies[" + std::to_string(p.arbitrage_strategy_count) + "]";
                arb.type = parse_arb_strategy_type(an["type"], (base + ".type").c_str());
                arb.params = parse_arb_params(an["params"], base + ".params");
                if (arb.type == ArbitrageStrategyType::None) {
                    throw std::runtime_error("config: arbitrage strategy type must be specified");
                }
                ++p.arbitrage_strategy_count;
            }
        }
        ++cfg.product_count;
    }

    if (cfg.product_count == 0)
        throw std::runtime_error("config: at least one product must be configured");

    return cfg;
}

} // namespace omm
