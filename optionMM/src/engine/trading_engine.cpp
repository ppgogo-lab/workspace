#include "engine/trading_engine.h"
#include "strategy/option_mm_core.h"
#include "strategy/pcp_arbitrage.h"
#include "strategy/simple_mm.h"
#include "common/thread_utils.h"
#include "logger/logger.h"
#include "pricing/black76.h"

#include <cstring>
#include <cmath>
#include <algorithm>
#include <chrono>

namespace omm {

namespace {

constexpr int kStrategyGatewayBurstCap = 32;
constexpr int kStrategyTimerBurstCap = 8;
constexpr int kStrategySignalBurstCap = 128;
constexpr int kDispatcherCallbackLeadBurstCap = 16;
constexpr int kDispatcherCallbackInterleaveBurstCap = 8;
constexpr int kDispatcherOrderBurstCap = 64;
constexpr int kDispatcherQuoteBurstCap = 128;
constexpr int kDispatcherArbIntentBurstCap = 16;
constexpr int kCoalescedTimerSlotCount = 2;
constexpr int kArbEventBurstCap = 32;

void pin_if_configured(int core_id) noexcept {
    if (core_id < 0) return;

    const int hw_threads = static_cast<int>(std::thread::hardware_concurrency());
    if (hw_threads > 0 && core_id >= hw_threads) return;

    pin_thread_to_core(core_id);
}

void update_max(std::atomic<uint32_t>& metric, uint32_t candidate) noexcept {
    uint32_t prev = metric.load(std::memory_order_relaxed);
    while (prev < candidate
           && !metric.compare_exchange_weak(prev, candidate,
                                            std::memory_order_relaxed,
                                            std::memory_order_relaxed)) {
    }
}

template<typename T>
void publish_latest(std::atomic<uint64_t>& version, T& slot, const T& value) noexcept {
    const uint64_t cur = version.load(std::memory_order_relaxed);
    version.store(cur + 1, std::memory_order_release);
    slot = value;
    version.store(cur + 2, std::memory_order_release);
}

template<typename T>
bool read_latest(const std::atomic<uint64_t>& version,
                 const T& slot,
                 uint64_t seen_version,
                 T* out,
                 uint64_t* published_version) noexcept {
    for (int attempt = 0; attempt < 4; ++attempt) {
        const uint64_t v1 = version.load(std::memory_order_acquire);
        if (v1 == 0 || (v1 & 1u) != 0) {
            spin_pause();
            continue;
        }

        const T snapshot = slot;
        const uint64_t v2 = version.load(std::memory_order_acquire);
        if (v1 == v2 && (v2 & 1u) == 0) {
            if (v2 == seen_version) return false;
            *out = snapshot;
            *published_version = v2;
            return true;
        }
    }
    return false;
}

int timer_mailbox_slot(TimerEventType type) noexcept {
    switch (type) {
    case TimerEventType::HedgeCheck:
        return 0;
    case TimerEventType::QuoteRefresh:
        return 1;
    default:
        return -1;
    }
}

} // namespace

// ─── Construction / destruction ───────────────────────────────────────────────

TradingEngine::TradingEngine(const SystemConfig& cfg,
                              std::unique_ptr<IGateway>     gateway,
                              std::unique_ptr<IFeedHandler> feed)
    : cfg_(cfg)
    , gateway_(std::move(gateway))
    , feed_(std::move(feed))
    , pre_risk_{
        PreTradeRisk(cfg.risk.hard), PreTradeRisk(cfg.risk.hard),
        PreTradeRisk(cfg.risk.hard), PreTradeRisk(cfg.risk.hard),
        PreTradeRisk(cfg.risk.hard), PreTradeRisk(cfg.risk.hard),
        PreTradeRisk(cfg.risk.hard), PreTradeRisk(cfg.risk.hard),
        PreTradeRisk(cfg.risk.hard), PreTradeRisk(cfg.risk.hard),
        PreTradeRisk(cfg.risk.hard), PreTradeRisk(cfg.risk.hard),
        PreTradeRisk(cfg.risk.hard), PreTradeRisk(cfg.risk.hard),
        PreTradeRisk(cfg.risk.hard), PreTradeRisk(cfg.risk.hard),
        PreTradeRisk(cfg.risk.hard), PreTradeRisk(cfg.risk.hard),
        PreTradeRisk(cfg.risk.hard), PreTradeRisk(cfg.risk.hard),
        PreTradeRisk(cfg.risk.hard), PreTradeRisk(cfg.risk.hard),
        PreTradeRisk(cfg.risk.hard), PreTradeRisk(cfg.risk.hard),
        PreTradeRisk(cfg.risk.hard), PreTradeRisk(cfg.risk.hard),
        PreTradeRisk(cfg.risk.hard), PreTradeRisk(cfg.risk.hard),
        PreTradeRisk(cfg.risk.hard), PreTradeRisk(cfg.risk.hard),
        PreTradeRisk(cfg.risk.hard), PreTradeRisk(cfg.risk.hard),
      }
    , post_risk_(cfg.risk.soft)
{
    std::memset(instr_to_product_, 0xFF, sizeof(instr_to_product_));
    // Apply initial MM params from config
    for (int i = 0; i < cfg_.product_count && i < MAX_PRODUCTS; ++i) {
        mm_params_[i].apply(cfg_.products[i].params);
        for (int slot = 0; slot < cfg_.products[i].arbitrage_strategy_count
             && slot < MAX_ARBITRAGE_STRATEGIES_PER_PRODUCT; ++slot) {
            arb_strategy_types_[i][slot] = cfg_.products[i].arbitrage_strategies[slot].type;
            arb_params_[i][slot].apply(cfg_.products[i].arbitrage_strategies[slot].params);
        }
    }
    // Wire the feed's tick_buf pointer now that tick_buf_ is constructed
    if (feed_) feed_->set_tick_buf(&tick_buf_);
}

TradingEngine::~TradingEngine() { stop(); }

// ─── Startup ──────────────────────────────────────────────────────────────────

void TradingEngine::populate_instrument_registry() noexcept {
    // Query instruments from gateway (blocking at startup)
    gateway_->query_instruments(instruments_, &n_instruments_, MAX_INSTRUMENTS);

    // Build instr_to_product_ mapping and per-product option index
    std::memset(option_count_, 0, sizeof(option_count_));

    for (uint16_t i = 0; i < n_instruments_; ++i) {
        instruments_[i].instrument_id = i;
        instruments_[i].product_index = 0xFF;
        for (int p = 0; p < cfg_.product_count; ++p) {
            if (instruments_[i].code == cfg_.products[p].underlying_id ||
                instruments_[i].underlying_code == cfg_.products[p].underlying_id) {
                instruments_[i].product_index = static_cast<uint8_t>(p);
                instr_to_product_[i] = static_cast<uint8_t>(p);
                break;
            }
        }
    }

    for (uint16_t i = 0; i < n_instruments_; ++i) {
        if (instruments_[i].kind != InstrumentKind::Option) continue;
        for (uint16_t u = 0; u < n_instruments_; ++u) {
            if (instruments_[u].kind != InstrumentKind::Future) continue;
            if (!(instruments_[u].code == instruments_[i].underlying_code)) continue;
            instruments_[i].underlying_id = u;
            break;
        }

        // Index options per product for fast batch repricing on future ticks
        const Instrument& instr = instruments_[i];
        if (instr.kind == InstrumentKind::Option) {
            uint8_t p = instr.product_index;
            if (p < MAX_PRODUCTS && option_count_[p] < MAX_INSTRUMENTS) {
                option_ids_[p][option_count_[p]] = i;
                // Cache log(K) so pricer_loop avoids log() per option per tick
                option_log_K_[p][option_count_[p]] = std::log(instr.strike);
                option_count_[p]++;
            }
        }
    }

    // Wire instrument registry to feed handler
    if (feed_)
        feed_->set_instruments(instruments_, n_instruments_);
    if (gateway_)
        gateway_->set_instruments(instruments_, n_instruments_);

    // Seed option_T_ so pricer_loop has valid values before timer fires
    refresh_option_T();
}

void TradingEngine::refresh_option_T() noexcept {
    const int64_t now_ns = get_monotonic_ns();
    static constexpr double NS_PER_YEAR = 365.0 * 24.0 * 3600.0 * 1e9;
    const double r = cfg_.pricing.risk_free_rate;
    for (int p = 0; p < cfg_.product_count && p < MAX_PRODUCTS; ++p) {
        for (uint16_t oi = 0; oi < option_count_[p]; ++oi) {
            const Instrument& opt = instruments_[option_ids_[p][oi]];
            double T = (opt.expiry_epoch_ns - now_ns) / NS_PER_YEAR;
            if (T < 1e-4) T = 1e-4;
            option_T_[p][oi]      = T;
            option_sqrt_T_[p][oi] = std::sqrt(T);
            option_disc_[p][oi]   = std::exp(-r * T);
        }
    }
}

void TradingEngine::init_strategies() noexcept {
    for (int i = 0; i < cfg_.product_count && i < MAX_PRODUCTS; ++i) {
        if (std::strncmp(cfg_.products[i].strategy_type, "option_mm_core",
                         sizeof(cfg_.products[i].strategy_type)) == 0) {
            auto* s = new OptionMMCoreStrategy();
            s->init(static_cast<uint8_t>(i),
                    &quote_buf_[i],
                    &order_buf_[i],
                    &pre_risk_[i],
                    &mm_params_[i],
                    instruments_,
                    tick_snapshot_,
                    &post_risk_,
                    &monitor_alerts_[i]);
            strategies_[i].reset(s);
        } else {
            auto* s = new SimpleMMStrategy();
            s->init(static_cast<uint8_t>(i),
                    &quote_buf_[i],
                    &order_buf_[i],
                    &pre_risk_[i],
                    &mm_params_[i],
                    instruments_);
            strategies_[i].reset(s);
        }
    }
}

void TradingEngine::init_vol_surfaces() noexcept {
    for (int i = 0; i < cfg_.product_count && i < MAX_PRODUCTS; ++i) {
        // Initialise both ping-pong buffers with a flat default surface
        for (int pass = 0; pass < 2; ++pass) {
            SVIVolSurface* s = vol_surfaces_[i].get_inactive();
            s->n_slices = 1;
            s->slices[0].a       = cfg_.pricing.risk_free_rate * 0.04;
            s->slices[0].b       = 0.05;
            s->slices[0].rho     = -0.2;
            s->slices[0].m       = 0.0;
            s->slices[0].sigma   = 0.1;
            s->slices[0].expiry_T = 0.25;
            s->slices[0].valid   = true;
            vol_surfaces_[i].publish();

            WingVolSurface* w = wing_surfaces_[i].get_inactive();
            w->n_slices = 1;
            w->slices[0].ATM_vol    = 0.20;
            w->slices[0].slope_call = -0.1;
            w->slices[0].slope_put  =  0.1;
            w->slices[0].curve_call =  0.05;
            w->slices[0].curve_put  =  0.05;
            w->slices[0].expiry_T   =  0.25;
            w->slices[0].valid      =  true;
            wing_surfaces_[i].publish();

            OrcWingVolSurface* ow = orc_wing_surfaces_[i].get_inactive();
            ow->n_slices = 1;
            ow->slices[0].ref_price      = 100.0;
            ow->slices[0].atm_forward    = 100.0;
            ow->slices[0].ssr            = 1.0;
            ow->slices[0].vol_ref        = 0.20;
            ow->slices[0].slope_ref      = -0.1;
            ow->slices[0].vcr            = 0.0;
            ow->slices[0].scr            = 0.0;
            ow->slices[0].put_curv       = 0.05;
            ow->slices[0].call_curv      = 0.05;
            ow->slices[0].down_cutoff    = -0.15;
            ow->slices[0].up_cutoff      = 0.15;
            ow->slices[0].down_smoothing = 0.5;
            ow->slices[0].up_smoothing   = 0.5;
            ow->slices[0].expiry_T       = 0.25;
            ow->slices[0].valid          = true;
            orc_wing_surfaces_[i].publish();
        }
        surface_versions_[i].store(1, std::memory_order_relaxed);
    }
}

void TradingEngine::init_arbitrage_strategies() noexcept {
    for (int product = 0; product < cfg_.product_count && product < MAX_PRODUCTS; ++product) {
        for (int slot = 0;
             slot < cfg_.products[product].arbitrage_strategy_count
                 && slot < MAX_ARBITRAGE_STRATEGIES_PER_PRODUCT;
             ++slot) {
            const ArbitrageStrategyType type = arb_strategy_types_[product][slot];
            if (type != ArbitrageStrategyType::PCP) continue;

            auto* strategy = new PCPArbitrageStrategy();
            strategy->init(static_cast<uint8_t>(product),
                           &arb_intent_buf_[product],
                           &arb_params_[product][slot],
                           instruments_,
                           tick_snapshot_,
                           greeks_snapshot_,
                           cfg_.pricing.risk_free_rate,
                           cfg_.risk.hard,
                           cfg_.instance.account_id);
            arbitrage_strategies_[product][slot].reset(strategy);
        }
    }
}

void TradingEngine::start() {
    setup_fp_environment();
    populate_instrument_registry();
    init_strategies();
    init_arbitrage_strategies();
    init_vol_surfaces();

    stop_flag_.store(false, std::memory_order_relaxed);
    gateway_dispatcher_running_.store(true, std::memory_order_relaxed);

    OMM_LOG_INFO("startup", "TradingEngine starting: {} products, {} instruments",
                 cfg_.product_count, n_instruments_);

    // Spawn threads in order: gateway dispatcher first so it is ready
    // to handle acks before any orders are sent.
    gateway_dispatcher_thread_ = std::thread([this] { gateway_dispatcher_loop(); });
    pricer_thread_              = std::thread([this] { pricer_loop(); });

    for (int i = 0; i < cfg_.product_count && i < MAX_PRODUCTS; ++i) {
        strategy_threads_[i] = std::thread([this, i] { strategy_loop(i); });
        if (cfg_.products[i].arbitrage_strategy_count > 0) {
            arb_threads_[i] = std::thread([this, i] { arb_loop(i); });
        }
    }

    if (monitoring_deferred_mode()) {
        monitor_publisher_thread_ = std::thread([this] { monitor_publish_loop(); });
    }
    vol_fitter_thread_    = std::thread([this] { vol_fitter_loop(); });
    risk_monitor_thread_  = std::thread([this] { risk_monitor_loop(); });
    timer_thread_         = std::thread([this] { timer_loop(); });

    if (feed_) feed_->start();
}

void TradingEngine::stop() noexcept {
    OMM_LOG_INFO("shutdown", "TradingEngine stopping");
    stop_flag_.store(true, std::memory_order_release);

    if (feed_) feed_->stop();

    auto join = [](std::thread& t) { if (t.joinable()) t.join(); };
    join(pricer_thread_);
    for (auto& t : strategy_threads_) join(t);
    for (auto& t : arb_threads_) join(t);
    join(gateway_dispatcher_thread_);
    join(monitor_publisher_thread_);
    join(vol_fitter_thread_);
    join(risk_monitor_thread_);
    join(timer_thread_);

    if (gateway_) gateway_->disconnect();
}

uint64_t TradingEngine::total_coalesced_signal_writes() const noexcept {
    uint64_t total = 0;
    for (int i = 0; i < cfg_.product_count && i < MAX_PRODUCTS; ++i)
        total += coalesced_signal_writes_[i].load(std::memory_order_relaxed);
    return total;
}

uint64_t TradingEngine::total_coalesced_signal_overwrites() const noexcept {
    uint64_t total = 0;
    for (int i = 0; i < cfg_.product_count && i < MAX_PRODUCTS; ++i)
        total += coalesced_signal_overwrites_[i].load(std::memory_order_relaxed);
    return total;
}

uint64_t TradingEngine::total_coalesced_timer_writes() const noexcept {
    uint64_t total = 0;
    for (int i = 0; i < cfg_.product_count && i < MAX_PRODUCTS; ++i)
        total += coalesced_timer_writes_[i].load(std::memory_order_relaxed);
    return total;
}

uint64_t TradingEngine::total_coalesced_timer_overwrites() const noexcept {
    uint64_t total = 0;
    for (int i = 0; i < cfg_.product_count && i < MAX_PRODUCTS; ++i)
        total += coalesced_timer_overwrites_[i].load(std::memory_order_relaxed);
    return total;
}

uint64_t TradingEngine::total_signal_emit_count() const noexcept {
    uint64_t total = 0;
    for (int i = 0; i < cfg_.product_count && i < MAX_PRODUCTS; ++i)
        total += signal_emit_count_[i].load(std::memory_order_relaxed);
    return total;
}

uint64_t TradingEngine::total_signal_suppressed_count() const noexcept {
    uint64_t total = 0;
    for (int i = 0; i < cfg_.product_count && i < MAX_PRODUCTS; ++i)
        total += signal_suppressed_count_[i].load(std::memory_order_relaxed);
    return total;
}

uint64_t TradingEngine::total_pending_future_tick_overwrites() const noexcept {
    uint64_t total = 0;
    for (int i = 0; i < cfg_.product_count && i < MAX_PRODUCTS; ++i)
        total += pending_future_tick_overwrites_[i].load(std::memory_order_relaxed);
    return total;
}

uint32_t TradingEngine::max_signal_queue_depth() const noexcept {
    uint32_t max_depth = 0;
    for (int i = 0; i < cfg_.product_count && i < MAX_PRODUCTS; ++i) {
        max_depth = std::max(max_depth,
            max_signal_queue_depth_[i].load(std::memory_order_relaxed));
    }
    return max_depth;
}

uint32_t TradingEngine::max_signal_mailbox_depth() const noexcept {
    uint32_t max_depth = 0;
    for (int i = 0; i < cfg_.product_count && i < MAX_PRODUCTS; ++i) {
        max_depth = std::max(max_depth,
            max_signal_mailbox_depth_[i].load(std::memory_order_relaxed));
    }
    return max_depth;
}

uint32_t TradingEngine::max_timer_queue_depth() const noexcept {
    uint32_t max_depth = 0;
    for (int i = 0; i < cfg_.product_count && i < MAX_PRODUCTS; ++i) {
        max_depth = std::max(max_depth,
            max_timer_queue_depth_[i].load(std::memory_order_relaxed));
    }
    return max_depth;
}

int64_t TradingEngine::last_signal_emit_ts(uint16_t instrument_id) const noexcept {
    if (instrument_id >= MAX_INSTRUMENTS) return 0;
    return last_signal_emit_ts_[instrument_id].load(std::memory_order_acquire);
}

int64_t TradingEngine::last_strategy_signal_ts(uint16_t instrument_id) const noexcept {
    if (instrument_id >= MAX_INSTRUMENTS) return 0;
    return last_strategy_signal_ts_[instrument_id].load(std::memory_order_acquire);
}

int64_t TradingEngine::last_quote_ack_route_ts(uint16_t instrument_id) const noexcept {
    if (instrument_id >= MAX_INSTRUMENTS) return 0;
    return last_quote_ack_route_ts_[instrument_id].load(std::memory_order_acquire);
}

int64_t TradingEngine::last_quote_cancel_route_ts(uint16_t instrument_id) const noexcept {
    if (instrument_id >= MAX_INSTRUMENTS) return 0;
    return last_quote_cancel_route_ts_[instrument_id].load(std::memory_order_acquire);
}

int64_t TradingEngine::last_quote_ack_route_latency_ns(uint16_t instrument_id) const noexcept {
    if (instrument_id >= MAX_INSTRUMENTS) return 0;
    return last_quote_ack_route_latency_ns_[instrument_id].load(std::memory_order_acquire);
}

int64_t TradingEngine::last_quote_cancel_route_latency_ns(uint16_t instrument_id) const noexcept {
    if (instrument_id >= MAX_INSTRUMENTS) return 0;
    return last_quote_cancel_route_latency_ns_[instrument_id].load(std::memory_order_acquire);
}

bool TradingEngine::strategy_runtime_stats(int product_idx,
                                           StrategyRuntimeStats* out) const noexcept {
    if (product_idx < 0 || product_idx >= product_count()) return false;
    if (!strategies_[product_idx]) return false;
    return strategies_[product_idx]->read_runtime_stats(out);
}

int TradingEngine::find_arbitrage_slot(int product_idx,
                                       ArbitrageStrategyType type) const noexcept {
    if (product_idx < 0 || product_idx >= product_count()) return -1;
    if (type == ArbitrageStrategyType::None) return -1;
    for (int slot = 0; slot < cfg_.products[product_idx].arbitrage_strategy_count
         && slot < MAX_ARBITRAGE_STRATEGIES_PER_PRODUCT; ++slot) {
        if (arb_strategy_types_[product_idx][slot] == type) return slot;
    }
    return -1;
}

bool TradingEngine::arbitrage_strategy_state(int product_idx,
                                             ArbitrageStrategyType type,
                                             ArbStrategyMonitorState* out) const noexcept {
    const int slot = find_arbitrage_slot(product_idx, type);
    if (slot < 0) return false;
    const auto& strategy = arbitrage_strategies_[product_idx][slot];
    if (!strategy) return false;
    return strategy->read_monitor_state(out);
}

int TradingEngine::arbitrage_pcp_monitor_states(int product_idx,
                                                ArbitrageStrategyType type,
                                                PCPPairMonitorState* out,
                                                int max_count) const noexcept {
    if (out == nullptr || max_count <= 0) return 0;
    if (product_idx < 0 || product_idx >= product_count()) return 0;
    const int slot = find_arbitrage_slot(product_idx, type);
    if (slot < 0) return 0;
    const auto& strategy = arbitrage_strategies_[product_idx][slot];
    if (!strategy) return 0;
    return strategy->read_pcp_monitor_states(out, max_count);
}

bool TradingEngine::arbitrage_params_snapshot(int product_idx,
                                              ArbitrageStrategyType type,
                                              ArbParamsConfig* out) const noexcept {
    if (out == nullptr) return false;
    const int slot = find_arbitrage_slot(product_idx, type);
    if (slot < 0) return false;
    *out = arb_params_[product_idx][slot].snapshot();
    return true;
}

bool TradingEngine::set_arbitrage_enabled(int product_idx,
                                          ArbitrageStrategyType type,
                                          bool enabled) noexcept {
    const int slot = find_arbitrage_slot(product_idx, type);
    if (slot < 0) return false;
    arb_params_[product_idx][slot].enabled.store(enabled, std::memory_order_release);
    return true;
}

AtomicArbParams* TradingEngine::arbitrage_params(int product_idx,
                                                 ArbitrageStrategyType type) noexcept {
    const int slot = find_arbitrage_slot(product_idx, type);
    if (slot < 0) return nullptr;
    return &arb_params_[product_idx][slot];
}

void TradingEngine::coalesce_signal(uint8_t product_idx,
                                    uint16_t option_slot,
                                    const PricingSignal& sig) noexcept {
    publish_latest(coalesced_signal_versions_[product_idx][option_slot],
                   coalesced_signal_mailbox_[product_idx][option_slot],
                   sig);
    coalesced_signal_writes_[product_idx].fetch_add(1, std::memory_order_relaxed);

    if (!coalesced_signal_index_buf_[product_idx].try_push(option_slot)) {
        coalesced_signal_rescan_needed_[product_idx].store(true,
                                                           std::memory_order_release);
        update_max(max_signal_mailbox_depth_[product_idx],
                   static_cast<uint32_t>(coalesced_signal_index_buf_[product_idx].capacity()));
        return;
    }

    update_max(max_signal_mailbox_depth_[product_idx],
               static_cast<uint32_t>(coalesced_signal_index_buf_[product_idx].size_approx()));
}

int TradingEngine::drain_coalesced_signals(int product_idx,
                                           uint64_t* seen_versions,
                                           int budget) noexcept {
    if (budget <= 0) return 0;

    int drained = 0;
    PricingSignal sig{};
    uint16_t option_slot = 0;
    uint64_t published_version = 0;

    auto consume_slot = [&](uint16_t slot) {
        if (slot >= option_count_[product_idx]) return false;

        const uint64_t seen = seen_versions[slot];
        if (!read_latest(coalesced_signal_versions_[product_idx][slot],
                         coalesced_signal_mailbox_[product_idx][slot],
                         seen,
                         &sig,
                         &published_version)) {
            return false;
        }

        const uint64_t writes_since_last = (published_version - seen) / 2u;
        if (writes_since_last > 1) {
            coalesced_signal_overwrites_[product_idx].fetch_add(writes_since_last - 1,
                                                                std::memory_order_relaxed);
        }
        seen_versions[slot] = published_version;
        if (sig.instrument_id < MAX_INSTRUMENTS) {
            last_strategy_signal_ts_[sig.instrument_id].store(
                get_monotonic_ns(), std::memory_order_release);
        }
        strategies_[product_idx]->on_signal(sig);
        return true;
    };

    while (drained < budget
           && coalesced_signal_index_buf_[product_idx].try_pop(option_slot)) {
        if (consume_slot(option_slot)) ++drained;
    }

    if (drained < budget
        && coalesced_signal_rescan_needed_[product_idx].exchange(false,
                                                                 std::memory_order_acquire)) {
        for (uint16_t slot = 0;
             slot < option_count_[product_idx] && drained < budget;
             ++slot) {
            if (consume_slot(slot)) ++drained;
        }
    }

    return drained;
}

void TradingEngine::coalesce_timer_event(int product_idx, const TimerEvent& ev) noexcept {
    const int slot = timer_mailbox_slot(ev.type);
    if (slot < 0 || slot >= kCoalescedTimerSlotCount) return;

    publish_latest(coalesced_timer_versions_[product_idx][slot],
                   coalesced_timer_mailbox_[product_idx][slot],
                   ev);
    coalesced_timer_writes_[product_idx].fetch_add(1, std::memory_order_relaxed);
}

int TradingEngine::drain_coalesced_timers(int product_idx,
                                          uint64_t* seen_versions,
                                          int budget) noexcept {
    if (budget <= 0) return 0;

    int drained = 0;
    TimerEvent ev{};
    uint64_t published_version = 0;

    for (int slot = 0; slot < kCoalescedTimerSlotCount && drained < budget; ++slot) {
        const uint64_t seen = seen_versions[slot];
        if (!read_latest(coalesced_timer_versions_[product_idx][slot],
                         coalesced_timer_mailbox_[product_idx][slot],
                         seen,
                         &ev,
                         &published_version)) {
            continue;
        }

        const uint64_t writes_since_last = (published_version - seen) / 2u;
        if (writes_since_last > 1) {
            coalesced_timer_overwrites_[product_idx].fetch_add(writes_since_last - 1,
                                                               std::memory_order_relaxed);
        }
        seen_versions[slot] = published_version;
        strategies_[product_idx]->on_timer(ev);
        ++drained;
    }

    return drained;
}

bool TradingEngine::should_emit_signal(uint8_t product_idx,
                                       uint16_t option_slot,
                                       const PricingSignal& sig,
                                       uint64_t surface_version) const noexcept {
    if (product_idx >= MAX_PRODUCTS || option_slot >= MAX_INSTRUMENTS) return true;

    const SignalEmitState& last = last_emitted_signal_[product_idx][option_slot];
    const bool current_valid =
        sig.instrument_id < MAX_INSTRUMENTS
        && sig.calc_ts_ns > 0
        && sig.theo_bid > 0.0
        && sig.theo_ask >= sig.theo_bid;
    if (last.surface_version != surface_version || last.valid != current_valid) {
        return true;
    }

    const double price_eps_ticks = cfg_.pricing.signal_emit_price_epsilon_ticks;
    const double underlying_eps_ticks = cfg_.pricing.signal_emit_underlying_epsilon_ticks;
    const double delta_eps = cfg_.pricing.signal_emit_delta_epsilon;
    const double vega_eps = cfg_.pricing.signal_emit_vega_epsilon;
    if (price_eps_ticks <= 0.0
        && underlying_eps_ticks <= 0.0
        && delta_eps <= 0.0
        && vega_eps <= 0.0) {
        return true;
    }

    if (!current_valid) {
        return false;
    }

    const Instrument& opt = instruments_[sig.instrument_id];
    const double option_tick = opt.tick_size > 0.0 ? opt.tick_size : 0.01;
    const double price_eps = price_eps_ticks * option_tick;

    double underlying_tick = option_tick;
    if (sig.underlying_id < MAX_INSTRUMENTS) {
        const double maybe_tick = instruments_[sig.underlying_id].tick_size;
        if (maybe_tick > 0.0) underlying_tick = maybe_tick;
    }
    const double underlying_eps = underlying_eps_ticks * underlying_tick;

    if (std::fabs(last.theo_bid - sig.theo_bid) > price_eps) return true;
    if (std::fabs(last.theo_ask - sig.theo_ask) > price_eps) return true;
    if (std::fabs(static_cast<double>(last.delta) - static_cast<double>(sig.delta)) > delta_eps) return true;
    if (std::fabs(static_cast<double>(last.vega) - static_cast<double>(sig.vega)) > vega_eps) return true;
    if (std::fabs(static_cast<double>(last.underlying_bid)
                - static_cast<double>(sig.underlying_ref_bid)) > underlying_eps) {
        return true;
    }
    if (std::fabs(static_cast<double>(last.underlying_ask)
                - static_cast<double>(sig.underlying_ref_ask)) > underlying_eps) {
        return true;
    }
    return false;
}

void TradingEngine::note_signal_emitted(uint8_t product_idx,
                                        uint16_t option_slot,
                                        uint16_t instrument_id,
                                        const PricingSignal& sig,
                                        uint64_t surface_version) noexcept {
    if (product_idx >= MAX_PRODUCTS || option_slot >= MAX_INSTRUMENTS) return;

    SignalEmitState& last = last_emitted_signal_[product_idx][option_slot];
    last.valid = instrument_id < MAX_INSTRUMENTS
        && sig.calc_ts_ns > 0
        && sig.theo_bid > 0.0
        && sig.theo_ask >= sig.theo_bid;
    last.theo_bid = sig.theo_bid;
    last.theo_ask = sig.theo_ask;
    last.delta = sig.delta;
    last.vega = sig.vega;
    last.underlying_bid = sig.underlying_ref_bid;
    last.underlying_ask = sig.underlying_ref_ask;
    last.surface_version = surface_version;

    signal_emit_count_[product_idx].fetch_add(1, std::memory_order_relaxed);
    if (instrument_id < MAX_INSTRUMENTS) {
        last_signal_emit_ts_[instrument_id].store(sig.calc_ts_ns, std::memory_order_release);
    }
}

void TradingEngine::publish_monitor_tick(const MarketTick& tick) noexcept {
    switch (cfg_.monitoring.hot_path_publish_mode) {
    case MonitoringPublishMode::Full:
        monitor_ticks_.publish(tick);
        break;
    case MonitoringPublishMode::Deferred:
        (void)deferred_monitor_ticks_.try_push(tick);
        break;
    case MonitoringPublishMode::Off:
        break;
    }
}

void TradingEngine::publish_monitor_order(const Order& order) noexcept {
    switch (cfg_.monitoring.hot_path_publish_mode) {
    case MonitoringPublishMode::Full:
        monitor_orders_.publish(order);
        break;
    case MonitoringPublishMode::Deferred:
        (void)deferred_monitor_orders_.try_push(order);
        break;
    case MonitoringPublishMode::Off:
        break;
    }
}

void TradingEngine::publish_monitor_quote(const Quote& quote) noexcept {
    switch (cfg_.monitoring.hot_path_publish_mode) {
    case MonitoringPublishMode::Full:
        monitor_quotes_.publish(quote);
        break;
    case MonitoringPublishMode::Deferred:
        (void)deferred_monitor_quotes_.try_push(quote);
        break;
    case MonitoringPublishMode::Off:
        break;
    }
}

void TradingEngine::publish_monitor_trade(const Trade& trade) noexcept {
    switch (cfg_.monitoring.hot_path_publish_mode) {
    case MonitoringPublishMode::Full:
        monitor_trades_.publish(trade);
        break;
    case MonitoringPublishMode::Deferred:
        (void)deferred_monitor_trades_.try_push(trade);
        break;
    case MonitoringPublishMode::Off:
        break;
    }
}

// ─── Pricer thread ────────────────────────────────────────────────────────────
// Trigger model: a FUTURE tick drives repricing of ALL options for that product.
//   - Future tick arrives → update tick_snapshot_ for the future → for each
//     option in the product, compute Black-76 using the future price as F and
//     the vol surface for sigma → emit one PricingSignal per option.
//   - Option tick arrives → update tick_snapshot_ only (for the vol fitter to
//     read mid-prices for IV inversion). No PricingSignal emitted.
//
// This is the correct market-making model: the forward price drives repricing,
// not individual option prints. A single future tick reprices the whole book.

void TradingEngine::pricer_loop() noexcept {
    set_thread_name("omm-pricer");
    pin_if_configured(cfg_.affinity.pricer_core);

    MarketTick tick{};
    MarketTick pending_future_tick[MAX_PRODUCTS]{};
    bool pending_product[MAX_PRODUCTS]{};
    uint16_t next_option_offset[MAX_PRODUCTS]{};
    int rr_cursor = 0;

    while (!stop_flag_.load(std::memory_order_relaxed)) {
        bool did_work = false;
        if (tick_buf_.try_pop(tick)) {
            did_work = true;

            const uint16_t id = tick.instrument_id;
            if (id < MAX_INSTRUMENTS) {
                const Instrument& instr = instruments_[id];
                if (instr.instrument_id != INVALID_INSTRUMENT_ID) {
                    tick_snapshot_[id] = tick;
                    publish_monitor_tick(tick);

                    if (instr.kind == InstrumentKind::Future) {
                        const uint8_t prod = instr_to_product_[id];
                        if (prod < MAX_PRODUCTS && tick.last_price > 1e-10) {
                            pending_future_tick[prod] = tick;
                            if (pending_product[prod]) {
                                pending_future_tick_overwrites_[prod].fetch_add(
                                    1, std::memory_order_relaxed);
                            } else {
                                pending_product[prod] = true;
                                next_option_offset[prod] = 0;
                            }
                        }
                    }
                }
            }
        }

        const int product_count = std::max(1, std::min(cfg_.product_count, static_cast<int>(MAX_PRODUCTS)));
        int selected_prod = -1;
        for (int scan = 0; scan < product_count; ++scan) {
            const int p = (rr_cursor + scan) % product_count;
            if (pending_product[p]) {
                selected_prod = p;
                rr_cursor = (p + 1) % product_count;
                break;
            }
        }

        if (selected_prod < 0) {
            if (!did_work) spin_pause();
            continue;
        }

        const uint8_t prod = static_cast<uint8_t>(selected_prod);
        const MarketTick& future_tick = pending_future_tick[prod];
        const double F_mid = future_tick.last_price;
        const double F_bid = future_tick.bid_price[0] > 0.0 ? future_tick.bid_price[0] : F_mid;
        const double F_ask = future_tick.ask_price[0] > F_bid ? future_tick.ask_price[0] : F_mid;
        const uint16_t n = option_count_[prod];
        if (n == 0 || F_mid < 1e-10) {
            pending_product[prod] = false;
            next_option_offset[prod] = 0;
            continue;
        }

        const IVolSurface* surf = nullptr;
        if (cfg_.pricing.vol_method == VolMethod::Wing) {
            surf = wing_surfaces_[prod].get();
        } else if (cfg_.pricing.vol_method == VolMethod::OrcWing) {
            surf = orc_wing_surfaces_[prod].get();
        } else {
            surf = vol_surfaces_[prod].get();
        }

        const int64_t now = get_monotonic_ns();
        constexpr uint16_t MAX_BATCH = 128;
        alignas(32) double F_mid_arr[MAX_BATCH];
        alignas(32) double F_bid_arr[MAX_BATCH];
        alignas(32) double F_ask_arr[MAX_BATCH];
        alignas(32) double K_arr[MAX_BATCH];
        alignas(32) double T_arr[MAX_BATCH];
        alignas(32) double sqrt_T_arr[MAX_BATCH];
        alignas(32) double disc_arr[MAX_BATCH];
        alignas(32) double sigma_arr[MAX_BATCH];
        alignas(32) uint8_t is_call_arr[MAX_BATCH];
        alignas(32) Black76Result mid_results[MAX_BATCH];
        alignas(32) Black76Result bid_results[MAX_BATCH];
        alignas(32) Black76Result ask_results[MAX_BATCH];
        alignas(64) PricingSignal sigs[MAX_BATCH];

        const uint16_t start = next_option_offset[prod];
        const uint16_t batch_n = std::min<uint16_t>(MAX_BATCH, n - start);
        const double log_F_mid = std::log(F_mid);
        const uint64_t surface_version = surface_versions_[prod].load(std::memory_order_acquire);

        for (uint16_t bi = 0; bi < batch_n; ++bi) {
            const uint16_t oi = start + bi;
            const uint16_t opt_id = option_ids_[prod][oi];
            const Instrument& opt = instruments_[opt_id];

            F_mid_arr[bi] = F_mid;
            F_bid_arr[bi] = F_bid;
            F_ask_arr[bi] = F_ask;
            K_arr[bi] = opt.strike;
            T_arr[bi] = option_T_[prod][oi];
            sqrt_T_arr[bi] = option_sqrt_T_[prod][oi];
            disc_arr[bi] = option_disc_[prod][oi];
            sigma_arr[bi] = (cfg_.pricing.vol_method == VolMethod::OrcWing)
                ? surf->get_vol_by_strike(F_mid, opt.strike, T_arr[bi])
                : surf->get_vol(option_log_K_[prod][oi] - log_F_mid, T_arr[bi]);
            is_call_arr[bi] = (opt.option_type == OptionType::Call) ? 1 : 0;
        }

        compute_batch_precomputed(F_mid_arr, K_arr, T_arr, sqrt_T_arr, disc_arr,
                                  sigma_arr, is_call_arr, mid_results, batch_n);
        compute_batch_precomputed(F_bid_arr, K_arr, T_arr, sqrt_T_arr, disc_arr,
                                  sigma_arr, is_call_arr, bid_results, batch_n);
        compute_batch_precomputed(F_ask_arr, K_arr, T_arr, sqrt_T_arr, disc_arr,
                                  sigma_arr, is_call_arr, ask_results, batch_n);

        alignas(64) PricingSignal emitted_sigs[MAX_BATCH];
        uint16_t emitted_slots[MAX_BATCH];
        int emitted_count = 0;

        for (uint16_t bi = 0; bi < batch_n; ++bi) {
            const uint16_t oi = start + bi;
            const uint16_t opt_id = option_ids_[prod][oi];
            const Instrument& opt = instruments_[opt_id];
            const Black76Result& mid_res = mid_results[bi];

            PricingSignal& sig = sigs[bi];
            sig.instrument_id = opt_id;
            sig.underlying_id = opt.underlying_id;
            sig.flags = PricingFlagHasUnderlyingRef;
            sig.sequence_no = future_tick.sequence_no;
            sig.calc_ts_ns = now;
            sig.theo_bid = std::min(bid_results[bi].price, ask_results[bi].price);
            sig.theo_ask = std::max(bid_results[bi].price, ask_results[bi].price);
            sig.delta = static_cast<float>(mid_res.delta);
            sig.vega = static_cast<float>(mid_res.vega);
            sig.underlying_ref_bid = static_cast<float>(future_tick.bid_price[0]);
            sig.underlying_ref_ask = static_cast<float>(future_tick.ask_price[0]);

            Greeks greek{};
            greek.instrument_id = opt_id;
            greek.theo_price = mid_res.price;
            greek.delta = mid_res.delta;
            greek.gamma = mid_res.gamma;
            greek.vega = mid_res.vega;
            greek.theta = mid_res.theta;
            greek.rho = mid_res.rho;
            greek.iv = sigma_arr[bi];
            greek.T = T_arr[bi];
            greek.calc_ts_ns = now;
            greeks_snapshot_[opt_id] = greek;

            if (!should_emit_signal(prod, oi, sig, surface_version)) {
                signal_suppressed_count_[prod].fetch_add(1, std::memory_order_relaxed);
                continue;
            }

            emitted_slots[emitted_count] = oi;
            emitted_sigs[emitted_count] = sig;
            ++emitted_count;
        }

        const uint32_t queued_before_push = static_cast<uint32_t>(signal_buf_[prod].size_approx());
        const uint32_t projected_queue_depth = std::min<uint32_t>(
            static_cast<uint32_t>(signal_buf_[prod].capacity()),
            queued_before_push + static_cast<uint32_t>(emitted_count));
        update_max(max_signal_queue_depth_[prod], projected_queue_depth);
        if (!signal_buf_[prod].try_push_batch(emitted_sigs, emitted_count)) {
            // Phase 5: if the bounded signal ring is saturated, collapse this
            // batch into latest-only per-option mailboxes instead of spinning.
            // The strategy drains the mailbox within its normal signal budget,
            // so stale theo work is overwritten rather than backpressuring the
            // pricer thread indefinitely.
            update_max(max_signal_queue_depth_[prod],
                       static_cast<uint32_t>(signal_buf_[prod].capacity()));
            for (int bi = 0; bi < emitted_count; ++bi) {
                coalesce_signal(prod, emitted_slots[bi], emitted_sigs[bi]);
            }
        }

        for (int bi = 0; bi < emitted_count; ++bi) {
            note_signal_emitted(prod,
                                emitted_slots[bi],
                                emitted_sigs[bi].instrument_id,
                                emitted_sigs[bi],
                                surface_version);
        }

        next_option_offset[prod] = static_cast<uint16_t>(start + batch_n);
        if (next_option_offset[prod] >= n) {
            pending_product[prod] = false;
            next_option_offset[prod] = 0;
        }
    }
}

// Strategy thread ──────────────────────────────────────────────────────────

void TradingEngine::strategy_loop(int idx) noexcept {
    set_thread_name("omm-strat");
    if (idx >= 0 && idx < cfg_.product_count && idx < MAX_PRODUCTS) {
        pin_if_configured(cfg_.products[idx].strategy_core);
    }

    GatewayEvent ev{};
    TimerEvent timer_ev{};
    PricingSignal sig{};
    uint64_t coalesced_signal_seen_versions[MAX_INSTRUMENTS]{};
    uint64_t coalesced_timer_seen_versions[kCoalescedTimerSlotCount]{};

    while (!stop_flag_.load(std::memory_order_relaxed)) {
        bool did_work = false;
        update_max(max_timer_queue_depth_[idx],
                   static_cast<uint32_t>(timer_buf_[idx].size_approx()));

        // Fairness policy: gateway events still have top priority, followed by
        // timers, but each outer-loop pass is bounded so callback or timer
        // bursts cannot monopolize the strategy thread. Every pass reserves a
        // pricing-signal slice before polling again, which prevents fresh theo
        // updates from being starved behind an unbounded backlog.
        for (int drained = 0;
             drained < kStrategyGatewayBurstCap && gateway_event_buf_[idx].try_pop(ev);
             ++drained) {
            did_work = true;
            switch (ev.type) {
            case GatewayEventType::OrderAck:
                strategies_[idx]->on_order_ack(ev.order);
                break;
            case GatewayEventType::QuoteAck:
                if (ev.quote.instrument_id < MAX_INSTRUMENTS) {
                    const int64_t now_ns = get_monotonic_ns();
                    last_quote_ack_route_ts_[ev.quote.instrument_id].store(
                        now_ns, std::memory_order_release);
                    last_quote_ack_route_latency_ns_[ev.quote.instrument_id].store(
                        ev.quote.ack_ts > 0 ? std::max<int64_t>(0, now_ns - ev.quote.ack_ts) : 0,
                        std::memory_order_release);
                }
                strategies_[idx]->on_quote_ack(ev.quote);
                break;
            case GatewayEventType::QuoteCancel:
                if (ev.quote.instrument_id < MAX_INSTRUMENTS) {
                    const int64_t now_ns = get_monotonic_ns();
                    last_quote_cancel_route_ts_[ev.quote.instrument_id].store(
                        now_ns, std::memory_order_release);
                    last_quote_cancel_route_latency_ns_[ev.quote.instrument_id].store(
                        ev.quote.ack_ts > 0 ? std::max<int64_t>(0, now_ns - ev.quote.ack_ts) : 0,
                        std::memory_order_release);
                }
                strategies_[idx]->on_quote_cancel(ev.quote);
                break;
            case GatewayEventType::QuoteReject:
                strategies_[idx]->on_quote_reject(ev.quote);
                break;
            case GatewayEventType::OrderFill:
            case GatewayEventType::QuoteFill:
                strategies_[idx]->on_fill(ev.trade);
                break;
            case GatewayEventType::OrderCancel:
                strategies_[idx]->on_order_cancel(ev.order.client_order_id);
                break;
            case GatewayEventType::OrderReject:
                strategies_[idx]->on_order_reject(ev.order);
                break;
            default:
                break;
            }
        }

        int timer_budget = kStrategyTimerBurstCap;
        for (; timer_budget > 0 && timer_buf_[idx].try_pop(timer_ev); --timer_budget) {
            did_work = true;
            strategies_[idx]->on_timer(timer_ev);
        }
        const int coalesced_timers =
            drain_coalesced_timers(idx, coalesced_timer_seen_versions, timer_budget);
        if (coalesced_timers > 0) did_work = true;

        int signal_budget = kStrategySignalBurstCap;
        for (; signal_budget > 0 && signal_buf_[idx].try_pop(sig); --signal_budget) {
            did_work = true;
            if (sig.instrument_id < MAX_INSTRUMENTS) {
                last_strategy_signal_ts_[sig.instrument_id].store(
                    get_monotonic_ns(), std::memory_order_release);
            }
            strategies_[idx]->on_signal(sig);
        }
        const int coalesced_signals =
            drain_coalesced_signals(idx, coalesced_signal_seen_versions, signal_budget);
        if (coalesced_signals > 0) did_work = true;

        if (!did_work) spin_pause();
    }
}

void TradingEngine::arb_loop(int idx) noexcept {
    set_thread_name("omm-arb");
    if (idx >= 0 && idx < cfg_.product_count && idx < MAX_PRODUCTS) {
        const int arb_core = cfg_.products[idx].arbitrage_core;
        if (arb_core >= 0 && arb_core != cfg_.products[idx].strategy_core) {
            pin_if_configured(arb_core);
        }
    }

    GatewayEvent ev{};
    while (!stop_flag_.load(std::memory_order_relaxed)) {
        bool did_work = false;

        for (int drained = 0;
             drained < kArbEventBurstCap && arb_event_buf_[idx].try_pop(ev);
             ++drained) {
            did_work = true;
            for (int slot = 0; slot < cfg_.products[idx].arbitrage_strategy_count
                 && slot < MAX_ARBITRAGE_STRATEGIES_PER_PRODUCT; ++slot) {
                auto& strategy = arbitrage_strategies_[idx][slot];
                if (!strategy) continue;
                switch (ev.type) {
                case GatewayEventType::OrderAck:
                    strategy->on_order_ack(ev.order);
                    break;
                case GatewayEventType::OrderFill:
                case GatewayEventType::QuoteFill:
                    strategy->on_fill(ev.trade);
                    break;
                case GatewayEventType::OrderCancel:
                    strategy->on_order_cancel(ev.order.client_order_id);
                    break;
                case GatewayEventType::OrderReject:
                    strategy->on_order_reject(ev.order);
                    break;
                default:
                    break;
                }
            }
        }

        const Timestamp now_ns = get_monotonic_ns();
        for (int slot = 0; slot < cfg_.products[idx].arbitrage_strategy_count
             && slot < MAX_ARBITRAGE_STRATEGIES_PER_PRODUCT; ++slot) {
            auto& strategy = arbitrage_strategies_[idx][slot];
            if (!strategy) continue;
            strategy->evaluate(now_ns);
        }

        if (!did_work) {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }
}

// ─── Gateway dispatcher thread ────────────────────────────────────────────────

void TradingEngine::gateway_dispatcher_loop() noexcept {
    set_thread_name("omm-gw-disp");
    pin_if_configured(cfg_.affinity.gateway_dispatcher_core);

    while (!stop_flag_.load(std::memory_order_relaxed)) {
        bool did_work = false;
        auto drain_callbacks = [&](int burst_cap) {
            GatewayEvent ev{};
            for (int drained = 0;
                 drained < burst_cap && gateway_->callback_buf.try_pop(ev);
                 ++drained) {
                did_work = true;
                const int p = ev.product_index;
                if (p >= MAX_PRODUCTS) continue;

                bool route_to_arbitrage = false;
                switch (ev.type) {
                case GatewayEventType::OrderAck:
                    publish_monitor_order(ev.order);
                    route_to_arbitrage = is_arb_order_id(ev.order.client_order_id);
                    break;
                case GatewayEventType::QuoteAck:
                    publish_monitor_quote(ev.quote);
                    break;
                case GatewayEventType::QuoteCancel:
                    publish_monitor_quote(ev.quote);
                    break;
                case GatewayEventType::QuoteReject:
                    publish_monitor_quote(ev.quote);
                    break;
                case GatewayEventType::OrderFill:
                case GatewayEventType::QuoteFill:
                    publish_monitor_trade(ev.trade);
                    route_to_arbitrage = is_arb_order_id(ev.trade.client_order_id);
                    {
                        Order filled{};
                        filled.client_order_id = ev.trade.client_order_id;
                        filled.instrument_id   = ev.trade.instrument_id;
                        filled.product_index   = ev.trade.product_index;
                        filled.exchange_id     = ev.trade.exchange_id;
                        filled.side            = ev.trade.side;
                        filled.status          = OrderStatus::Filled;
                        filled.price           = ev.trade.fill_price;
                        filled.volume          = ev.trade.fill_volume;
                        filled.avg_fill_price  = ev.trade.fill_price;
                        filled.filled_volume   = ev.trade.fill_volume;
                        filled.ack_ts          = ev.trade.fill_ts;
                        publish_monitor_order(filled);
                    }
                    (void)risk_buf_.try_push(ev.trade);  // forward to risk monitor
                    OMM_LOG_INFO("fill", "instr={} side={} qty={} price={:.4f} order_id={}",
                                 ev.trade.instrument_id,
                                 ev.trade.side == Side::Buy ? "buy" : "sell",
                                 ev.trade.fill_volume,
                                 ev.trade.fill_price,
                                 ev.trade.client_order_id);
                    break;
                case GatewayEventType::OrderCancel:
                    publish_monitor_order(ev.order);
                    route_to_arbitrage = is_arb_order_id(ev.order.client_order_id);
                    break;
                case GatewayEventType::OrderReject:
                    publish_monitor_order(ev.order);
                    route_to_arbitrage = is_arb_order_id(ev.order.client_order_id);
                    break;
                default:
                    break;
                }

                if (route_to_arbitrage) {
                    while (!stop_flag_.load(std::memory_order_relaxed)
                        && !arb_event_buf_[p].try_push(ev)) {
                        spin_pause();
                    }
                } else {
                    while (!stop_flag_.load(std::memory_order_relaxed)
                        && !gateway_event_buf_[p].try_push(ev)) {
                        spin_pause();
                    }
                }
            }
        };

        // Phase 3 fairness policy: give callbacks a bounded head start so acks
        // and fills reach the strategy sooner, then interleave smaller callback
        // slices between per-product send bursts so the send path is not
        // starved in the other direction.
        drain_callbacks(kDispatcherCallbackLeadBurstCap);

        // Round-robin over all strategy output buffers
        for (int i = 0; i < cfg_.product_count && i < MAX_PRODUCTS; ++i) {
            Order order{};
            for (int drained = 0;
                 drained < kDispatcherOrderBurstCap && order_buf_[i].try_pop(order);
                 ++drained) {
                did_work = true;
                gateway_->send_order(order);
                publish_monitor_order(order);
            }

            Quote quote{};
            for (int drained = 0;
                 drained < kDispatcherQuoteBurstCap && quote_buf_[i].try_pop(quote);
                 ++drained) {
                did_work = true;
                gateway_->send_quote(quote);
                publish_monitor_quote(quote);
            }

            ArbIntent intent{};
            for (int drained = 0;
                 drained < kDispatcherArbIntentBurstCap && arb_intent_buf_[i].try_pop(intent);
                 ++drained) {
                did_work = true;
                if (intent.kind == ArbIntentKind::SubmitOrder) {
                    gateway_->send_order(intent.order);
                    publish_monitor_order(intent.order);
                } else if (intent.kind == ArbIntentKind::CancelOrder) {
                    gateway_->cancel_order(intent.order.client_order_id, intent.order.instrument_id);
                }
            }
            drain_callbacks(kDispatcherCallbackInterleaveBurstCap);
        }
        if (!did_work) spin_pause();
    }
    gateway_dispatcher_running_.store(false, std::memory_order_release);
}

void TradingEngine::monitor_publish_loop() noexcept {
    set_thread_name("omm-monitor");

    constexpr int kMonitorPublishBurstCap = 128;

    while (gateway_dispatcher_running_.load(std::memory_order_acquire)
           || !deferred_monitor_ticks_.empty_approx()
           || !deferred_monitor_orders_.empty_approx()
           || !deferred_monitor_quotes_.empty_approx()
           || !deferred_monitor_trades_.empty_approx()) {
        bool did_work = false;

        MarketTick tick{};
        for (int drained = 0;
             drained < kMonitorPublishBurstCap && deferred_monitor_ticks_.try_pop(tick);
             ++drained) {
            did_work = true;
            monitor_ticks_.publish(tick);
        }

        Order order{};
        for (int drained = 0;
             drained < kMonitorPublishBurstCap && deferred_monitor_orders_.try_pop(order);
             ++drained) {
            did_work = true;
            monitor_orders_.publish(order);
        }

        Quote quote{};
        for (int drained = 0;
             drained < kMonitorPublishBurstCap && deferred_monitor_quotes_.try_pop(quote);
             ++drained) {
            did_work = true;
            monitor_quotes_.publish(quote);
        }

        Trade trade{};
        for (int drained = 0;
             drained < kMonitorPublishBurstCap && deferred_monitor_trades_.try_pop(trade);
             ++drained) {
            did_work = true;
            monitor_trades_.publish(trade);
        }

        if (!did_work) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
}

// ─── Vol fitter thread ────────────────────────────────────────────────────────
// Runs every fit_interval_seconds. For each product:
//   1. Collect option instruments grouped by expiry.
//   2. For each expiry slice, read the latest tick snapshot to get market mid-prices.
//   3. Invert Black-76 to get market implied vols.
//   4. Call fit_svi_slice() to fit SVI parameters.
//   5. Publish the new surface via VolSurfaceManager atomic swap.
//
// Needs >= 5 valid option quotes per expiry to attempt a fit; otherwise keeps
// the existing surface unchanged.

void TradingEngine::vol_fitter_loop() noexcept {
    set_thread_name("omm-volfitter");
    pin_if_configured(cfg_.affinity.vol_fitter_core);

    // Working buffers (stack-allocated, reused each iteration)
    double  strikes[MAX_STRIKES];
    double  mkt_vols[MAX_STRIKES];
    double  fwd_prices[MAX_EXPIRIES];
    double  expiry_Ts[MAX_EXPIRIES];
    int     expiry_counts[MAX_EXPIRIES];
    // Per-expiry strike/vol accumulation (indices into strikes[]/mkt_vols[])
    int     expiry_start[MAX_EXPIRIES];

    while (!stop_flag_.load(std::memory_order_relaxed)) {
        // Sleep in 100ms chunks so we can respond to stop quickly
        for (int slept = 0;
             slept < cfg_.pricing.fit_interval_seconds * 1000;
             slept += 100) {
            if (stop_flag_.load(std::memory_order_relaxed)) return;
            struct timespec ts{0, 100'000'000};
            nanosleep(&ts, nullptr);
        }

        const double r = cfg_.pricing.risk_free_rate;

        for (int p = 0; p < cfg_.product_count && p < MAX_PRODUCTS; ++p) {
            // ── Pass 1: discover expiry slices and collect forward prices ──────
            int n_expiries = 0;
            std::memset(expiry_counts, 0, sizeof(expiry_counts));
            std::memset(fwd_prices,    0, sizeof(fwd_prices));
            std::memset(expiry_Ts,     0, sizeof(expiry_Ts));

            for (uint16_t id = 0; id < n_instruments_; ++id) {
                const Instrument& instr = instruments_[id];
                if (instr.instrument_id == INVALID_INSTRUMENT_ID) continue;
                if (instr.product_index != static_cast<uint8_t>(p)) continue;

                if (instr.kind == InstrumentKind::Future) {
                    // Use this future's last price as forward for all expiries
                    // (in production, match by expiry; here use a single forward)
                    const MarketTick& t = tick_snapshot_[id];
                    if (t.last_price > 1e-10) {
                        for (int e = 0; e < MAX_EXPIRIES; ++e)
                            if (fwd_prices[e] < 1e-10) fwd_prices[e] = t.last_price;
                    }
                    continue;
                }

                if (instr.kind != InstrumentKind::Option) continue;

                const MarketTick& t = tick_snapshot_[id];
                if (t.recv_ts_ns == 0) continue;  // no tick yet

                // Compute T for this instrument
                double T = (instr.expiry_epoch_ns - t.recv_ts_ns)
                           / (365.0 * 24.0 * 3600.0 * 1e9);
                if (T < 1.0 / 365.0) continue;  // skip expiries < 1 day out

                // Find or create expiry bucket (match within 1 day = 1/365 years)
                int ei = -1;
                for (int e = 0; e < n_expiries; ++e) {
                    if (std::fabs(expiry_Ts[e] - T) < 1.0 / 365.0) { ei = e; break; }
                }
                if (ei < 0) {
                    if (n_expiries >= MAX_EXPIRIES) continue;
                    ei = n_expiries++;
                    expiry_Ts[ei] = T;
                    // Forward price for this expiry: use underlying's last tick
                    if (instr.underlying_id < n_instruments_) {
                        const MarketTick& ut = tick_snapshot_[instr.underlying_id];
                        if (ut.last_price > 1e-10) fwd_prices[ei] = ut.last_price;
                    }
                }
                expiry_counts[ei]++;
            }

            if (n_expiries == 0) continue;

            // Sort expiry slices by T ascending
            // Simple insertion sort (n_expiries <= MAX_EXPIRIES = 24)
            for (int i = 1; i < n_expiries; ++i) {
                double tT = expiry_Ts[i]; double tF = fwd_prices[i]; int tc = expiry_counts[i];
                int j = i - 1;
                while (j >= 0 && expiry_Ts[j] > tT) {
                    expiry_Ts[j+1] = expiry_Ts[j]; fwd_prices[j+1] = fwd_prices[j];
                    expiry_counts[j+1] = expiry_counts[j]; --j;
                }
                expiry_Ts[j+1] = tT; fwd_prices[j+1] = tF; expiry_counts[j+1] = tc;
            }

            // Compute start indices into the flat strikes[]/mkt_vols[] arrays
            expiry_start[0] = 0;
            for (int e = 1; e < n_expiries; ++e)
                expiry_start[e] = expiry_start[e-1] + expiry_counts[e-1];

            // Reset counts for pass 2
            std::memset(expiry_counts, 0, sizeof(expiry_counts));

            // ── Pass 2: collect (strike, market_iv) pairs per expiry ──────────
            for (uint16_t id = 0; id < n_instruments_; ++id) {
                const Instrument& instr = instruments_[id];
                if (instr.instrument_id == INVALID_INSTRUMENT_ID) continue;
                if (instr.product_index != static_cast<uint8_t>(p)) continue;
                if (instr.kind != InstrumentKind::Option) continue;

                const MarketTick& t = tick_snapshot_[id];
                if (t.recv_ts_ns == 0) continue;

                double T = (instr.expiry_epoch_ns - t.recv_ts_ns)
                           / (365.0 * 24.0 * 3600.0 * 1e9);
                if (T < 1.0 / 365.0) continue;

                // Find expiry bucket
                int ei = -1;
                for (int e = 0; e < n_expiries; ++e) {
                    if (std::fabs(expiry_Ts[e] - T) < 1.0 / 365.0) { ei = e; break; }
                }
                if (ei < 0) continue;

                double F = fwd_prices[ei];
                if (F < 1e-10) continue;

                // Mid-price from best bid/ask
                double bid = t.bid_price[0], ask = t.ask_price[0];
                if (bid <= 0.0 || ask <= 0.0 || ask < bid) continue;
                double mid = 0.5 * (bid + ask);

                bool is_call = (instr.option_type == OptionType::Call);
                double iv = implied_vol(mid, F, instr.strike, T, r, is_call);
                if (iv < 0.01 || iv > 3.0) continue;  // reject implausible vols

                int slot = expiry_start[ei] + expiry_counts[ei];
                if (slot >= MAX_STRIKES * MAX_EXPIRIES) continue;
                strikes[slot]  = instr.strike;
                mkt_vols[slot] = iv;
                expiry_counts[ei]++;
            }

            // ── Pass 3: fit per expiry, build new surface ─────────────────────
            if (cfg_.pricing.vol_method == VolMethod::Wing) {
                WingVolSurface* surf = wing_surfaces_[p].get_inactive();
                surf->n_slices = 0;

                for (int e = 0; e < n_expiries; ++e) {
                    int cnt = expiry_counts[e];
                    if (cnt < 5) {
                        const WingVolSurface* cur =
                            static_cast<const WingVolSurface*>(wing_surfaces_[p].get());
                        for (int s = 0; s < cur->n_slices; ++s) {
                            if (std::fabs(cur->slices[s].expiry_T - expiry_Ts[e]) < 1.0/365.0) {
                                surf->slices[surf->n_slices++] = cur->slices[s];
                                break;
                            }
                        }
                        continue;
                    }

                    WingParams params{};
                    params.expiry_T = expiry_Ts[e];
                    bool ok = fit_wing_slice(
                        strikes  + expiry_start[e],
                        mkt_vols + expiry_start[e],
                        cnt, fwd_prices[e], expiry_Ts[e], params);

                    if (ok) {
                        surf->slices[surf->n_slices++] = params;
                        OMM_LOG_INFO("volfitter",
                            "product={} expiry_T={:.4f} n={} ATM={:.4f} sc={:.4f} sp={:.4f} cc={:.4f} cp={:.4f}",
                            p, expiry_Ts[e], cnt,
                            params.ATM_vol, params.slope_call, params.slope_put,
                            params.curve_call, params.curve_put);
                    } else {
                        OMM_LOG_WARN("volfitter",
                            "product={} expiry_T={:.4f} wing fit failed (n={})", p, expiry_Ts[e], cnt);
                    }
                }

                if (surf->n_slices > 0) {
                    wing_surfaces_[p].publish();
                    surface_versions_[p].fetch_add(1, std::memory_order_release);
                }
            } else if (cfg_.pricing.vol_method == VolMethod::OrcWing) {
                OrcWingVolSurface* surf = orc_wing_surfaces_[p].get_inactive();
                surf->n_slices = 0;

                for (int e = 0; e < n_expiries; ++e) {
                    int cnt = expiry_counts[e];
                    if (cnt < 8) {
                        const OrcWingVolSurface* cur =
                            static_cast<const OrcWingVolSurface*>(orc_wing_surfaces_[p].get());
                        for (int s = 0; s < cur->n_slices; ++s) {
                            if (std::fabs(cur->slices[s].expiry_T - expiry_Ts[e]) < 1.0 / 365.0) {
                                surf->slices[surf->n_slices++] = cur->slices[s];
                                break;
                            }
                        }
                        continue;
                    }

                    OrcWingParams params{};
                    params.ref_price = fwd_prices[e];
                    params.atm_forward = fwd_prices[e];
                    params.expiry_T = expiry_Ts[e];
                    const bool ok = fit_orc_wing_slice(
                        strikes + expiry_start[e],
                        mkt_vols + expiry_start[e],
                        cnt, fwd_prices[e], expiry_Ts[e], params);

                    if (ok) {
                        surf->slices[surf->n_slices++] = params;
                        OMM_LOG_INFO("volfitter",
                            "product={} expiry_T={:.4f} n={} vr={:.4f} sr={:.4f} pc={:.4f} cc={:.4f} dc={:.4f} uc={:.4f} dsm={:.4f} usm={:.4f}",
                            p, expiry_Ts[e], cnt,
                            params.vol_ref, params.slope_ref, params.put_curv, params.call_curv,
                            params.down_cutoff, params.up_cutoff, params.down_smoothing, params.up_smoothing);
                    } else {
                        OMM_LOG_WARN("volfitter",
                            "product={} expiry_T={:.4f} orcWing fit failed (n={})", p, expiry_Ts[e], cnt);
                    }
                }

                if (surf->n_slices > 0) {
                    orc_wing_surfaces_[p].publish();
                    surface_versions_[p].fetch_add(1, std::memory_order_release);
                }
            } else {
                SVIVolSurface* surf = vol_surfaces_[p].get_inactive();
                surf->n_slices = 0;

                for (int e = 0; e < n_expiries; ++e) {
                    int cnt = expiry_counts[e];
                    if (cnt < 5) {
                        // Not enough quotes — copy existing slice if available
                        const SVIVolSurface* cur =
                            static_cast<const SVIVolSurface*>(vol_surfaces_[p].get());
                        for (int s = 0; s < cur->n_slices; ++s) {
                            if (std::fabs(cur->slices[s].expiry_T - expiry_Ts[e]) < 1.0/365.0) {
                                surf->slices[surf->n_slices++] = cur->slices[s];
                                break;
                            }
                        }
                        continue;
                    }

                    SVIParams params{};
                    params.expiry_T = expiry_Ts[e];
                    bool ok = fit_svi_slice(
                        strikes  + expiry_start[e],
                        mkt_vols + expiry_start[e],
                        cnt, fwd_prices[e], expiry_Ts[e], params);

                    if (ok) {
                        surf->slices[surf->n_slices++] = params;
                        OMM_LOG_INFO("volfitter",
                            "product={} expiry_T={:.4f} n={} a={:.4f} b={:.4f} rho={:.3f} m={:.4f} sigma={:.4f}",
                            p, expiry_Ts[e], cnt,
                            params.a, params.b, params.rho, params.m, params.sigma);
                    } else {
                        OMM_LOG_WARN("volfitter",
                            "product={} expiry_T={:.4f} fit failed (n={})", p, expiry_Ts[e], cnt);
                    }
                }

                if (surf->n_slices > 0) {
                    vol_surfaces_[p].publish();
                    surface_versions_[p].fetch_add(1, std::memory_order_release);
                }
            }
        }
    }
}

// ─── Risk monitor thread ──────────────────────────────────────────────────────

void TradingEngine::risk_monitor_loop() noexcept {
    set_thread_name("omm-risk");
    pin_if_configured(cfg_.affinity.risk_monitor_core);

    while (!stop_flag_.load(std::memory_order_relaxed)) {
        // Process fills from risk_buf_
        Trade trade{};
        while (risk_buf_.try_pop(trade))
            post_risk_.on_fill(trade);

        // Periodic Greeks limit check against latest snapshot
        post_risk_.check_limits(greeks_snapshot_, n_instruments_);

        // Log any new risk breaches
        if (post_risk_.any_breach())
            OMM_LOG_WARN("risk", "breach flags: pos={} delta={} gamma={} vega={}",
                         (int)post_risk_.position_breach(),
                         (int)post_risk_.delta_breach(),
                         (int)post_risk_.gamma_breach(),
                         (int)post_risk_.vega_breach());

        struct timespec ts{0, 100'000'000};
        nanosleep(&ts, nullptr);
    }
}

// ─── Timer thread ─────────────────────────────────────────────────────────────

void TradingEngine::timer_loop() noexcept {
    set_thread_name("omm-timer");
    pin_if_configured(cfg_.affinity.timer_core);

    int64_t last_hedge_ns   = get_monotonic_ns();
    int64_t last_quote_refresh_ns = last_hedge_ns;
    int64_t last_T_refresh_ns = last_hedge_ns;
    const int64_t hedge_interval_ns =
        static_cast<int64_t>(cfg_.timer.hedge_check_interval_ms) * 1'000'000LL;
    const int64_t quote_refresh_interval_ns =
        static_cast<int64_t>(cfg_.timer.quote_refresh_interval_ms) * 1'000'000LL;
    static constexpr int64_t T_REFRESH_NS = 1'000'000'000LL;  // 1 second

    while (!stop_flag_.load(std::memory_order_relaxed)) {
        // Sleep 100ms at a time so stop_flag is checked promptly
        struct timespec ts{0, 100'000'000};
        nanosleep(&ts, nullptr);

        if (stop_flag_.load(std::memory_order_relaxed)) break;

        const int64_t now = get_monotonic_ns();

        // Refresh option_T_ every second (T changes by ~1/86400 per day)
        if (now - last_T_refresh_ns >= T_REFRESH_NS) {
            refresh_option_T();
            last_T_refresh_ns = now;
        }

        // Hedge check at configured interval
        if (now - last_hedge_ns >= hedge_interval_ns) {
            TimerEvent ev{};
            ev.trigger_ts_ns = now;
            ev.type          = TimerEventType::HedgeCheck;

            for (int i = 0; i < cfg_.product_count && i < MAX_PRODUCTS; ++i) {
                if (!strategies_[i]) continue;
                // Duplicate hedge checks are idempotent; keep only the latest
                // one outstanding per product instead of spinning on timer_buf_.
                coalesce_timer_event(i, ev);
            }
            last_hedge_ns = now;
        }

        if (quote_refresh_interval_ns > 0 && now - last_quote_refresh_ns >= quote_refresh_interval_ns) {
            TimerEvent ev{};
            ev.trigger_ts_ns = now;
            ev.type = TimerEventType::QuoteRefresh;
            for (int i = 0; i < cfg_.product_count && i < MAX_PRODUCTS; ++i) {
                if (!strategies_[i]) continue;
                // Quote refresh is also latest-only safe: if the strategy is
                // behind, a newer refresh supersedes an older pending refresh.
                coalesce_timer_event(i, ev);
            }
            last_quote_refresh_ns = now;
        }
    }
}

// ─── Manual order / cancel (called from gRPC server thread) ──────────────────

bool TradingEngine::submit_manual_order(const Order& o) noexcept {
    if (!gateway_ || !gateway_->is_connected()) return false;
    // Route through the product's order buffer so the gateway dispatcher sends it
    uint8_t prod = (o.product_index < MAX_PRODUCTS) ? o.product_index : 0;
    return order_buf_[prod].try_push(o);
}

bool TradingEngine::cancel_order(OrderId id, uint16_t instrument_id) noexcept {
    if (!gateway_ || !gateway_->is_connected()) return false;
    return gateway_->cancel_order(id, instrument_id);
}

bool TradingEngine::cancel_quote(QuoteId id, uint16_t instrument_id) noexcept {
    if (!gateway_ || !gateway_->is_connected()) return false;
    return gateway_->cancel_quote(id, instrument_id);
}

} // namespace omm
