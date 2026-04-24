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
#include <ctime>

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
    if (!enabled || priority <= 0) return;
    if (!try_set_realtime_priority(priority)) {
        OMM_LOG_WARN("sched",
                     "failed to set SCHED_FIFO priority={} for {} (requires CAP_SYS_NICE/root)",
                     priority,
                     thread_name);
    }
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

void merge_order_recovery(GatewayOrderRecoveryHandle* dst,
                          const GatewayOrderRecoveryHandle& src) noexcept {
    if (dst == nullptr) return;
    dst->valid = dst->valid || src.valid;
    dst->is_quote_leg = dst->is_quote_leg || src.is_quote_leg;
    if (dst->client_quote_id == 0) dst->client_quote_id = src.client_quote_id;
    if (src.exchange_local_id[0] != '\0') {
        std::strncpy(dst->exchange_local_id, src.exchange_local_id,
                     sizeof(dst->exchange_local_id) - 1);
        dst->exchange_local_id[sizeof(dst->exchange_local_id) - 1] = '\0';
    }
    if (src.order_sys_id[0] != '\0') {
        std::strncpy(dst->order_sys_id, src.order_sys_id,
                     sizeof(dst->order_sys_id) - 1);
        dst->order_sys_id[sizeof(dst->order_sys_id) - 1] = '\0';
    }
}

void merge_quote_recovery(GatewayQuoteRecoveryHandle* dst,
                          const GatewayQuoteRecoveryHandle& src) noexcept {
    if (dst == nullptr) return;
    dst->valid = dst->valid || src.valid;
    if (dst->bid_order_id == 0) dst->bid_order_id = src.bid_order_id;
    if (dst->ask_order_id == 0) dst->ask_order_id = src.ask_order_id;

    auto copy_if_present = [](char* dst_buf, std::size_t dst_size, const char* src_buf) noexcept {
        if (src_buf == nullptr || src_buf[0] == '\0') return;
        std::strncpy(dst_buf, src_buf, dst_size - 1);
        dst_buf[dst_size - 1] = '\0';
    };
    copy_if_present(dst->quote_local_id, sizeof(dst->quote_local_id), src.quote_local_id);
    copy_if_present(dst->quote_sys_id, sizeof(dst->quote_sys_id), src.quote_sys_id);
    copy_if_present(dst->bid_local_id, sizeof(dst->bid_local_id), src.bid_local_id);
    copy_if_present(dst->ask_local_id, sizeof(dst->ask_local_id), src.ask_local_id);
    copy_if_present(dst->bid_order_sys_id, sizeof(dst->bid_order_sys_id), src.bid_order_sys_id);
    copy_if_present(dst->ask_order_sys_id, sizeof(dst->ask_order_sys_id), src.ask_order_sys_id);
}

bool has_order_recovery(const GatewayOrderRecoveryHandle& handle) noexcept {
    return handle.valid
        || handle.is_quote_leg
        || handle.client_quote_id != 0
        || handle.exchange_local_id[0] != '\0'
        || handle.order_sys_id[0] != '\0';
}

bool has_quote_recovery(const GatewayQuoteRecoveryHandle& handle) noexcept {
    return handle.valid
        || handle.bid_order_id != 0
        || handle.ask_order_id != 0
        || handle.quote_local_id[0] != '\0'
        || handle.quote_sys_id[0] != '\0'
        || handle.bid_local_id[0] != '\0'
        || handle.ask_local_id[0] != '\0'
        || handle.bid_order_sys_id[0] != '\0'
        || handle.ask_order_sys_id[0] != '\0';
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

void TradingEngine::rebuild_book_state_from_history() noexcept {
    if (!repository_) return;
    std::vector<Trade> trades;
    if (!repository_->load_trade_history(&trades)) {
        OMM_LOG_WARN("repo", "failed to load trade history for book rebuild");
        return;
    }
    std::lock_guard<std::mutex> lock(book_state_mutex_);
    book_positions_.clear();
    for (const Trade& trade : trades) {
        rebuild_book_position_locked(trade);
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

    RecoveryState recovery{};
    if (!repository_->load_recovery_state(&recovery)) {
        OMM_LOG_WARN("repo", "failed to load recovery state");
        repository_->start();
        return;
    }

    apply_recovery_state(recovery);
    rebuild_book_state_from_history();
    seed_gateway_recovery(recovery);
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
    request_recovery_cancels(recovery);
}

void TradingEngine::apply_recovery_state(const RecoveryState& state) noexcept {
    if (!state.positions.empty()) {
        post_risk_.restore_positions(state.positions.data(),
                                     static_cast<uint16_t>(state.positions.size()));
    }
    if (state.has_risk_params) {
        post_risk_.set_limits(state.risk_params);
    }
    for (int i = 0; i < cfg_.product_count && i < MAX_PRODUCTS; ++i) {
        if (state.mm_params[i].valid) {
            mm_params_[i].apply(state.mm_params[i].params);
        }
    }
    for (const auto& entry : state.arb_params) {
        if (!entry.valid) continue;
        AtomicArbParams* params = arbitrage_params(entry.product_index, entry.strategy_type);
        if (params != nullptr) params->apply(entry.params);
    }
    {
        std::lock_guard<std::mutex> lock(live_state_mutex_);
        live_orders_.clear();
        live_quotes_.clear();
        quote_leg_to_quote_.clear();
        for (const auto& order : state.live_orders) {
            LiveOrderState live{};
            live.order = order.order;
            live.recovery = order.recovery;
            live_orders_.emplace(order.order.client_order_id, live);
            if (order.recovery.is_quote_leg && order.recovery.client_quote_id != 0) {
                quote_leg_to_quote_[order.order.client_order_id] = order.recovery.client_quote_id;
            }
        }
        for (const auto& quote : state.live_quotes) {
            LiveQuoteState live{};
            live.quote = quote.quote;
            live.recovery = quote.recovery;
            live.remaining_bid = quote.quote.bid_volume;
            live.remaining_ask = quote.quote.ask_volume;
            live_quotes_.emplace(quote.quote.client_quote_id, live);
            if (quote.recovery.bid_order_id != 0) {
                quote_leg_to_quote_[quote.recovery.bid_order_id] = quote.quote.client_quote_id;
            }
            if (quote.recovery.ask_order_id != 0) {
                quote_leg_to_quote_[quote.recovery.ask_order_id] = quote.quote.client_quote_id;
            }
        }
    }
}

void TradingEngine::seed_gateway_recovery(const RecoveryState& state) noexcept {
    if (!gateway_) return;

    for (const auto& order : state.live_orders) {
        gateway_->restore_order_recovery(order);
    }
    for (const auto& quote : state.live_quotes) {
        gateway_->restore_quote_recovery(quote);
    }
}

void TradingEngine::request_recovery_cancels(const RecoveryState& state) noexcept {
    if (!gateway_ || !gateway_->is_connected()) return;

    for (const auto& order : state.live_orders) {
        (void)gateway_->cancel_order(order.order.client_order_id, order.order.instrument_id);
    }
    for (const auto& quote : state.live_quotes) {
        if (cfg_.gateway.type == GatewayType::CTP) {
            if (quote.recovery.bid_order_id != 0) {
                (void)gateway_->cancel_order(quote.recovery.bid_order_id, quote.quote.instrument_id);
            }
            if (quote.recovery.ask_order_id != 0) {
                (void)gateway_->cancel_order(quote.recovery.ask_order_id, quote.quote.instrument_id);
            }
        } else {
            (void)gateway_->cancel_quote(quote.quote.client_quote_id, quote.quote.instrument_id);
        }
    }
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

    persist_shutdown_state();

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

void TradingEngine::persist_order_event(OrderPersistenceEventType type,
                                        const Order& order,
                                        const GatewayOrderRecoveryHandle* recovery) noexcept {
    if (!repository_) return;
    OrderPersistenceEvent event{};
    event.type = type;
    event.order = order;
    if (recovery != nullptr) event.recovery = *recovery;
    if (!repository_->enqueue_order_event(event)) {
        OMM_LOG_WARN("repo", "order persistence queue full type={} order_id={}",
                     static_cast<int>(type), order.client_order_id);
    }
}

void TradingEngine::persist_quote_event(QuotePersistenceEventType type,
                                        const Quote& quote,
                                        const GatewayQuoteRecoveryHandle* recovery) noexcept {
    if (!repository_) return;
    QuotePersistenceEvent event{};
    event.type = type;
    event.quote = quote;
    if (recovery != nullptr) event.recovery = *recovery;
    if (!repository_->enqueue_quote_event(event)) {
        OMM_LOG_WARN("repo", "quote persistence queue full type={} quote_id={}",
                     static_cast<int>(type), quote.client_quote_id);
    }
}

void TradingEngine::persist_trade(const Trade& trade) noexcept {
    if (!repository_) return;
    if (!repository_->enqueue_trade(trade)) {
        OMM_LOG_WARN("repo", "trade persistence queue full order_id={} trade_id={}",
                     trade.client_order_id, trade.trade_id);
    }
}

void TradingEngine::persist_end_of_day_snapshot() noexcept {
    if (!repository_) return;
    const EndOfDaySnapshot snapshot = build_end_of_day_snapshot(
        0,
        instruments_,
        n_instruments_,
        greeks_snapshot_,
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

    MarketTick tick{};
    MarketTick pending_future_tick[MAX_PRODUCTS]{};
    bool pending_product[MAX_PRODUCTS]{};
    uint16_t next_option_offset[MAX_PRODUCTS]{};
    uint16_t cold_greeks_offset[MAX_PRODUCTS]{};
    int64_t next_cold_greeks_due_ns[MAX_PRODUCTS]{};
    int rr_cursor = 0;
    int cold_rr_cursor = 0;
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
        const MarketTick& future_tick = tick_snapshot_[future_id];
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
            sigma_arr[bi] = (cfg_.pricing.vol_method == VolMethod::OrcWing)
                ? surf->get_vol_by_strike(F_mid, opt.strike, T_arr[bi])
                : surf->get_vol(option_log_K_[prod][oi] - log_F_mid, T_arr[bi]);
            is_call_arr[bi] = (opt.option_type == OptionType::Call) ? 1 : 0;
        }

        compute_batch_precomputed(F_arr, K_arr, T_arr, sqrt_T_arr, disc_arr,
                                  sigma_arr, is_call_arr, results, batch_n);

        for (uint16_t bi = 0; bi < batch_n; ++bi) {
            const uint16_t opt_id = option_ids_[prod][start + bi];
            Greeks greek = greeks_snapshot_[opt_id];
            greek.theta = results[bi].theta;
            greek.rho = results[bi].rho;
            greek.T = T_arr[bi];
            greeks_snapshot_[opt_id] = greek;
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
                    tick_snapshot_[id] = tick;
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
            if (refresh_cold_greeks_batch(product_count)) {
                did_work = true;
            }
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
        alignas(32) Black76QuoteResult mid_results[MAX_BATCH];
        alignas(32) Black76QuoteResult bid_results[MAX_BATCH];
        alignas(32) Black76QuoteResult ask_results[MAX_BATCH];
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

        compute_batch_quote_precomputed(F_mid_arr, K_arr, sqrt_T_arr, disc_arr,
                                        sigma_arr, is_call_arr, mid_results, batch_n);
        compute_batch_quote_precomputed(F_bid_arr, K_arr, sqrt_T_arr, disc_arr,
                                        sigma_arr, is_call_arr, bid_results, batch_n);
        compute_batch_quote_precomputed(F_ask_arr, K_arr, sqrt_T_arr, disc_arr,
                                        sigma_arr, is_call_arr, ask_results, batch_n);

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

            Greeks greek = greeks_snapshot_[opt_id];
            greek.instrument_id = opt_id;
            greek.theo_price = mid_res.price;
            greek.delta = mid_res.delta;
            greek.gamma = mid_res.gamma;
            greek.vega = mid_res.vega;
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
    apply_realtime_if_configured(cfg_.scheduling.enable_realtime,
                                 cfg_.scheduling.strategy_priority,
                                 "omm-strat");

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
    apply_realtime_if_configured(cfg_.scheduling.enable_realtime,
                                 cfg_.scheduling.arbitrage_priority,
                                 "omm-arb");

    GatewayEvent ev{};
    ArbMarketTrigger trigger{};
    Timestamp next_maintenance_ns = get_monotonic_ns() + kArbMaintenanceIntervalNs;
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
            spin_pause();
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

    struct DeferredCallbackSideEffect {
        GatewayEvent event{};
        GatewayOrderRecoveryHandle order_recovery{};
        GatewayQuoteRecoveryHandle quote_recovery{};
    };

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
                                                normalized.event.type,
                                                &normalized.order_recovery);
                    route_to_arbitrage = is_arb_order_id(normalized.event.order.client_order_id);
                    break;
                case GatewayEventType::QuoteAck:
                    handle_gateway_quote_update(normalized.event.quote,
                                                normalized.event.type,
                                                &normalized.quote_recovery);
                    break;
                case GatewayEventType::QuoteCancel:
                    handle_gateway_quote_update(normalized.event.quote,
                                                normalized.event.type,
                                                &normalized.quote_recovery);
                    break;
                case GatewayEventType::QuoteReject:
                    handle_gateway_quote_update(normalized.event.quote,
                                                normalized.event.type,
                                                &normalized.quote_recovery);
                    break;
                case GatewayEventType::OrderFill:
                case GatewayEventType::QuoteFill:
                    handle_gateway_fill(&normalized.event.trade, &normalized.event.type);
                    route_to_arbitrage = is_arb_order_id(normalized.event.trade.client_order_id);
                    break;
                case GatewayEventType::OrderCancel:
                    handle_gateway_order_update(normalized.event.order,
                                                normalized.event.type,
                                                &normalized.order_recovery);
                    route_to_arbitrage = is_arb_order_id(normalized.event.order.client_order_id);
                    break;
                case GatewayEventType::OrderReject:
                    handle_gateway_order_update(normalized.event.order,
                                                normalized.event.type,
                                                &normalized.order_recovery);
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
                    {
                        GatewayOrderRecoveryHandle refreshed_recovery{};
                        if (gateway_->get_order_recovery_handle(side.event.order.client_order_id,
                                                                &refreshed_recovery)) {
                            merge_order_recovery(&side.order_recovery, refreshed_recovery);
                            update_live_order_recovery(side.event.order.client_order_id,
                                                       side.order_recovery);
                        }
                    }
                    publish_monitor_order(side.event.order);
                    persist_order_event(OrderPersistenceEventType::Ack,
                                        side.event.order,
                                        has_order_recovery(side.order_recovery)
                                            ? &side.order_recovery
                                            : nullptr);
                    break;
                case GatewayEventType::QuoteAck: {
                    GatewayQuoteRecoveryHandle refreshed_recovery{};
                    if (gateway_->get_quote_recovery_handle(side.event.quote.client_quote_id,
                                                            &refreshed_recovery)) {
                        merge_quote_recovery(&side.quote_recovery, refreshed_recovery);
                        update_live_quote_recovery(side.event.quote.client_quote_id,
                                                   side.quote_recovery);
                    }
                    publish_monitor_quote(side.event.quote);
                    persist_quote_event(QuotePersistenceEventType::Ack,
                                        side.event.quote,
                                        has_quote_recovery(side.quote_recovery)
                                            ? &side.quote_recovery
                                            : nullptr);
                    break;
                }
                case GatewayEventType::QuoteCancel:
                    publish_monitor_quote(side.event.quote);
                    persist_quote_event(QuotePersistenceEventType::Cancel,
                                        side.event.quote,
                                        has_quote_recovery(side.quote_recovery)
                                            ? &side.quote_recovery
                                            : nullptr);
                    break;
                case GatewayEventType::QuoteReject:
                    publish_monitor_quote(side.event.quote);
                    persist_quote_event(QuotePersistenceEventType::Reject,
                                        side.event.quote,
                                        has_quote_recovery(side.quote_recovery)
                                            ? &side.quote_recovery
                                            : nullptr);
                    break;
                case GatewayEventType::OrderFill:
                case GatewayEventType::QuoteFill: {
                    publish_monitor_trade(side.event.trade);
                    persist_trade(side.event.trade);

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
                    persist_order_event(OrderPersistenceEventType::Cancel,
                                        side.event.order,
                                        has_order_recovery(side.order_recovery)
                                            ? &side.order_recovery
                                            : nullptr);
                    break;
                case GatewayEventType::OrderReject:
                    publish_monitor_order(side.event.order);
                    persist_order_event(OrderPersistenceEventType::Reject,
                                        side.event.order,
                                        has_order_recovery(side.order_recovery)
                                            ? &side.order_recovery
                                            : nullptr);
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
                GatewayOrderRecoveryHandle recovery{};
                const bool sent = gateway_->send_order(order, &recovery);
                publish_monitor_order(order);
                if (sent) {
                    const bool has_recovery = has_order_recovery(recovery);
                    track_live_order_submit(order, has_recovery ? &recovery : nullptr);
                    if (has_recovery) {
                        persist_order_event(OrderPersistenceEventType::Submit, order, &recovery);
                    } else {
                        persist_order_event(OrderPersistenceEventType::Submit, order);
                    }
                } else {
                    Order rejected = order;
                    rejected.status = OrderStatus::Rejected;
                    rejected.ack_ts = get_monotonic_ns();
                    persist_order_event(OrderPersistenceEventType::Reject, rejected);
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
                GatewayQuoteRecoveryHandle recovery{};
                const bool sent = gateway_->send_quote(quote, &recovery);
                publish_monitor_quote(quote);
                if (sent && (quote.bid_volume > 0 || quote.ask_volume > 0)) {
                    if (has_quote_recovery(recovery)) {
                        track_live_quote_submit(quote, &recovery);
                        persist_quote_event(QuotePersistenceEventType::Submit, quote, &recovery);
                    } else {
                        track_live_quote_submit(quote, nullptr);
                        persist_quote_event(QuotePersistenceEventType::Submit, quote);
                    }
                } else if (!sent) {
                    Quote rejected = quote;
                    rejected.bid_status = OrderStatus::Rejected;
                    rejected.ask_status = OrderStatus::Rejected;
                    rejected.ack_ts = get_monotonic_ns();
                    persist_quote_event(QuotePersistenceEventType::Reject, rejected);
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
                    GatewayOrderRecoveryHandle recovery{};
                    const bool sent = gateway_->send_order(intent.order, &recovery);
                    publish_monitor_order(intent.order);
                    if (sent) {
                        const bool has_recovery = has_order_recovery(recovery);
                        track_live_order_submit(intent.order, has_recovery ? &recovery : nullptr);
                        if (has_recovery) {
                            persist_order_event(OrderPersistenceEventType::Submit, intent.order, &recovery);
                        } else {
                            persist_order_event(OrderPersistenceEventType::Submit, intent.order);
                        }
                    } else {
                        Order rejected = intent.order;
                        rejected.status = OrderStatus::Rejected;
                        rejected.ack_ts = get_monotonic_ns();
                        persist_order_event(OrderPersistenceEventType::Reject, rejected);
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
    apply_realtime_if_configured(cfg_.scheduling.enable_realtime,
                                 cfg_.scheduling.vol_fitter_priority,
                                 "omm-volfitter");
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
            post_risk_.check_limits(greeks_snapshot_, n_instruments_);
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
        const Greeks& greeks = greeks_snapshot_[pos.instrument_id];
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

void TradingEngine::track_live_order_submit(const Order& order,
                                            const GatewayOrderRecoveryHandle* recovery) noexcept {
    std::lock_guard<std::mutex> lock(live_state_mutex_);
    LiveOrderState state{};
    state.order = order;
    if (recovery != nullptr) {
        state.recovery = *recovery;
    }
    live_orders_[order.client_order_id] = state;
}

void TradingEngine::track_live_quote_submit(const Quote& quote,
                                            const GatewayQuoteRecoveryHandle* recovery) noexcept {
    std::lock_guard<std::mutex> lock(live_state_mutex_);
    auto existing = live_quotes_.find(quote.client_quote_id);
    if (existing != live_quotes_.end()) {
        if (existing->second.recovery.bid_order_id != 0) {
            quote_leg_to_quote_.erase(existing->second.recovery.bid_order_id);
        }
        if (existing->second.recovery.ask_order_id != 0) {
            quote_leg_to_quote_.erase(existing->second.recovery.ask_order_id);
        }
    }

    LiveQuoteState state{};
    state.quote = quote;
    state.remaining_bid = quote.bid_volume;
    state.remaining_ask = quote.ask_volume;
    if (recovery != nullptr) {
        state.recovery = *recovery;
    }
    if (cfg_.gateway.type == GatewayType::CTP) {
        if (state.recovery.bid_order_id == 0) {
            state.recovery.bid_order_id = quote.client_quote_id;
        }
        if (state.recovery.ask_order_id == 0) {
            state.recovery.ask_order_id = quote.client_quote_id | (1ULL << 47);
        }
    }
    if (state.recovery.bid_order_id != 0) {
        quote_leg_to_quote_[state.recovery.bid_order_id] = quote.client_quote_id;
    }
    if (state.recovery.ask_order_id != 0) {
        quote_leg_to_quote_[state.recovery.ask_order_id] = quote.client_quote_id;
    }
    live_quotes_[quote.client_quote_id] = state;
}

void TradingEngine::handle_gateway_order_update(Order& order,
                                                GatewayEventType type,
                                                GatewayOrderRecoveryHandle* recovery) noexcept {
    if (recovery != nullptr) *recovery = GatewayOrderRecoveryHandle{};
    std::lock_guard<std::mutex> lock(live_state_mutex_);
    auto order_it = live_orders_.find(order.client_order_id);
    if (order_it != live_orders_.end()) {
        LiveOrderState& state = order_it->second;
        Order& live = state.order;
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
        if (recovery != nullptr) {
            *recovery = state.recovery;
        }
        if (type == GatewayEventType::OrderCancel || type == GatewayEventType::OrderReject) {
            live_orders_.erase(order_it);
        }
        return;
    }

    auto quote_leg_it = quote_leg_to_quote_.find(order.client_order_id);
    if (quote_leg_it == quote_leg_to_quote_.end()) return;
    auto quote_it = live_quotes_.find(quote_leg_it->second);
    if (quote_it == live_quotes_.end()) return;
    order.book_id = quote_it->second.quote.book_id;
    order.product_index = quote_it->second.quote.product_index;
    order.account_id = quote_it->second.quote.account_id;
    if (order.exchange_id.empty()) {
        order.exchange_id = quote_it->second.quote.exchange_id;
    }
    if (recovery != nullptr) {
        *recovery = GatewayOrderRecoveryHandle{};
        recovery->valid = true;
        recovery->is_quote_leg = true;
        recovery->client_quote_id = quote_it->second.quote.client_quote_id;
        if (order.side == Side::Buy) {
            std::strncpy(recovery->exchange_local_id,
                         quote_it->second.recovery.bid_local_id,
                         sizeof(recovery->exchange_local_id) - 1);
            std::strncpy(recovery->order_sys_id,
                         quote_it->second.recovery.bid_order_sys_id,
                         sizeof(recovery->order_sys_id) - 1);
        } else {
            std::strncpy(recovery->exchange_local_id,
                         quote_it->second.recovery.ask_local_id,
                         sizeof(recovery->exchange_local_id) - 1);
            std::strncpy(recovery->order_sys_id,
                         quote_it->second.recovery.ask_order_sys_id,
                         sizeof(recovery->order_sys_id) - 1);
        }
    }
}

void TradingEngine::handle_gateway_quote_update(Quote& quote,
                                                GatewayEventType type,
                                                GatewayQuoteRecoveryHandle* recovery) noexcept {
    if (recovery != nullptr) *recovery = GatewayQuoteRecoveryHandle{};
    std::lock_guard<std::mutex> lock(live_state_mutex_);
    auto it = live_quotes_.find(quote.client_quote_id);
    if (it == live_quotes_.end()) return;

    LiveQuoteState& live = it->second;
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
    if (recovery != nullptr) {
        *recovery = live.recovery;
    }

    if (type == GatewayEventType::QuoteCancel || type == GatewayEventType::QuoteReject) {
        if (live.recovery.bid_order_id != 0) {
            quote_leg_to_quote_.erase(live.recovery.bid_order_id);
        }
        if (live.recovery.ask_order_id != 0) {
            quote_leg_to_quote_.erase(live.recovery.ask_order_id);
        }
        live_quotes_.erase(it);
    }
}

void TradingEngine::update_live_order_recovery(OrderId order_id,
                                               const GatewayOrderRecoveryHandle& recovery) noexcept {
    std::lock_guard<std::mutex> lock(live_state_mutex_);
    auto it = live_orders_.find(order_id);
    if (it == live_orders_.end()) return;
    merge_order_recovery(&it->second.recovery, recovery);
}

void TradingEngine::update_live_quote_recovery(QuoteId quote_id,
                                               const GatewayQuoteRecoveryHandle& recovery) noexcept {
    std::lock_guard<std::mutex> lock(live_state_mutex_);
    auto it = live_quotes_.find(quote_id);
    if (it == live_quotes_.end()) return;

    LiveQuoteState& live = it->second;
    if (live.recovery.bid_order_id != 0) {
        quote_leg_to_quote_.erase(live.recovery.bid_order_id);
    }
    if (live.recovery.ask_order_id != 0) {
        quote_leg_to_quote_.erase(live.recovery.ask_order_id);
    }
    merge_quote_recovery(&live.recovery, recovery);
    if (live.recovery.bid_order_id != 0) {
        quote_leg_to_quote_[live.recovery.bid_order_id] = quote_id;
    }
    if (live.recovery.ask_order_id != 0) {
        quote_leg_to_quote_[live.recovery.ask_order_id] = quote_id;
    }
}

void TradingEngine::handle_gateway_fill(Trade* trade,
                                        GatewayEventType* type) noexcept {
    if (trade == nullptr || type == nullptr) return;
    std::lock_guard<std::mutex> lock(live_state_mutex_);

    if (*type == GatewayEventType::QuoteFill) {
        auto quote_it = live_quotes_.find(static_cast<QuoteId>(trade->client_order_id));
        if (quote_it != live_quotes_.end()) {
            LiveQuoteState& live = quote_it->second;
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
                if (live.recovery.bid_order_id != 0) {
                    quote_leg_to_quote_.erase(live.recovery.bid_order_id);
                }
                if (live.recovery.ask_order_id != 0) {
                    quote_leg_to_quote_.erase(live.recovery.ask_order_id);
                }
                live_quotes_.erase(quote_it);
            }
            return;
        }
    }

    auto quote_leg_it = quote_leg_to_quote_.find(trade->client_order_id);
    if (quote_leg_it != quote_leg_to_quote_.end()) {
        const QuoteId quote_id = quote_leg_it->second;
        auto quote_it = live_quotes_.find(quote_id);
        if (quote_it != live_quotes_.end()) {
            LiveQuoteState& live = quote_it->second;
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
                if (live.recovery.bid_order_id != 0) {
                    quote_leg_to_quote_.erase(live.recovery.bid_order_id);
                }
                if (live.recovery.ask_order_id != 0) {
                    quote_leg_to_quote_.erase(live.recovery.ask_order_id);
                }
                live_quotes_.erase(quote_it);
            }
            return;
        }
    }

    auto order_it = live_orders_.find(trade->client_order_id);
    if (order_it == live_orders_.end()) return;

    Order& live = order_it->second.order;
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
        live_orders_.erase(order_it);
    }
}

void TradingEngine::cancel_all_live_orders_and_quotes() noexcept {
    std::vector<Order> orders;
    std::vector<LiveQuoteState> quotes;
    {
        std::lock_guard<std::mutex> lock(live_state_mutex_);
        orders.reserve(live_orders_.size());
        for (const auto& entry : live_orders_) {
            orders.push_back(entry.second.order);
        }
        quotes.reserve(live_quotes_.size());
        for (const auto& entry : live_quotes_) {
            quotes.push_back(entry.second);
        }
    }

    if (!gateway_ || !gateway_->is_connected()) return;
    for (const Order& order : orders) {
        (void)gateway_->cancel_order(order.client_order_id, order.instrument_id);
    }
    for (const LiveQuoteState& quote : quotes) {
        if (cfg_.gateway.type == GatewayType::CTP) {
            if (quote.recovery.bid_order_id != 0) {
                (void)gateway_->cancel_order(quote.recovery.bid_order_id, quote.quote.instrument_id);
            }
            if (quote.recovery.ask_order_id != 0) {
                (void)gateway_->cancel_order(quote.recovery.ask_order_id, quote.quote.instrument_id);
            }
        } else {
            (void)gateway_->cancel_quote(quote.quote.client_quote_id, quote.quote.instrument_id);
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
