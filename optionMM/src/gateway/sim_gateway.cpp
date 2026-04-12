#include "gateway/sim_gateway.h"
#include "common/thread_utils.h"
#include <cstring>

namespace omm {

bool SimGateway::send_order(const Order& order) noexcept {
    if (!connected_.load(std::memory_order_relaxed)) return false;

    orders_sent_.fetch_add(1, std::memory_order_relaxed);
    uint64_t exch_id = exchange_order_id_seq_.fetch_add(1, std::memory_order_relaxed);

    // 1. Push OrderAck
    GatewayEvent ack{};
    ack.type          = GatewayEventType::OrderAck;
    ack.product_index = order.product_index;
    ack.order         = order;
    ack.order.exchange_order_id = exch_id;
    ack.order.status  = OrderStatus::New;
    ack.order.ack_ts  = get_monotonic_ns();
    (void)callback_buf.try_push(ack);

    // 2. Simulate immediate fill if price is marketable
    double mkt = last_price_[order.instrument_id];
    bool fillable = (order.order_type == OrderType::Market)
                 || (order.side == Side::Buy  && order.price >= mkt && mkt > 0)
                 || (order.side == Side::Sell && order.price <= mkt && mkt > 0);

    if (fillable && order.volume > 0) {
        orders_filled_.fetch_add(1, std::memory_order_relaxed);

        GatewayEvent fill{};
        fill.type          = GatewayEventType::OrderFill;
        fill.product_index = order.product_index;
        fill.trade.client_order_id = order.client_order_id;
        fill.trade.instrument_id   = order.instrument_id;
        fill.trade.product_index   = order.product_index;
        fill.trade.side            = order.side;
        fill.trade.fill_price      = (order.order_type == OrderType::Market)
                                     ? mkt : order.price;
        fill.trade.fill_volume     = order.volume;
        fill.trade.fill_ts         = get_monotonic_ns();
        fill.trade.trade_id        = exch_id;
        // Copy account/exchange IDs
        fill.trade.account_id      = order.account_id;
        fill.trade.exchange_id     = order.exchange_id;
        (void)callback_buf.try_push(fill);
    }
    return true;
}

bool SimGateway::send_quote(const Quote& quote) noexcept {
    if (!connected_.load(std::memory_order_relaxed)) return false;

    quotes_sent_.fetch_add(1, std::memory_order_relaxed);
    uint64_t exch_id = exchange_order_id_seq_.fetch_add(1, std::memory_order_relaxed);

    // Push QuoteAck
    GatewayEvent ack{};
    ack.type          = GatewayEventType::QuoteAck;
    ack.product_index = quote.product_index;
    ack.quote         = quote;
    ack.quote.exchange_quote_id = exch_id;
    ack.quote.bid_status = OrderStatus::New;
    ack.quote.ask_status = OrderStatus::New;
    ack.quote.ack_ts     = get_monotonic_ns();
    (void)callback_buf.try_push(ack);

    // Simulate fills: bid fills if last_price <= bid_price (someone sells to us)
    //                 ask fills if last_price >= ask_price (someone buys from us)
    double mkt = last_price_[quote.instrument_id];
    if (mkt > 0) {
        if (quote.bid_volume > 0 && mkt <= quote.bid_price) {
            GatewayEvent fill{};
            fill.type          = GatewayEventType::QuoteFill;
            fill.product_index = quote.product_index;
            fill.trade.client_order_id = quote.client_quote_id;
            fill.trade.instrument_id   = quote.instrument_id;
            fill.trade.product_index   = quote.product_index;
            fill.trade.side            = Side::Buy;
            fill.trade.fill_price      = quote.bid_price;
            fill.trade.fill_volume     = quote.bid_volume;
            fill.trade.fill_ts         = get_monotonic_ns();
            (void)callback_buf.try_push(fill);
        }
        if (quote.ask_volume > 0 && mkt >= quote.ask_price) {
            GatewayEvent fill{};
            fill.type          = GatewayEventType::QuoteFill;
            fill.product_index = quote.product_index;
            fill.trade.client_order_id = quote.client_quote_id;
            fill.trade.instrument_id   = quote.instrument_id;
            fill.trade.product_index   = quote.product_index;
            fill.trade.side            = Side::Sell;
            fill.trade.fill_price      = quote.ask_price;
            fill.trade.fill_volume     = quote.ask_volume;
            fill.trade.fill_ts         = get_monotonic_ns();
            (void)callback_buf.try_push(fill);
        }
    }
    return true;
}

bool SimGateway::cancel_order(OrderId id,
                               uint16_t instrument_id) noexcept {
    if (!connected_.load(std::memory_order_relaxed)) return false;

    GatewayEvent ev{};
    ev.type = GatewayEventType::OrderCancel;
    ev.order.client_order_id = id;
    ev.order.instrument_id   = instrument_id;
    ev.order.status          = OrderStatus::Cancelled;
    ev.order.ack_ts          = get_monotonic_ns();
    (void)callback_buf.try_push(ev);
    return true;
}

} // namespace omm
