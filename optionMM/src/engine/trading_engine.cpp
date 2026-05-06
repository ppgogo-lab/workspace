#include "engine/trading_engine.h"
#include "engine_loop_common.h"
#include "engine_workers.h"
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

// TradingEngine still owns the shared memory layout and public facade.
// Long-running loops live in *_worker.cpp files; helpers here are for shared
// state that multiple workers need to access through the engine.

// Latest-only mailbox protocol used for coalesced signals/timers.
// Odd version means a writer is in progress; even version means the slot can be
// copied. Consumers track the last even version they processed.
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

// Shared position accounting for post-trade risk and per-book snapshots.
// Keep this templated so Position and BookPosition stay in sync without
// duplicating the fill-price/realized-PnL rules.
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
    // Constructor only wires static ownership and config-derived state.
    // Anything that depends on gateway instruments or repository contents is
    // initialized in start(), after the engine has a concrete runtime context.
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
    for (int i = 0; i < MAX_PRODUCTS; ++i) {
        product_base_offset_type_[i].store(
            static_cast<uint8_t>(BaseOffsetType::Price), std::memory_order_relaxed);
        product_base_offset_value_[i].store(0.0, std::memory_order_relaxed);
    }
    for (int i = 0; i < cfg_.product_count && i < MAX_PRODUCTS; ++i) {
        mm_params_[i].apply(cfg_.products[i].params);
        (void)set_product_pricing(i, cfg_.products[i].pricing);
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

ProductPricingConfig TradingEngine::product_pricing(int i) const noexcept {
    ProductPricingConfig pricing{};
    if (i < 0 || i >= MAX_PRODUCTS) return pricing;
    pricing.base_offset_type = static_cast<BaseOffsetType>(
        product_base_offset_type_[i].load(std::memory_order_relaxed));
    pricing.base_offset_value = product_base_offset_value_[i].load(std::memory_order_relaxed);
    return pricing;
}

bool TradingEngine::set_product_pricing(int i, const ProductPricingConfig& pricing) noexcept {
    if (i < 0 || i >= cfg_.product_count || i >= MAX_PRODUCTS) return false;
    product_base_offset_type_[i].store(
        static_cast<uint8_t>(pricing.base_offset_type), std::memory_order_relaxed);
    product_base_offset_value_[i].store(pricing.base_offset_value, std::memory_order_relaxed);
    return true;
}

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
        instruments_[i].tick_size = std::max(0.0, instruments_[i].tick_size);
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
    // Startup order matters:
    // 1. instrument registry builds routing tables used by every worker;
    // 2. persistence/calendar can override config bootstrap state;
    // 3. strategies/surfaces are initialized before any worker can read them;
    // 4. gateway dispatcher starts before producers so acknowledgements cannot
    //    be missed during startup.
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
    gateway_dispatcher_thread_ = std::thread([this] { GatewayDispatcherWorker(*this).run(); });
    pricer_thread_              = std::thread([this] { PricerWorker(*this).run(); });

    for (int i = 0; i < cfg_.product_count && i < MAX_PRODUCTS; ++i) {
        strategy_threads_[i] = std::thread([this, i] { StrategyWorker(*this, i).run(); });
        if (cfg_.products[i].arbitrage_strategy_count > 0) {
            arb_threads_[i] = std::thread([this, i] { ArbitrageWorker(*this, i).run(); });
        }
    }

    if (monitoring_deferred_mode() || repository_) {
        monitor_publisher_thread_ = std::thread([this] { MonitorPublisherWorker(*this).run(); });
    }
    vol_fitter_thread_    = std::thread([this] { VolFitterWorker(*this).run(); });
    risk_monitor_thread_  = std::thread([this] { RiskMonitorWorker(*this).run(); });
    timer_thread_         = std::thread([this] { TimerWorker(*this).run(); });

    if (feed_) feed_->start();
}

void TradingEngine::stop() noexcept {
    // stop_flag_ asks workers to exit their loops. Joining before persistence
    // shutdown lets deferred events drain into repository queues where possible.
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

// Monitoring accessors below intentionally tolerate eventually consistent
// single-writer counters. They are read by UI/gRPC paths and must not stall hot
// pricing or strategy threads.
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

// Pricing signals can arrive faster than a strategy thread can consume them.
// For each option slot, keep only the latest signal and queue the slot index as
// a wake-up hint. If the index queue overflows, the consumer rescans all slots.
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

// Timer events are latest-only by type: hedge checks and quote refreshes are
// periodic nudges, so redundant queued copies can be collapsed safely.
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

// Hot workers call publish_monitor_* directly. Depending on config, events are
// either published inline or deferred to MonitorPublisherWorker to keep the hot
// path from doing heavier history/persistence work.
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

// Book positions are keyed by book + instrument. The risk worker owns updates;
// readers take book_state_mutex_ to build consistent snapshots for monitoring.
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
    // Zero active sessions is treated as a protective shutdown: prevent new
    // strategy dispatch, persist disabled params, and cancel outstanding live
    // orders/quotes through the gateway.
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
    request_cancel_all_live_orders_and_quotes();
}

// The gateway may report fills/acks with only exchange-side identifiers. These
// live-state helpers restore product/book/account metadata before events are
// routed to strategies, monitoring, persistence, and risk.
void TradingEngine::track_live_order_submit(const Order& order) noexcept {
    LiveOrderState state{};
    state.order = order;
    if (!live_orders_.insert(order.client_order_id, state)) {
        live_state_drops_.fetch_add(1, std::memory_order_relaxed);
    }
}

void TradingEngine::track_live_quote_submit(const Quote& quote,
                                            OrderId bid_order_id,
                                            OrderId ask_order_id) noexcept {
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

void TradingEngine::request_cancel_all_live_orders_and_quotes() noexcept {
    cancel_all_live_requested_.store(true, std::memory_order_release);
}

void TradingEngine::cancel_all_live_orders_and_quotes() noexcept {
    std::array<Order, MAX_OPEN_ORDERS> orders{};
    std::size_t order_count = 0;
    std::array<LiveQuoteState, MAX_OPEN_ORDERS> quotes{};
    std::size_t quote_count = 0;
    live_orders_.for_each([&](OrderId, const LiveOrderState& state) noexcept {
        if (order_count < orders.size()) orders[order_count++] = state.order;
    });
    live_quotes_.for_each([&](QuoteId, const LiveQuoteState& state) noexcept {
        if (quote_count < quotes.size()) quotes[quote_count++] = state;
    });

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
