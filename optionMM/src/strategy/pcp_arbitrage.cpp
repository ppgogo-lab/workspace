#include "strategy/pcp_arbitrage.h"
#include "common/thread_utils.h"

#include <algorithm>
#include <cmath>

namespace omm {

namespace {

// For aggressive/taker-style execution we cross the spread:
//   buy  -> lift best ask
//   sell -> hit best bid
[[nodiscard]] double bid_price_for_side(const MarketTick& tick, Side side) noexcept {
    return side == Side::Buy ? tick.ask_price[0] : tick.bid_price[0];
}

[[nodiscard]] int side_sign(Side side) noexcept {
    return side == Side::Buy ? 1 : -1;
}

} // namespace

void PCPArbitrageStrategy::init(uint8_t product_idx,
                                SPSCRingBuffer<ArbIntent, 256>* intent_buf,
                                AtomicArbParams* params,
                                const Instrument* instruments,
                                const MarketTick* tick_snapshot,
                                const Greeks* greeks_snapshot,
                                double risk_free_rate,
                                const HardRiskConfig& hard_risk_cfg,
                                const AccountId& account_id) noexcept {
    product_idx_ = product_idx;
    intent_buf_ = intent_buf;
    params_ = params;
    instruments_ = instruments;
    tick_snapshot_ = tick_snapshot;
    greeks_snapshot_ = greeks_snapshot;
    risk_free_rate_ = risk_free_rate;
    account_id_ = account_id;
    pre_risk_ = std::make_unique<PreTradeRisk>(hard_risk_cfg);
    pair_count_ = 0;
    local_order_seq_ = 0;
    last_scan_ts_ns_ = 0;
    next_trigger_ts_ns_ = 0;
    attempt_active_ = false;
    cleanup_active_ = false;
    active_call_id_ = INVALID_INSTRUMENT_ID;
    active_put_id_ = INVALID_INSTRUMENT_ID;
    active_future_id_ = INVALID_INSTRUMENT_ID;
    last_suppress_flags_ = ArbSuppressNone;
    for (auto& order : working_orders_) order = WorkingOrder{};
    build_pairs();
    refresh_monitor_state(pair_count_ == 0 ? ArbSuppressNoPairs : ArbSuppressNone, get_monotonic_ns());
}

bool PCPArbitrageStrategy::owns_order_id(OrderId id) const noexcept {
    return is_arb_order_id(id)
        && arb_order_product(id) == product_idx_
        && arb_order_type(id) == strategy_type();
}

bool PCPArbitrageStrategy::is_enabled() const noexcept {
    return params_ && params_->enabled.load(std::memory_order_relaxed);
}

bool PCPArbitrageStrategy::read_monitor_state(ArbStrategyMonitorState* out) const noexcept {
    if (out == nullptr) return false;
    out->product_index = product_idx_;
    out->strategy_type = strategy_type();
    out->enabled = is_enabled();
    out->running = monitor_running_.load(std::memory_order_relaxed);
    out->cleanup_active = monitor_cleanup_active_.load(std::memory_order_relaxed);
    out->live_orders = monitor_live_orders_.load(std::memory_order_relaxed);
    out->pair_count = pair_count_;
    out->active_call_id = monitor_active_call_id_.load(std::memory_order_relaxed);
    out->active_put_id = monitor_active_put_id_.load(std::memory_order_relaxed);
    out->active_future_id = monitor_active_future_id_.load(std::memory_order_relaxed);
    out->suppress_flags = monitor_suppress_flags_.load(std::memory_order_relaxed);
    out->last_edge_ticks = monitor_last_edge_ticks_.load(std::memory_order_relaxed);
    out->last_trigger_edge_ticks = monitor_last_trigger_edge_ticks_.load(std::memory_order_relaxed);
    out->last_eval_ts_ns = monitor_last_eval_ts_ns_.load(std::memory_order_relaxed);
    out->last_trigger_ts_ns = monitor_last_trigger_ts_ns_.load(std::memory_order_relaxed);
    return true;
}

void PCPArbitrageStrategy::build_pairs() noexcept {
    // PCP is intra-product in v1. We first locate the product future, then
    // pair every call with the matching put on:
    //   - same underlying future
    //   - same expiry
    //   - same strike
    uint16_t future_id = INVALID_INSTRUMENT_ID;
    for (uint16_t id = 0; id < MAX_INSTRUMENTS; ++id) {
        const Instrument& instr = instruments_[id];
        if (instr.instrument_id == INVALID_INSTRUMENT_ID) continue;
        if (instr.product_index != product_idx_) continue;
        if (instr.kind == InstrumentKind::Future) {
            future_id = id;
            break;
        }
    }
    if (future_id == INVALID_INSTRUMENT_ID) return;

    for (uint16_t call_id = 0; call_id < MAX_INSTRUMENTS; ++call_id) {
        const Instrument& call = instruments_[call_id];
        if (call.instrument_id == INVALID_INSTRUMENT_ID) continue;
        if (call.product_index != product_idx_) continue;
        if (call.kind != InstrumentKind::Option || call.option_type != OptionType::Call) continue;

        for (uint16_t put_id = 0; put_id < MAX_INSTRUMENTS; ++put_id) {
            const Instrument& put = instruments_[put_id];
            if (put.instrument_id == INVALID_INSTRUMENT_ID) continue;
            if (put.product_index != product_idx_) continue;
            if (put.kind != InstrumentKind::Option || put.option_type != OptionType::Put) continue;
            if (put.underlying_id != call.underlying_id) continue;
            if (put.expiry_date != call.expiry_date) continue;
            if (std::fabs(put.strike - call.strike) > 1e-9) continue;

            if (pair_count_ >= kMaxPairs) return;
            Pair& pair = pairs_[pair_count_++];
            pair.active = true;
            pair.call_id = call_id;
            pair.put_id = put_id;
            pair.future_id = future_id;
            pair.expiry_date = call.expiry_date;
            pair.strike = call.strike;
            break;
        }
    }
}

double PCPArbitrageStrategy::discount_factor(const Pair& pair, Timestamp now_ns) const noexcept {
    if (!pair.active || pair.call_id >= MAX_INSTRUMENTS) return 1.0;
    const Instrument& call = instruments_[pair.call_id];
    static constexpr double kNsPerYear = 365.0 * 24.0 * 3600.0 * 1e9;
    double T = (call.expiry_epoch_ns - now_ns) / kNsPerYear;
    if (T < 1e-4) T = 1e-4;
    // Futures-option PCP uses the discounted futures-minus-strike term.
    return std::exp(-risk_free_rate_ * T);
}

bool PCPArbitrageStrategy::market_valid(const MarketTick& tick, Timestamp now_ns) const noexcept {
    return tick.recv_ts_ns > 0
        && now_ns - tick.recv_ts_ns <= kMarketStaleNs
        && tick.bid_price[0] > 0.0
        && tick.ask_price[0] > 0.0
        && tick.ask_price[0] >= tick.bid_price[0]
        && tick.bid_volume[0] > 0
        && tick.ask_volume[0] > 0;
}

Volume PCPArbitrageStrategy::executable_volume(const Pair& pair,
                                               Direction dir,
                                               int max_order_volume) const noexcept {
    if (!pair.active || max_order_volume <= 0) return 0;
    const MarketTick& call_tick = tick_snapshot_[pair.call_id];
    const MarketTick& put_tick = tick_snapshot_[pair.put_id];
    const MarketTick& future_tick = tick_snapshot_[pair.future_id];

    if (dir == Direction::LongSyntheticShortFuture) {
        // Buy call at ask, sell put at bid, sell future at bid. Size is bounded
        // by the tightest displayed size across the three aggressive legs.
        return std::max<Volume>(0, std::min({max_order_volume,
                                             call_tick.ask_volume[0],
                                             put_tick.bid_volume[0],
                                             future_tick.bid_volume[0]}));
    }
    if (dir == Direction::ShortSyntheticLongFuture) {
        // Sell call at bid, buy put at ask, buy future at ask.
        return std::max<Volume>(0, std::min({max_order_volume,
                                             call_tick.bid_volume[0],
                                             put_tick.ask_volume[0],
                                             future_tick.ask_volume[0]}));
    }
    return 0;
}

bool PCPArbitrageStrategy::scan_best_opportunity(Timestamp now_ns,
                                                 Pair* best_pair,
                                                 Direction* best_dir,
                                                 Volume* best_volume,
                                                 double* best_edge_ticks,
                                                 uint32_t* suppress_flags) noexcept {
    if (best_pair == nullptr || best_dir == nullptr || best_volume == nullptr
        || best_edge_ticks == nullptr || suppress_flags == nullptr) {
        return false;
    }

    bool saw_valid_pair = false;
    *best_pair = Pair{};
    *best_dir = Direction::None;
    *best_volume = 0;
    *best_edge_ticks = 0.0;

    const ArbParamsConfig cfg = params_->snapshot();

    for (uint16_t i = 0; i < pair_count_; ++i) {
        const Pair& pair = pairs_[i];
        if (!pair.active) continue;

        const MarketTick& call_tick = tick_snapshot_[pair.call_id];
        const MarketTick& put_tick = tick_snapshot_[pair.put_id];
        const MarketTick& future_tick = tick_snapshot_[pair.future_id];
        if (!market_valid(call_tick, now_ns)
            || !market_valid(put_tick, now_ns)
            || !market_valid(future_tick, now_ns)) {
            continue;
        }

        saw_valid_pair = true;
        const double future_tick_size =
            instruments_[pair.future_id].tick_size > 0.0 ? instruments_[pair.future_id].tick_size : 1.0;
        const double discount = discount_factor(pair, now_ns);

        // Put-call parity for options on a future:
        //
        //   C - P = DF * (F - K)
        //
        // Rearranged as an executable arbitrage edge:
        //
        //   long synthetic / short future
        //     = DF * (F_bid - K) - (C_ask - P_bid)
        //
        //   short synthetic / long future
        //     = (C_bid - P_ask) - DF * (F_ask - K)
        //
        // We use bid/ask on each leg because the strategy is evaluating the
        // edge at immediately executable prices, not mid prices.
        //
        // The result is normalized by futures tick size so the configured edge
        // threshold is expressed in ticks rather than currency units.
        const double long_synth_edge =
            (discount * (future_tick.bid_price[0] - pair.strike)
             - (call_tick.ask_price[0] - put_tick.bid_price[0])) / future_tick_size;
        const Volume long_synth_volume =
            executable_volume(pair, Direction::LongSyntheticShortFuture, cfg.max_order_volume);
        if (long_synth_volume > 0 && long_synth_edge > *best_edge_ticks) {
            *best_pair = pair;
            *best_dir = Direction::LongSyntheticShortFuture;
            *best_volume = long_synth_volume;
            *best_edge_ticks = long_synth_edge;
        }

        const double short_synth_edge =
            ((call_tick.bid_price[0] - put_tick.ask_price[0])
             - discount * (future_tick.ask_price[0] - pair.strike)) / future_tick_size;
        const Volume short_synth_volume =
            executable_volume(pair, Direction::ShortSyntheticLongFuture, cfg.max_order_volume);
        if (short_synth_volume > 0 && short_synth_edge > *best_edge_ticks) {
            *best_pair = pair;
            *best_dir = Direction::ShortSyntheticLongFuture;
            *best_volume = short_synth_volume;
            *best_edge_ticks = short_synth_edge;
        }
    }

    if (!saw_valid_pair) {
        *suppress_flags |= ArbSuppressInvalidMarket;
        return false;
    }
    return *best_dir != Direction::None;
}

bool PCPArbitrageStrategy::enqueue_order(uint16_t instrument_id,
                                         Side side,
                                         double price,
                                         Volume volume,
                                         bool cleanup,
                                         double edge_ticks,
                                         const Pair& pair,
                                         Timestamp now_ns) noexcept {
    if (!intent_buf_ || !pre_risk_ || instrument_id >= MAX_INSTRUMENTS || volume <= 0 || price <= 0.0) {
        return false;
    }

    if (live_order_count() >= params_->max_live_orders.load(std::memory_order_relaxed)) {
        last_suppress_flags_ |= ArbSuppressLiveOrders;
        return false;
    }

    int slot_idx = -1;
    for (int i = 0; i < kMaxWorkingOrders; ++i) {
        if (!working_orders_[i].used) {
            slot_idx = i;
            break;
        }
    }
    if (slot_idx < 0) {
        last_suppress_flags_ |= ArbSuppressLiveOrders;
        return false;
    }

    Order order{};
    order.client_order_id = make_arb_order_id(product_idx_, strategy_type(), ++local_order_seq_);
    order.instrument_id = instrument_id;
    order.product_index = product_idx_;
    order.account_id = account_id_;
    order.exchange_id = instruments_[instrument_id].exchange_id;
    order.side = side;
    order.offset = cleanup ? OffsetFlag::Close : OffsetFlag::Open;
    order.order_type = OrderType::Limit;
    order.price = price;
    order.volume = volume;
    order.send_ts = now_ns;

    // Arbitrage orders still go through the same pre-trade risk checks as any
    // other engine order. The strategy only owns signal generation and local
    // execution state, not a bypass around hard limits.
    if (pre_risk_->check_order(order) != PreTradeRisk::RejectReason::OK) {
        return false;
    }

    WorkingOrder& slot = working_orders_[slot_idx];
    slot = WorkingOrder{};
    slot.used = true;
    slot.cleanup = cleanup;
    slot.order = order;
    slot.send_ts = now_ns;

    ArbIntent intent{};
    intent.order = order;
    intent.strategy_type = strategy_type();
    intent.kind = ArbIntentKind::SubmitOrder;
    intent.cleanup = cleanup;
    intent.intent_ts = now_ns;
    intent.edge_ticks = edge_ticks;
    intent.call_instrument_id = pair.call_id;
    intent.put_instrument_id = pair.put_id;
    intent.future_instrument_id = pair.future_id;

    if (!intent_buf_->try_push(intent)) {
        slot = WorkingOrder{};
        last_suppress_flags_ |= ArbSuppressIntentBackpressure;
        return false;
    }
    return true;
}

void PCPArbitrageStrategy::start_attempt(const Pair& pair,
                                         Direction dir,
                                         Volume volume,
                                         double edge_ticks,
                                         Timestamp now_ns) noexcept {
    // The initial attempt is always a three-leg basket. We record the active
    // tuple so later fills/cancels can be interpreted as either:
    //   1. a completed parity basket, or
    //   2. a residual inventory problem that requires cleanup.
    attempt_active_ = true;
    cleanup_active_ = false;
    set_active_pair(pair);
    monitor_last_trigger_edge_ticks_.store(edge_ticks, std::memory_order_relaxed);
    monitor_last_trigger_ts_ns_.store(now_ns, std::memory_order_relaxed);

    const MarketTick& call_tick = tick_snapshot_[pair.call_id];
    const MarketTick& put_tick = tick_snapshot_[pair.put_id];
    const MarketTick& future_tick = tick_snapshot_[pair.future_id];

    if (dir == Direction::LongSyntheticShortFuture) {
        // Synthetic long future = +Call - Put. We hedge that by shorting the
        // listed future when the synthetic is cheap.
        (void)enqueue_order(pair.call_id, Side::Buy, call_tick.ask_price[0], volume, false, edge_ticks, pair, now_ns);
        (void)enqueue_order(pair.put_id, Side::Sell, put_tick.bid_price[0], volume, false, edge_ticks, pair, now_ns);
        (void)enqueue_order(pair.future_id, Side::Sell, future_tick.bid_price[0], volume, false, edge_ticks, pair, now_ns);
    } else if (dir == Direction::ShortSyntheticLongFuture) {
        // Synthetic short future = -Call + Put. We hedge that by buying the
        // listed future when the synthetic is rich.
        (void)enqueue_order(pair.call_id, Side::Sell, call_tick.bid_price[0], volume, false, edge_ticks, pair, now_ns);
        (void)enqueue_order(pair.put_id, Side::Buy, put_tick.ask_price[0], volume, false, edge_ticks, pair, now_ns);
        (void)enqueue_order(pair.future_id, Side::Buy, future_tick.ask_price[0], volume, false, edge_ticks, pair, now_ns);
    }

    if (live_order_count() == 0) {
        attempt_active_ = false;
        next_trigger_ts_ns_ = now_ns
            + static_cast<int64_t>(params_->cooldown_ms.load(std::memory_order_relaxed) * 1'000'000.0);
    }
}

void PCPArbitrageStrategy::cancel_stale_orders(Timestamp now_ns, int64_t timeout_ns) noexcept {
    if (!intent_buf_) return;
    for (auto& order : working_orders_) {
        if (!order.used || order.done || order.cancel_requested) continue;
        if (now_ns - order.send_ts < timeout_ns) continue;

        // Initial parity baskets are sent aggressively, but a leg may still sit
        // live if the venue does not fill it immediately. After the timeout we
        // cancel so the strategy can decide whether to clean up the residual.
        ArbIntent intent{};
        intent.order.client_order_id = order.order.client_order_id;
        intent.order.instrument_id = order.order.instrument_id;
        intent.order.product_index = product_idx_;
        intent.strategy_type = strategy_type();
        intent.kind = ArbIntentKind::CancelOrder;
        intent.cleanup = order.cleanup;
        intent.intent_ts = now_ns;
        if (intent_buf_->try_push(intent)) {
            order.cancel_requested = true;
        } else {
            last_suppress_flags_ |= ArbSuppressIntentBackpressure;
        }
    }
}

void PCPArbitrageStrategy::submit_cleanup_orders(Timestamp now_ns) noexcept {
    if (active_call_id_ >= MAX_INSTRUMENTS
        || active_put_id_ >= MAX_INSTRUMENTS
        || active_future_id_ >= MAX_INSTRUMENTS) {
        return;
    }

    std::array<uint16_t, 3> instruments{
        active_call_id_, active_put_id_, active_future_id_,
    };
    std::array<int32_t, 3> net_qty{};

    for (const auto& order : working_orders_) {
        if (!order.used) continue;
        for (std::size_t i = 0; i < instruments.size(); ++i) {
            if (order.order.instrument_id != instruments[i]) continue;
            // Residual position per leg = signed filled quantity across all
            // primary and cleanup orders already executed for that instrument.
            net_qty[i] += side_sign(order.order.side) * order.filled_volume;
        }
    }

    Pair pair{};
    pair.active = true;
    pair.call_id = active_call_id_;
    pair.put_id = active_put_id_;
    pair.future_id = active_future_id_;

    bool submitted = false;
    for (std::size_t i = 0; i < instruments.size(); ++i) {
        if (net_qty[i] == 0) continue;
        const uint16_t instrument_id = instruments[i];
        const MarketTick& tick = tick_snapshot_[instrument_id];
        if (!market_valid(tick, now_ns)) {
            last_suppress_flags_ |= ArbSuppressCleanupPending | ArbSuppressInvalidMarket;
            continue;
        }

        const Side flatten_side = net_qty[i] > 0 ? Side::Sell : Side::Buy;
        const double price = bid_price_for_side(tick, flatten_side);
        // Cleanup is sized to the smaller of:
        //   - the remaining residual inventory
        //   - the configured per-order cap
        const Volume qty = std::min<Volume>(std::abs(net_qty[i]),
                                            params_->max_order_volume.load(std::memory_order_relaxed));
        if (enqueue_order(instrument_id, flatten_side, price, qty, true, 0.0, pair, now_ns)) {
            submitted = true;
        }
    }

    cleanup_active_ = submitted || cleanup_active_;
    if (!submitted) {
        last_suppress_flags_ |= ArbSuppressCleanupPending;
    }
}

void PCPArbitrageStrategy::clear_attempt_state() noexcept {
    for (auto& order : working_orders_) order = WorkingOrder{};
    attempt_active_ = false;
    cleanup_active_ = false;
    active_call_id_ = INVALID_INSTRUMENT_ID;
    active_put_id_ = INVALID_INSTRUMENT_ID;
    active_future_id_ = INVALID_INSTRUMENT_ID;
}

void PCPArbitrageStrategy::set_active_pair(const Pair& pair) noexcept {
    active_call_id_ = pair.call_id;
    active_put_id_ = pair.put_id;
    active_future_id_ = pair.future_id;
}

void PCPArbitrageStrategy::refresh_monitor_state(uint32_t suppress_flags, Timestamp now_ns) noexcept {
    last_suppress_flags_ = suppress_flags;
    monitor_running_.store(is_enabled(), std::memory_order_relaxed);
    monitor_cleanup_active_.store(cleanup_active_, std::memory_order_relaxed);
    monitor_live_orders_.store(static_cast<uint8_t>(live_order_count()), std::memory_order_relaxed);
    monitor_suppress_flags_.store(suppress_flags, std::memory_order_relaxed);
    monitor_active_call_id_.store(active_call_id_, std::memory_order_relaxed);
    monitor_active_put_id_.store(active_put_id_, std::memory_order_relaxed);
    monitor_active_future_id_.store(active_future_id_, std::memory_order_relaxed);
    monitor_last_eval_ts_ns_.store(now_ns, std::memory_order_relaxed);
}

int PCPArbitrageStrategy::live_order_count() const noexcept {
    int count = 0;
    for (const auto& order : working_orders_) {
        if (order.used && !order.done) ++count;
    }
    return count;
}

PCPArbitrageStrategy::WorkingOrder* PCPArbitrageStrategy::find_working_order(OrderId id) noexcept {
    for (auto& order : working_orders_) {
        if (order.used && order.order.client_order_id == id) return &order;
    }
    return nullptr;
}

const PCPArbitrageStrategy::WorkingOrder* PCPArbitrageStrategy::find_working_order(OrderId id) const noexcept {
    for (const auto& order : working_orders_) {
        if (order.used && order.order.client_order_id == id) return &order;
    }
    return nullptr;
}

void PCPArbitrageStrategy::maybe_finalize_attempt(Timestamp now_ns) noexcept {
    if (!attempt_active_ || live_order_count() != 0) return;

    if (!cleanup_active_) {
        // Stage 1: the original three-leg basket is done or cancelled. Decide
        // whether this was a clean basket completion or whether a partial fill
        // left residual exposure behind.
        bool any_primary_orders = false;
        bool any_primary_fills = false;
        bool all_primary_full = true;
        for (const auto& order : working_orders_) {
            if (!order.used || order.cleanup) continue;
            any_primary_orders = true;
            any_primary_fills = any_primary_fills || order.filled_volume > 0;
            if (order.filled_volume < order.order.volume) {
                all_primary_full = false;
            }
        }

        if (any_primary_orders && all_primary_full) {
            clear_attempt_state();
            next_trigger_ts_ns_ = now_ns
                + static_cast<int64_t>(params_->cooldown_ms.load(std::memory_order_relaxed) * 1'000'000.0);
            return;
        }
        if (!any_primary_fills || !params_->cleanup_on_partial.load(std::memory_order_relaxed)) {
            clear_attempt_state();
            next_trigger_ts_ns_ = now_ns
                + static_cast<int64_t>(params_->cooldown_ms.load(std::memory_order_relaxed) * 1'000'000.0);
            return;
        }

        cleanup_active_ = true;
        submit_cleanup_orders(now_ns);
        return;
    }

    // Stage 2: cleanup is active. Recompute residual inventory by leg and keep
    // flattening until the tuple is fully flat.
    std::array<uint16_t, 3> instruments{
        active_call_id_, active_put_id_, active_future_id_,
    };
    std::array<int32_t, 3> net_qty{};
    for (const auto& order : working_orders_) {
        if (!order.used) continue;
        for (std::size_t i = 0; i < instruments.size(); ++i) {
            if (order.order.instrument_id != instruments[i]) continue;
            net_qty[i] += side_sign(order.order.side) * order.filled_volume;
        }
    }

    const bool flat = std::all_of(net_qty.begin(),
                                  net_qty.end(),
                                  [](int32_t qty) { return qty == 0; });
    if (flat) {
        clear_attempt_state();
        next_trigger_ts_ns_ = now_ns
            + static_cast<int64_t>(params_->cooldown_ms.load(std::memory_order_relaxed) * 1'000'000.0);
        return;
    }

    submit_cleanup_orders(now_ns);
}

void PCPArbitrageStrategy::evaluate(Timestamp now_ns) noexcept {
    uint32_t suppress_flags = ArbSuppressNone;
    if (!params_) {
        refresh_monitor_state(suppress_flags, now_ns);
        return;
    }
    if (!params_->enabled.load(std::memory_order_relaxed)) {
        suppress_flags |= ArbSuppressDisabled;
        refresh_monitor_state(suppress_flags, now_ns);
        return;
    }

    if (pair_count_ == 0) {
        suppress_flags |= ArbSuppressNoPairs;
        refresh_monitor_state(suppress_flags, now_ns);
        return;
    }

    const int64_t scan_interval_ns = std::max<int64_t>(
        100'000LL,
        static_cast<int64_t>(params_->scan_interval_ms.load(std::memory_order_relaxed) * 1'000'000.0));
    const int64_t cleanup_timeout_ns = std::max<int64_t>(
        1'000'000LL,
        static_cast<int64_t>(params_->cleanup_timeout_ms.load(std::memory_order_relaxed) * 1'000'000.0));

    cancel_stale_orders(now_ns, cleanup_timeout_ns);
    maybe_finalize_attempt(now_ns);

    if (now_ns - last_scan_ts_ns_ < scan_interval_ns) {
        if (cleanup_active_) suppress_flags |= ArbSuppressCleanupPending;
        refresh_monitor_state(last_suppress_flags_ | suppress_flags, now_ns);
        return;
    }
    last_scan_ts_ns_ = now_ns;

    // The steady-state evaluation loop is:
    //   1. cancel stale live orders
    //   2. finalize or clean up any previous attempt
    //   3. scan all parity pairs
    //   4. if idle and above threshold, launch a new basket
    Pair best_pair{};
    Direction best_dir = Direction::None;
    Volume best_volume = 0;
    double best_edge_ticks = 0.0;
    (void)scan_best_opportunity(now_ns,
                                &best_pair,
                                &best_dir,
                                &best_volume,
                                &best_edge_ticks,
                                &suppress_flags);
    monitor_last_edge_ticks_.store(best_edge_ticks, std::memory_order_relaxed);

    if (cleanup_active_) suppress_flags |= ArbSuppressCleanupPending;
    if (attempt_active_ || live_order_count() > 0) {
        refresh_monitor_state(suppress_flags, now_ns);
        return;
    }
    if (now_ns < next_trigger_ts_ns_) {
        suppress_flags |= ArbSuppressCooldown;
        refresh_monitor_state(suppress_flags, now_ns);
        return;
    }
    if (best_dir == Direction::None || best_volume <= 0
        || best_edge_ticks < params_->min_edge_ticks.load(std::memory_order_relaxed)) {
        refresh_monitor_state(suppress_flags, now_ns);
        return;
    }

    start_attempt(best_pair, best_dir, best_volume, best_edge_ticks, now_ns);
    refresh_monitor_state(suppress_flags, now_ns);
}

void PCPArbitrageStrategy::on_order_ack(const Order& order) noexcept {
    if (!owns_order_id(order.client_order_id)) return;
    if (pre_risk_) pre_risk_->on_order_ack(order);
    if (auto* state = find_working_order(order.client_order_id)) {
        state->acked = true;
    }
    refresh_monitor_state(last_suppress_flags_, get_monotonic_ns());
}

void PCPArbitrageStrategy::on_fill(const Trade& trade) noexcept {
    if (!owns_order_id(trade.client_order_id)) return;
    if (auto* state = find_working_order(trade.client_order_id)) {
        state->filled_volume += trade.fill_volume;
        const bool fully_filled = state->filled_volume >= state->order.volume;
        if (pre_risk_) {
            pre_risk_->on_order_fill(trade.client_order_id, trade.fill_volume, fully_filled);
        }
        if (fully_filled) {
            state->done = true;
        }
    }
    refresh_monitor_state(last_suppress_flags_, get_monotonic_ns());
}

void PCPArbitrageStrategy::on_order_cancel(OrderId id) noexcept {
    if (!owns_order_id(id)) return;
    if (pre_risk_) pre_risk_->on_order_cancel(id);
    if (auto* state = find_working_order(id)) {
        state->done = true;
    }
    refresh_monitor_state(last_suppress_flags_, get_monotonic_ns());
}

void PCPArbitrageStrategy::on_order_reject(const Order& order) noexcept {
    if (!owns_order_id(order.client_order_id)) return;
    if (pre_risk_) pre_risk_->on_order_cancel(order.client_order_id);
    if (auto* state = find_working_order(order.client_order_id)) {
        state->done = true;
    }
    refresh_monitor_state(last_suppress_flags_, get_monotonic_ns());
}

} // namespace omm
