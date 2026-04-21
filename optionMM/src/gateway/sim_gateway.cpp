#include "gateway/sim_gateway.h"

#include "common/thread_utils.h"

#include <algorithm>
#include <chrono>
#include <cstdio>

namespace omm {

namespace {

constexpr int kDefaultWorkerSleepMs = 5;
constexpr int kFastWorkerSleepUs = 50;

double clamp_probability(double value) noexcept {
    return std::clamp(value, 0.0, 1.0);
}

} // namespace

SimGateway::~SimGateway() {
    disconnect();
}

void SimGateway::set_sim_config(const SimConfig& cfg) noexcept {
    settings_.ack_latency_ms = std::max(0, cfg.gateway_ack_latency_ms);
    settings_.cancel_latency_ms = std::max(0, cfg.gateway_cancel_latency_ms);
    settings_.fill_interval_ms = std::max(1, cfg.gateway_fill_interval_ms);
    settings_.order_fill_probability = clamp_probability(cfg.gateway_order_fill_probability);
    settings_.quote_cross_fill_probability = clamp_probability(cfg.gateway_quote_cross_fill_probability);
    settings_.quote_passive_fill_probability = clamp_probability(cfg.gateway_quote_passive_fill_probability);
    settings_.partial_fill_probability = clamp_probability(cfg.gateway_partial_fill_probability);
    settings_.reject_probability = clamp_probability(cfg.gateway_reject_probability);
    settings_.max_fill_size = cfg.gateway_max_fill_size;
    settings_.slippage_ticks = std::max(0, cfg.gateway_slippage_ticks);
    settings_.quote_near_touch_ticks = std::max(0.0, cfg.gateway_quote_near_touch_ticks);
    settings_.random_seed = cfg.random_seed;
}

bool SimGateway::send_order(const Order& order) noexcept {
    if (!connected_.load(std::memory_order_relaxed)) return false;

    orders_sent_.fetch_add(1, std::memory_order_relaxed);

    const Timestamp now_ns = get_monotonic_ns();
    const uint64_t exch_id = exchange_order_id_seq_.fetch_add(1, std::memory_order_relaxed);

    std::lock_guard<std::mutex> lock(state_mutex_);
    auto slot_it = std::find_if(active_orders_.begin(), active_orders_.end(), [](const ActiveOrder& item) {
        return !item.used;
    });
    if (slot_it == active_orders_.end()) return false;

    ActiveOrder& slot = *slot_it;
    slot = ActiveOrder{};
    slot.used = true;
    slot.order = order;
    slot.order.exchange_order_id = exch_id;
    slot.order.status = OrderStatus::New;
    slot.remaining_volume = order.volume;
    slot.exchange_order_id = exch_id;
    slot.ack_due_ns = now_ns + static_cast<Timestamp>(settings_.ack_latency_ms) * 1'000'000LL;
    slot.next_fill_due_ns = slot.ack_due_ns + static_cast<Timestamp>(settings_.fill_interval_ms) * 1'000'000LL;
    slot.reject_pending = order.volume <= 0
                       || (order.order_type != OrderType::Market && order.price <= 0.0);
    return true;
}

bool SimGateway::send_quote(const Quote& quote) noexcept {
    if (!connected_.load(std::memory_order_relaxed)) return false;
    if (quote.instrument_id >= MAX_INSTRUMENTS) return false;

    quotes_sent_.fetch_add(1, std::memory_order_relaxed);
    const Timestamp now_ns = get_monotonic_ns();

    std::lock_guard<std::mutex> lock(state_mutex_);
    ActiveQuote& slot = active_quotes_[quote.instrument_id];

    if (quote.bid_volume == 0 && quote.ask_volume == 0) {
        GatewayEvent cancel{};
        cancel.type = GatewayEventType::QuoteCancel;
        cancel.product_index = quote.product_index;
        cancel.quote = slot.used ? slot.quote : quote;
        cancel.quote.client_quote_id = quote.client_quote_id;
        cancel.quote.bid_volume = 0;
        cancel.quote.ask_volume = 0;
        cancel.quote.ack_ts = now_ns;
        (void)callback_buf.try_push(cancel);
        slot = ActiveQuote{};
        return true;
    }

    if ((quote.bid_volume > 0 && quote.bid_price <= 0.0)
        || (quote.ask_volume > 0 && quote.ask_price <= 0.0)
        || (quote.bid_volume > 0 && quote.ask_volume > 0 && quote.ask_price <= quote.bid_price)) {
        GatewayEvent reject{};
        reject.type = GatewayEventType::QuoteReject;
        reject.product_index = quote.product_index;
        reject.quote = quote;
        reject.quote.ack_ts = now_ns;
        (void)callback_buf.try_push(reject);
        return false;
    }

    const uint64_t exch_id = exchange_order_id_seq_.fetch_add(1, std::memory_order_relaxed);
    slot = ActiveQuote{};
    slot.used = true;
    slot.quote = quote;
    slot.quote.exchange_quote_id = exch_id;
    slot.quote.bid_status = OrderStatus::New;
    slot.quote.ask_status = OrderStatus::New;
    slot.remaining_bid = std::max<Volume>(0, quote.bid_volume);
    slot.remaining_ask = std::max<Volume>(0, quote.ask_volume);
    slot.exchange_quote_id = exch_id;
    slot.ack_due_ns = now_ns + static_cast<Timestamp>(settings_.ack_latency_ms) * 1'000'000LL;
    slot.next_fill_due_ns = slot.ack_due_ns + static_cast<Timestamp>(settings_.fill_interval_ms) * 1'000'000LL;
    return true;
}

bool SimGateway::cancel_order(OrderId id, uint16_t instrument_id) noexcept {
    if (!connected_.load(std::memory_order_relaxed)) return false;

    const Timestamp now_ns = get_monotonic_ns();
    std::lock_guard<std::mutex> lock(state_mutex_);
    for (auto& order : active_orders_) {
        if (!order.used) continue;
        if (order.order.client_order_id != id) continue;
        if (order.order.instrument_id != instrument_id) continue;
        if (order.remaining_volume <= 0) return false;
        order.cancel_pending = true;
        order.cancel_due_ns = now_ns + static_cast<Timestamp>(settings_.cancel_latency_ms) * 1'000'000LL;
        return true;
    }
    return false;
}

bool SimGateway::cancel_quote(QuoteId id, uint16_t instrument_id) noexcept {
    if (!connected_.load(std::memory_order_relaxed)) return false;
    if (instrument_id >= MAX_INSTRUMENTS) return false;

    const Timestamp now_ns = get_monotonic_ns();
    std::lock_guard<std::mutex> lock(state_mutex_);
    ActiveQuote& quote = active_quotes_[instrument_id];
    if (!quote.used) return false;
    if (quote.quote.client_quote_id != id) return false;
    if (quote.cancel_pending) return true;
    if (quote.remaining_bid <= 0 && quote.remaining_ask <= 0) return false;
    quote.cancel_pending = true;
    quote.cancel_due_ns = now_ns + static_cast<Timestamp>(settings_.cancel_latency_ms) * 1'000'000LL;
    return true;
}

bool SimGateway::get_order_recovery_handle(
        OrderId id,
        GatewayOrderRecoveryHandle* out) const noexcept {
    if (out == nullptr) return false;
    *out = GatewayOrderRecoveryHandle{};

    std::lock_guard<std::mutex> lock(state_mutex_);
    for (const auto& order : active_orders_) {
        if (!order.used || order.order.client_order_id != id) continue;
        out->valid = true;
        std::snprintf(out->order_sys_id,
                      sizeof(out->order_sys_id),
                      "%llu",
                      static_cast<unsigned long long>(order.exchange_order_id));
        return true;
    }
    return false;
}

bool SimGateway::get_quote_recovery_handle(
        QuoteId id,
        GatewayQuoteRecoveryHandle* out) const noexcept {
    if (out == nullptr) return false;
    *out = GatewayQuoteRecoveryHandle{};

    std::lock_guard<std::mutex> lock(state_mutex_);
    for (const auto& quote : active_quotes_) {
        if (!quote.used || quote.quote.client_quote_id != id) continue;
        out->valid = true;
        std::snprintf(out->quote_sys_id,
                      sizeof(out->quote_sys_id),
                      "%llu",
                      static_cast<unsigned long long>(quote.exchange_quote_id));
        return true;
    }
    return false;
}

void SimGateway::restore_order_recovery(const GatewayRecoveredOrder& recovered) noexcept {
    std::lock_guard<std::mutex> lock(state_mutex_);
    auto slot_it = std::find_if(active_orders_.begin(), active_orders_.end(), [](const ActiveOrder& item) {
        return !item.used;
    });
    if (slot_it == active_orders_.end()) return;

    ActiveOrder& slot = *slot_it;
    slot = ActiveOrder{};
    slot.used = true;
    slot.ack_sent = true;
    slot.order = recovered.order;
    slot.remaining_volume = std::max<Volume>(0, recovered.order.volume - recovered.order.filled_volume);
    slot.fill_notional =
        recovered.order.avg_fill_price * static_cast<double>(recovered.order.filled_volume);
    slot.exchange_order_id = recovered.order.exchange_order_id;
}

void SimGateway::restore_quote_recovery(const GatewayRecoveredQuote& recovered) noexcept {
    if (recovered.quote.instrument_id >= MAX_INSTRUMENTS) return;

    std::lock_guard<std::mutex> lock(state_mutex_);
    ActiveQuote& slot = active_quotes_[recovered.quote.instrument_id];
    slot = ActiveQuote{};
    slot.used = true;
    slot.ack_sent = true;
    slot.quote = recovered.quote;
    slot.remaining_bid = std::max<Volume>(0, recovered.quote.bid_volume);
    slot.remaining_ask = std::max<Volume>(0, recovered.quote.ask_volume);
    slot.exchange_quote_id = recovered.quote.exchange_quote_id;
}

void SimGateway::start_worker() {
    stop_worker();
    worker_running_.store(true, std::memory_order_release);
    worker_thread_ = std::thread([this] { worker_loop(); });
}

void SimGateway::stop_worker() {
    worker_running_.store(false, std::memory_order_release);
    if (worker_thread_.joinable()) worker_thread_.join();
}

void SimGateway::worker_loop() noexcept {
    set_thread_name("omm-sim-gw");

    std::mt19937 rng(settings_.random_seed);
    const bool low_latency_poll =
        settings_.ack_latency_ms == 0 || settings_.cancel_latency_ms == 0;

    while (worker_running_.load(std::memory_order_acquire)) {
        const Timestamp now_ns = get_monotonic_ns();
        process_orders(now_ns, rng);
        process_quotes(now_ns, rng);
        if (low_latency_poll) {
            std::this_thread::sleep_for(std::chrono::microseconds(kFastWorkerSleepUs));
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(kDefaultWorkerSleepMs));
        }
    }
}

void SimGateway::process_orders(Timestamp now_ns, std::mt19937& rng) noexcept {
    std::lock_guard<std::mutex> lock(state_mutex_);
    for (auto& active : active_orders_) {
        if (!active.used) continue;

        if (!active.ack_sent && now_ns >= active.ack_due_ns) {
            if (active.order.volume <= 0 || (active.order.order_type != OrderType::Market && active.order.price <= 0.0)) {
                active.reject_pending = true;
            } else if (sample_probability(settings_.reject_probability, rng)) {
                active.reject_pending = true;
            }

            if (active.reject_pending) {
                GatewayEvent reject{};
                reject.type = GatewayEventType::OrderReject;
                reject.product_index = active.order.product_index;
                reject.order = active.order;
                reject.order.exchange_order_id = active.exchange_order_id;
                reject.order.status = OrderStatus::Rejected;
                reject.order.ack_ts = now_ns;
                (void)callback_buf.try_push(reject);
                active = ActiveOrder{};
                continue;
            }

            GatewayEvent ack{};
            ack.type = GatewayEventType::OrderAck;
            ack.product_index = active.order.product_index;
            ack.order = active.order;
            ack.order.exchange_order_id = active.exchange_order_id;
            ack.order.status = OrderStatus::New;
            ack.order.ack_ts = now_ns;
            (void)callback_buf.try_push(ack);
            active.ack_sent = true;
        }

        if (!active.ack_sent) continue;

        if (active.cancel_pending && now_ns >= active.cancel_due_ns) {
            GatewayEvent cancel{};
            cancel.type = GatewayEventType::OrderCancel;
            cancel.product_index = active.order.product_index;
            cancel.order = active.order;
            cancel.order.exchange_order_id = active.exchange_order_id;
            cancel.order.status = OrderStatus::Cancelled;
            cancel.order.filled_volume = active.order.volume - active.remaining_volume;
            cancel.order.avg_fill_price = cancel.order.filled_volume > 0
                ? active.fill_notional / static_cast<double>(cancel.order.filled_volume)
                : 0.0;
            cancel.order.ack_ts = now_ns;
            (void)callback_buf.try_push(cancel);
            active = ActiveOrder{};
            continue;
        }

        if (active.remaining_volume <= 0 || now_ns < active.next_fill_due_ns) continue;

        const double mkt = market_price(active.order.instrument_id);
        const bool marketable = (active.order.order_type == OrderType::Market)
            || (active.order.side == Side::Buy && mkt > 0.0 && active.order.price >= mkt)
            || (active.order.side == Side::Sell && mkt > 0.0 && active.order.price <= mkt);
        if (!marketable || !sample_probability(settings_.order_fill_probability, rng)) {
            active.next_fill_due_ns = now_ns + static_cast<Timestamp>(settings_.fill_interval_ms) * 1'000'000LL;
            continue;
        }

        const Volume fill_volume = sample_fill_volume(active.remaining_volume, rng);
        const double fill_price = fill_price_for_order(active);
        orders_filled_.fetch_add(1, std::memory_order_relaxed);

        GatewayEvent fill{};
        fill.type = GatewayEventType::OrderFill;
        fill.product_index = active.order.product_index;
        fill.trade.client_order_id = active.order.client_order_id;
        fill.trade.instrument_id = active.order.instrument_id;
        fill.trade.product_index = active.order.product_index;
        fill.trade.side = active.order.side;
        fill.trade.offset = active.order.offset;
        fill.trade.fill_price = fill_price;
        fill.trade.fill_volume = fill_volume;
        fill.trade.fill_ts = now_ns;
        fill.trade.trade_id = active.exchange_order_id;
        fill.trade.account_id = active.order.account_id;
        fill.trade.exchange_id = active.order.exchange_id;
        (void)callback_buf.try_push(fill);

        active.remaining_volume -= fill_volume;
        active.fill_notional += fill_price * static_cast<double>(fill_volume);
        active.next_fill_due_ns = now_ns + static_cast<Timestamp>(settings_.fill_interval_ms) * 1'000'000LL;
        if (active.remaining_volume <= 0) active = ActiveOrder{};
    }
}

void SimGateway::process_quotes(Timestamp now_ns, std::mt19937& rng) noexcept {
    std::lock_guard<std::mutex> lock(state_mutex_);
    for (auto& active : active_quotes_) {
        if (!active.used) continue;

        if (!active.ack_sent && now_ns >= active.ack_due_ns) {
            GatewayEvent ack{};
            ack.type = GatewayEventType::QuoteAck;
            ack.product_index = active.quote.product_index;
            ack.quote = active.quote;
            ack.quote.exchange_quote_id = active.exchange_quote_id;
            ack.quote.ack_ts = now_ns;
            ack.quote.bid_status = active.remaining_bid > 0 ? OrderStatus::New : OrderStatus::Filled;
            ack.quote.ask_status = active.remaining_ask > 0 ? OrderStatus::New : OrderStatus::Filled;
            (void)callback_buf.try_push(ack);
            active.ack_sent = true;
        }

        if (active.cancel_pending && now_ns >= active.cancel_due_ns) {
            GatewayEvent cancel{};
            cancel.type = GatewayEventType::QuoteCancel;
            cancel.product_index = active.quote.product_index;
            cancel.quote = active.quote;
            cancel.quote.ack_ts = now_ns;
            cancel.quote.bid_volume = 0;
            cancel.quote.ask_volume = 0;
            cancel.quote.bid_status = OrderStatus::Cancelled;
            cancel.quote.ask_status = OrderStatus::Cancelled;
            (void)callback_buf.try_push(cancel);
            active = ActiveQuote{};
            continue;
        }

        if (!active.ack_sent || now_ns < active.next_fill_due_ns) continue;

        const double mkt = market_price(active.quote.instrument_id);
        const double tick = tick_size(active.quote.instrument_id);
        const double passive_ticks = settings_.quote_near_touch_ticks * tick;
        bool filled_any = false;

        if (active.remaining_bid > 0) {
            const bool crossed = mkt > 0.0 && mkt <= active.quote.bid_price;
            const bool near_touch = mkt > 0.0 && mkt <= active.quote.bid_price + passive_ticks;
            const double probability = crossed
                ? settings_.quote_cross_fill_probability
                : (near_touch ? settings_.quote_passive_fill_probability : 0.0);
            if (probability > 0.0 && sample_probability(probability, rng)) {
                const Volume fill_volume = sample_fill_volume(active.remaining_bid, rng);
                GatewayEvent fill{};
                fill.type = GatewayEventType::QuoteFill;
                fill.product_index = active.quote.product_index;
                fill.trade.client_order_id = active.quote.client_quote_id;
                fill.trade.instrument_id = active.quote.instrument_id;
                fill.trade.product_index = active.quote.product_index;
                fill.trade.side = Side::Buy;
                fill.trade.offset = active.quote.bid_offset;
                fill.trade.fill_price = active.quote.bid_price;
                fill.trade.fill_volume = fill_volume;
                fill.trade.fill_ts = now_ns;
                fill.trade.trade_id = active.exchange_quote_id;
                fill.trade.account_id = active.quote.account_id;
                fill.trade.exchange_id = active.quote.exchange_id;
                (void)callback_buf.try_push(fill);
                active.remaining_bid -= fill_volume;
                filled_any = true;
            }
        }

        if (active.remaining_ask > 0) {
            const bool crossed = mkt > 0.0 && mkt >= active.quote.ask_price;
            const bool near_touch = mkt > 0.0 && mkt >= active.quote.ask_price - passive_ticks;
            const double probability = crossed
                ? settings_.quote_cross_fill_probability
                : (near_touch ? settings_.quote_passive_fill_probability : 0.0);
            if (probability > 0.0 && sample_probability(probability, rng)) {
                const Volume fill_volume = sample_fill_volume(active.remaining_ask, rng);
                GatewayEvent fill{};
                fill.type = GatewayEventType::QuoteFill;
                fill.product_index = active.quote.product_index;
                fill.trade.client_order_id = active.quote.client_quote_id;
                fill.trade.instrument_id = active.quote.instrument_id;
                fill.trade.product_index = active.quote.product_index;
                fill.trade.side = Side::Sell;
                fill.trade.offset = active.quote.ask_offset;
                fill.trade.fill_price = active.quote.ask_price;
                fill.trade.fill_volume = fill_volume;
                fill.trade.fill_ts = now_ns;
                fill.trade.trade_id = active.exchange_quote_id;
                fill.trade.account_id = active.quote.account_id;
                fill.trade.exchange_id = active.quote.exchange_id;
                (void)callback_buf.try_push(fill);
                active.remaining_ask -= fill_volume;
                filled_any = true;
            }
        }

        active.next_fill_due_ns = now_ns + static_cast<Timestamp>(settings_.fill_interval_ms) * 1'000'000LL;
        if (active.remaining_bid <= 0 && active.remaining_ask <= 0) {
            active = ActiveQuote{};
        } else if (!filled_any) {
            continue;
        }
    }
}

bool SimGateway::sample_probability(double p, std::mt19937& rng) noexcept {
    if (p <= 0.0) return false;
    if (p >= 1.0) return true;
    std::uniform_real_distribution<double> dist(0.0, 1.0);
    return dist(rng) < p;
}

Volume SimGateway::sample_fill_volume(Volume remaining, std::mt19937& rng) noexcept {
    if (remaining <= 1) return remaining;

    const Volume cap = settings_.max_fill_size > 0
        ? std::min(settings_.max_fill_size, remaining)
        : remaining;
    if (cap <= 1) return cap;
    if (!sample_probability(settings_.partial_fill_probability, rng)) {
        return cap == remaining ? remaining : cap;
    }

    std::uniform_int_distribution<Volume> dist(1, cap);
    return dist(rng);
}

double SimGateway::market_price(uint16_t instrument_id) const noexcept {
    if (instrument_id >= MAX_INSTRUMENTS) return 0.0;
    return last_price_[instrument_id].load(std::memory_order_relaxed);
}

double SimGateway::tick_size(uint16_t instrument_id) const noexcept {
    if (const Instrument* instr = instrument_by_id(instrument_id); instr != nullptr && instr->tick_size > 0.0) {
        return instr->tick_size;
    }
    return 0.01;
}

double SimGateway::fill_price_for_order(const ActiveOrder& active) const noexcept {
    const double mkt = market_price(active.order.instrument_id);
    const double tick = tick_size(active.order.instrument_id);
    const double slip = static_cast<double>(settings_.slippage_ticks) * tick;
    if (active.order.order_type == OrderType::Market) {
        if (mkt <= 0.0) return active.order.price;
        return active.order.side == Side::Buy ? mkt + slip : std::max(0.0, mkt - slip);
    }
    if (mkt <= 0.0) return active.order.price;
    if (active.order.side == Side::Buy) return std::min(active.order.price, mkt + slip);
    return std::max(active.order.price, mkt - slip);
}

} // namespace omm
