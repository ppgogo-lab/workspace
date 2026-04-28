#include "engine/trading_engine.h"
#include "strategy/option_mm_core.h"
#include "strategy/pcp_arbitrage.h"
#include "strategy/simple_mm.h"
#include "common/huge_pages.h"
#include "common/numa_utils.h"
#include "common/thread_utils.h"
#include "logger/logger.h"
#include "pricing/black76.h"
#include "pricing/svi.h"
#include "pricing/orc_wing.h"
#include "pricing/wing.h"

#include <cstring>
#include <cmath>
#include <algorithm>
#include <array>
#include <chrono>
#include <ctime>
#include <stdexcept>

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
constexpr int kArbMarketTriggerBurstCap = 128;
constexpr int64_t kArbMaintenanceIntervalNs = 5'000'000LL;
constexpr int64_t kTimerIdleSleepCapNs = 5'000'000LL;
constexpr int64_t kRiskIdleSleepCapNs = 5'000'000LL;
constexpr int64_t kRiskCheckIntervalNs = 5'000'000LL;

void pin_if_configured(int core_id) noexcept {
    if (core_id < 0) return;

    const int hw_threads = static_cast<int>(std::thread::hardware_concurrency());
    if (hw_threads > 0 && core_id >= hw_threads) return;

    pin_thread_to_core(core_id);
}

void apply_realtime_if_configured(bool enabled,
                                  int priority,
                                  const char* thread_name) noexcept {
    (void)thread_name;
    if (!enabled || priority <= 0) return;
    (void)try_set_realtime_priority(priority);
}

void sleep_for_ns_interruptible(const std::atomic<bool>& stop_flag,
                                int64_t sleep_ns,
                                int64_t chunk_cap_ns) noexcept {
    while (sleep_ns > 0 && !stop_flag.load(std::memory_order_relaxed)) {
        const int64_t chunk = std::min(sleep_ns, chunk_cap_ns);
        struct timespec ts{
            static_cast<time_t>(chunk / 1'000'000'000LL),
            static_cast<long>(chunk % 1'000'000'000LL),
        };
        nanosleep(&ts, nullptr);
        sleep_ns -= chunk;
    }
}

// Helper to update max value (for single-writer statistics)
void update_max(uint32_t& metric, uint32_t candidate) noexcept {
    if (candidate > metric) {
        metric = candidate;
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

template<typename PositionLike>
void apply_fill_to_position(PositionLike* pos, const Trade& trade) noexcept {
    if (pos == nullptr) return;
    pos->instrument_id = trade.instrument_id;
    pos->product_index = trade.product_index;

    const int32_t qty = trade.fill_volume;
    if (trade.side == Side::Buy) {
        const double total_cost =
            pos->avg_long_price * static_cast<double>(pos->long_position)
            + trade.fill_price * static_cast<double>(qty);
        pos->long_position += qty;
        pos->long_today += qty;
        pos->avg_long_price = pos->long_position > 0
            ? total_cost / static_cast<double>(pos->long_position)
            : 0.0;
    } else {
        if (pos->long_position > 0) {
            const int32_t close_qty = std::min<int32_t>(qty, pos->long_position);
            pos->realized_pnl += (trade.fill_price - pos->avg_long_price)
                * static_cast<double>(close_qty);
        }
        const double total_cost =
            pos->avg_short_price * static_cast<double>(pos->short_position)
            + trade.fill_price * static_cast<double>(qty);
        pos->short_position += qty;
        pos->short_today += qty;
        pos->avg_short_price = pos->short_position > 0
            ? total_cost / static_cast<double>(pos->short_position)
            : 0.0;
    }
    pos->net_position = pos->long_position - pos->short_position;
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
    mm_book_ids_.fill(INVALID_BOOK_ID);
    for (auto& row : arb_book_ids_) {
        row.fill(INVALID_BOOK_ID);
    }
    init_identity_from_config();
    if (cfg_.persistence.enabled) {
        repository_ = std::make_unique<DataRepository>(
            cfg_.persistence, cfg_.gateway.type, cfg_.pricing.vol_method);
    }
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

int TradingEngine::enable_huge_pages_for_large_arrays() noexcept {
    if (!huge_pages_available()) {
        OMM_LOG_INFO("hugepages", "Transparent huge pages not available on this system");
        return 0;
    }

    int enabled_count = 0;

    // Enable huge pages for large arrays (>= 2MB)
    // These are the hot data structures that benefit most from reduced TLB misses

    // Ring buffers (if large enough)
    if (enable_huge_pages(&tick_buf_, sizeof(tick_buf_))) ++enabled_count;
    if (enable_huge_pages(&deferred_monitor_ticks_, sizeof(deferred_monitor_ticks_))) ++enabled_count;
    if (enable_huge_pages(&deferred_monitor_orders_, sizeof(deferred_monitor_orders_))) ++enabled_count;
    if (enable_huge_pages(&deferred_monitor_quotes_, sizeof(deferred_monitor_quotes_))) ++enabled_count;
    if (enable_huge_pages(&deferred_monitor_trades_, sizeof(deferred_monitor_trades_))) ++enabled_count;
    if (enable_huge_pages(&deferred_persist_order_events_, sizeof(deferred_persist_order_events_))) ++enabled_count;
    if (enable_huge_pages(&deferred_persist_quote_events_, sizeof(deferred_persist_quote_events_))) ++enabled_count;
    if (enable_huge_pages(&deferred_persist_trades_, sizeof(deferred_persist_trades_))) ++enabled_count;

    // Per-product arrays
    for (int p = 0; p < MAX_PRODUCTS; ++p) {
        if (enable_huge_pages(&signal_buf_[p], sizeof(signal_buf_[p]))) ++enabled_count;
        if (enable_huge_pages(&gateway_event_buf_[p], sizeof(gateway_event_buf_[p]))) ++enabled_count;
        if (enable_huge_pages(&order_buf_[p], sizeof(order_buf_[p]))) ++enabled_count;
        if (enable_huge_pages(&quote_buf_[p], sizeof(quote_buf_[p]))) ++enabled_count;
        if (enable_huge_pages(&arb_market_trigger_buf_[p], sizeof(arb_market_trigger_buf_[p]))) ++enabled_count;
    }

    // Coalesced signal mailboxes (large 2D arrays)
    if (enable_huge_pages(&coalesced_signal_mailbox_, sizeof(coalesced_signal_mailbox_))) ++enabled_count;
    if (enable_huge_pages(&coalesced_signal_versions_, sizeof(coalesced_signal_versions_))) ++enabled_count;
    if (enable_huge_pages(&last_emitted_signal_, sizeof(last_emitted_signal_))) ++enabled_count;

    // Option data arrays (hot path)
    if (enable_huge_pages(&option_ids_, sizeof(option_ids_))) ++enabled_count;
    if (enable_huge_pages(&option_log_K_, sizeof(option_log_K_))) ++enabled_count;
    if (enable_huge_pages(&option_T_, sizeof(option_T_))) ++enabled_count;
    if (enable_huge_pages(&option_sqrt_T_, sizeof(option_sqrt_T_))) ++enabled_count;
    if (enable_huge_pages(&option_disc_, sizeof(option_disc_))) ++enabled_count;

    // Snapshots
    if (enable_huge_pages(&greeks_snapshot_, sizeof(greeks_snapshot_))) ++enabled_count;
    if (enable_huge_pages(&tick_snapshot_, sizeof(tick_snapshot_))) ++enabled_count;

    // Timestamp arrays
    if (enable_huge_pages(&last_signal_emit_ts_, sizeof(last_signal_emit_ts_))) ++enabled_count;
    if (enable_huge_pages(&last_strategy_signal_ts_, sizeof(last_strategy_signal_ts_))) ++enabled_count;
    if (enable_huge_pages(&last_quote_ack_route_ts_, sizeof(last_quote_ack_route_ts_))) ++enabled_count;
    if (enable_huge_pages(&last_quote_cancel_route_ts_, sizeof(last_quote_cancel_route_ts_))) ++enabled_count;
    if (enable_huge_pages(&last_quote_ack_route_latency_ns_, sizeof(last_quote_ack_route_latency_ns_))) ++enabled_count;
    if (enable_huge_pages(&last_quote_cancel_route_latency_ns_, sizeof(last_quote_cancel_route_latency_ns_))) ++enabled_count;

    OMM_LOG_INFO("hugepages", "Enabled transparent huge pages for {} memory regions", enabled_count);
    return enabled_count;
}

bool TradingEngine::enable_numa_awareness() noexcept {
    if (!numa_available_multi_node()) {
        OMM_LOG_INFO("numa", "NUMA not available or single-node system - skipping NUMA optimization");
        return false;
    }

    const int node_count = numa_node_count();
    OMM_LOG_INFO("numa", "NUMA available: {} nodes detected", node_count);

    // Log NUMA topology
    for (int node = 0; node < node_count; ++node) {
        std::size_t total_bytes = 0;
        if (numa_get_memory_stats(node, nullptr, &total_bytes)) {
            OMM_LOG_INFO("numa", "  Node {}: {} MB total memory",
                        node, total_bytes / (1024 * 1024));
        }
    }

    // Log core-to-node mapping for configured cores
    if (cfg_.affinity.feed_core >= 0) {
        int node = get_numa_node_for_core(cfg_.affinity.feed_core);
        OMM_LOG_INFO("numa", "Feed thread: core {} -> NUMA node {}",
                    cfg_.affinity.feed_core, node);
    }
    if (cfg_.affinity.pricer_core >= 0) {
        int node = get_numa_node_for_core(cfg_.affinity.pricer_core);
        OMM_LOG_INFO("numa", "Pricer thread: core {} -> NUMA node {}",
                    cfg_.affinity.pricer_core, node);
    }
    if (cfg_.affinity.gateway_dispatcher_core >= 0) {
        int node = get_numa_node_for_core(cfg_.affinity.gateway_dispatcher_core);
        OMM_LOG_INFO("numa", "Gateway dispatcher: core {} -> NUMA node {}",
                    cfg_.affinity.gateway_dispatcher_core, node);
    }

    for (int p = 0; p < cfg_.product_count && p < MAX_PRODUCTS; ++p) {
        if (cfg_.products[p].strategy_core >= 0) {
            int node = get_numa_node_for_core(cfg_.products[p].strategy_core);
            OMM_LOG_INFO("numa", "Strategy[{}]: core {} -> NUMA node {}",
                        p, cfg_.products[p].strategy_core, node);
        }
    }

    OMM_LOG_INFO("numa", "NUMA awareness enabled - threads will bind to local nodes");
    return true;
}

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

}

void TradingEngine::refresh_option_T() noexcept {
    const double r = cfg_.pricing.risk_free_rate;
    for (int p = 0; p < cfg_.product_count && p < MAX_PRODUCTS; ++p) {
        // Get vol surface for slice index caching
        const IVolSurface* surf = nullptr;
        if (cfg_.pricing.vol_method == VolMethod::Wing) {
            surf = wing_surfaces_[p].get();
        } else if (cfg_.pricing.vol_method == VolMethod::OrcWing) {
            surf = orc_wing_surfaces_[p].get();
        } else {
            surf = vol_surfaces_[p].get();
        }

        for (uint16_t oi = 0; oi < option_count_[p]; ++oi) {
            const Instrument& opt = instruments_[option_ids_[p][oi]];
            const double T = option_time_to_expiry_years(opt);
            option_T_[p][oi]      = T;
            option_sqrt_T_[p][oi] = std::sqrt(T);
            option_disc_[p][oi]   = std::exp(-r * T);

            // Cache expiry slice index (eliminates linear scan in hot path)
            if (surf && cfg_.pricing.vol_method == VolMethod::SVI) {
                const auto* svi_surf = static_cast<const SVIVolSurface*>(surf);
                option_expiry_slice_[p][oi] = static_cast<int8_t>(
                    svi_surf->find_expiry_slice_index(T));
            } else {
                option_expiry_slice_[p][oi] = -1;  // Not applicable for other methods
            }
        }
    }
}

double TradingEngine::option_time_to_expiry_years(const Instrument& opt) const noexcept {
    if (trading_calendar_ready_) {
        return trading_calendar_.time_to_expiry_years(
            opt.exchange_id.view(), std::time(nullptr), opt.expiry_date);
    }
    static constexpr double NS_PER_YEAR = 365.0 * 24.0 * 3600.0 * 1e9;
    const double fallback = (opt.expiry_epoch_ns - get_monotonic_ns()) / NS_PER_YEAR;
    return fallback > 1e-4 ? fallback : 1e-4;
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
                    &tick_snapshot_,
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
        surface_versions_[i] = 1;  // Plain store (single writer)
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
                           &tick_snapshot_,
                           &greeks_snapshot_,
                           cfg_.pricing.risk_free_rate,
                           cfg_.risk.hard,
                           cfg_.instance.account_id);
            arbitrage_strategies_[product][slot].reset(strategy);
        }
    }
}

void TradingEngine::init_identity_from_config() noexcept {
    identity_state_.users.clear();
    identity_state_.books.clear();
    identity_state_.arb_book_bindings.clear();
    identity_state_.mm_book_ids.fill(INVALID_BOOK_ID);
    for (int i = 0; i < cfg_.book_count && i < MAX_BOOKS; ++i) {
        PersistedBook book{};
        book.book_id = cfg_.books[i].book_id != INVALID_BOOK_ID
            ? cfg_.books[i].book_id
            : static_cast<BookId>(i + 1);
        std::strncpy(book.book_code, cfg_.books[i].book_code, sizeof(book.book_code) - 1);
        std::strncpy(book.display_name, cfg_.books[i].display_name, sizeof(book.display_name) - 1);
        std::strncpy(book.description, cfg_.books[i].description, sizeof(book.description) - 1);
        book.active = cfg_.books[i].active;
        identity_state_.books.push_back(book);
    }
    for (int i = 0; i < cfg_.user_count && i < MAX_USERS; ++i) {
        PersistedUser user{};
        user.user_id = cfg_.users[i].user_id != INVALID_USER_ID
            ? cfg_.users[i].user_id
            : static_cast<UserId>(i + 1);
        std::strncpy(user.username, cfg_.users[i].username, sizeof(user.username) - 1);
        std::strncpy(user.display_name, cfg_.users[i].display_name, sizeof(user.display_name) - 1);
        std::strncpy(user.password_hash, cfg_.users[i].password, sizeof(user.password_hash) - 1);
        user.active = cfg_.users[i].active;
        user.default_book_id = cfg_.users[i].default_book_id;
        identity_state_.users.push_back(user);
    }
    for (int product = 0; product < cfg_.product_count && product < MAX_PRODUCTS; ++product) {
        identity_state_.mm_book_ids[product] = cfg_.products[product].mm_book_id;
        mm_book_ids_[product] = cfg_.products[product].mm_book_id;
        for (int slot = 0;
             slot < cfg_.products[product].arbitrage_strategy_count
                 && slot < MAX_ARBITRAGE_STRATEGIES_PER_PRODUCT;
             ++slot) {
            arb_book_ids_[product][slot] = cfg_.products[product].arbitrage_strategies[slot].book_id;
            if (cfg_.products[product].arbitrage_strategies[slot].type
                != ArbitrageStrategyType::None
                && cfg_.products[product].arbitrage_strategies[slot].book_id != INVALID_BOOK_ID) {
                PersistedArbBookBinding binding{};
                binding.product_index = static_cast<uint8_t>(product);
                binding.strategy_type = cfg_.products[product].arbitrage_strategies[slot].type;
                binding.book_id = cfg_.products[product].arbitrage_strategies[slot].book_id;
                identity_state_.arb_book_bindings.push_back(binding);
            }
        }
    }
}

void TradingEngine::apply_identity_state(const IdentityState& state) noexcept {
    identity_state_ = state;
    mm_book_ids_.fill(INVALID_BOOK_ID);
    for (auto& row : arb_book_ids_) {
        row.fill(INVALID_BOOK_ID);
    }
    for (int product = 0; product < cfg_.product_count && product < MAX_PRODUCTS; ++product) {
        mm_book_ids_[product] = state.mm_book_ids[product];
    }
    for (const auto& binding : state.arb_book_bindings) {
        const int slot = find_arbitrage_slot(binding.product_index, binding.strategy_type);
        if (slot >= 0) {
            arb_book_ids_[binding.product_index][slot] = binding.book_id;
        }
    }
}

void TradingEngine::init_persistence() noexcept {
    if (!repository_) return;

    repository_->set_instruments(instruments_, n_instruments_);
    if (!repository_->open()) {
        OMM_LOG_ERROR("repo", "failed to open repository path={}", cfg_.persistence.data_path);
        repository_.reset();
        return;
    }
    IdentityState loaded_identity{};
    if (repository_->sync_identity_state(cfg_, &loaded_identity)) {
        apply_identity_state(loaded_identity);
    } else {
        OMM_LOG_WARN("repo", "failed to sync identity state, using config bootstrap");
    }
    (void)repository_->persist_instruments();

    repository_->start();
    for (int i = 0; i < cfg_.product_count && i < MAX_PRODUCTS; ++i) {
        persist_mm_params_update(i, mm_params_[i].snapshot());
        for (int slot = 0; slot < cfg_.products[i].arbitrage_strategy_count
             && slot < MAX_ARBITRAGE_STRATEGIES_PER_PRODUCT; ++slot) {
            persist_arb_params_update(i,
                                      arb_strategy_types_[i][slot],
                                      arb_params_[i][slot].snapshot());
        }
    }
    persist_risk_limits_update(post_risk_.limits());
}

bool TradingEngine::init_trading_calendar() noexcept {
    std::string error;
    if (repository_) {
        if (cfg_.exchange_calendar_count > 0 || cfg_.exchange_trading_time_count > 0) {
            if (!repository_->seed_exchange_calendar(cfg_)) {
                OMM_LOG_ERROR("calendar", "failed to seed exchange calendar from config");
                return false;
            }
        }
        std::vector<ExchangeTradingCalendar> calendars;
        if (!repository_->load_exchange_calendars(&calendars)
            || !trading_calendar_.load(std::move(calendars))) {
            OMM_LOG_ERROR("calendar", "failed to load exchange calendar from repository");
            return false;
        }
    } else if (!trading_calendar_.load_from_config(cfg_)) {
        OMM_LOG_ERROR("calendar", "failed to load exchange calendar from config");
        return false;
    }

    if (!trading_calendar_.validate_products(cfg_, &error)) {
        OMM_LOG_ERROR("calendar", "{}", error);
        return false;
    }
    trading_calendar_ready_ = true;
    return true;
}

void TradingEngine::persist_shutdown_state() noexcept {
    if (!repository_) return;

    PositionSnapshotEvent positions{};
    positions.snapshot_ts = get_monotonic_ns();
    positions.n_instruments = n_instruments_;
    const Position* current_positions = post_risk_.positions();
    for (uint16_t i = 0; i < n_instruments_; ++i) {
        positions.positions[i] = current_positions[i];
    }
    (void)repository_->enqueue_positions_snapshot(positions);
    persist_end_of_day_snapshot();
    repository_->stop();
}

void TradingEngine::start() {
    setup_fp_environment();
    populate_instrument_registry();
    init_persistence();
    if (!init_trading_calendar()) {
        throw std::runtime_error("failed to initialize exchange trading calendar");
    }
    refresh_option_T();
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

    if (monitoring_deferred_mode() || repository_) {
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

    persist_shutdown_state();

    if (gateway_) gateway_->disconnect();
}

int TradingEngine::read_all_greeks(Greeks* out, int max_count) const noexcept {
    if (out == nullptr || max_count <= 0) return 0;
    const int count = std::min<int>(max_count, n_instruments_);
    for (int i = 0; i < count; ++i) {
        Greeks greek{};
        if (greeks_snapshot_.read(static_cast<uint16_t>(i), &greek)) {
            out[i] = greek;
        } else {
            out[i] = Greeks{};
            out[i].instrument_id = static_cast<uint16_t>(i);
        }
    }
    return count;
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
        total += signal_emit_count_[i];  // Plain read (eventual consistency OK)
    return total;
}

uint64_t TradingEngine::total_signal_suppressed_count() const noexcept {
    uint64_t total = 0;
    for (int i = 0; i < cfg_.product_count && i < MAX_PRODUCTS; ++i)
        total += signal_suppressed_count_[i];  // Plain read (eventual consistency OK)
    return total;
}

uint64_t TradingEngine::total_pending_future_tick_overwrites() const noexcept {
    uint64_t total = 0;
    for (int i = 0; i < cfg_.product_count && i < MAX_PRODUCTS; ++i)
        total += pending_future_tick_overwrites_[i];  // Plain read (eventual consistency OK)
    return total;
}

uint32_t TradingEngine::max_signal_queue_depth() const noexcept {
    uint32_t max_depth = 0;
    for (int i = 0; i < cfg_.product_count && i < MAX_PRODUCTS; ++i) {
        max_depth = std::max(max_depth, max_signal_queue_depth_[i]);  // Plain read
    }
    return max_depth;
}

uint32_t TradingEngine::max_signal_mailbox_depth() const noexcept {
    uint32_t max_depth = 0;
    for (int i = 0; i < cfg_.product_count && i < MAX_PRODUCTS; ++i) {
        max_depth = std::max(max_depth, max_signal_mailbox_depth_[i]);  // Plain read
    }
    return max_depth;
}

uint32_t TradingEngine::max_timer_queue_depth() const noexcept {
    uint32_t max_depth = 0;
    for (int i = 0; i < cfg_.product_count && i < MAX_PRODUCTS; ++i) {
        max_depth = std::max(max_depth, max_timer_queue_depth_[i]);  // Plain read
    }
    return max_depth;
}

int64_t TradingEngine::last_signal_emit_ts(uint16_t instrument_id) const noexcept {
    if (instrument_id >= MAX_INSTRUMENTS) return 0;
    return last_signal_emit_ts_[instrument_id];  // Plain read
}

int64_t TradingEngine::last_strategy_signal_ts(uint16_t instrument_id) const noexcept {
    if (instrument_id >= MAX_INSTRUMENTS) return 0;
    return last_strategy_signal_ts_[instrument_id];  // Plain read (eventual consistency OK)
}

int64_t TradingEngine::last_quote_ack_route_ts(uint16_t instrument_id) const noexcept {
    if (instrument_id >= MAX_INSTRUMENTS) return 0;
    return last_quote_ack_route_ts_[instrument_id];  // Plain read (eventual consistency OK)
}

int64_t TradingEngine::last_quote_cancel_route_ts(uint16_t instrument_id) const noexcept {
    if (instrument_id >= MAX_INSTRUMENTS) return 0;
    return last_quote_cancel_route_ts_[instrument_id];  // Plain read (eventual consistency OK)
}

int64_t TradingEngine::last_quote_ack_route_latency_ns(uint16_t instrument_id) const noexcept {
    if (instrument_id >= MAX_INSTRUMENTS) return 0;
    return last_quote_ack_route_latency_ns_[instrument_id];  // Plain read (eventual consistency OK)
}

int64_t TradingEngine::last_quote_cancel_route_latency_ns(uint16_t instrument_id) const noexcept {
    if (instrument_id >= MAX_INSTRUMENTS) return 0;
    return last_quote_cancel_route_latency_ns_[instrument_id];  // Plain read (eventual consistency OK)
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
    persist_arb_params_update(product_idx, type, arb_params_[product_idx][slot].snapshot());
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
            last_strategy_signal_ts_[sig.instrument_id] = get_monotonic_ns();  // Plain store (single writer)
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

    ++signal_emit_count_[product_idx];  // Plain increment (single writer)
    if (instrument_id < MAX_INSTRUMENTS) {
        last_signal_emit_ts_[instrument_id] = sig.calc_ts_ns;  // Plain store (single writer)
    }
}

void TradingEngine::publish_monitor_tick(const TopOfBookTick& tick) noexcept {
    switch (cfg_.monitoring.hot_path_publish_mode) {
    case MonitoringPublishMode::Full:
    case MonitoringPublishMode::Deferred:
        if (!deferred_monitor_ticks_.try_push(tick)) {
            deferred_monitor_drops_.fetch_add(1, std::memory_order_relaxed);
        }
        break;
    case MonitoringPublishMode::Off:
        break;
    }
}

void TradingEngine::publish_monitor_order(const Order& order) noexcept {
    switch (cfg_.monitoring.hot_path_publish_mode) {
    case MonitoringPublishMode::Full:
    case MonitoringPublishMode::Deferred:
        if (!deferred_monitor_orders_.try_push(order)) {
            deferred_monitor_drops_.fetch_add(1, std::memory_order_relaxed);
        }
        break;
    case MonitoringPublishMode::Off:
        break;
    }
}

void TradingEngine::publish_monitor_quote(const Quote& quote) noexcept {
    switch (cfg_.monitoring.hot_path_publish_mode) {
    case MonitoringPublishMode::Full:
    case MonitoringPublishMode::Deferred:
        if (!deferred_monitor_quotes_.try_push(quote)) {
            deferred_monitor_drops_.fetch_add(1, std::memory_order_relaxed);
        }
        break;
    case MonitoringPublishMode::Off:
        break;
    }
}

void TradingEngine::publish_monitor_trade(const Trade& trade) noexcept {
    switch (cfg_.monitoring.hot_path_publish_mode) {
    case MonitoringPublishMode::Full:
    case MonitoringPublishMode::Deferred:
        if (!deferred_monitor_trades_.try_push(trade)) {
            deferred_monitor_drops_.fetch_add(1, std::memory_order_relaxed);
        }
        break;
    case MonitoringPublishMode::Off:
        break;
    }
}

void TradingEngine::defer_order_persistence(
        OrderPersistenceEventType type,
        const Order& order) noexcept {
    if (!repository_) return;
    OrderPersistenceEvent event{};
    event.type = type;
    event.order = order;
    if (!deferred_persist_order_events_.try_push(event)) {
        deferred_persistence_drops_.fetch_add(1, std::memory_order_relaxed);
    }
}

void TradingEngine::defer_quote_persistence(
        QuotePersistenceEventType type,
        const Quote& quote,
        const void* unused) noexcept {
    (void)unused;
    if (!repository_) return;
    QuotePersistenceEvent event{};
    event.type = type;
    event.quote = quote;
    if (!deferred_persist_quote_events_.try_push(event)) {
        deferred_persistence_drops_.fetch_add(1, std::memory_order_relaxed);
    }
}

void TradingEngine::defer_trade_persistence(const Trade& trade) noexcept {
    if (!repository_) return;
    if (!deferred_persist_trades_.try_push(trade)) {
        deferred_persistence_drops_.fetch_add(1, std::memory_order_relaxed);
    }
}

void TradingEngine::persist_end_of_day_snapshot() noexcept {
    if (!repository_) return;
    std::array<Greeks, MAX_INSTRUMENTS> greeks{};
    (void)read_all_greeks(greeks.data(), n_instruments_);
    const EndOfDaySnapshot snapshot = build_end_of_day_snapshot(
        0,
        instruments_,
        n_instruments_,
        greeks.data(),
        cfg_.pricing.vol_method,
        vol_surfaces_,
        wing_surfaces_,
        orc_wing_surfaces_,
        cfg_.product_count);
    EndOfDaySnapshot eod_snapshot = snapshot;
    if (eod_snapshot.trading_day == 0) {
        const auto now = std::time(nullptr);
        std::tm local_tm{};
#if defined(_WIN32)
        localtime_s(&local_tm, &now);
#else
        localtime_r(&now, &local_tm);
#endif
        eod_snapshot.trading_day = (local_tm.tm_year + 1900) * 10000
                                 + (local_tm.tm_mon + 1) * 100
                                 + local_tm.tm_mday;
    }
    (void)repository_->persist_end_of_day_snapshot(eod_snapshot);
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
    apply_realtime_if_configured(cfg_.scheduling.enable_realtime,
                                 cfg_.scheduling.pricer_priority,
                                 "omm-pricer");

    // Bind to local NUMA node for optimal memory access
    if (numa_available_multi_node()) {
        bind_thread_to_local_numa_node();
    }

    TopOfBookTick tick{};
    TopOfBookTick pending_future_tick[MAX_PRODUCTS]{};
    bool pending_product[MAX_PRODUCTS]{};
    uint16_t next_option_offset[MAX_PRODUCTS]{};
    uint16_t cold_greeks_offset[MAX_PRODUCTS]{};
    int64_t next_cold_greeks_due_ns[MAX_PRODUCTS]{};
    int rr_cursor = 0;
    int cold_rr_cursor = 0;
    int spin_count = 0;  // Adaptive spinning counter
    const int64_t cold_greeks_interval_ns =
        static_cast<int64_t>(std::max(1, cfg_.pricing.cold_greeks_interval_ms)) * 1'000'000LL;
    const uint16_t cold_greeks_batch_cap = static_cast<uint16_t>(
        std::max(1, std::min(cfg_.pricing.cold_greeks_batch_size, 128)));

    auto refresh_cold_greeks_batch = [&](int product_count) noexcept -> bool {
        const int64_t now = get_monotonic_ns();
        int selected = -1;
        for (int scan = 0; scan < product_count; ++scan) {
            const int p = (cold_rr_cursor + scan) % product_count;
            if (option_count_[p] > 0 && now >= next_cold_greeks_due_ns[p]) {
                selected = p;
                cold_rr_cursor = (p + 1) % product_count;
                break;
            }
        }
        if (selected < 0) return false;

        const uint8_t prod = static_cast<uint8_t>(selected);
        const uint16_t n = option_count_[prod];
        const uint16_t start = cold_greeks_offset[prod];
        const uint16_t batch_n = std::min<uint16_t>(cold_greeks_batch_cap, n - start);
        if (batch_n == 0) {
            cold_greeks_offset[prod] = 0;
            next_cold_greeks_due_ns[prod] = now + cold_greeks_interval_ns;
            return true;
        }

        const uint16_t first_opt_id = option_ids_[prod][0];
        if (first_opt_id >= MAX_INSTRUMENTS) {
            next_cold_greeks_due_ns[prod] = now + cold_greeks_interval_ns;
            return true;
        }
        const uint16_t future_id = instruments_[first_opt_id].underlying_id;
        if (future_id >= MAX_INSTRUMENTS) {
            next_cold_greeks_due_ns[prod] = now + cold_greeks_interval_ns;
            return true;
        }
        TopOfBookTick future_tick{};
        if (!tick_snapshot_.read(future_id, &future_tick)) {
            next_cold_greeks_due_ns[prod] = now + cold_greeks_interval_ns;
            return true;
        }
        const double F_mid = future_tick.last_price;
        if (F_mid < 1e-10) {
            next_cold_greeks_due_ns[prod] = now + cold_greeks_interval_ns;
            return true;
        }

        const IVolSurface* surf = nullptr;
        if (cfg_.pricing.vol_method == VolMethod::Wing) {
            surf = wing_surfaces_[prod].get();
        } else if (cfg_.pricing.vol_method == VolMethod::OrcWing) {
            surf = orc_wing_surfaces_[prod].get();
        } else {
            surf = vol_surfaces_[prod].get();
        }

        alignas(32) double F_arr[128];
        alignas(32) double K_arr[128];
        alignas(32) double T_arr[128];
        alignas(32) double sqrt_T_arr[128];
        alignas(32) double disc_arr[128];
        alignas(32) double sigma_arr[128];
        alignas(32) uint8_t is_call_arr[128];
        alignas(32) Black76Result results[128];
        const double log_F_mid = std::log(F_mid);

        for (uint16_t bi = 0; bi < batch_n; ++bi) {
            const uint16_t oi = start + bi;
            const uint16_t opt_id = option_ids_[prod][oi];
            const Instrument& opt = instruments_[opt_id];
            F_arr[bi] = F_mid;
            K_arr[bi] = opt.strike;
            T_arr[bi] = option_T_[prod][oi];
            sqrt_T_arr[bi] = option_sqrt_T_[prod][oi];
            disc_arr[bi] = option_disc_[prod][oi];
            sigma_arr[bi] = 0.20;
            is_call_arr[bi] = (opt.option_type == OptionType::Call) ? 1 : 0;
        }

        if (cfg_.pricing.vol_method == VolMethod::OrcWing) {
            const auto* orc_surf = static_cast<const OrcWingVolSurface*>(surf);
            orc_surf->get_vols_by_strike(F_mid, K_arr, T_arr, sigma_arr, batch_n);
        } else {
            for (uint16_t bi = 0; bi < batch_n; ++bi) {
                const uint16_t oi = start + bi;
                sigma_arr[bi] = surf->get_vol(option_log_K_[prod][oi] - log_F_mid, T_arr[bi]);
            }
        }

        compute_batch_precomputed(F_arr, K_arr, T_arr, sqrt_T_arr, disc_arr,
                                  sigma_arr, is_call_arr, results, batch_n);

        for (uint16_t bi = 0; bi < batch_n; ++bi) {
            const uint16_t opt_id = option_ids_[prod][start + bi];
            Greeks greek{};
            (void)greeks_snapshot_.read(opt_id, &greek);
            greek.theta = results[bi].theta;
            greek.rho = results[bi].rho;
            greek.T = T_arr[bi];
            greeks_snapshot_.publish(opt_id, greek);
        }

        cold_greeks_offset[prod] = static_cast<uint16_t>(start + batch_n);
        if (cold_greeks_offset[prod] >= n) {
            cold_greeks_offset[prod] = 0;
            next_cold_greeks_due_ns[prod] = now + cold_greeks_interval_ns;
        }
        return true;
    };

    while (!stop_flag_.load(std::memory_order_relaxed)) {
        bool did_work = false;
        if (tick_buf_.try_pop(tick)) {
            did_work = true;

            const uint16_t id = tick.instrument_id;
            if (id < MAX_INSTRUMENTS) {
                const Instrument& instr = instruments_[id];
                if (instr.instrument_id != INVALID_INSTRUMENT_ID) {
                    tick_snapshot_.publish(id, tick);
                    publish_monitor_tick(tick);

                    const uint8_t tick_prod = instr_to_product_[id];
                    if (tick_prod < MAX_PRODUCTS
                        && cfg_.products[tick_prod].arbitrage_strategy_count > 0) {
                        ArbMarketTrigger trigger{};
                        trigger.instrument_id = id;
                        trigger.product_index = tick_prod;
                        trigger.sequence_no = tick.sequence_no;
                        trigger.recv_ts_ns = tick.recv_ts_ns;
                        (void)arb_market_trigger_buf_[tick_prod].try_push(trigger);
                    }

                    if (instr.kind == InstrumentKind::Future) {
                        const uint8_t prod = tick_prod;
                        if (prod < MAX_PRODUCTS && tick.last_price > 1e-10) {
                            pending_future_tick[prod] = tick;
                            if (pending_product[prod]) {
                                ++pending_future_tick_overwrites_[prod];  // Plain increment (single writer)
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
            if (refresh_cold_greeks_batch(product_count)) {
                did_work = true;
            }
            if (!did_work) {
                adaptive_spin_pause(spin_count);
            } else {
                spin_count = 0;  // Reset on work
            }
            continue;
        }

        const uint8_t prod = static_cast<uint8_t>(selected_prod);
        const TopOfBookTick& future_tick = pending_future_tick[prod];
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
        alignas(32) Black76QuoteResult mid_results[MAX_BATCH];
        alignas(32) Black76QuoteResult bid_results[MAX_BATCH];
        alignas(32) Black76QuoteResult ask_results[MAX_BATCH];
        alignas(64) PricingSignal sigs[MAX_BATCH];

        const uint16_t start = next_option_offset[prod];
        const uint16_t batch_n = std::min<uint16_t>(MAX_BATCH, n - start);
        const double log_F_mid = std::log(F_mid);
        const uint64_t surface_version = surface_versions_[prod];  // Plain read (eventual consistency OK)

        // Populate arrays (F, K, T, sqrt_T, disc, is_call)
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
            is_call_arr[bi] = (opt.option_type == OptionType::Call) ? 1 : 0;
        }

        // Compute volatilities (hoisted vol method dispatch eliminates branch misprediction + virtual calls)
        if (cfg_.pricing.vol_method == VolMethod::SVI) {
            const auto* svi_surf = static_cast<const SVIVolSurface*>(surf);
            for (uint16_t bi = 0; bi < batch_n; ++bi) {
                const uint16_t oi = start + bi;
                const int8_t slice_idx = option_expiry_slice_[prod][oi];
                if (slice_idx >= 0) {
                    sigma_arr[bi] = svi_surf->get_vol_cached(
                        option_log_K_[prod][oi] - log_F_mid, T_arr[bi], slice_idx);
                } else {
                    sigma_arr[bi] = svi_surf->get_vol(
                        option_log_K_[prod][oi] - log_F_mid, T_arr[bi]);
                }
            }
        } else if (cfg_.pricing.vol_method == VolMethod::OrcWing) {
            const auto* orc_surf = static_cast<const OrcWingVolSurface*>(surf);
            orc_surf->get_vols_by_strike(F_mid, K_arr, T_arr, sigma_arr, batch_n);
        } else if (cfg_.pricing.vol_method == VolMethod::Wing) {
            const auto* wing_surf = static_cast<const WingVolSurface*>(surf);
            for (uint16_t bi = 0; bi < batch_n; ++bi) {
                sigma_arr[bi] = wing_surf->get_vol_by_strike(F_mid, K_arr[bi], T_arr[bi]);
            }
        } else {
            // SABR, CubicSpline, or other generic surfaces
            for (uint16_t bi = 0; bi < batch_n; ++bi) {
                const uint16_t oi = start + bi;
                sigma_arr[bi] = surf->get_vol(option_log_K_[prod][oi] - log_F_mid, T_arr[bi]);
            }
        }

        // Fused batch pricing: computes bid, mid, ask in single pass (3× → 1×)
        compute_batch_quote_fused(F_bid_arr, F_mid_arr, F_ask_arr, K_arr,
                                  sqrt_T_arr, disc_arr, sigma_arr, is_call_arr,
                                  bid_results, mid_results, ask_results, batch_n);

        alignas(64) PricingSignal emitted_sigs[MAX_BATCH];
        uint16_t emitted_slots[MAX_BATCH];
        int emitted_count = 0;

        for (uint16_t bi = 0; bi < batch_n; ++bi) {
            const uint16_t oi = start + bi;
            const uint16_t opt_id = option_ids_[prod][oi];
            const Instrument& opt = instruments_[opt_id];
            const Black76QuoteResult& mid_res = mid_results[bi];

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
            (void)greeks_snapshot_.read(opt_id, &greek);
            greek.instrument_id = opt_id;
            greek.theo_price = mid_res.price;
            greek.delta = mid_res.delta;
            greek.gamma = mid_res.gamma;
            greek.vega = mid_res.vega;
            greek.iv = sigma_arr[bi];
            greek.T = T_arr[bi];
            greek.calc_ts_ns = now;
            greeks_snapshot_.publish(opt_id, greek);

            if (!should_emit_signal(prod, oi, sig, surface_version)) {
                ++signal_suppressed_count_[prod];  // Plain increment (single writer)
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
    apply_realtime_if_configured(cfg_.scheduling.enable_realtime,
                                 cfg_.scheduling.strategy_priority,
                                 "omm-strat");

    // Bind to local NUMA node for optimal memory access
    if (numa_available_multi_node()) {
        bind_thread_to_local_numa_node();
    }

    GatewayEvent ev{};
    TimerEvent timer_ev{};
    uint64_t coalesced_signal_seen_versions[MAX_INSTRUMENTS]{};
    uint64_t coalesced_timer_seen_versions[kCoalescedTimerSlotCount]{};
    int spin_count = 0;  // Adaptive spinning counter

    while (!stop_flag_.load(std::memory_order_relaxed)) {
        bool did_work = false;
        update_max(max_timer_queue_depth_[idx],
                   static_cast<uint32_t>(timer_buf_[idx].size_approx()));

        // Gateway events (order/quote acks, fills, cancels) arrive in bursts.
        // Batch-pop to amortize atomic overhead. Each event is independent, so
        // we can process them in any order. The burst cap prevents callback
        // floods from starving pricing signals.
        constexpr int kGatewayBatchSize = 8;
        alignas(64) GatewayEvent ev_batch[kGatewayBatchSize];
        int gateway_budget = kStrategyGatewayBurstCap;
        while (gateway_budget > 0) {
            const int batch_size = gateway_event_buf_[idx].try_pop_batch(ev_batch,
                                                                          std::min(gateway_budget, kGatewayBatchSize));
            if (batch_size == 0) break;  // No more events available
            did_work = true;
            gateway_budget -= batch_size;
            // Process all events in the batch
            for (int i = 0; i < batch_size; ++i) {
                const GatewayEvent& ev = ev_batch[i];
                switch (ev.type) {
                case GatewayEventType::OrderAck:
                    strategies_[idx]->on_order_ack(ev.order);
                    break;
                case GatewayEventType::QuoteAck:
                    if (ev.quote.instrument_id < MAX_INSTRUMENTS) {
                        const int64_t now_ns = get_monotonic_ns();
                        last_quote_ack_route_ts_[ev.quote.instrument_id] = now_ns;  // Plain store (single writer)
                        last_quote_ack_route_latency_ns_[ev.quote.instrument_id] =
                            ev.quote.ack_ts > 0 ? std::max<int64_t>(0, now_ns - ev.quote.ack_ts) : 0;  // Plain store (single writer)
                    }
                    strategies_[idx]->on_quote_ack(ev.quote);
                    break;
                case GatewayEventType::QuoteCancel:
                    if (ev.quote.instrument_id < MAX_INSTRUMENTS) {
                        const int64_t now_ns = get_monotonic_ns();
                        last_quote_cancel_route_ts_[ev.quote.instrument_id] = now_ns;  // Plain store (single writer)
                        last_quote_cancel_route_latency_ns_[ev.quote.instrument_id] =
                            ev.quote.ack_ts > 0 ? std::max<int64_t>(0, now_ns - ev.quote.ack_ts) : 0;  // Plain store (single writer)
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
        }

        int timer_budget = kStrategyTimerBurstCap;
        for (; timer_budget > 0 && timer_buf_[idx].try_pop(timer_ev); --timer_budget) {
            did_work = true;
            strategies_[idx]->on_timer(timer_ev);
        }
        const int coalesced_timers =
            drain_coalesced_timers(idx, coalesced_timer_seen_versions, timer_budget);
        if (coalesced_timers > 0) did_work = true;

        // Batch-pop pricing signals to amortize atomic overhead
        constexpr int kSignalBatchSize = 8;
        alignas(64) PricingSignal sig_batch[kSignalBatchSize];
        int signal_budget = kStrategySignalBurstCap;
        while (signal_budget > 0) {
            const int batch_size = signal_buf_[idx].try_pop_batch(sig_batch,
                                                                   std::min(signal_budget, kSignalBatchSize));
            if (batch_size == 0) break;  // No more signals available
            did_work = true;
            signal_budget -= batch_size;
            // Process all signals in the batch
            for (int i = 0; i < batch_size; ++i) {
                const PricingSignal& sig = sig_batch[i];
                if (sig.instrument_id < MAX_INSTRUMENTS) {
                    last_strategy_signal_ts_[sig.instrument_id] = get_monotonic_ns();  // Plain store (single writer)
                }
                strategies_[idx]->on_signal(sig);
            }
        }
        const int coalesced_signals =
            drain_coalesced_signals(idx, coalesced_signal_seen_versions, signal_budget);
        if (coalesced_signals > 0) did_work = true;

        if (!did_work) {
            adaptive_spin_pause(spin_count);
        } else {
            spin_count = 0;  // Reset on work
        }
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
    apply_realtime_if_configured(cfg_.scheduling.enable_realtime,
                                 cfg_.scheduling.arbitrage_priority,
                                 "omm-arb");

    // Bind to local NUMA node for optimal memory access
    if (numa_available_multi_node()) {
        bind_thread_to_local_numa_node();
    }

    GatewayEvent ev{};
    ArbMarketTrigger trigger{};
    Timestamp next_maintenance_ns = get_monotonic_ns() + kArbMaintenanceIntervalNs;
    int spin_count = 0;  // Adaptive spinning counter
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
        for (int drained = 0;
             drained < kArbMarketTriggerBurstCap && arb_market_trigger_buf_[idx].try_pop(trigger);
             ++drained) {
            did_work = true;
            const Timestamp eval_ts = get_monotonic_ns();
            for (int slot = 0; slot < cfg_.products[idx].arbitrage_strategy_count
                 && slot < MAX_ARBITRAGE_STRATEGIES_PER_PRODUCT; ++slot) {
                auto& strategy = arbitrage_strategies_[idx][slot];
                if (!strategy) continue;
                strategy->on_market_update(trigger.instrument_id, eval_ts);
            }
        }

        if (now_ns >= next_maintenance_ns) {
            did_work = true;
            next_maintenance_ns = now_ns + kArbMaintenanceIntervalNs;
            for (int slot = 0; slot < cfg_.products[idx].arbitrage_strategy_count
                 && slot < MAX_ARBITRAGE_STRATEGIES_PER_PRODUCT; ++slot) {
                auto& strategy = arbitrage_strategies_[idx][slot];
                if (!strategy) continue;
                strategy->on_timer(now_ns);
            }
        }

        if (!did_work) {
            adaptive_spin_pause(spin_count);
        } else {
            spin_count = 0;  // Reset on work
        }
    }
}

// ─── Gateway dispatcher thread ────────────────────────────────────────────────

void TradingEngine::gateway_dispatcher_loop() noexcept {
    set_thread_name("omm-gw-disp");
    pin_if_configured(cfg_.affinity.gateway_dispatcher_core);
    apply_realtime_if_configured(cfg_.scheduling.enable_realtime,
                                 cfg_.scheduling.gateway_dispatcher_priority,
                                 "omm-gw-disp");

    // Bind to local NUMA node for optimal memory access
    if (numa_available_multi_node()) {
        bind_thread_to_local_numa_node();
    }

    struct DeferredCallbackSideEffect {
        GatewayEvent event{};
    };

    int spin_count = 0;  // Adaptive spinning counter
    while (!stop_flag_.load(std::memory_order_relaxed)) {
        bool did_work = false;
        auto drain_callbacks = [&](int burst_cap) {
            DeferredCallbackSideEffect deferred[kDispatcherCallbackLeadBurstCap]{};
            int deferred_count = 0;
            GatewayEvent ev{};
            for (int drained = 0;
                 drained < burst_cap && gateway_->callback_buf.try_pop(ev);
                 ++drained) {
                did_work = true;
                const int p = ev.product_index;
                if (p >= MAX_PRODUCTS) continue;

                DeferredCallbackSideEffect normalized{};
                normalized.event = ev;
                bool route_to_arbitrage = false;
                switch (normalized.event.type) {
                case GatewayEventType::OrderAck:
                    handle_gateway_order_update(normalized.event.order,
                                                normalized.event.type);
                    route_to_arbitrage = is_arb_order_id(normalized.event.order.client_order_id);
                    break;
                case GatewayEventType::QuoteAck:
                    handle_gateway_quote_update(normalized.event.quote,
                                                normalized.event.type);
                    break;
                case GatewayEventType::QuoteCancel:
                    handle_gateway_quote_update(normalized.event.quote,
                                                normalized.event.type);
                    break;
                case GatewayEventType::QuoteReject:
                    handle_gateway_quote_update(normalized.event.quote,
                                                normalized.event.type);
                    break;
                case GatewayEventType::OrderFill:
                case GatewayEventType::QuoteFill:
                    handle_gateway_fill(&normalized.event.trade, &normalized.event.type);
                    route_to_arbitrage = is_arb_order_id(normalized.event.trade.client_order_id);
                    break;
                case GatewayEventType::OrderCancel:
                    handle_gateway_order_update(normalized.event.order,
                                                normalized.event.type);
                    route_to_arbitrage = is_arb_order_id(normalized.event.order.client_order_id);
                    break;
                case GatewayEventType::OrderReject:
                    handle_gateway_order_update(normalized.event.order,
                                                normalized.event.type);
                    route_to_arbitrage = is_arb_order_id(normalized.event.order.client_order_id);
                    break;
                default:
                    break;
                }

                if (route_to_arbitrage) {
                    while (!stop_flag_.load(std::memory_order_relaxed)
                        && !arb_event_buf_[p].try_push(normalized.event)) {
                        spin_pause();
                    }
                } else {
                    while (!stop_flag_.load(std::memory_order_relaxed)
                        && !gateway_event_buf_[p].try_push(normalized.event)) {
                        spin_pause();
                    }
                }

                deferred[deferred_count++] = normalized;
            }

            for (int i = 0; i < deferred_count; ++i) {
                DeferredCallbackSideEffect& side = deferred[i];
                switch (side.event.type) {
                case GatewayEventType::OrderAck:
                    publish_monitor_order(side.event.order);
                    defer_order_persistence(OrderPersistenceEventType::Ack,
                                            side.event.order);
                    break;
                case GatewayEventType::QuoteAck: {
                    publish_monitor_quote(side.event.quote);
                    defer_quote_persistence(QuotePersistenceEventType::Ack,
                                            side.event.quote,
                                            nullptr);
                    break;
                }
                case GatewayEventType::QuoteCancel:
                    publish_monitor_quote(side.event.quote);
                    defer_quote_persistence(QuotePersistenceEventType::Cancel,
                                            side.event.quote,
                                            nullptr);
                    break;
                case GatewayEventType::QuoteReject:
                    publish_monitor_quote(side.event.quote);
                    defer_quote_persistence(QuotePersistenceEventType::Reject,
                                            side.event.quote,
                                            nullptr);
                    break;
                case GatewayEventType::OrderFill:
                case GatewayEventType::QuoteFill: {
                    publish_monitor_trade(side.event.trade);
                    defer_trade_persistence(side.event.trade);

                    Order filled{};
                    filled.client_order_id = side.event.trade.client_order_id;
                    filled.instrument_id   = side.event.trade.instrument_id;
                    filled.product_index   = side.event.trade.product_index;
                    filled.exchange_id     = side.event.trade.exchange_id;
                    filled.side            = side.event.trade.side;
                    filled.status          = OrderStatus::Filled;
                    filled.price           = side.event.trade.fill_price;
                    filled.volume          = side.event.trade.fill_volume;
                    filled.avg_fill_price  = side.event.trade.fill_price;
                    filled.filled_volume   = side.event.trade.fill_volume;
                    filled.ack_ts          = side.event.trade.fill_ts;
                    filled.book_id         = side.event.trade.book_id;
                    publish_monitor_order(filled);
                    (void)risk_buf_.try_push(side.event.trade);
                    break;
                }
                case GatewayEventType::OrderCancel:
                    publish_monitor_order(side.event.order);
                    defer_order_persistence(OrderPersistenceEventType::Cancel,
                                            side.event.order);
                    break;
                case GatewayEventType::OrderReject:
                    publish_monitor_order(side.event.order);
                    defer_order_persistence(OrderPersistenceEventType::Reject,
                                            side.event.order);
                    break;
                default:
                    break;
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
                if (strategy_dispatch_suspended_.load(std::memory_order_acquire) && !order.is_manual) {
                    continue;
                }
                if (!order.is_manual) {
                    order.book_id = mm_book_ids_[i];
                }
                const bool sent = gateway_->send_order(order);
                publish_monitor_order(order);
                if (sent) {
                    track_live_order_submit(order);
                    defer_order_persistence(OrderPersistenceEventType::Submit, order);
                } else {
                    Order rejected = order;
                    rejected.status = OrderStatus::Rejected;
                    rejected.ack_ts = get_monotonic_ns();
                    defer_order_persistence(OrderPersistenceEventType::Reject, rejected);
                }
                if (((drained + 1) % kDispatcherCallbackInterleaveBurstCap) == 0) {
                    drain_callbacks(kDispatcherCallbackInterleaveBurstCap);
                }
            }

            Quote quote{};
            for (int drained = 0;
                 drained < kDispatcherQuoteBurstCap && quote_buf_[i].try_pop(quote);
                 ++drained) {
                did_work = true;
                if (strategy_dispatch_suspended_.load(std::memory_order_acquire)) {
                    continue;
                }
                quote.book_id = mm_book_ids_[i];
                OrderId bid_order_id = 0;
                OrderId ask_order_id = 0;
                const bool sent = gateway_->send_quote(quote, &bid_order_id, &ask_order_id);
                publish_monitor_quote(quote);
                if (sent && (quote.bid_volume > 0 || quote.ask_volume > 0)) {
                    track_live_quote_submit(quote, bid_order_id, ask_order_id);
                    defer_quote_persistence(QuotePersistenceEventType::Submit, quote, nullptr);
                } else if (!sent) {
                    Quote rejected = quote;
                    rejected.bid_status = OrderStatus::Rejected;
                    rejected.ask_status = OrderStatus::Rejected;
                    rejected.ack_ts = get_monotonic_ns();
                    defer_quote_persistence(QuotePersistenceEventType::Reject, rejected, nullptr);
                }
                if (((drained + 1) % kDispatcherCallbackInterleaveBurstCap) == 0) {
                    drain_callbacks(kDispatcherCallbackInterleaveBurstCap);
                }
            }

            ArbIntent intent{};
            for (int drained = 0;
                 drained < kDispatcherArbIntentBurstCap && arb_intent_buf_[i].try_pop(intent);
                 ++drained) {
                did_work = true;
                if (strategy_dispatch_suspended_.load(std::memory_order_acquire)) {
                    continue;
                }
                if (intent.kind == ArbIntentKind::SubmitOrder) {
                    intent.order.book_id = arb_book_id_for_type(i, intent.strategy_type);
                    const bool sent = gateway_->send_order(intent.order);
                    publish_monitor_order(intent.order);
                    if (sent) {
                        track_live_order_submit(intent.order);
                        defer_order_persistence(OrderPersistenceEventType::Submit, intent.order);
                    } else {
                        Order rejected = intent.order;
                        rejected.status = OrderStatus::Rejected;
                        rejected.ack_ts = get_monotonic_ns();
                        defer_order_persistence(OrderPersistenceEventType::Reject, rejected);
                    }
                } else if (intent.kind == ArbIntentKind::CancelOrder) {
                    gateway_->cancel_order(intent.order.client_order_id, intent.order.instrument_id);
                }
                if (((drained + 1) % kDispatcherCallbackInterleaveBurstCap) == 0) {
                    drain_callbacks(kDispatcherCallbackInterleaveBurstCap);
                }
            }
            drain_callbacks(kDispatcherCallbackInterleaveBurstCap);
        }
        if (!did_work) {
            adaptive_spin_pause(spin_count);
        } else {
            spin_count = 0;  // Reset on work
        }
    }
    gateway_dispatcher_running_.store(false, std::memory_order_release);
}

void TradingEngine::monitor_publish_loop() noexcept {
    set_thread_name("omm-monitor");

    // Bind to local NUMA node for optimal memory access
    if (numa_available_multi_node()) {
        bind_thread_to_local_numa_node();
    }

    constexpr int kMonitorPublishBurstCap = 128;
    constexpr int kMonitorBatchSize = 16;  // Larger batch for monitoring (less latency-sensitive)

    while (gateway_dispatcher_running_.load(std::memory_order_acquire)
           || !deferred_monitor_ticks_.empty_approx()
           || !deferred_monitor_orders_.empty_approx()
           || !deferred_monitor_quotes_.empty_approx()
           || !deferred_monitor_trades_.empty_approx()
           || !deferred_persist_order_events_.empty_approx()
           || !deferred_persist_quote_events_.empty_approx()
           || !deferred_persist_trades_.empty_approx()) {
        bool did_work = false;

        // Batch-drain monitoring events to reduce atomic overhead
        alignas(64) TopOfBookTick tick_batch[kMonitorBatchSize];
        int tick_budget = kMonitorPublishBurstCap;
        while (tick_budget > 0) {
            const int batch_size = deferred_monitor_ticks_.try_pop_batch(tick_batch,
                                                                          std::min(tick_budget, kMonitorBatchSize));
            if (batch_size == 0) break;
            did_work = true;
            tick_budget -= batch_size;
            for (int i = 0; i < batch_size; ++i) {
                monitor_ticks_.publish(tick_batch[i]);
            }
        }

        alignas(64) Order order_batch[kMonitorBatchSize];
        int order_budget = kMonitorPublishBurstCap;
        while (order_budget > 0) {
            const int batch_size = deferred_monitor_orders_.try_pop_batch(order_batch,
                                                                           std::min(order_budget, kMonitorBatchSize));
            if (batch_size == 0) break;
            did_work = true;
            order_budget -= batch_size;
            for (int i = 0; i < batch_size; ++i) {
                monitor_orders_.publish(order_batch[i]);
            }
        }

        alignas(64) Quote quote_batch[kMonitorBatchSize];
        int quote_budget = kMonitorPublishBurstCap;
        while (quote_budget > 0) {
            const int batch_size = deferred_monitor_quotes_.try_pop_batch(quote_batch,
                                                                           std::min(quote_budget, kMonitorBatchSize));
            if (batch_size == 0) break;
            did_work = true;
            quote_budget -= batch_size;
            for (int i = 0; i < batch_size; ++i) {
                monitor_quotes_.publish(quote_batch[i]);
            }
        }

        alignas(64) Trade trade_batch[kMonitorBatchSize];
        int trade_budget = kMonitorPublishBurstCap;
        while (trade_budget > 0) {
            const int batch_size = deferred_monitor_trades_.try_pop_batch(trade_batch,
                                                                           std::min(trade_budget, kMonitorBatchSize));
            if (batch_size == 0) break;
            did_work = true;
            trade_budget -= batch_size;
            for (int i = 0; i < batch_size; ++i) {
                monitor_trades_.publish(trade_batch[i]);
            }
        }

        if (repository_) {
            // Batch-drain persistence events to reduce atomic overhead
            constexpr int kPersistenceBatchSize = 16;

            alignas(64) OrderPersistenceEvent order_event_batch[kPersistenceBatchSize];
            int order_budget = kMonitorPublishBurstCap;
            while (order_budget > 0) {
                const int batch_size = deferred_persist_order_events_.try_pop_batch(
                    order_event_batch, std::min(order_budget, kPersistenceBatchSize));
                if (batch_size == 0) break;
                did_work = true;
                order_budget -= batch_size;

                const int enqueued = repository_->enqueue_order_events_batch(order_event_batch, batch_size);
                if (enqueued < batch_size) {
                    deferred_persistence_drops_.fetch_add(batch_size - enqueued, std::memory_order_relaxed);
                }
            }

            alignas(64) QuotePersistenceEvent quote_event_batch[kPersistenceBatchSize];
            int quote_budget = kMonitorPublishBurstCap;
            while (quote_budget > 0) {
                const int batch_size = deferred_persist_quote_events_.try_pop_batch(
                    quote_event_batch, std::min(quote_budget, kPersistenceBatchSize));
                if (batch_size == 0) break;
                did_work = true;
                quote_budget -= batch_size;

                const int enqueued = repository_->enqueue_quote_events_batch(quote_event_batch, batch_size);
                if (enqueued < batch_size) {
                    deferred_persistence_drops_.fetch_add(batch_size - enqueued, std::memory_order_relaxed);
                }
            }

            alignas(64) Trade trade_batch[kPersistenceBatchSize];
            int trade_budget = kMonitorPublishBurstCap;
            while (trade_budget > 0) {
                const int batch_size = deferred_persist_trades_.try_pop_batch(
                    trade_batch, std::min(trade_budget, kPersistenceBatchSize));
                if (batch_size == 0) break;
                did_work = true;
                trade_budget -= batch_size;

                const int enqueued = repository_->enqueue_trades_batch(trade_batch, batch_size);
                if (enqueued < batch_size) {
                    deferred_persistence_drops_.fetch_add(batch_size - enqueued, std::memory_order_relaxed);
                }
            }
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
    apply_realtime_if_configured(cfg_.scheduling.enable_realtime,
                                 cfg_.scheduling.vol_fitter_priority,
                                 "omm-volfitter");
    pin_if_configured(cfg_.affinity.vol_fitter_core);

    // Bind to local NUMA node for optimal memory access
    if (numa_available_multi_node()) {
        bind_thread_to_local_numa_node();
    }

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
                    TopOfBookTick t{};
                    if (!tick_snapshot_.read(id, &t)) continue;
                    if (t.last_price > 1e-10) {
                        for (int e = 0; e < MAX_EXPIRIES; ++e)
                            if (fwd_prices[e] < 1e-10) fwd_prices[e] = t.last_price;
                    }
                    continue;
                }

                if (instr.kind != InstrumentKind::Option) continue;

                TopOfBookTick t{};
                if (!tick_snapshot_.read(id, &t)) continue;
                if (t.recv_ts_ns == 0) continue;  // no tick yet

                // Compute trading-session-adjusted T for this instrument.
                double T = option_time_to_expiry_years(instr);
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
                        TopOfBookTick ut{};
                        if (!tick_snapshot_.read(instr.underlying_id, &ut)) continue;
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

                TopOfBookTick t{};
                if (!tick_snapshot_.read(id, &t)) continue;
                if (t.recv_ts_ns == 0) continue;

                double T = option_time_to_expiry_years(instr);
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
                    ++surface_versions_[p];  // Plain increment (single writer)
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
                    const OrcWingParams* seed = nullptr;
                    const OrcWingVolSurface* cur =
                        static_cast<const OrcWingVolSurface*>(orc_wing_surfaces_[p].get());
                    for (int s = 0; s < cur->n_slices; ++s) {
                        if (std::fabs(cur->slices[s].expiry_T - expiry_Ts[e]) < 1.0 / 365.0) {
                            seed = &cur->slices[s];
                            break;
                        }
                    }
                    const bool ok = fit_orc_wing_slice_seeded(
                        strikes + expiry_start[e],
                        mkt_vols + expiry_start[e],
                        cnt, fwd_prices[e], expiry_Ts[e], seed, params);

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
                    ++surface_versions_[p];  // Plain increment (single writer)
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
                    ++surface_versions_[p];  // Plain increment (single writer)
                }
            }
        }
    }
}

// ─── Risk monitor thread ──────────────────────────────────────────────────────

void TradingEngine::risk_monitor_loop() noexcept {
    set_thread_name("omm-risk");
    pin_if_configured(cfg_.affinity.risk_monitor_core);
    apply_realtime_if_configured(cfg_.scheduling.enable_realtime,
                                 cfg_.scheduling.risk_monitor_priority,
                                 "omm-risk");
    int64_t last_snapshot_ts = 0;
    int64_t last_limit_check_ts = 0;
    uint8_t last_breach_mask = 0;

    while (!stop_flag_.load(std::memory_order_relaxed)) {
        bool did_work = false;
        // Process fills from risk_buf_
        Trade trade{};
        while (risk_buf_.try_pop(trade)) {
            did_work = true;
            post_risk_.on_fill(trade);
            std::lock_guard<std::mutex> lock(book_state_mutex_);
            rebuild_book_position_locked(trade);
        }

        const int64_t now_ns = get_monotonic_ns();
        if (did_work || now_ns - last_limit_check_ts >= kRiskCheckIntervalNs) {
            std::array<Greeks, MAX_INSTRUMENTS> greeks{};
            (void)read_all_greeks(greeks.data(), n_instruments_);
            post_risk_.check_limits(greeks.data(), n_instruments_);
            last_limit_check_ts = now_ns;

            const uint8_t breach_mask =
                (post_risk_.position_breach() ? 1u << 0 : 0u)
                | (post_risk_.delta_breach() ? 1u << 1 : 0u)
                | (post_risk_.gamma_breach() ? 1u << 2 : 0u)
                | (post_risk_.vega_breach() ? 1u << 3 : 0u);
            if (breach_mask != 0 && breach_mask != last_breach_mask) {
                OMM_LOG_WARN("risk", "breach flags: pos={} delta={} gamma={} vega={}",
                             (int)post_risk_.position_breach(),
                             (int)post_risk_.delta_breach(),
                             (int)post_risk_.gamma_breach(),
                             (int)post_risk_.vega_breach());
            }
            last_breach_mask = breach_mask;
        }

        if (repository_) {
            const int64_t snapshot_interval_ns =
                static_cast<int64_t>(cfg_.persistence.snapshot_interval_ms) * 1'000'000LL;
            if (snapshot_interval_ns > 0 && now_ns - last_snapshot_ts >= snapshot_interval_ns) {
                PositionSnapshotEvent snapshot{};
                snapshot.snapshot_ts = now_ns;
                snapshot.n_instruments = n_instruments_;
                const Position* positions = post_risk_.positions();
                for (uint16_t i = 0; i < n_instruments_; ++i) {
                    snapshot.positions[i] = positions[i];
                }
                if (!repository_->enqueue_positions_snapshot(snapshot)) {
                    OMM_LOG_WARN("repo", "position snapshot queue full");
                }
                last_snapshot_ts = now_ns;
            }
        }

        const int64_t next_snapshot_due = repository_
            ? last_snapshot_ts
                + static_cast<int64_t>(cfg_.persistence.snapshot_interval_ms) * 1'000'000LL
            : now_ns + kRiskIdleSleepCapNs;
        const int64_t next_limit_due = last_limit_check_ts + kRiskCheckIntervalNs;
        const int64_t next_due = std::min(next_snapshot_due, next_limit_due);
        const int64_t sleep_ns = std::max<int64_t>(0, next_due - get_monotonic_ns());
        if (!did_work) {
            sleep_for_ns_interruptible(stop_flag_, sleep_ns, kRiskIdleSleepCapNs);
        }
    }
}

// ─── Timer thread ─────────────────────────────────────────────────────────────

void TradingEngine::timer_loop() noexcept {
    set_thread_name("omm-timer");
    pin_if_configured(cfg_.affinity.timer_core);
    apply_realtime_if_configured(cfg_.scheduling.enable_realtime,
                                 cfg_.scheduling.timer_priority,
                                 "omm-timer");

    // Bind to local NUMA node for optimal memory access
    if (numa_available_multi_node()) {
        bind_thread_to_local_numa_node();
    }

    int64_t last_hedge_ns   = get_monotonic_ns();
    int64_t last_quote_refresh_ns = last_hedge_ns;
    int64_t last_T_refresh_ns = last_hedge_ns;
    const int64_t hedge_interval_ns =
        static_cast<int64_t>(cfg_.timer.hedge_check_interval_ms) * 1'000'000LL;
    const int64_t quote_refresh_interval_ns =
        static_cast<int64_t>(cfg_.timer.quote_refresh_interval_ms) * 1'000'000LL;
    static constexpr int64_t T_REFRESH_NS = 1'000'000'000LL;  // 1 second

    while (!stop_flag_.load(std::memory_order_relaxed)) {
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

        int64_t next_deadline = last_T_refresh_ns + T_REFRESH_NS;
        if (hedge_interval_ns > 0) {
            next_deadline = std::min(next_deadline, last_hedge_ns + hedge_interval_ns);
        }
        if (quote_refresh_interval_ns > 0) {
            next_deadline = std::min(next_deadline,
                                     last_quote_refresh_ns + quote_refresh_interval_ns);
        }
        const int64_t sleep_ns =
            std::max<int64_t>(0, next_deadline - get_monotonic_ns());
        sleep_for_ns_interruptible(stop_flag_, sleep_ns, kTimerIdleSleepCapNs);
    }
}

uint64_t TradingEngine::book_position_key(BookId book_id,
                                          uint16_t instrument_id) noexcept {
    return (static_cast<uint64_t>(book_id) << 16) | instrument_id;
}

BookId TradingEngine::arb_book_id_for_type(int product_idx,
                                           ArbitrageStrategyType type) const noexcept {
    const int slot = find_arbitrage_slot(product_idx, type);
    if (slot < 0 || product_idx < 0 || product_idx >= MAX_PRODUCTS) {
        return INVALID_BOOK_ID;
    }
    return arb_book_ids_[product_idx][slot];
}

void TradingEngine::rebuild_book_position_locked(const Trade& trade) noexcept {
    if (trade.book_id == INVALID_BOOK_ID) return;
    BookPosition& position = book_positions_[book_position_key(trade.book_id, trade.instrument_id)];
    position.book_id = trade.book_id;
    apply_fill_to_position(&position, trade);
}

int TradingEngine::book_positions_snapshot(BookPosition* out, int max_count) const noexcept {
    if (out == nullptr || max_count <= 0) return 0;
    std::lock_guard<std::mutex> lock(book_state_mutex_);
    int count = 0;
    for (const auto& entry : book_positions_) {
        const BookPosition& pos = entry.second;
        if (pos.net_position == 0
            && pos.long_position == 0
            && pos.short_position == 0
            && pos.realized_pnl == 0.0) {
            continue;
        }
        if (count >= max_count) break;
        out[count++] = pos;
    }
    return count;
}

int TradingEngine::book_portfolios_snapshot(BookPortfolioGreeks* out,
                                            int max_count) const noexcept {
    if (out == nullptr || max_count <= 0) return 0;
    std::unordered_map<uint64_t, BookPortfolioGreeks> portfolios;
    const Timestamp now_ns = get_monotonic_ns();

    std::lock_guard<std::mutex> lock(book_state_mutex_);
    for (const auto& entry : book_positions_) {
        const BookPosition& pos = entry.second;
        if (pos.instrument_id >= MAX_INSTRUMENTS || pos.book_id == INVALID_BOOK_ID) continue;
        Greeks greeks{};
        (void)greeks_snapshot_.read(pos.instrument_id, &greeks);
        const double net = static_cast<double>(pos.net_position);
        const double avg_entry = pos.net_position > 0 ? pos.avg_long_price : pos.avg_short_price;
        const double unrealized = net * (greeks.theo_price - avg_entry);

        const auto accumulate = [&](uint8_t product_index) {
            const uint64_t key = (static_cast<uint64_t>(pos.book_id) << 8) | product_index;
            auto& portfolio = portfolios[key];
            portfolio.book_id = pos.book_id;
            portfolio.product_index = product_index;
            portfolio.net_delta += greeks.delta * net;
            portfolio.net_gamma += greeks.gamma * net;
            portfolio.net_vega += greeks.vega * net;
            portfolio.net_theta += greeks.theta * net;
            portfolio.pnl_realized += pos.realized_pnl;
            portfolio.pnl_unrealized += unrealized;
            portfolio.calc_ts = now_ns;
        };

        accumulate(pos.product_index);
        accumulate(0xFF);
    }

    int count = 0;
    for (const auto& entry : portfolios) {
        if (count >= max_count) break;
        out[count++] = entry.second;
    }
    return count;
}

void TradingEngine::note_session_activated() noexcept {
    strategy_dispatch_suspended_.store(false, std::memory_order_release);
}

void TradingEngine::shutdown_on_zero_sessions() noexcept {
    strategy_dispatch_suspended_.store(true, std::memory_order_release);
    for (int i = 0; i < cfg_.product_count && i < MAX_PRODUCTS; ++i) {
        MMParamsConfig mm = mm_params_[i].snapshot();
        if (mm.enabled) {
            mm.enabled = false;
            mm_params_[i].apply(mm);
            persist_mm_params_update(i, mm);
        }
        for (int slot = 0;
             slot < cfg_.products[i].arbitrage_strategy_count
                 && slot < MAX_ARBITRAGE_STRATEGIES_PER_PRODUCT;
             ++slot) {
            ArbParamsConfig arb = arb_params_[i][slot].snapshot();
            if (arb.enabled) {
                arb.enabled = false;
                arb_params_[i][slot].apply(arb);
                persist_arb_params_update(i, arb_strategy_types_[i][slot], arb);
            }
        }
    }
    cancel_all_live_orders_and_quotes();
}

void TradingEngine::track_live_order_submit(const Order& order) noexcept {
    std::lock_guard<std::mutex> lock(live_state_mutex_);
    LiveOrderState state{};
    state.order = order;
    if (!live_orders_.insert(order.client_order_id, state)) {
        live_state_drops_.fetch_add(1, std::memory_order_relaxed);
    }
}

void TradingEngine::track_live_quote_submit(const Quote& quote,
                                            OrderId bid_order_id,
                                            OrderId ask_order_id) noexcept {
    std::lock_guard<std::mutex> lock(live_state_mutex_);
    LiveQuoteState* existing = live_quotes_.find(quote.client_quote_id);
    if (existing != nullptr) {
        // Remove old quote leg mappings if they exist
        if (existing->bid_order_id != 0) {
            quote_leg_to_quote_.erase(existing->bid_order_id);
        }
        if (existing->ask_order_id != 0) {
            quote_leg_to_quote_.erase(existing->ask_order_id);
        }
    }

    LiveQuoteState state{};
    state.quote = quote;
    state.remaining_bid = quote.bid_volume;
    state.remaining_ask = quote.ask_volume;
    state.bid_order_id = bid_order_id;
    state.ask_order_id = ask_order_id;

    // Map quote leg order IDs to quote ID for tracking
    if (bid_order_id != 0) {
        (void)quote_leg_to_quote_.insert(bid_order_id, quote.client_quote_id);
    }
    if (ask_order_id != 0) {
        (void)quote_leg_to_quote_.insert(ask_order_id, quote.client_quote_id);
    }
    if (!live_quotes_.insert(quote.client_quote_id, state)) {
        live_state_drops_.fetch_add(1, std::memory_order_relaxed);
    }
}

void TradingEngine::handle_gateway_order_update(Order& order,
                                                GatewayEventType type) noexcept {
    std::lock_guard<std::mutex> lock(live_state_mutex_);
    LiveOrderState* state = live_orders_.find(order.client_order_id);
    if (state != nullptr) {
        Order& live = state->order;
        order.book_id = live.book_id;
        order.product_index = live.product_index;
        order.account_id = live.account_id;
        if (order.exchange_id.empty()) {
            order.exchange_id = live.exchange_id;
        }
        if (order.exchange_order_id != 0) {
            live.exchange_order_id = order.exchange_order_id;
        } else {
            order.exchange_order_id = live.exchange_order_id;
        }
        if (type == GatewayEventType::OrderCancel || type == GatewayEventType::OrderReject) {
            live_orders_.erase(order.client_order_id);
        }
        return;
    }

    QuoteId* quote_id = quote_leg_to_quote_.find(order.client_order_id);
    if (quote_id == nullptr) return;
    LiveQuoteState* live_quote = live_quotes_.find(*quote_id);
    if (live_quote == nullptr) return;
    order.book_id = live_quote->quote.book_id;
    order.product_index = live_quote->quote.product_index;
    order.account_id = live_quote->quote.account_id;
    if (order.exchange_id.empty()) {
        order.exchange_id = live_quote->quote.exchange_id;
    }
}

void TradingEngine::handle_gateway_quote_update(Quote& quote,
                                                GatewayEventType type) noexcept {
    std::lock_guard<std::mutex> lock(live_state_mutex_);
    LiveQuoteState* state = live_quotes_.find(quote.client_quote_id);
    if (state == nullptr) return;

    LiveQuoteState& live = *state;
    quote.book_id = live.quote.book_id;
    quote.product_index = live.quote.product_index;
    quote.account_id = live.quote.account_id;
    if (quote.exchange_id.empty()) {
        quote.exchange_id = live.quote.exchange_id;
    }
    live.quote.exchange_quote_id = quote.exchange_quote_id != 0
        ? quote.exchange_quote_id
        : live.quote.exchange_quote_id;
    live.quote.bid_status = quote.bid_status;
    live.quote.ask_status = quote.ask_status;
    live.quote.ack_ts = quote.ack_ts != 0 ? quote.ack_ts : live.quote.ack_ts;

    if (type == GatewayEventType::QuoteCancel || type == GatewayEventType::QuoteReject) {
        live_quotes_.erase(quote.client_quote_id);
    }
}

void TradingEngine::handle_gateway_fill(Trade* trade,
                                        GatewayEventType* type) noexcept {
    if (trade == nullptr || type == nullptr) return;
    std::lock_guard<std::mutex> lock(live_state_mutex_);

    if (*type == GatewayEventType::QuoteFill) {
        const QuoteId direct_quote_id = static_cast<QuoteId>(trade->client_order_id);
        LiveQuoteState* quote_state = live_quotes_.find(direct_quote_id);
        if (quote_state != nullptr) {
            LiveQuoteState& live = *quote_state;
            trade->book_id = live.quote.book_id;
            trade->product_index = live.quote.product_index;
            trade->account_id = live.quote.account_id;
            if (trade->exchange_id.empty()) {
                trade->exchange_id = live.quote.exchange_id;
            }
            if (trade->side == Side::Buy) {
                live.remaining_bid = std::max<Volume>(0, live.remaining_bid - trade->fill_volume);
            } else {
                live.remaining_ask = std::max<Volume>(0, live.remaining_ask - trade->fill_volume);
            }
            if (live.remaining_bid <= 0 && live.remaining_ask <= 0) {
                live_quotes_.erase(direct_quote_id);
            }
            return;
        }
    }

    QuoteId* mapped_quote_id = quote_leg_to_quote_.find(trade->client_order_id);
    if (mapped_quote_id != nullptr) {
        const QuoteId quote_id = *mapped_quote_id;
        LiveQuoteState* quote_state = live_quotes_.find(quote_id);
        if (quote_state != nullptr) {
            LiveQuoteState& live = *quote_state;
            trade->book_id = live.quote.book_id;
            trade->product_index = live.quote.product_index;
            trade->account_id = live.quote.account_id;
            if (trade->exchange_id.empty()) {
                trade->exchange_id = live.quote.exchange_id;
            }
            trade->client_order_id = quote_id;
            *type = GatewayEventType::QuoteFill;
            if (trade->side == Side::Buy) {
                live.remaining_bid = std::max<Volume>(0, live.remaining_bid - trade->fill_volume);
            } else {
                live.remaining_ask = std::max<Volume>(0, live.remaining_ask - trade->fill_volume);
            }
            if (live.remaining_bid <= 0 && live.remaining_ask <= 0) {
                live_quotes_.erase(quote_id);
            }
            return;
        }
    }

    LiveOrderState* order_state = live_orders_.find(trade->client_order_id);
    if (order_state == nullptr) return;

    Order& live = order_state->order;
    trade->book_id = live.book_id;
    trade->product_index = live.product_index;
    trade->account_id = live.account_id;
    if (trade->exchange_id.empty()) {
        trade->exchange_id = live.exchange_id;
    }
    const Volume prior_filled = live.filled_volume;
    live.filled_volume += trade->fill_volume;
    const double total_notional =
        live.avg_fill_price * static_cast<double>(prior_filled)
        + trade->fill_price * static_cast<double>(trade->fill_volume);
    if (live.filled_volume > 0) {
        live.avg_fill_price = total_notional / static_cast<double>(live.filled_volume);
    }
    if (live.filled_volume >= live.volume) {
        live_orders_.erase(trade->client_order_id);
    }
}

void TradingEngine::cancel_all_live_orders_and_quotes() noexcept {
    std::array<Order, MAX_OPEN_ORDERS> orders{};
    std::size_t order_count = 0;
    std::array<LiveQuoteState, MAX_OPEN_ORDERS> quotes{};
    std::size_t quote_count = 0;
    {
        std::lock_guard<std::mutex> lock(live_state_mutex_);
        live_orders_.for_each([&](OrderId, const LiveOrderState& state) noexcept {
            if (order_count < orders.size()) orders[order_count++] = state.order;
        });
        live_quotes_.for_each([&](QuoteId, const LiveQuoteState& state) noexcept {
            if (quote_count < quotes.size()) quotes[quote_count++] = state;
        });
    }

    if (!gateway_ || !gateway_->is_connected()) return;
    for (std::size_t i = 0; i < order_count; ++i) {
        const Order& order = orders[i];
        (void)gateway_->cancel_order(order.client_order_id, order.instrument_id);
    }
    for (std::size_t i = 0; i < quote_count; ++i) {
        const LiveQuoteState& quote = quotes[i];
        (void)gateway_->cancel_quote(quote.quote.client_quote_id, quote.quote.instrument_id);
    }
}

// ─── Manual order / cancel (called from gRPC server thread) ──────────────────

bool TradingEngine::submit_manual_order(const Order& o) noexcept {
    if (!gateway_ || !gateway_->is_connected()) return false;
    // Route through the product's order buffer so the gateway dispatcher sends it
    uint8_t prod = (o.product_index < MAX_PRODUCTS) ? o.product_index : 0;
    return order_buf_[prod].try_push(o);
}

void TradingEngine::persist_mm_params_update(int product_index,
                                             const MMParamsConfig& params) noexcept {
    if (!repository_ || product_index < 0 || product_index >= MAX_PRODUCTS) return;
    MMParamsPersistenceEvent event{};
    event.product_index = static_cast<uint8_t>(product_index);
    event.params = params;
    event.update_ts = get_monotonic_ns();
    if (!repository_->enqueue_mm_params(event)) {
        OMM_LOG_WARN("repo", "mm params queue full product={}", product_index);
    }
}

void TradingEngine::persist_arb_params_update(int product_index,
                                              ArbitrageStrategyType type,
                                              const ArbParamsConfig& params) noexcept {
    if (!repository_ || product_index < 0 || product_index >= MAX_PRODUCTS) return;
    ArbParamsPersistenceEvent event{};
    event.product_index = static_cast<uint8_t>(product_index);
    event.strategy_type = type;
    event.params = params;
    event.update_ts = get_monotonic_ns();
    if (!repository_->enqueue_arb_params(event)) {
        OMM_LOG_WARN("repo", "arb params queue full product={} type={}",
                     product_index, static_cast<int>(type));
    }
}

void TradingEngine::persist_risk_limits_update(const SoftRiskConfig& cfg) noexcept {
    if (!repository_) return;
    RiskParamsPersistenceEvent event{};
    event.params = cfg;
    event.update_ts = get_monotonic_ns();
    if (!repository_->enqueue_risk_params(event)) {
        OMM_LOG_WARN("repo", "risk params queue full");
    }
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
