#include <gtest/gtest.h>

#include "common/config.h"
#include "common/thread_utils.h"
#include "common/types.h"
#include "engine/trading_engine.h"
#include "gateway/sim_gateway.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace omm;

#if defined(__SANITIZE_ADDRESS__)
#  define RUNNING_ASAN 1
#elif defined(__has_feature)
#  if __has_feature(address_sanitizer)
#    define RUNNING_ASAN 1
#  endif
#endif

namespace {

const char* exchange_name(Exchange exchange) {
    switch (exchange) {
    case Exchange::SHFE: return "SHFE";
    case Exchange::DCE: return "DCE";
    case Exchange::CZCE: return "CZCE";
    case Exchange::CFFEX: return "CFFEX";
    case Exchange::GFEX: return "GFEX";
    default: return "UNKNOWN";
    }
}

const char* monitoring_mode_name(MonitoringPublishMode mode) {
    switch (mode) {
    case MonitoringPublishMode::Full: return "full";
    case MonitoringPublishMode::Deferred: return "deferred";
    case MonitoringPublishMode::Off: return "off";
    default: return "unknown";
    }
}

const char* hot_path_greeks_mode_name(HotPathGreeksMode mode) {
    switch (mode) {
    case HotPathGreeksMode::Full: return "full";
    case HotPathGreeksMode::Compact: return "compact";
    case HotPathGreeksMode::Off: return "off";
    default: return "unknown";
    }
}

MonitoringPublishMode monitoring_mode_from_env() {
    const char* value = std::getenv("OMM_LATENCY_MONITORING");
    if (value == nullptr || value[0] == '\0') return MonitoringPublishMode::Deferred;
    if (std::strcmp(value, "off") == 0) return MonitoringPublishMode::Off;
    if (std::strcmp(value, "deferred") == 0) return MonitoringPublishMode::Deferred;
    if (std::strcmp(value, "full") == 0) return MonitoringPublishMode::Full;
    return MonitoringPublishMode::Deferred;
}

HotPathGreeksMode hot_path_greeks_mode_from_env() {
    const char* value = std::getenv("OMM_LATENCY_GREEKS");
    if (value == nullptr || value[0] == '\0') return HotPathGreeksMode::Compact;
    if (std::strcmp(value, "off") == 0) return HotPathGreeksMode::Off;
    if (std::strcmp(value, "compact") == 0) return HotPathGreeksMode::Compact;
    if (std::strcmp(value, "full") == 0) return HotPathGreeksMode::Full;
    return HotPathGreeksMode::Compact;
}

std::vector<int> parse_core_list(const char* value) {
    std::vector<int> cores;
    if (value == nullptr || value[0] == '\0') return cores;
    std::stringstream ss(value);
    std::string token;
    while (std::getline(ss, token, ',')) {
        if (token.empty()) continue;
        cores.push_back(std::atoi(token.c_str()));
    }
    return cores;
}

std::vector<int> latency_core_list_from_env() {
    const char* value = std::getenv("OMM_LATENCY_PIN_CORES");
    const unsigned hw_threads = std::thread::hardware_concurrency();
    if (value != nullptr && std::strcmp(value, "auto") == 0) {
        std::vector<int> cores;
        const int count = std::min<unsigned>(hw_threads, 12);
        cores.reserve(static_cast<size_t>(count));
        for (int i = 0; i < count; ++i) cores.push_back(i);
        return cores;
    }
    return parse_core_list(value);
}

class LatencySimGateway : public SimGateway {
public:
    bool send_quote(const Quote& q,
                    OrderId* bid_order_id = nullptr,
                    OrderId* ask_order_id = nullptr) noexcept override {
        if (q.instrument_id < MAX_INSTRUMENTS) {
            strategy_send_ts_[q.instrument_id].store(q.send_ts, std::memory_order_relaxed);
            gateway_recv_ts_[q.instrument_id].store(get_monotonic_ns(), std::memory_order_release);
            quote_message_count_[q.instrument_id].fetch_add(1, std::memory_order_release);
            if (q.bid_volume == 0 && q.ask_volume == 0) {
                cancel_message_count_[q.instrument_id].fetch_add(1, std::memory_order_release);
            } else {
                live_quote_count_[q.instrument_id].fetch_add(1, std::memory_order_release);
            }
        }

        if (q.bid_volume == 0 && q.ask_volume == 0) {
            if (cancel_quote(q.client_quote_id, q.instrument_id)) {
                return true;
            }
        }
        return SimGateway::send_quote(q, bid_order_id, ask_order_id);
    }

    int64_t get_last_strategy_send_ts(uint16_t id) const noexcept {
        return strategy_send_ts_[id].load(std::memory_order_acquire);
    }

    int64_t get_last_gateway_recv_ts(uint16_t id) const noexcept {
        return gateway_recv_ts_[id].load(std::memory_order_acquire);
    }

    uint64_t live_quote_count(uint16_t id) const noexcept {
        return live_quote_count_[id].load(std::memory_order_acquire);
    }

    uint64_t quote_message_count(uint16_t id) const noexcept {
        return quote_message_count_[id].load(std::memory_order_acquire);
    }

    uint64_t cancel_message_count(uint16_t id) const noexcept {
        return cancel_message_count_[id].load(std::memory_order_acquire);
    }

private:
    std::atomic<int64_t> strategy_send_ts_[MAX_INSTRUMENTS]{};
    std::atomic<int64_t> gateway_recv_ts_[MAX_INSTRUMENTS]{};
    std::atomic<uint64_t> live_quote_count_[MAX_INSTRUMENTS]{};
    std::atomic<uint64_t> quote_message_count_[MAX_INSTRUMENTS]{};
    std::atomic<uint64_t> cancel_message_count_[MAX_INSTRUMENTS]{};
};

static Instrument make_future(uint16_t id,
                              const char* code,
                              uint8_t product_idx,
                              double tick_size,
                              Exchange exchange) {
    Instrument f{};
    f.instrument_id = id;
    f.underlying_id = id;
    f.product_index = product_idx;
    f.kind = InstrumentKind::Future;
    f.multiplier = 1.0;
    f.tick_size = tick_size;
    f.exchange = exchange;
    std::strncpy(f.code.data, code, sizeof(f.code.data) - 1);
    std::strncpy(f.underlying_code.data, code, sizeof(f.underlying_code.data) - 1);
    std::strncpy(f.exchange_id.data, exchange_name(exchange), sizeof(f.exchange_id.data) - 1);
    f.expiry_date = 20260428;
    f.expiry_epoch_ns = get_monotonic_ns()
                      + static_cast<int64_t>(0.25 * 365.0 * 24.0 * 3600.0 * 1e9);
    return f;
}

static Instrument make_option(uint16_t id,
                              uint16_t underlying_id,
                              const char* underlying_code,
                              uint8_t product_idx,
                              double strike,
                              OptionType otype,
                              double tick_size,
                              Exchange exchange) {
    Instrument o{};
    o.instrument_id = id;
    o.underlying_id = underlying_id;
    o.product_index = product_idx;
    o.kind = InstrumentKind::Option;
    o.option_type = otype;
    o.strike = strike;
    o.tick_size = tick_size;
    o.multiplier = 1.0;
    o.exchange = exchange;
    std::strncpy(o.underlying_code.data, underlying_code, sizeof(o.underlying_code.data) - 1);
    std::strncpy(o.exchange_id.data, exchange_name(exchange), sizeof(o.exchange_id.data) - 1);
    o.expiry_date = 20260428;
    o.expiry_epoch_ns = get_monotonic_ns()
                      + static_cast<int64_t>(0.25 * 365.0 * 24.0 * 3600.0 * 1e9);
    return o;
}

static void add_latency_calendar(SystemConfig& cfg) {
    cfg.exchange_calendar_count = 2;
    const int32_t dates[] = {20260423, 20260424, 20260425, 20260426, 20260427, 20260428};
    const bool trading[] = {true, true, false, false, true, true};
    for (int ex = 0; ex < cfg.exchange_calendar_count; ++ex) {
        cfg.exchange_calendars[ex].exchange_id = ExchangeId(ex == 0 ? "SHFE" : "GFEX");
        for (int i = 0; i < 6; ++i) {
            cfg.exchange_calendars[ex].days[i].date = dates[i];
            cfg.exchange_calendars[ex].days[i].is_trading_day = trading[i];
        }
        cfg.exchange_calendars[ex].day_count = 6;
    }

    cfg.exchange_trading_time_count = 2;
    auto set_session = [&](int ex, int idx, int8_t start_offset, const char* start,
                           int8_t end_offset, const char* end) {
        auto& s = cfg.exchange_trading_times[ex].sessions[idx];
        s.start_day_offset = start_offset;
        s.end_day_offset = end_offset;
        std::strncpy(s.start_time, start, sizeof(s.start_time) - 1);
        std::strncpy(s.end_time, end, sizeof(s.end_time) - 1);
    };
    for (int ex = 0; ex < cfg.exchange_trading_time_count; ++ex) {
        cfg.exchange_trading_times[ex].exchange_id = ExchangeId(ex == 0 ? "SHFE" : "GFEX");
        set_session(ex, 0, -1, "21:00:00", 0, "02:00:00");
        set_session(ex, 1, 0, "09:00:00", 0, "10:15:00");
        set_session(ex, 2, 0, "10:30:00", 0, "11:30:00");
        set_session(ex, 3, 0, "13:00:00", 0, "15:00:00");
        cfg.exchange_trading_times[ex].session_count = 4;
    }
}

static TopOfBookTick make_tick(uint16_t id, double last, double bid, double ask, uint64_t sequence_no = 0) {
    TopOfBookTick t{};
    t.instrument_id = id;
    t.last_price = last;
    t.bid_price[0] = bid;
    t.ask_price[0] = ask;
    t.bid_volume[0] = 50;
    t.ask_volume[0] = 50;
    t.recv_ts_ns = get_monotonic_ns();
    t.exchange_ts_ns = t.recv_ts_ns;
    t.sequence_no = sequence_no;
    return t;
}

struct OptionQuoteSeed {
    uint16_t id{INVALID_INSTRUMENT_ID};
    double strike{0.0};
    OptionType type{OptionType::Call};
    double last{0.0};
    double bid{0.0};
    double ask{0.0};
};

struct ProductScenario {
    uint8_t product_index{0};
    uint16_t future_id{INVALID_INSTRUMENT_ID};
    std::string underlying_code;
    Exchange exchange{Exchange::SHFE};
    double future_base{0.0};
    double future_swing{0.0};
    double future_tick{0.0};
    double option_tick{0.0};
    std::vector<OptionQuoteSeed> options;
};

struct ScenarioConfig {
    const char* label{"option_mm_core"};
    Exchange exchange{Exchange::SHFE};
    int product_count{1};
    int options_per_product{16};
    int iterations{200};
    int64_t timeout_ns{5'000'000LL};
    MonitoringPublishMode monitoring_mode{MonitoringPublishMode::Deferred};
    HotPathGreeksMode hot_path_greeks_mode{HotPathGreeksMode::Compact};
    int gateway_cancel_latency_ms{0};
    double signal_emit_price_epsilon_ticks{0.0};
    double signal_emit_underlying_epsilon_ticks{0.0};
    double signal_emit_delta_epsilon{0.0};
    double signal_emit_vega_epsilon{0.0};
};

struct ScenarioResult {
    std::vector<int64_t> tick_to_gateway_ns;
    std::vector<int64_t> tick_to_signal_emit_ns;
    std::vector<int64_t> signal_emit_to_strategy_ns;
    std::vector<int64_t> strategy_to_quote_send_ns;
    std::vector<int64_t> quote_send_to_gateway_ns;
    std::vector<int64_t> quote_ack_route_latency_ns;
    std::vector<int64_t> quote_cancel_route_latency_ns;
    std::vector<size_t> quotes_by_product;
    std::vector<StrategyRuntimeStats> strategy_stats;
    size_t expected_total_quotes{0};
    uint64_t coalesced_signal_writes{0};
    uint64_t coalesced_signal_overwrites{0};
    uint64_t coalesced_timer_writes{0};
    uint64_t coalesced_timer_overwrites{0};
    uint64_t signal_emits{0};
    uint64_t signal_suppressed{0};
    uint64_t pending_future_tick_overwrites{0};
    uint32_t max_signal_queue_depth{0};
    uint32_t max_signal_mailbox_depth{0};
    uint32_t max_timer_queue_depth{0};
};

static int64_t percentile_sorted(const std::vector<int64_t>& sorted, double p) {
    if (sorted.empty()) return 0;
    const size_t idx = static_cast<size_t>(p * static_cast<double>(sorted.size() - 1));
    return sorted[idx];
}

static void print_series(const char* label, std::vector<int64_t> values) {
    if (values.empty()) return;
    std::sort(values.begin(), values.end());
    std::cout << "[" << label << "] min=" << values.front()
              << " ns p50=" << percentile_sorted(values, 0.50)
              << " ns p95=" << percentile_sorted(values, 0.95)
              << " ns p99=" << percentile_sorted(values, 0.99)
              << " ns p99.9=" << percentile_sorted(values, 0.999)
              << " ns max=" << values.back()
              << " ns count=" << values.size() << "\n";
}

static void expect_stage_p99_under(const char* label,
                                   std::vector<int64_t> values,
                                   int64_t budget_ns,
                                   size_t min_samples) {
    ASSERT_GE(values.size(), min_samples) << label << " did not capture enough samples";
    std::sort(values.begin(), values.end());
    EXPECT_GE(values.front(), 0LL) << label << " produced negative latency";
    EXPECT_LT(percentile_sorted(values, 0.99), budget_ns)
        << label << " p99 exceeded " << budget_ns << " ns";
}

static ProductScenario build_product(uint8_t product_idx,
                                     uint16_t* next_id,
                                     const ScenarioConfig& cfg) {
    ProductScenario product{};
    product.product_index = product_idx;
    product.future_id = (*next_id)++;
    product.exchange = cfg.exchange;

    if ((product_idx & 1u) == 0u) {
        product.underlying_code = "cu2501";
        product.future_base = 75000.0;
        product.future_swing = 40.0;
        product.future_tick = 10.0;
        product.option_tick = 2.0;
    } else {
        product.underlying_code = "rb2501";
        product.future_base = 3500.0;
        product.future_swing = 4.0;
        product.future_tick = 1.0;
        product.option_tick = 0.5;
    }

    const int half = std::max(2, cfg.options_per_product / 2);
    const double premium_base = product.future_base > 10000.0 ? 900.0 : 80.0;
    const double premium_slope = product.future_base > 10000.0 ? 70.0 : 6.0;
    const int center = half / 2;

    auto add_option = [&](OptionType type, int i) {
        const double strike_offset = static_cast<double>(i - center);
        const double strike = product.future_base + strike_offset * product.future_tick * 8.0;
        const double mid = std::max(product.option_tick * 4.0,
                                    premium_base - premium_slope * std::fabs(strike_offset));
        OptionQuoteSeed seed{};
        seed.id = (*next_id)++;
        seed.strike = strike;
        seed.type = type;
        seed.last = mid;
        seed.bid = std::max(product.option_tick, mid - product.option_tick);
        seed.ask = std::max(seed.bid + product.option_tick, mid + product.option_tick);
        product.options.push_back(seed);
    };

    for (int i = 0; i < half; ++i) add_option(OptionType::Call, i);
    for (int i = 0; i < half; ++i) add_option(OptionType::Put, i);
    return product;
}

static void configure_option_mm_core_product(ProductConfig* cfg_product,
                                             const ProductScenario& product) {
    std::strncpy(cfg_product->underlying_id.data,
                 product.underlying_code.c_str(),
                 sizeof(cfg_product->underlying_id.data) - 1);
    std::strncpy(cfg_product->exchange_id.data,
                 exchange_name(product.exchange),
                 sizeof(cfg_product->exchange_id.data) - 1);
    std::strncpy(cfg_product->strategy_type,
                 "option_mm_core",
                 sizeof(cfg_product->strategy_type) - 1);
    cfg_product->strategy_core = -1;
    cfg_product->params.bid_spread = 0.5;
    cfg_product->params.ask_spread = 0.5;
    cfg_product->params.quote_volume = 5;
    cfg_product->params.product_delta_threshold = 1'000'000.0;
    cfg_product->params.product_vega_threshold = 1'000'000.0;
    cfg_product->params.min_quote_interval_ms = 0.0;
    cfg_product->params.max_position = 5000;
    cfg_product->params.warning_position = 2500;
    cfg_product->params.base_half_spread_ticks = 1.0;
    cfg_product->params.min_half_spread_ticks = 1.0;
    cfg_product->params.max_half_spread_ticks = 8.0;
    cfg_product->params.inventory_skew_per_lot_ticks = 0.01;
    cfg_product->params.follow_weight = 0.35;
    cfg_product->params.requote_price_epsilon_ticks = 0.0;
    cfg_product->params.market_width_widen_threshold_ticks = 6.0;
    cfg_product->params.underlying_move_widen_threshold_ticks = 0.0;
    cfg_product->params.use_one_sided_at_limits = true;
    cfg_product->params.enabled = true;
}

static void push_option_market_ticks(TradingEngine* engine, const ProductScenario& product) {
    for (const auto& seed : product.options) {
        TopOfBookTick tick = make_tick(seed.id, seed.last, seed.bid, seed.ask);
        while (!engine->tick_buf().try_push(tick)) spin_pause();
    }
}

static int64_t push_future_tick(TradingEngine* engine,
                                const ProductScenario& product,
                                double future_price,
                                uint64_t sequence_no) {
    TopOfBookTick tick{};
    tick.instrument_id = product.future_id;
    tick.last_price = future_price;
    tick.bid_price[0] = future_price - product.future_tick;
    tick.ask_price[0] = future_price + product.future_tick;
    tick.bid_volume[0] = 200;
    tick.ask_volume[0] = 200;
    tick.exchange_ts_ns = get_monotonic_ns();
    tick.recv_ts_ns = tick.exchange_ts_ns;
    tick.sequence_no = sequence_no;
    while (!engine->tick_buf().try_push(tick)) spin_pause();
    return tick.recv_ts_ns;
}

static ScenarioResult run_latency_scenario(const ScenarioConfig& cfg) {
    SystemConfig sys{};
    sys.product_count = cfg.product_count;
    sys.pricing.risk_free_rate = 0.025;
    sys.pricing.fit_interval_seconds = 60;
    sys.pricing.signal_emit_price_epsilon_ticks = cfg.signal_emit_price_epsilon_ticks;
    sys.pricing.signal_emit_underlying_epsilon_ticks = cfg.signal_emit_underlying_epsilon_ticks;
    sys.pricing.signal_emit_delta_epsilon = cfg.signal_emit_delta_epsilon;
    sys.pricing.signal_emit_vega_epsilon = cfg.signal_emit_vega_epsilon;
    sys.risk.hard.max_volume_per_order = 1000;
    sys.risk.soft.max_net_position = 50000;
    sys.risk.soft.max_delta = 1'000'000.0;
    sys.risk.soft.max_gamma = 1'000'000.0;
    sys.risk.soft.max_vega = 1'000'000.0;
    sys.timer.hedge_check_interval_ms = 60000;
    sys.timer.quote_refresh_interval_ms = 60000;
    sys.monitoring.hot_path_publish_mode = cfg.monitoring_mode;
    sys.pricing.hot_path_greeks_mode = cfg.hot_path_greeks_mode;
    sys.execution.low_latency_mode =
        cfg.monitoring_mode == MonitoringPublishMode::Off
        && cfg.hot_path_greeks_mode != HotPathGreeksMode::Full;
    sys.scheduling.low_latency_spin = true;
    sys.affinity.feed_core = -1;
    sys.affinity.pricer_core = -1;
    sys.affinity.gateway_dispatcher_core = -1;
    sys.affinity.vol_fitter_core = -1;
    sys.affinity.risk_monitor_core = -1;
    sys.affinity.timer_core = -1;
    sys.affinity.grpc_server_core = -1;

    const std::vector<int> latency_cores = latency_core_list_from_env();
    if (latency_cores.size() >= static_cast<size_t>(7 + cfg.product_count)) {
        sys.affinity.feed_core = latency_cores[0];
        sys.affinity.pricer_core = latency_cores[1];
        sys.affinity.gateway_dispatcher_core = latency_cores[2];
        sys.affinity.vol_fitter_core = latency_cores[3];
        sys.affinity.risk_monitor_core = latency_cores[4];
        sys.affinity.timer_core = latency_cores[5];
        sys.affinity.grpc_server_core = latency_cores[6];
    }

    std::vector<ProductScenario> products;
    products.reserve(cfg.product_count);
    uint16_t next_id = 0;
    for (int p = 0; p < cfg.product_count; ++p) {
        products.push_back(build_product(static_cast<uint8_t>(p), &next_id, cfg));
        configure_option_mm_core_product(&sys.products[p], products.back());
        if (latency_cores.size() >= static_cast<size_t>(7 + cfg.product_count)) {
            sys.products[p].strategy_core = latency_cores[7 + p];
        }
    }
    add_latency_calendar(sys);

    auto gw = std::make_unique<LatencySimGateway>();
    auto* lat_gw = gw.get();

    SimConfig sim{};
    sim.gateway_ack_latency_ms = 0;
    sim.gateway_cancel_latency_ms = cfg.gateway_cancel_latency_ms;
    sim.gateway_fill_interval_ms = 1;
    sim.gateway_order_fill_probability = 0.0;
    sim.gateway_quote_cross_fill_probability = 0.0;
    sim.gateway_quote_passive_fill_probability = 0.0;
    sim.gateway_partial_fill_probability = 0.0;
    sim.gateway_reject_probability = 0.0;
    gw->set_sim_config(sim);

    for (const auto& product : products) {
        gw->add_instrument(make_future(product.future_id,
                                       product.underlying_code.c_str(),
                                       product.product_index,
                                       product.future_tick,
                                       product.exchange));
        gw->set_last_price(product.future_id, product.future_base);

        for (const auto& seed : product.options) {
            gw->add_instrument(make_option(seed.id,
                                           product.future_id,
                                           product.underlying_code.c_str(),
                                           product.product_index,
                                           seed.strike,
                                           seed.type,
                                           product.option_tick,
                                           product.exchange));
            gw->set_last_price(seed.id, seed.last);
        }
    }

    gw->connect(sys.gateway);

    auto engine = std::make_unique<TradingEngine>(sys, std::move(gw), nullptr);
    engine->start();

    uint64_t sequence_no = 1;
    for (const auto& product : products) {
        push_option_market_ticks(engine.get(), product);
        (void)push_future_tick(engine.get(), product, product.future_base, sequence_no++);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(750));

    std::array<int64_t, MAX_INSTRUMENTS> seen_ack_route_ts{};
    std::array<int64_t, MAX_INSTRUMENTS> seen_cancel_route_ts{};
    for (const auto& product : products) {
        for (const auto& seed : product.options) {
            seen_ack_route_ts[seed.id] = engine->last_quote_ack_route_ts(seed.id);
            seen_cancel_route_ts[seed.id] = engine->last_quote_cancel_route_ts(seed.id);
        }
    }

    ScenarioResult result{};
    result.expected_total_quotes =
        static_cast<size_t>(cfg.iterations) * static_cast<size_t>(cfg.options_per_product);
    result.quotes_by_product.assign(products.size(), 0);
    result.strategy_stats.assign(products.size(), StrategyRuntimeStats{});
    result.tick_to_gateway_ns.reserve(result.expected_total_quotes / 4);
    result.tick_to_signal_emit_ns.reserve(result.expected_total_quotes / 4);
    result.signal_emit_to_strategy_ns.reserve(result.expected_total_quotes / 4);
    result.strategy_to_quote_send_ns.reserve(result.expected_total_quotes / 4);
    result.quote_send_to_gateway_ns.reserve(result.expected_total_quotes / 4);
    result.quote_ack_route_latency_ns.reserve(result.expected_total_quotes / 4);
    result.quote_cancel_route_latency_ns.reserve(result.expected_total_quotes / 8);

    auto drain_callback_latencies = [&]() {
        for (const auto& product : products) {
            for (const auto& seed : product.options) {
                const int64_t ack_route_ts = engine->last_quote_ack_route_ts(seed.id);
                if (ack_route_ts > seen_ack_route_ts[seed.id]) {
                    seen_ack_route_ts[seed.id] = ack_route_ts;
                    const int64_t latency = engine->last_quote_ack_route_latency_ns(seed.id);
                    if (latency > 0) result.quote_ack_route_latency_ns.push_back(latency);
                }

                const int64_t cancel_route_ts = engine->last_quote_cancel_route_ts(seed.id);
                if (cancel_route_ts > seen_cancel_route_ts[seed.id]) {
                    seen_cancel_route_ts[seed.id] = cancel_route_ts;
                    const int64_t latency = engine->last_quote_cancel_route_latency_ns(seed.id);
                    if (latency > 0) result.quote_cancel_route_latency_ns.push_back(latency);
                }
            }
        }
    };

    for (int i = 0; i < cfg.iterations; ++i) {
        const ProductScenario& product = products[static_cast<size_t>(i) % products.size()];
        push_option_market_ticks(engine.get(), product);

        std::vector<uint64_t> base_live_counts(product.options.size(), 0);
        for (size_t oi = 0; oi < product.options.size(); ++oi) {
            const auto& seed = product.options[oi];
            base_live_counts[oi] = lat_gw->live_quote_count(seed.id);
        }

        const int product_iter = i / std::max(1, cfg.product_count);
        const double swing = (product_iter & 1) ? product.future_swing : -product.future_swing;
        const double future_price = product.future_base + swing;
        lat_gw->set_last_price(product.future_id, future_price);
        const int64_t tick_ts = push_future_tick(engine.get(), product, future_price, sequence_no++);
        const int64_t deadline = tick_ts + cfg.timeout_ns;

        std::vector<uint8_t> seen(product.options.size(), 0);
        size_t captured = 0;

        while (get_monotonic_ns() < deadline && captured < product.options.size()) {
            drain_callback_latencies();

            for (size_t oi = 0; oi < product.options.size(); ++oi) {
                if (seen[oi]) continue;
                const auto& seed = product.options[oi];
                if (lat_gw->live_quote_count(seed.id) <= base_live_counts[oi]) continue;

                const int64_t gateway_ts = lat_gw->get_last_gateway_recv_ts(seed.id);
                const int64_t strategy_send_ts = lat_gw->get_last_strategy_send_ts(seed.id);
                const int64_t signal_emit_ts = engine->last_signal_emit_ts(seed.id);
                const int64_t strategy_signal_ts = engine->last_strategy_signal_ts(seed.id);

                if (gateway_ts > tick_ts) {
                    result.tick_to_gateway_ns.push_back(gateway_ts - tick_ts);
                    result.quotes_by_product[product.product_index]++;
                }
                if (signal_emit_ts >= tick_ts && signal_emit_ts <= gateway_ts) {
                    result.tick_to_signal_emit_ns.push_back(signal_emit_ts - tick_ts);
                }
                if (strategy_signal_ts >= signal_emit_ts && signal_emit_ts > 0) {
                    result.signal_emit_to_strategy_ns.push_back(strategy_signal_ts - signal_emit_ts);
                }
                if (strategy_send_ts >= strategy_signal_ts && strategy_signal_ts > 0) {
                    result.strategy_to_quote_send_ns.push_back(strategy_send_ts - strategy_signal_ts);
                }
                if (gateway_ts >= strategy_send_ts && strategy_send_ts > 0) {
                    result.quote_send_to_gateway_ns.push_back(gateway_ts - strategy_send_ts);
                }

                seen[oi] = 1;
                ++captured;
            }

            if (captured < product.options.size()) spin_pause();
        }

        drain_callback_latencies();
        std::this_thread::sleep_for(std::chrono::microseconds(150));
    }

    if (!products.empty()) {
        const ProductScenario& overload_product = products.front();
        push_option_market_ticks(engine.get(), overload_product);
        for (int burst = 0; burst < 8; ++burst) {
            const double price = overload_product.future_base
                               + static_cast<double>(burst) * overload_product.future_tick;
            lat_gw->set_last_price(overload_product.future_id, price);
            (void)push_future_tick(engine.get(), overload_product, price, sequence_no++);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        drain_callback_latencies();
    }

    result.coalesced_signal_writes = engine->total_coalesced_signal_writes();
    result.coalesced_signal_overwrites = engine->total_coalesced_signal_overwrites();
    result.coalesced_timer_writes = engine->total_coalesced_timer_writes();
    result.coalesced_timer_overwrites = engine->total_coalesced_timer_overwrites();
    result.signal_emits = engine->total_signal_emit_count();
    result.signal_suppressed = engine->total_signal_suppressed_count();
    result.pending_future_tick_overwrites = engine->total_pending_future_tick_overwrites();
    result.max_signal_queue_depth = engine->max_signal_queue_depth();
    result.max_signal_mailbox_depth = engine->max_signal_mailbox_depth();
    result.max_timer_queue_depth = engine->max_timer_queue_depth();
    for (size_t i = 0; i < products.size(); ++i) {
        (void)engine->strategy_runtime_stats(static_cast<int>(i), &result.strategy_stats[i]);
    }
    engine->stop();

    std::cout << "\n[SCENARIO] " << cfg.label
              << " exchange=" << exchange_name(cfg.exchange)
              << " products=" << cfg.product_count
              << " options_per_product=" << cfg.options_per_product
              << " iterations=" << cfg.iterations
              << " cancel_latency_ms=" << cfg.gateway_cancel_latency_ms
              << " monitoring=" << monitoring_mode_name(cfg.monitoring_mode)
              << " hot_path_greeks=" << hot_path_greeks_mode_name(cfg.hot_path_greeks_mode)
              << " execution_low_latency=" << (sys.execution.low_latency_mode ? "on" : "off")
              << " low_latency_spin=" << (sys.scheduling.low_latency_spin ? "on" : "off")
              << "\n";
    if (latency_cores.size() >= static_cast<size_t>(7 + cfg.product_count)) {
        std::cout << "[PINNING] feed=" << sys.affinity.feed_core
                  << " pricer=" << sys.affinity.pricer_core
                  << " gateway_dispatcher=" << sys.affinity.gateway_dispatcher_core
                  << " vol_fitter=" << sys.affinity.vol_fitter_core
                  << " risk_monitor=" << sys.affinity.risk_monitor_core
                  << " timer=" << sys.affinity.timer_core
                  << " grpc=" << sys.affinity.grpc_server_core;
        for (int p = 0; p < cfg.product_count; ++p) {
            std::cout << " strategy" << p << "=" << sys.products[p].strategy_core;
        }
        std::cout << "\n";
    } else {
        std::cout << "[PINNING] disabled; set OMM_LATENCY_PIN_CORES=auto or a comma-separated core list\n";
    }
    std::cout << "[COUNTS] captured_quotes=" << result.tick_to_gateway_ns.size()
              << " expected_quotes=" << result.expected_total_quotes
              << " capture_ratio="
              << (result.expected_total_quotes == 0
                    ? 0.0
                    : 100.0 * static_cast<double>(result.tick_to_gateway_ns.size())
                        / static_cast<double>(result.expected_total_quotes))
              << "%\n";
    for (size_t i = 0; i < result.quotes_by_product.size(); ++i) {
        std::cout << "[COUNTS] product " << i << " quotes=" << result.quotes_by_product[i]
                  << " single_instr_reevals=" << result.strategy_stats[i].single_instrument_reevaluations
                  << " full_book_reevals=" << result.strategy_stats[i].full_book_reevaluations
                  << "\n";
    }
    print_series("LATENCY tick->gateway", result.tick_to_gateway_ns);
    print_series("STAGE tick->signal_emit", result.tick_to_signal_emit_ns);
    print_series("STAGE signal_emit->strategy", result.signal_emit_to_strategy_ns);
    print_series("STAGE strategy->quote_send", result.strategy_to_quote_send_ns);
    print_series("STAGE quote_send->gateway", result.quote_send_to_gateway_ns);
    print_series("CALLBACK QuoteAck route", result.quote_ack_route_latency_ns);
    print_series("CALLBACK QuoteCancel route", result.quote_cancel_route_latency_ns);
    std::cout << "[SIGNAL] emits=" << result.signal_emits
              << " suppressed=" << result.signal_suppressed
              << " future_overwrites=" << result.pending_future_tick_overwrites << "\n";
    std::cout << "[COALESCE] signal writes=" << result.coalesced_signal_writes
              << " overwrites=" << result.coalesced_signal_overwrites
              << " timer writes=" << result.coalesced_timer_writes
              << " timer overwrites=" << result.coalesced_timer_overwrites << "\n";
    std::cout << "[QUEUE] max_signal_ring=" << result.max_signal_queue_depth
              << " max_signal_mailbox=" << result.max_signal_mailbox_depth
              << " max_timer_ring=" << result.max_timer_queue_depth << "\n";

    return result;
}

} // namespace

TEST(LatencyTest, TickToQuoteLatency) {
#ifdef RUNNING_ASAN
    GTEST_SKIP() << "Skipping latency measurement under ASAN (overhead distorts results)";
#endif

    ScenarioConfig cfg{};
    cfg.label = "OptionMMCore direct-replace latency";
    cfg.exchange = Exchange::SHFE;
    cfg.product_count = 1;
    cfg.options_per_product = 160;
    cfg.iterations = 320;
    cfg.timeout_ns = 6'000'000LL;
    cfg.monitoring_mode = monitoring_mode_from_env();
    cfg.hot_path_greeks_mode = hot_path_greeks_mode_from_env();

    ScenarioResult result = run_latency_scenario(cfg);

    const size_t min_quotes = std::max<size_t>(256, result.expected_total_quotes / 20);
    EXPECT_GT(result.tick_to_gateway_ns.size(), min_quotes)
        << "Expected a stable quote stream from option_mm_core direct-replace benchmark";
    EXPECT_GT(result.signal_emits, 0u);
    EXPECT_GT(result.strategy_stats[0].single_instrument_reevaluations, 0u);
    EXPECT_GT(result.pending_future_tick_overwrites, 0u)
        << "Expected the production-scale direct-replace scenario to overwrite pending future ticks";

    const size_t min_stage_samples = std::max<size_t>(128, result.tick_to_gateway_ns.size() / 2);
    expect_stage_p99_under("Direct-replace tick->signal_emit",
                           result.tick_to_signal_emit_ns,
                           10'000'000LL,
                           min_stage_samples);
    expect_stage_p99_under("Direct-replace signal_emit->strategy",
                           result.signal_emit_to_strategy_ns,
                           10'000'000LL,
                           min_stage_samples);
    expect_stage_p99_under("Direct-replace strategy->quote_send",
                           result.strategy_to_quote_send_ns,
                           10'000'000LL,
                           min_stage_samples);
    expect_stage_p99_under("Direct-replace quote_send->gateway",
                           result.quote_send_to_gateway_ns,
                           5'000'000LL,
                           min_stage_samples);
}

TEST(LatencyTest, TickToQuoteLatencyCancelFirst) {
#ifdef RUNNING_ASAN
    GTEST_SKIP() << "Skipping latency measurement under ASAN (overhead distorts results)";
#endif

    ScenarioConfig cfg{};
    cfg.label = "OptionMMCore cancel-first latency";
    cfg.exchange = Exchange::GFEX;
    cfg.product_count = 1;
    cfg.options_per_product = 16;
    cfg.iterations = 120;
    cfg.timeout_ns = 10'000'000LL;
    cfg.monitoring_mode = monitoring_mode_from_env();
    cfg.hot_path_greeks_mode = hot_path_greeks_mode_from_env();
    cfg.gateway_cancel_latency_ms = 1;

    ScenarioResult result = run_latency_scenario(cfg);

    EXPECT_GT(result.tick_to_gateway_ns.size(), 32u)
        << "Expected a measurable replacement stream from cancel-first benchmark";
    EXPECT_GT(result.quote_cancel_route_latency_ns.size(), 0u)
        << "Expected cancel-first benchmark to capture quote-cancel callback routing";
    EXPECT_GT(result.strategy_stats[0].single_instrument_reevaluations, 0u);

    const size_t min_stage_samples = std::max<size_t>(16, result.tick_to_gateway_ns.size() / 2);
    expect_stage_p99_under("Cancel-first tick->signal_emit",
                           result.tick_to_signal_emit_ns,
                           10'000'000LL,
                           min_stage_samples);
    expect_stage_p99_under("Cancel-first signal_emit->strategy",
                           result.signal_emit_to_strategy_ns,
                           10'000'000LL,
                           min_stage_samples);
    expect_stage_p99_under("Cancel-first strategy->quote_send",
                           result.strategy_to_quote_send_ns,
                           10'000'000LL,
                           min_stage_samples);
    expect_stage_p99_under("Cancel-first quote_send->gateway",
                           result.quote_send_to_gateway_ns,
                           5'000'000LL,
                           min_stage_samples);
    expect_stage_p99_under("Cancel-first quote cancel callback route",
                           result.quote_cancel_route_latency_ns,
                           20'000'000LL,
                           1);
}
