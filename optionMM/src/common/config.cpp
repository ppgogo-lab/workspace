#include "common/config.h"

#include <yaml-cpp/yaml.h>
#include <cstring>
#include <stdexcept>
#include <string>

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
    else throw std::runtime_error("config: feed.type must be 'multicast' or 'fpga', got: " + type);

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
    return c;
}

static GatewayConfig parse_gateway(const YAML::Node& n) {
    require(n, "gateway");
    GatewayConfig c;
    std::string type = get<std::string>(n["type"], "gateway.type", "ctp");
    if (type == "ctp")       c.type = GatewayType::CTP;
    else if (type == "femas") c.type = GatewayType::FEMAS;
    else throw std::runtime_error("config: gateway.type must be 'ctp' or 'femas', got: " + type);

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

static PricingConfig parse_pricing(const YAML::Node& n) {
    PricingConfig c;
    if (!n) return c;
    c.risk_free_rate      = get<double>(n["risk_free_rate"], "pricing.risk_free_rate", 0.025);
    c.fit_interval_seconds = get<int>(n["vol_surface"]["fit_interval_seconds"],
                                      "pricing.vol_surface.fit_interval_seconds", 60);
    c.sabr_beta           = get<double>(n["sabr_beta"], "pricing.sabr_beta", 1.0);

    std::string method = get<std::string>(n["vol_surface"]["method"],
                                          "pricing.vol_surface.method", "svi");
    if      (method == "svi")          c.vol_method = VolMethod::SVI;
    else if (method == "sabr")         c.vol_method = VolMethod::SABR;
    else if (method == "cubic_spline") c.vol_method = VolMethod::CubicSpline;
    else if (method == "wing")         c.vol_method = VolMethod::Wing;
    else throw std::runtime_error("config: pricing.vol_surface.method must be svi/sabr/cubic_spline/wing");
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
    p.hedge_delta_threshold  = get<double>(n["hedge_delta_threshold"],
                                           (path_prefix+".hedge_delta_threshold").c_str(), 50.0);
    p.min_quote_interval_ms  = get<double>(n["min_quote_interval_ms"],
                                           (path_prefix+".min_quote_interval_ms").c_str(), 100.0);
    p.max_position           = get<int>(n["max_position"], (path_prefix+".max_position").c_str(), 500);
    p.enabled                = get<bool>(n["enabled"], (path_prefix+".enabled").c_str(), true);
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
    cfg.pricing   = parse_pricing(root["pricing"]);
    cfg.risk      = parse_risk(root["risk"]);
    cfg.timer     = parse_timer(root["timer"]);
    cfg.monitoring.grpc_listen_addr[0] = '\0';
    if (auto mon = root["monitoring"])
        str_copy(cfg.monitoring.grpc_listen_addr,
                 sizeof(cfg.monitoring.grpc_listen_addr),
                 mon["grpc_listen_addr"], "monitoring.grpc_listen_addr", "0.0.0.0:50051");
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

        str_copy(p.strategy_type, sizeof(p.strategy_type),
                 pn["strategy_type"], "products[].strategy_type", "simple_mm");
        p.params = parse_mm_params(pn["params"],
                                   "products[" + std::to_string(cfg.product_count) + "].params");
        ++cfg.product_count;
    }

    if (cfg.product_count == 0)
        throw std::runtime_error("config: at least one product must be configured");

    return cfg;
}

} // namespace omm
