#include "strategy/pcp_arbitrage.h"
#include "common/thread_utils.h"

#include <algorithm>
#include <cmath>

namespace omm {

namespace {

// For aggressive/taker-style execution we cross the spread:
//   buy  -> lift best ask
//   sell -> hit best bid
[[nodiscard]] double bid_price_for_side(const TopOfBookTick& tick, Side side) noexcept {
    return side == Side::Buy ? tick.ask_price : tick.bid_price;
}

[[nodiscard]] int side_sign(Side side) noexcept {
    return side == Side::Buy ? 1 : -1;
}

} // namespace

/**
 * @brief Implements Init.
 * @param product_idx Parameter supplied by the caller.
 * @param intent_buf Parameter supplied by the caller.
 * @param params Parameter supplied by the caller.
 * @param instruments Parameter supplied by the caller.
 * @param tick_snapshot Parameter supplied by the caller.
 * @param greeks_snapshot Parameter supplied by the caller.
 * @param risk_free_rate Parameter supplied by the caller.
 * @param hard_risk_cfg Parameter supplied by the caller.
 * @param account_id Parameter supplied by the caller.
 * @return None.
 * @note Noexcept API preserves hot-path failure and latency invariants.
 */
void PCPArbitrageStrategy::init(uint8_t product_idx,
                                SPSCRingBuffer<ArbIntent, 256>* intent_buf,
                                AtomicArbParams* params,
                                const Instrument* instruments,
                                const SnapshotArray<TopOfBookTick, MAX_INSTRUMENTS>* tick_snapshot,
                                const SnapshotArray<Greeks, MAX_INSTRUMENTS>* greeks_snapshot,
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
    last_pair_monitor_publish_ts_ns_ = 0;
    attempt_active_ = false;
    cleanup_active_ = false;
    active_call_id_ = INVALID_INSTRUMENT_ID;
    active_put_id_ = INVALID_INSTRUMENT_ID;
    active_future_id_ = INVALID_INSTRUMENT_ID;
    last_suppress_flags_ = ArbSuppressNone;
    for (auto& order : working_orders_) order = WorkingOrder{};
    first_pair_for_instrument_.fill(kInvalidPairIndex);
    for (auto& links : next_pair_for_instrument_) {
        links.fill(kInvalidPairIndex);
    }
    build_pairs();
    refresh_monitor_state(pair_count_ == 0 ? ArbSuppressNoPairs : ArbSuppressNone, get_monotonic_ns());
}

/**
 * @brief Implements Owns order id.
 * @param id Parameter supplied by the caller.
 * @return Return value produced by the operation.
 * @note Noexcept API preserves hot-path failure and latency invariants.
 */
bool PCPArbitrageStrategy::owns_order_id(OrderId id) const noexcept {
    return is_arb_order_id(id)
        && arb_order_product(id) == product_idx_
        && arb_order_type(id) == strategy_type();
}

/**
 * @brief Implements Is enabled.
 * @return Return value produced by the operation.
 * @note Noexcept API preserves hot-path failure and latency invariants.
 */
bool PCPArbitrageStrategy::is_enabled() const noexcept {
    return params_ && params_->enabled.load(std::memory_order_relaxed);
}

/**
 * @brief Implements Read monitor state.
 * @param out Parameter supplied by the caller.
 * @return Return value produced by the operation.
 * @note Noexcept API preserves hot-path failure and latency invariants.
 */
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

/**
 * @brief Implements Read pcp monitor states.
 * @param out Parameter supplied by the caller.
 * @param max_count Parameter supplied by the caller.
 * @return Return value produced by the operation.
 * @note Noexcept API preserves hot-path failure and latency invariants.
 */
int PCPArbitrageStrategy::read_pcp_monitor_states(PCPPairMonitorState* out,
                                                  int max_count) const noexcept {
    if (out == nullptr || max_count <= 0) return 0;

    for (int attempt = 0; attempt < 4; ++attempt) {
        const uint64_t v1 = monitor_pair_snapshot_version_.load(std::memory_order_acquire);
        if ((v1 & 1u) != 0) {
            spin_pause();
            continue;
        }

        const uint16_t count = monitor_pair_count_;
        const int to_copy = std::min<int>(count, max_count);
        for (int i = 0; i < to_copy; ++i) {
            out[i] = monitor_pairs_[static_cast<std::size_t>(i)];
        }

        const uint64_t v2 = monitor_pair_snapshot_version_.load(std::memory_order_acquire);
        if (v1 == v2 && (v2 & 1u) == 0) {
            return to_copy;
        }
    }
    return 0;
}

/**
 * @brief Implements Build pairs.
 * @return None.
 * @note Noexcept API preserves hot-path failure and latency invariants.
 */
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
            const uint16_t pair_index = static_cast<uint16_t>(pair_count_ - 1);
            next_pair_for_instrument_[pair_index][0] = first_pair_for_instrument_[call_id];
            first_pair_for_instrument_[call_id] = pair_index;
            next_pair_for_instrument_[pair_index][1] = first_pair_for_instrument_[put_id];
            first_pair_for_instrument_[put_id] = pair_index;
            next_pair_for_instrument_[pair_index][2] = first_pair_for_instrument_[future_id];
            first_pair_for_instrument_[future_id] = pair_index;
            break;
        }
    }
}

/**
 * @brief Implements Discount factor.
 * @param pair Parameter supplied by the caller.
 * @param now_ns Parameter supplied by the caller.
 * @return Return value produced by the operation.
 * @note Noexcept API preserves hot-path failure and latency invariants.
 */
double PCPArbitrageStrategy::discount_factor(const Pair& pair, Timestamp now_ns) const noexcept {
    if (!pair.active || pair.call_id >= MAX_INSTRUMENTS) return 1.0;
    const Instrument& call = instruments_[pair.call_id];
    static constexpr double kNsPerYear = 365.0 * 24.0 * 3600.0 * 1e9;
    double T = (call.expiry_epoch_ns - now_ns) / kNsPerYear;
    if (T < 1e-4) T = 1e-4;
    // Futures-option PCP uses the discounted futures-minus-strike term.
    return std::exp(-risk_free_rate_ * T);
}

/**
 * @brief Implements Market valid.
 * @param tick Parameter supplied by the caller.
 * @param now_ns Parameter supplied by the caller.
 * @return Return value produced by the operation.
 * @note Noexcept API preserves hot-path failure and latency invariants.
 */
bool PCPArbitrageStrategy::market_valid(const TopOfBookTick& tick, Timestamp now_ns) const noexcept {
    return tick.recv_ts_ns > 0
        && now_ns - tick.recv_ts_ns <= kMarketStaleNs
        && tick.bid_price > 0.0
        && tick.ask_price > 0.0
        && tick.ask_price >= tick.bid_price
        && tick.bid_volume > 0
        && tick.ask_volume > 0;
}

/**
 * @brief Implements Executable volume.
 * @param pair Parameter supplied by the caller.
 * @param dir Parameter supplied by the caller.
 * @param max_order_volume Parameter supplied by the caller.
 * @return Return value produced by the operation.
 * @note Noexcept API preserves hot-path failure and latency invariants.
 */
Volume PCPArbitrageStrategy::executable_volume(const Pair& pair,
                                               Direction dir,
                                               int max_order_volume) const noexcept {
    if (!pair.active || max_order_volume <= 0) return 0;
    TopOfBookTick call_tick{};
    TopOfBookTick put_tick{};
    TopOfBookTick future_tick{};
    if (!tick_snapshot_
        || !tick_snapshot_->read(pair.call_id, &call_tick)
        || !tick_snapshot_->read(pair.put_id, &put_tick)
        || !tick_snapshot_->read(pair.future_id, &future_tick)) {
        return 0;
    }

    if (dir == Direction::LongSyntheticShortFuture) {
        // Buy call at ask, sell put at bid, sell future at bid. Size is bounded
        // by the tightest displayed size across the three aggressive legs.
        return std::max<Volume>(0, std::min({max_order_volume,
                                             call_tick.ask_volume,
                                             put_tick.bid_volume,
                                             future_tick.bid_volume}));
    }
    if (dir == Direction::ShortSyntheticLongFuture) {
        // Sell call at bid, buy put at ask, buy future at ask.
        return std::max<Volume>(0, std::min({max_order_volume,
                                             call_tick.bid_volume,
                                             put_tick.ask_volume,
                                             future_tick.ask_volume}));
    }
    return 0;
}

/**
 * @brief Implements Scan best opportunity.
 * @param now_ns Parameter supplied by the caller.
 * @param best_pair Parameter supplied by the caller.
 * @param best_pair_index Parameter supplied by the caller.
 * @param best_dir Parameter supplied by the caller.
 * @param best_volume Parameter supplied by the caller.
 * @param best_edge_ticks Parameter supplied by the caller.
 * @param suppress_flags Parameter supplied by the caller.
 * @param trigger_instrument_id Parameter supplied by the caller.
 * @return Return value produced by the operation.
 * @note Noexcept API preserves hot-path failure and latency invariants.
 */
bool PCPArbitrageStrategy::scan_best_opportunity(Timestamp now_ns,
                                                 Pair* best_pair,
                                                 uint16_t* best_pair_index,
                                                 Direction* best_dir,
                                                 Volume* best_volume,
                                                 double* best_edge_ticks,
                                                 uint32_t* suppress_flags,
                                                 uint16_t trigger_instrument_id) noexcept {
    if (best_pair == nullptr || best_pair_index == nullptr || best_dir == nullptr || best_volume == nullptr
        || best_edge_ticks == nullptr || suppress_flags == nullptr) {
        return false;
    }

    bool saw_valid_pair = false;
    *best_pair = Pair{};
    *best_pair_index = static_cast<uint16_t>(pair_count_);
    *best_dir = Direction::None;
    *best_volume = 0;
    *best_edge_ticks = 0.0;

    const ArbParamsConfig cfg = params_->snapshot();

    auto scan_pair = [&](uint16_t i) noexcept {
        const Pair& pair = pairs_[i];
        if (!pair.active) return;

        TopOfBookTick call_tick{};
        TopOfBookTick put_tick{};
        TopOfBookTick future_tick{};
        if (!tick_snapshot_
            || !tick_snapshot_->read(pair.call_id, &call_tick)
            || !tick_snapshot_->read(pair.put_id, &put_tick)
            || !tick_snapshot_->read(pair.future_id, &future_tick)) {
            return;
        }
        if (!market_valid(call_tick, now_ns)
            || !market_valid(put_tick, now_ns)
            || !market_valid(future_tick, now_ns)) {
            return;
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
            (discount * (future_tick.bid_price - pair.strike)
             - (call_tick.ask_price - put_tick.bid_price)) / future_tick_size;
        const Volume long_synth_volume =
            executable_volume(pair, Direction::LongSyntheticShortFuture, cfg.max_order_volume);
        if (long_synth_volume > 0 && long_synth_edge > *best_edge_ticks) {
            *best_pair = pair;
            *best_pair_index = i;
            *best_dir = Direction::LongSyntheticShortFuture;
            *best_volume = long_synth_volume;
            *best_edge_ticks = long_synth_edge;
        }

        const double short_synth_edge =
            ((call_tick.bid_price - put_tick.ask_price)
             - discount * (future_tick.ask_price - pair.strike)) / future_tick_size;
        const Volume short_synth_volume =
            executable_volume(pair, Direction::ShortSyntheticLongFuture, cfg.max_order_volume);
        if (short_synth_volume > 0 && short_synth_edge > *best_edge_ticks) {
            *best_pair = pair;
            *best_pair_index = i;
            *best_dir = Direction::ShortSyntheticLongFuture;
            *best_volume = short_synth_volume;
            *best_edge_ticks = short_synth_edge;
        }
    };

    if (trigger_instrument_id < MAX_INSTRUMENTS) {
        for (uint16_t pair_index = first_pair_for_instrument_[trigger_instrument_id];
             pair_index != kInvalidPairIndex && pair_index < pair_count_;
             pair_index = next_pair_for_instrument(pair_index, trigger_instrument_id)) {
            scan_pair(pair_index);
        }
    } else {
        for (uint16_t i = 0; i < pair_count_; ++i) {
            scan_pair(i);
        }
    }

    if (!saw_valid_pair) {
        *suppress_flags |= ArbSuppressInvalidMarket;
        return false;
    }
    return *best_dir != Direction::None;
}

/**
 * @brief Implements Next pair for instrument.
 * @param pair_index Parameter supplied by the caller.
 * @param instrument_id Parameter supplied by the caller.
 * @return Return value produced by the operation.
 * @note Noexcept API preserves hot-path failure and latency invariants.
 */
uint16_t PCPArbitrageStrategy::next_pair_for_instrument(uint16_t pair_index,
                                                        uint16_t instrument_id) const noexcept {
    if (pair_index >= pair_count_) return kInvalidPairIndex;
    const Pair& pair = pairs_[pair_index];
    if (pair.call_id == instrument_id) return next_pair_for_instrument_[pair_index][0];
    if (pair.put_id == instrument_id) return next_pair_for_instrument_[pair_index][1];
    if (pair.future_id == instrument_id) return next_pair_for_instrument_[pair_index][2];
    return kInvalidPairIndex;
}

/**
 * @brief Implements Publish pair monitor states.
 * @param now_ns Parameter supplied by the caller.
 * @param selected_pair_index Parameter supplied by the caller.
 * @param selected_dir Parameter supplied by the caller.
 * @param selected_edge_ticks Parameter supplied by the caller.
 * @return None.
 * @note Noexcept API preserves hot-path failure and latency invariants.
 */
void PCPArbitrageStrategy::publish_pair_monitor_states(Timestamp now_ns,
                                                       uint16_t selected_pair_index,
                                                       Direction selected_dir,
                                                       double selected_edge_ticks) noexcept {
    const uint64_t cur = monitor_pair_snapshot_version_.load(std::memory_order_relaxed);
    monitor_pair_snapshot_version_.store(cur + 1, std::memory_order_release);

    monitor_pair_count_ = pair_count_;
    for (uint16_t i = 0; i < pair_count_; ++i) {
        const Pair& pair = pairs_[i];
        PCPPairMonitorState row{};
        row.product_index = product_idx_;
        row.strategy_type = strategy_type();
        row.call_id = pair.call_id;
        row.put_id = pair.put_id;
        row.future_id = pair.future_id;
        row.expiry_date = pair.expiry_date;
        row.strike = pair.strike;
        row.selected = i == selected_pair_index;
        row.eval_ts_ns = now_ns;

        if (!pair.active || pair.call_id >= MAX_INSTRUMENTS || pair.put_id >= MAX_INSTRUMENTS
            || pair.future_id >= MAX_INSTRUMENTS) {
            monitor_pairs_[i] = row;
            continue;
        }

        TopOfBookTick call_tick{};
        TopOfBookTick put_tick{};
        TopOfBookTick future_tick{};
        if (!tick_snapshot_
            || !tick_snapshot_->read(pair.call_id, &call_tick)
            || !tick_snapshot_->read(pair.put_id, &put_tick)
            || !tick_snapshot_->read(pair.future_id, &future_tick)) {
            monitor_pairs_[i] = row;
            continue;
        }
        const bool valid_call = market_valid(call_tick, now_ns);
        const bool valid_put = market_valid(put_tick, now_ns);
        const bool valid_future = market_valid(future_tick, now_ns);
        row.market_valid = valid_call && valid_put && valid_future;

        row.discount_factor = discount_factor(pair, now_ns);
        row.future_bid = future_tick.bid_price;
        row.future_ask = future_tick.ask_price;

        if (row.discount_factor > 1e-12
            && call_tick.bid_price > 0.0 && call_tick.ask_price > 0.0
            && put_tick.bid_price > 0.0 && put_tick.ask_price > 0.0) {
            row.synthetic_bid = pair.strike
                + (call_tick.bid_price - put_tick.ask_price) / row.discount_factor;
            row.synthetic_ask = pair.strike
                + (call_tick.ask_price - put_tick.bid_price) / row.discount_factor;
        }

        const double future_tick_size =
            instruments_[pair.future_id].tick_size > 0.0 ? instruments_[pair.future_id].tick_size : 1.0;
        row.long_synth_edge_ticks =
            (row.discount_factor * (future_tick.bid_price - pair.strike)
             - (call_tick.ask_price - put_tick.bid_price)) / future_tick_size;
        row.short_synth_edge_ticks =
            ((call_tick.bid_price - put_tick.ask_price)
             - row.discount_factor * (future_tick.ask_price - pair.strike)) / future_tick_size;

        const Volume long_volume =
            executable_volume(pair, Direction::LongSyntheticShortFuture,
                              params_->max_order_volume.load(std::memory_order_relaxed));
        const Volume short_volume =
            executable_volume(pair, Direction::ShortSyntheticLongFuture,
                              params_->max_order_volume.load(std::memory_order_relaxed));
        if (row.selected) {
            row.best_direction = selected_dir == Direction::LongSyntheticShortFuture
                ? PCPMonitorDirection::LongSyntheticShortFuture
                : (selected_dir == Direction::ShortSyntheticLongFuture
                    ? PCPMonitorDirection::ShortSyntheticLongFuture
                    : PCPMonitorDirection::None);
            row.best_edge_ticks = selected_edge_ticks;
            row.best_volume = (selected_dir == Direction::LongSyntheticShortFuture)
                ? long_volume
                : (selected_dir == Direction::ShortSyntheticLongFuture ? short_volume : 0);
        } else if (short_volume > 0 && row.short_synth_edge_ticks >= row.long_synth_edge_ticks) {
            row.best_direction = PCPMonitorDirection::ShortSyntheticLongFuture;
            row.best_edge_ticks = row.short_synth_edge_ticks;
            row.best_volume = short_volume;
        } else if (long_volume > 0) {
            row.best_direction = PCPMonitorDirection::LongSyntheticShortFuture;
            row.best_edge_ticks = row.long_synth_edge_ticks;
            row.best_volume = long_volume;
        } else {
            row.best_direction = PCPMonitorDirection::None;
            row.best_edge_ticks = std::max(row.long_synth_edge_ticks, row.short_synth_edge_ticks);
            row.best_volume = 0;
        }

        monitor_pairs_[i] = row;
    }

    monitor_pair_snapshot_version_.store(cur + 2, std::memory_order_release);
}

/**
 * @brief Implements Enqueue order.
 * @param instrument_id Parameter supplied by the caller.
 * @param side Parameter supplied by the caller.
 * @param price Parameter supplied by the caller.
 * @param volume Parameter supplied by the caller.
 * @param cleanup Parameter supplied by the caller.
 * @param edge_ticks Parameter supplied by the caller.
 * @param pair Parameter supplied by the caller.
 * @param now_ns Parameter supplied by the caller.
 * @return Return value produced by the operation.
 * @note Noexcept API preserves hot-path failure and latency invariants.
 */
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
    order.price_type = OrderPriceType::Limit;
    order.order_type = OrderType::GFD;
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

/**
 * @brief Implements Start attempt.
 * @param pair Parameter supplied by the caller.
 * @param dir Parameter supplied by the caller.
 * @param volume Parameter supplied by the caller.
 * @param edge_ticks Parameter supplied by the caller.
 * @param now_ns Parameter supplied by the caller.
 * @return None.
 * @note Noexcept API preserves hot-path failure and latency invariants.
 */
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

    TopOfBookTick call_tick{};
    TopOfBookTick put_tick{};
    TopOfBookTick future_tick{};
    if (!tick_snapshot_
        || !tick_snapshot_->read(pair.call_id, &call_tick)
        || !tick_snapshot_->read(pair.put_id, &put_tick)
        || !tick_snapshot_->read(pair.future_id, &future_tick)) {
        last_suppress_flags_ |= ArbSuppressInvalidMarket;
        return;
    }

    if (dir == Direction::LongSyntheticShortFuture) {
        // Synthetic long future = +Call - Put. We hedge that by shorting the
        // listed future when the synthetic is cheap.
        (void)enqueue_order(pair.call_id, Side::Buy, call_tick.ask_price, volume, false, edge_ticks, pair, now_ns);
        (void)enqueue_order(pair.put_id, Side::Sell, put_tick.bid_price, volume, false, edge_ticks, pair, now_ns);
        (void)enqueue_order(pair.future_id, Side::Sell, future_tick.bid_price, volume, false, edge_ticks, pair, now_ns);
    } else if (dir == Direction::ShortSyntheticLongFuture) {
        // Synthetic short future = -Call + Put. We hedge that by buying the
        // listed future when the synthetic is rich.
        (void)enqueue_order(pair.call_id, Side::Sell, call_tick.bid_price, volume, false, edge_ticks, pair, now_ns);
        (void)enqueue_order(pair.put_id, Side::Buy, put_tick.ask_price, volume, false, edge_ticks, pair, now_ns);
        (void)enqueue_order(pair.future_id, Side::Buy, future_tick.ask_price, volume, false, edge_ticks, pair, now_ns);
    }

    if (live_order_count() == 0) {
        attempt_active_ = false;
        next_trigger_ts_ns_ = now_ns
            + static_cast<int64_t>(params_->cooldown_ms.load(std::memory_order_relaxed) * 1'000'000.0);
    }
}

/**
 * @brief Implements Cancel stale orders.
 * @param now_ns Parameter supplied by the caller.
 * @param timeout_ns Parameter supplied by the caller.
 * @return None.
 * @note Noexcept API preserves hot-path failure and latency invariants.
 */
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

/**
 * @brief Implements Submit cleanup orders.
 * @param now_ns Parameter supplied by the caller.
 * @return None.
 * @note Noexcept API preserves hot-path failure and latency invariants.
 */
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
        TopOfBookTick tick{};
        if (!tick_snapshot_ || !tick_snapshot_->read(instrument_id, &tick)) {
            last_suppress_flags_ |= ArbSuppressCleanupPending | ArbSuppressInvalidMarket;
            continue;
        }
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

/**
 * @brief Implements Clear attempt state.
 * @return None.
 * @note Noexcept API preserves hot-path failure and latency invariants.
 */
void PCPArbitrageStrategy::clear_attempt_state() noexcept {
    for (auto& order : working_orders_) order = WorkingOrder{};
    attempt_active_ = false;
    cleanup_active_ = false;
    active_call_id_ = INVALID_INSTRUMENT_ID;
    active_put_id_ = INVALID_INSTRUMENT_ID;
    active_future_id_ = INVALID_INSTRUMENT_ID;
}

/**
 * @brief Implements Set active pair.
 * @param pair Parameter supplied by the caller.
 * @return None.
 * @note Noexcept API preserves hot-path failure and latency invariants.
 */
void PCPArbitrageStrategy::set_active_pair(const Pair& pair) noexcept {
    active_call_id_ = pair.call_id;
    active_put_id_ = pair.put_id;
    active_future_id_ = pair.future_id;
}

/**
 * @brief Implements Refresh monitor state.
 * @param suppress_flags Parameter supplied by the caller.
 * @param now_ns Parameter supplied by the caller.
 * @return None.
 * @note Noexcept API preserves hot-path failure and latency invariants.
 */
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

/**
 * @brief Implements Live order count.
 * @return Return value produced by the operation.
 * @note Noexcept API preserves hot-path failure and latency invariants.
 */
int PCPArbitrageStrategy::live_order_count() const noexcept {
    int count = 0;
    for (const auto& order : working_orders_) {
        if (order.used && !order.done) ++count;
    }
    return count;
}

/**
 * @brief Implements Find working order.
 * @param id Parameter supplied by the caller.
 * @return Return value produced by the operation.
 * @note Noexcept API preserves hot-path failure and latency invariants.
 */
PCPArbitrageStrategy::WorkingOrder* PCPArbitrageStrategy::find_working_order(OrderId id) noexcept {
    for (auto& order : working_orders_) {
        if (order.used && order.order.client_order_id == id) return &order;
    }
    return nullptr;
}

/**
 * @brief Implements Find working order.
 * @param id Parameter supplied by the caller.
 * @return Return value produced by the operation.
 * @note Noexcept API preserves hot-path failure and latency invariants.
 */
const PCPArbitrageStrategy::WorkingOrder* PCPArbitrageStrategy::find_working_order(OrderId id) const noexcept {
    for (const auto& order : working_orders_) {
        if (order.used && order.order.client_order_id == id) return &order;
    }
    return nullptr;
}

/**
 * @brief Implements Maybe finalize attempt.
 * @param now_ns Parameter supplied by the caller.
 * @return None.
 * @note Noexcept API preserves hot-path failure and latency invariants.
 */
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

/**
 * @brief Implements Evaluate impl.
 * @param now_ns Parameter supplied by the caller.
 * @param trigger_instrument_id Parameter supplied by the caller.
 * @param force_pair_monitor_publish Parameter supplied by the caller.
 * @return None.
 * @note Noexcept API preserves hot-path failure and latency invariants.
 */
void PCPArbitrageStrategy::evaluate_impl(Timestamp now_ns,
                                         uint16_t trigger_instrument_id,
                                         bool force_pair_monitor_publish) noexcept {
    uint32_t suppress_flags = ArbSuppressNone;
    if (!params_) {
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
    const bool enabled = params_->enabled.load(std::memory_order_relaxed);

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
    uint16_t selected_pair_index = static_cast<uint16_t>(pair_count_);
    Direction best_dir = Direction::None;
    Volume best_volume = 0;
    double best_edge_ticks = 0.0;
    (void)scan_best_opportunity(now_ns,
                                &best_pair,
                                &selected_pair_index,
                                &best_dir,
                                &best_volume,
                                &best_edge_ticks,
                                &suppress_flags,
                                trigger_instrument_id);
    const bool should_publish_pairs =
        force_pair_monitor_publish
        || now_ns - last_pair_monitor_publish_ts_ns_ >= kPairMonitorPublishIntervalNs;
    if (should_publish_pairs) {
        publish_pair_monitor_states(now_ns, selected_pair_index, best_dir, best_edge_ticks);
        last_pair_monitor_publish_ts_ns_ = now_ns;
    }
    monitor_last_edge_ticks_.store(best_edge_ticks, std::memory_order_relaxed);

    if (cleanup_active_) suppress_flags |= ArbSuppressCleanupPending;
    if (attempt_active_ || live_order_count() > 0) {
        refresh_monitor_state(suppress_flags, now_ns);
        return;
    }
    if (!enabled) {
        suppress_flags |= ArbSuppressDisabled;
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
    if (!should_publish_pairs) {
        publish_pair_monitor_states(now_ns, selected_pair_index, best_dir, best_edge_ticks);
        last_pair_monitor_publish_ts_ns_ = now_ns;
    }
    refresh_monitor_state(suppress_flags, now_ns);
}

/**
 * @brief Implements Evaluate.
 * @param now_ns Parameter supplied by the caller.
 * @return None.
 * @note Noexcept API preserves hot-path failure and latency invariants.
 */
void PCPArbitrageStrategy::evaluate(Timestamp now_ns) noexcept {
    evaluate_impl(now_ns, INVALID_INSTRUMENT_ID, true);
}

/**
 * @brief Implements On market update.
 * @param instrument_id Parameter supplied by the caller.
 * @param now_ns Parameter supplied by the caller.
 * @return None.
 * @note Noexcept API preserves hot-path failure and latency invariants.
 */
void PCPArbitrageStrategy::on_market_update(uint16_t instrument_id, Timestamp now_ns) noexcept {
    evaluate_impl(now_ns, instrument_id, false);
}

/**
 * @brief Implements On timer.
 * @param now_ns Parameter supplied by the caller.
 * @return None.
 * @note Noexcept API preserves hot-path failure and latency invariants.
 */
void PCPArbitrageStrategy::on_timer(Timestamp now_ns) noexcept {
    uint32_t suppress_flags = last_suppress_flags_;
    if (!params_) {
        refresh_monitor_state(suppress_flags, now_ns);
        return;
    }
    const int64_t cleanup_timeout_ns = std::max<int64_t>(
        1'000'000LL,
        static_cast<int64_t>(params_->cleanup_timeout_ms.load(std::memory_order_relaxed) * 1'000'000.0));
    cancel_stale_orders(now_ns, cleanup_timeout_ns);
    maybe_finalize_attempt(now_ns);
    if (cleanup_active_) suppress_flags |= ArbSuppressCleanupPending;
    refresh_monitor_state(suppress_flags, now_ns);
}

/**
 * @brief Implements On order ack.
 * @param order Parameter supplied by the caller.
 * @return None.
 * @note Noexcept API preserves hot-path failure and latency invariants.
 */
void PCPArbitrageStrategy::on_order_ack(const Order& order) noexcept {
    if (!owns_order_id(order.client_order_id)) return;
    if (pre_risk_) pre_risk_->on_order_ack(order);
    if (auto* state = find_working_order(order.client_order_id)) {
        state->acked = true;
    }
    refresh_monitor_state(last_suppress_flags_, get_monotonic_ns());
}

/**
 * @brief Implements On fill.
 * @param trade Parameter supplied by the caller.
 * @return None.
 * @note Noexcept API preserves hot-path failure and latency invariants.
 */
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

/**
 * @brief Implements On order cancel.
 * @param id Parameter supplied by the caller.
 * @return None.
 * @note Noexcept API preserves hot-path failure and latency invariants.
 */
void PCPArbitrageStrategy::on_order_cancel(OrderId id) noexcept {
    if (!owns_order_id(id)) return;
    if (pre_risk_) pre_risk_->on_order_cancel(id);
    if (auto* state = find_working_order(id)) {
        state->done = true;
    }
    refresh_monitor_state(last_suppress_flags_, get_monotonic_ns());
}

/**
 * @brief Implements On order reject.
 * @param order Parameter supplied by the caller.
 * @return None.
 * @note Noexcept API preserves hot-path failure and latency invariants.
 */
void PCPArbitrageStrategy::on_order_reject(const Order& order) noexcept {
    if (!owns_order_id(order.client_order_id)) return;
    if (pre_risk_) pre_risk_->on_order_cancel(order.client_order_id);
    if (auto* state = find_working_order(order.client_order_id)) {
        state->done = true;
    }
    refresh_monitor_state(last_suppress_flags_, get_monotonic_ns());
}

} // namespace omm
