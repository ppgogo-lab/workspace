#include "gateway/femas_gateway.h"
#include "logger/logger.h"

#include <chrono>
#include <cstdio>

namespace omm {

namespace {

constexpr char kMarketMakerHedge = USTP_FTDC_CHF_MarketMaker;
constexpr char kSpecHedge = USTP_FTDC_CHF_Speculation;

} // namespace

OffsetFlag FEMASGateway::decode_offset(char femas_offset) noexcept {
    switch (femas_offset) {
    case USTP_FTDC_OF_Close:
        return OffsetFlag::Close;
    case USTP_FTDC_OF_CloseToday:
        return OffsetFlag::CloseToday;
    case USTP_FTDC_OF_CloseYesterday:
        return OffsetFlag::CloseYesterday;
    default:
        return OffsetFlag::Open;
    }
}

char FEMASGateway::encode_offset(OffsetFlag offset) noexcept {
    switch (offset) {
    case OffsetFlag::Close:
        return USTP_FTDC_OF_Close;
    case OffsetFlag::CloseToday:
        return USTP_FTDC_OF_CloseToday;
    case OffsetFlag::CloseYesterday:
        return USTP_FTDC_OF_CloseYesterday;
    default:
        return USTP_FTDC_OF_Open;
    }
}

OrderStatus FEMASGateway::decode_order_status(char femas_status) noexcept {
    switch (femas_status) {
    case USTP_FTDC_OS_AllTraded:
        return OrderStatus::Filled;
    case USTP_FTDC_OS_Canceled:
        return OrderStatus::Cancelled;
    case USTP_FTDC_OS_PartTradedQueueing:
    case USTP_FTDC_OS_PartTradedNotQueueing:
        return OrderStatus::PartialFilled;
    case USTP_FTDC_OS_NoTradeQueueing:
    case USTP_FTDC_OS_NoTradeNotQueueing:
    case USTP_FTDC_OS_AcceptedNoReply:
    default:
        return OrderStatus::New;
    }
}

uint64_t FEMASGateway::parse_numeric_id(const char* text) noexcept {
    if (!text || !text[0]) return 0;
    return static_cast<uint64_t>(std::strtoull(text, nullptr, 10));
}

FEMASGateway::OrderState* FEMASGateway::alloc_order_state() noexcept {
    for (auto& state : order_states_) {
        if (!state.used) {
            state = OrderState{};
            state.used = true;
            return &state;
        }
    }
    return nullptr;
}

FEMASGateway::QuoteState* FEMASGateway::alloc_quote_state() noexcept {
    for (auto& state : quote_states_) {
        if (!state.used) {
            state = QuoteState{};
            state.used = true;
            return &state;
        }
    }
    return nullptr;
}

FEMASGateway::OrderState* FEMASGateway::find_order_by_local_id(const char* local_id) noexcept {
    if (!local_id || !local_id[0]) return nullptr;
    for (auto& state : order_states_) {
        if (state.used && std::strncmp(state.exchange_local_id, local_id, sizeof(state.exchange_local_id)) == 0) {
            return &state;
        }
    }
    return nullptr;
}

FEMASGateway::OrderState* FEMASGateway::find_order_by_sys_id(const char* order_sys_id) noexcept {
    if (!order_sys_id || !order_sys_id[0]) return nullptr;
    for (auto& state : order_states_) {
        if (state.used && state.order_sys_id[0] &&
            std::strncmp(state.order_sys_id, order_sys_id, sizeof(state.order_sys_id)) == 0) {
            return &state;
        }
    }
    return nullptr;
}

FEMASGateway::QuoteState* FEMASGateway::find_quote_by_client_id(QuoteId quote_id) noexcept {
    for (auto& state : quote_states_) {
        if (state.used && state.quote.client_quote_id == quote_id) return &state;
    }
    return nullptr;
}

FEMASGateway::QuoteState* FEMASGateway::find_quote_by_local_id(const char* local_id) noexcept {
    if (!local_id || !local_id[0]) return nullptr;
    for (auto& state : quote_states_) {
        if (!state.used) continue;
        if (std::strncmp(state.quote_local_id, local_id, sizeof(state.quote_local_id)) == 0) return &state;
    }
    return nullptr;
}

FEMASGateway::QuoteState* FEMASGateway::find_quote_by_sys_id(const char* quote_sys_id) noexcept {
    if (!quote_sys_id || !quote_sys_id[0]) return nullptr;
    for (auto& state : quote_states_) {
        if (!state.used || !state.quote_sys_id[0]) continue;
        if (std::strncmp(state.quote_sys_id, quote_sys_id, sizeof(state.quote_sys_id)) == 0) return &state;
    }
    return nullptr;
}

void FEMASGateway::clear_order_state(const char* local_id) noexcept {
    if (OrderState* state = find_order_by_local_id(local_id)) {
        *state = OrderState{};
    }
}

void FEMASGateway::clear_quote_state(QuoteId quote_id) noexcept {
    QuoteState* quote = find_quote_by_client_id(quote_id);
    if (!quote) return;
    clear_order_state(quote->bid_local_id);
    clear_order_state(quote->ask_local_id);
    *quote = QuoteState{};
}

void FEMASGateway::push_order_event(GatewayEventType type,
                                    const OrderState& state,
                                    OrderStatus status,
                                    Volume filled_volume) noexcept {
    GatewayEvent ev{};
    ev.type = type;
    ev.product_index = state.product_index;
    ev.order.client_order_id = state.is_quote_leg ? state.client_quote_id : state.client_order_id;
    ev.order.exchange_order_id = parse_numeric_id(state.order_sys_id);
    ev.order.instrument_id = state.instrument_id;
    ev.order.product_index = state.product_index;
    ev.order.side = state.side;
    ev.order.offset = state.offset;
    ev.order.status = status;
    ev.order.price = state.price;
    ev.order.volume = state.volume;
    ev.order.filled_volume = filled_volume;
    ev.order.ack_ts = get_monotonic_ns();
    (void)callback_buf.try_push(ev);
}

void FEMASGateway::push_trade_event(GatewayEventType type,
                                    const OrderState& state,
                                    const CUstpFtdcTradeField& trade) noexcept {
    GatewayEvent ev{};
    ev.type = type;
    ev.product_index = state.product_index;
    ev.trade.client_order_id = state.is_quote_leg ? state.client_quote_id : state.client_order_id;
    ev.trade.trade_id = parse_numeric_id(trade.TradeID);
    ev.trade.instrument_id = state.instrument_id;
    ev.trade.product_index = state.product_index;
    ev.trade.side = (trade.Direction == USTP_FTDC_D_Buy) ? Side::Buy : Side::Sell;
    ev.trade.offset = decode_offset(trade.OffsetFlag);
    ev.trade.fill_price = trade.TradePrice;
    ev.trade.fill_volume = trade.TradeVolume;
    ev.trade.fill_ts = get_monotonic_ns();
    std::strncpy(ev.trade.exchange_id.data, trade.ExchangeID, sizeof(ev.trade.exchange_id.data) - 1);
    (void)callback_buf.try_push(ev);
}

bool FEMASGateway::connect(const GatewayConfig& cfg) {
    cfg_ = cfg;

    api_ = CUstpFtdcTraderApi::CreateFtdcTraderApi("./femas_flow/");
    if (!api_) {
        OMM_LOG_ERROR("femas", "CreateFtdcTraderApi failed");
        return false;
    }

    api_->RegisterSpi(this);
    api_->SubscribePrivateTopic(USTP_TERT_QUICK);
    api_->SubscribePublicTopic(USTP_TERT_QUICK);
    api_->RegisterFront(const_cast<char*>(cfg_.femas.front_addr));
    api_->Init();

    OMM_LOG_INFO("femas", "connecting to front={}", cfg_.femas.front_addr);
    return true;
}

void FEMASGateway::disconnect() {
    trading_ready_.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lk(state_mutex_);
        for (auto& state : order_states_) state = OrderState{};
        for (auto& state : quote_states_) state = QuoteState{};
    }
    if (api_) {
        api_->RegisterSpi(nullptr);
        api_->Release();
        api_ = nullptr;
        OMM_LOG_INFO("femas", "disconnected");
    }
}

bool FEMASGateway::send_order(const Order& order) noexcept {
    if (!api_ || !trading_ready_.load(std::memory_order_relaxed)) return false;

    const Instrument* instr = instrument_by_id(order.instrument_id);
    if (!instr) {
        OMM_LOG_WARN("femas", "send_order missing instrument id={}", order.instrument_id);
        return false;
    }

    CUstpFtdcInputOrderField req{};
    std::strncpy(req.BrokerID, cfg_.femas.broker_id, sizeof(req.BrokerID) - 1);
    std::strncpy(req.ExchangeID, exchange_id(), sizeof(req.ExchangeID) - 1);
    std::strncpy(req.InvestorID, cfg_.femas.user_id, sizeof(req.InvestorID) - 1);
    std::strncpy(req.UserID, cfg_.femas.user_id, sizeof(req.UserID) - 1);
    std::strncpy(req.InstrumentID, instr->code.data, sizeof(req.InstrumentID) - 1);

    char local_id[16]{};
    encode_local_id(local_id, next_local_id());
    std::strncpy(req.UserOrderLocalID, local_id, sizeof(req.UserOrderLocalID) - 1);

    req.OrderPriceType = (order.order_type == OrderType::Market)
        ? USTP_FTDC_OPT_AnyPrice
        : USTP_FTDC_OPT_LimitPrice;
    req.Direction = (order.side == Side::Buy) ? USTP_FTDC_D_Buy : USTP_FTDC_D_Sell;
    req.OffsetFlag = encode_offset(order.offset);
    req.HedgeFlag = kSpecHedge;
    req.LimitPrice = order.price;
    req.Volume = order.volume;
    req.TimeCondition = USTP_FTDC_TC_GFD;
    req.VolumeCondition = USTP_FTDC_VC_AV;
    req.MinVolume = 1;
    req.ForceCloseReason = USTP_FTDC_FCR_NotForceClose;

    {
        std::lock_guard<std::mutex> lk(state_mutex_);
        OrderState* state = alloc_order_state();
        if (!state) {
            OMM_LOG_WARN("femas", "order state table full");
            return false;
        }
        state->client_order_id = order.client_order_id;
        state->instrument_id = order.instrument_id;
        state->product_index = order.product_index;
        state->side = order.side;
        state->offset = order.offset;
        state->price = order.price;
        state->volume = order.volume;
        std::strncpy(state->exchange_local_id, req.UserOrderLocalID, sizeof(state->exchange_local_id) - 1);
    }

    const int ret = api_->ReqOrderInsert(&req, next_req_id());
    if (ret != 0) {
        std::lock_guard<std::mutex> lk(state_mutex_);
        clear_order_state(local_id);
        OMM_LOG_WARN("femas", "ReqOrderInsert failed ret={} order_id={}", ret, order.client_order_id);
        return false;
    }
    return true;
}

bool FEMASGateway::send_quote(const Quote& quote) noexcept {
    if (!api_ || !trading_ready_.load(std::memory_order_relaxed)) return false;
    if (quote.bid_volume == 0 && quote.ask_volume == 0) {
        return cancel_order(quote.client_quote_id, quote.instrument_id);
    }

    const Instrument* instr = instrument_by_id(quote.instrument_id);
    if (!instr) {
        OMM_LOG_WARN("femas", "send_quote missing instrument id={}", quote.instrument_id);
        return false;
    }

    CUstpFtdcInputQuoteField req{};
    std::strncpy(req.BrokerID, cfg_.femas.broker_id, sizeof(req.BrokerID) - 1);
    std::strncpy(req.ExchangeID, exchange_id(), sizeof(req.ExchangeID) - 1);
    std::strncpy(req.InvestorID, cfg_.femas.user_id, sizeof(req.InvestorID) - 1);
    std::strncpy(req.UserID, cfg_.femas.user_id, sizeof(req.UserID) - 1);
    std::strncpy(req.QuoteUserID, cfg_.femas.user_id, sizeof(req.QuoteUserID) - 1);
    std::strncpy(req.InstrumentID, instr->code.data, sizeof(req.InstrumentID) - 1);

    char quote_local_id[16]{};
    char bid_local_id[16]{};
    char ask_local_id[16]{};
    encode_local_id(quote_local_id, next_local_id());
    encode_local_id(bid_local_id, next_local_id());
    encode_local_id(ask_local_id, next_local_id());
    std::strncpy(req.UserQuoteLocalID, quote_local_id, sizeof(req.UserQuoteLocalID) - 1);
    std::strncpy(req.BidUserOrderLocalID, bid_local_id, sizeof(req.BidUserOrderLocalID) - 1);
    std::strncpy(req.AskUserOrderLocalID, ask_local_id, sizeof(req.AskUserOrderLocalID) - 1);

    req.BidOffsetFlag = encode_offset(quote.bid_offset);
    req.AskOffsetFlag = encode_offset(quote.ask_offset);
    req.BidHedgeFlag = kMarketMakerHedge;
    req.AskHedgeFlag = kMarketMakerHedge;
    req.BidPrice = quote.bid_price;
    req.AskPrice = quote.ask_price;
    req.BidVolume = quote.bid_volume;
    req.AskVolume = quote.ask_volume;

    {
        std::lock_guard<std::mutex> lk(state_mutex_);
        QuoteState* quote_state = alloc_quote_state();
        OrderState* bid_state = alloc_order_state();
        OrderState* ask_state = alloc_order_state();
        if (!quote_state || !bid_state || !ask_state) {
            if (quote_state) *quote_state = QuoteState{};
            if (bid_state) *bid_state = OrderState{};
            if (ask_state) *ask_state = OrderState{};
            OMM_LOG_WARN("femas", "quote state table full");
            return false;
        }

        quote_state->quote = quote;
        std::strncpy(quote_state->quote_local_id, req.UserQuoteLocalID, sizeof(quote_state->quote_local_id) - 1);
        std::strncpy(quote_state->bid_local_id, req.BidUserOrderLocalID, sizeof(quote_state->bid_local_id) - 1);
        std::strncpy(quote_state->ask_local_id, req.AskUserOrderLocalID, sizeof(quote_state->ask_local_id) - 1);

        bid_state->is_quote_leg = true;
        bid_state->client_quote_id = quote.client_quote_id;
        bid_state->instrument_id = quote.instrument_id;
        bid_state->product_index = quote.product_index;
        bid_state->side = Side::Buy;
        bid_state->offset = quote.bid_offset;
        bid_state->price = quote.bid_price;
        bid_state->volume = quote.bid_volume;
        std::strncpy(bid_state->exchange_local_id, req.BidUserOrderLocalID, sizeof(bid_state->exchange_local_id) - 1);

        ask_state->is_quote_leg = true;
        ask_state->client_quote_id = quote.client_quote_id;
        ask_state->instrument_id = quote.instrument_id;
        ask_state->product_index = quote.product_index;
        ask_state->side = Side::Sell;
        ask_state->offset = quote.ask_offset;
        ask_state->price = quote.ask_price;
        ask_state->volume = quote.ask_volume;
        std::strncpy(ask_state->exchange_local_id, req.AskUserOrderLocalID, sizeof(ask_state->exchange_local_id) - 1);
    }

    const int ret = api_->ReqQuoteInsert(&req, next_req_id());
    if (ret != 0) {
        std::lock_guard<std::mutex> lk(state_mutex_);
        clear_quote_state(quote.client_quote_id);
        OMM_LOG_WARN("femas", "ReqQuoteInsert failed ret={} quote_id={}", ret, quote.client_quote_id);
        return false;
    }
    return true;
}

bool FEMASGateway::cancel_order(OrderId id, uint16_t instrument_id) noexcept {
    if (!api_ || !trading_ready_.load(std::memory_order_relaxed)) return false;

    {
        std::lock_guard<std::mutex> lk(state_mutex_);
        if (QuoteState* quote = find_quote_by_client_id(id)) {
            CUstpFtdcQuoteActionField req{};
            std::strncpy(req.BrokerID, cfg_.femas.broker_id, sizeof(req.BrokerID) - 1);
            std::strncpy(req.ExchangeID, exchange_id(), sizeof(req.ExchangeID) - 1);
            std::strncpy(req.InvestorID, cfg_.femas.user_id, sizeof(req.InvestorID) - 1);
            std::strncpy(req.UserID, cfg_.femas.user_id, sizeof(req.UserID) - 1);
            std::strncpy(req.UserQuoteLocalID, quote->quote_local_id, sizeof(req.UserQuoteLocalID) - 1);
            if (quote->quote_sys_id[0]) {
                std::strncpy(req.QuoteSysID, quote->quote_sys_id, sizeof(req.QuoteSysID) - 1);
            }
            encode_local_id(req.UserQuoteActionLocalID, next_local_id());
            req.ActionFlag = USTP_FTDC_AF_Delete;

            const int ret = api_->ReqQuoteAction(&req, next_req_id());
            if (ret != 0) {
                OMM_LOG_WARN("femas", "ReqQuoteAction failed ret={} quote_id={}", ret, id);
                return false;
            }
            return true;
        }
    }

    CUstpFtdcOrderActionField req{};
    {
        std::lock_guard<std::mutex> lk(state_mutex_);
        const OrderState* order_state = nullptr;
        for (auto& candidate : order_states_) {
            if (!candidate.used || candidate.is_quote_leg) continue;
            if (candidate.client_order_id == id) {
                order_state = &candidate;
                break;
            }
        }
        if (!order_state) {
            OMM_LOG_WARN("femas", "cancel_order missing state order_id={} instrument_id={}", id, instrument_id);
            return false;
        }

        std::strncpy(req.BrokerID, cfg_.femas.broker_id, sizeof(req.BrokerID) - 1);
        std::strncpy(req.ExchangeID, exchange_id(), sizeof(req.ExchangeID) - 1);
        std::strncpy(req.InvestorID, cfg_.femas.user_id, sizeof(req.InvestorID) - 1);
        std::strncpy(req.UserID, cfg_.femas.user_id, sizeof(req.UserID) - 1);
        std::strncpy(req.UserOrderLocalID, order_state->exchange_local_id, sizeof(req.UserOrderLocalID) - 1);
        if (order_state->order_sys_id[0]) {
            std::strncpy(req.OrderSysID, order_state->order_sys_id, sizeof(req.OrderSysID) - 1);
        }
        encode_local_id(req.UserOrderActionLocalID, next_local_id());
        req.ActionFlag = USTP_FTDC_AF_Delete;
    }

    const int ret = api_->ReqOrderAction(&req, next_req_id());
    if (ret != 0) {
        OMM_LOG_WARN("femas", "ReqOrderAction failed ret={} order_id={}", ret, id);
        return false;
    }
    return true;
}

bool FEMASGateway::cancel_quote(QuoteId id, uint16_t instrument_id) noexcept {
    return cancel_order(id, instrument_id);
}

bool FEMASGateway::query_instruments(Instrument* out, uint16_t* count, uint16_t max_count) {
    if (!api_ || !trading_ready_.load(std::memory_order_relaxed)) {
        *count = 0;
        return false;
    }

    {
        std::lock_guard<std::mutex> lk(qry_mutex_);
        qry_out_ = out;
        qry_count_ = count;
        qry_max_ = max_count;
        qry_done_ = false;
        *count = 0;
    }

    CUstpFtdcQryInstrumentField req{};
    std::strncpy(req.ExchangeID, exchange_id(), sizeof(req.ExchangeID) - 1);
    api_->ReqQryInstrument(&req, next_req_id());

    std::unique_lock<std::mutex> lk(qry_mutex_);
    qry_cv_.wait_for(lk, std::chrono::seconds(30), [this] { return qry_done_; });
    return qry_done_;
}

void FEMASGateway::OnFrontConnected() {
    OMM_LOG_INFO("femas", "front connected, logging in");

    CUstpFtdcReqUserLoginField req{};
    std::strncpy(req.BrokerID, cfg_.femas.broker_id, sizeof(req.BrokerID) - 1);
    std::strncpy(req.UserID, cfg_.femas.user_id, sizeof(req.UserID) - 1);
    std::strncpy(req.Password, cfg_.femas.password, sizeof(req.Password) - 1);
    api_->ReqUserLogin(&req, next_req_id());
}

void FEMASGateway::OnFrontDisconnected(int nReason) {
    trading_ready_.store(false, std::memory_order_release);
    OMM_LOG_WARN("femas", "front disconnected reason={}", nReason);

    GatewayEvent ev{};
    ev.type = GatewayEventType::Disconnected;
    (void)callback_buf.try_push(ev);
}

void FEMASGateway::OnRspUserLogin(CUstpFtdcRspUserLoginField* pLogin,
                                  CUstpFtdcRspInfoField* pRspInfo,
                                  int,
                                  bool) {
    if (pRspInfo && pRspInfo->ErrorID != 0) {
        OMM_LOG_ERROR("femas", "login failed ErrorID={} Msg={}", pRspInfo->ErrorID, pRspInfo->ErrorMsg);
        return;
    }

    if (pLogin && pLogin->MaxOrderLocalID[0]) {
        local_id_seq_.store(decode_local_id(pLogin->MaxOrderLocalID) + 1, std::memory_order_relaxed);
    }

    trading_ready_.store(true, std::memory_order_release);
    OMM_LOG_INFO("femas", "login ok TradingDay={} MaxOrderLocalID={}",
                 pLogin ? pLogin->TradingDay : "?",
                 pLogin ? pLogin->MaxOrderLocalID : "");

    GatewayEvent ev{};
    ev.type = GatewayEventType::Connected;
    (void)callback_buf.try_push(ev);
}

void FEMASGateway::OnRspOrderInsert(CUstpFtdcInputOrderField* pOrder,
                                    CUstpFtdcRspInfoField* pRspInfo,
                                    int,
                                    bool) {
    if (!pOrder) return;

    std::lock_guard<std::mutex> lk(state_mutex_);
    OrderState* state = find_order_by_local_id(pOrder->UserOrderLocalID);
    if (!state) return;

    if (pOrder->OrderSysID[0]) {
        std::strncpy(state->order_sys_id, pOrder->OrderSysID, sizeof(state->order_sys_id) - 1);
    }

    if (pRspInfo && pRspInfo->ErrorID != 0) {
        OMM_LOG_WARN("femas", "order rejected ErrorID={} Msg={} order_id={}",
                     pRspInfo->ErrorID, pRspInfo->ErrorMsg, state->client_order_id);
        push_order_event(GatewayEventType::OrderReject, *state, OrderStatus::Rejected);
        *state = OrderState{};
    }
}

void FEMASGateway::OnRtnOrder(CUstpFtdcOrderField* pOrder) {
    if (!pOrder) return;

    std::lock_guard<std::mutex> lk(state_mutex_);
    OrderState* state = find_order_by_local_id(pOrder->UserOrderLocalID);
    if (!state) state = find_order_by_sys_id(pOrder->OrderSysID);
    if (!state) {
        OMM_LOG_WARN("femas", "unmatched OnRtnOrder local_id={} sys_id={}",
                     pOrder->UserOrderLocalID, pOrder->OrderSysID);
        return;
    }

    if (pOrder->OrderSysID[0]) {
        std::strncpy(state->order_sys_id, pOrder->OrderSysID, sizeof(state->order_sys_id) - 1);
    }

    const OrderStatus status = decode_order_status(pOrder->OrderStatus);
    if (!state->acked && status != OrderStatus::Cancelled) {
        state->acked = true;
        push_order_event(GatewayEventType::OrderAck, *state, status, pOrder->VolumeTraded);
    } else if (status == OrderStatus::Cancelled) {
        push_order_event(GatewayEventType::OrderCancel, *state, status, pOrder->VolumeTraded);
        if (!state->is_quote_leg) {
            *state = OrderState{};
        }
    }
}

void FEMASGateway::OnRtnTrade(CUstpFtdcTradeField* pTrade) {
    if (!pTrade) return;

    std::lock_guard<std::mutex> lk(state_mutex_);
    OrderState* state = find_order_by_local_id(pTrade->UserOrderLocalID);
    if (!state) state = find_order_by_sys_id(pTrade->OrderSysID);
    if (!state) {
        OMM_LOG_WARN("femas", "unmatched OnRtnTrade local_id={} sys_id={}",
                     pTrade->UserOrderLocalID, pTrade->OrderSysID);
        return;
    }

    if (pTrade->OrderSysID[0]) {
        std::strncpy(state->order_sys_id, pTrade->OrderSysID, sizeof(state->order_sys_id) - 1);
    }

    push_trade_event(state->is_quote_leg ? GatewayEventType::QuoteFill : GatewayEventType::OrderFill,
                     *state, *pTrade);

    OMM_LOG_INFO("femas", "fill order_id={} quote_leg={} side={} qty={} price={:.4f}",
                 state->is_quote_leg ? state->client_quote_id : state->client_order_id,
                 state->is_quote_leg ? 1 : 0,
                 pTrade->Direction == USTP_FTDC_D_Buy ? "buy" : "sell",
                 pTrade->TradeVolume, pTrade->TradePrice);
}

void FEMASGateway::OnErrRtnOrderInsert(CUstpFtdcInputOrderField* pOrder,
                                       CUstpFtdcRspInfoField* pRspInfo) {
    if (!pOrder) return;

    std::lock_guard<std::mutex> lk(state_mutex_);
    OrderState* state = find_order_by_local_id(pOrder->UserOrderLocalID);
    if (!state) return;

    OMM_LOG_WARN("femas", "order insert error ErrorID={} Msg={} order_id={}",
                 pRspInfo ? pRspInfo->ErrorID : -1,
                 pRspInfo ? pRspInfo->ErrorMsg : "",
                 state->client_order_id);
    push_order_event(GatewayEventType::OrderReject, *state, OrderStatus::Rejected);
    *state = OrderState{};
}

void FEMASGateway::OnRspOrderAction(CUstpFtdcOrderActionField* pOrderAction,
                                    CUstpFtdcRspInfoField* pRspInfo,
                                    int,
                                    bool) {
    if (pRspInfo && pRspInfo->ErrorID != 0) {
        OMM_LOG_WARN("femas", "cancel failed ErrorID={} Msg={} local_id={} sys_id={}",
                     pRspInfo->ErrorID, pRspInfo->ErrorMsg,
                     pOrderAction ? pOrderAction->UserOrderLocalID : "",
                     pOrderAction ? pOrderAction->OrderSysID : "");
    }
}

void FEMASGateway::OnRspQuoteInsert(CUstpFtdcInputQuoteField* pQuote,
                                    CUstpFtdcRspInfoField* pRspInfo,
                                    int,
                                    bool) {
    if (!pQuote) return;

    std::lock_guard<std::mutex> lk(state_mutex_);
    QuoteState* state = find_quote_by_local_id(pQuote->UserQuoteLocalID);
    if (!state) return;

    if (pQuote->QuoteSysID[0]) {
        std::strncpy(state->quote_sys_id, pQuote->QuoteSysID, sizeof(state->quote_sys_id) - 1);
    }
    if (pRspInfo && pRspInfo->ErrorID != 0) {
        OMM_LOG_WARN("femas", "quote rejected ErrorID={} Msg={} quote_id={}",
                     pRspInfo->ErrorID, pRspInfo->ErrorMsg, state->quote.client_quote_id);

        GatewayEvent ev{};
        ev.type = GatewayEventType::QuoteReject;
        ev.product_index = state->quote.product_index;
        ev.quote = state->quote;
        ev.quote.ack_ts = get_monotonic_ns();
        (void)callback_buf.try_push(ev);

        clear_quote_state(state->quote.client_quote_id);
    }
}

void FEMASGateway::OnRtnQuote(CUstpFtdcRtnQuoteField* pQuote) {
    if (!pQuote) return;

    std::lock_guard<std::mutex> lk(state_mutex_);
    QuoteState* state = find_quote_by_local_id(pQuote->UserQuoteLocalID);
    if (!state) state = find_quote_by_sys_id(pQuote->QuoteSysID);
    if (!state) {
        OMM_LOG_WARN("femas", "unmatched OnRtnQuote local_id={} sys_id={}",
                     pQuote->UserQuoteLocalID, pQuote->QuoteSysID);
        return;
    }

    std::strncpy(state->quote_sys_id, pQuote->QuoteSysID, sizeof(state->quote_sys_id) - 1);
    std::strncpy(state->bid_order_sys_id, pQuote->BidOrderSysID, sizeof(state->bid_order_sys_id) - 1);
    std::strncpy(state->ask_order_sys_id, pQuote->AskOrderSysID, sizeof(state->ask_order_sys_id) - 1);
    if (OrderState* bid_state = find_order_by_local_id(state->bid_local_id)) {
        std::strncpy(bid_state->order_sys_id, pQuote->BidOrderSysID, sizeof(bid_state->order_sys_id) - 1);
    }
    if (OrderState* ask_state = find_order_by_local_id(state->ask_local_id)) {
        std::strncpy(ask_state->order_sys_id, pQuote->AskOrderSysID, sizeof(ask_state->order_sys_id) - 1);
    }

    state->quote.exchange_quote_id = parse_numeric_id(pQuote->QuoteSysID);
    state->quote.bid_status = OrderStatus::New;
    state->quote.ask_status = OrderStatus::New;
    state->quote.ack_ts = get_monotonic_ns();

    if (!state->acked) {
        state->acked = true;
        GatewayEvent ev{};
        ev.type = GatewayEventType::QuoteAck;
        ev.product_index = state->quote.product_index;
        ev.quote = state->quote;
        (void)callback_buf.try_push(ev);
    }

    if (pQuote->CancelTime[0]) {
        GatewayEvent ev{};
        ev.type = GatewayEventType::QuoteCancel;
        ev.product_index = state->quote.product_index;
        ev.quote = state->quote;
        ev.quote.bid_volume = 0;
        ev.quote.ask_volume = 0;
        ev.quote.ack_ts = get_monotonic_ns();
        (void)callback_buf.try_push(ev);
        clear_quote_state(state->quote.client_quote_id);
    } else if (pQuote->TradeTime[0]) {
        clear_quote_state(state->quote.client_quote_id);
    }
}

void FEMASGateway::OnErrRtnQuoteInsert(CUstpFtdcInputQuoteField* pQuote,
                                       CUstpFtdcRspInfoField* pRspInfo) {
    if (!pQuote) return;

    std::lock_guard<std::mutex> lk(state_mutex_);
    QuoteState* state = find_quote_by_local_id(pQuote->UserQuoteLocalID);
    if (!state) return;

    OMM_LOG_WARN("femas", "quote insert error ErrorID={} Msg={} quote_id={}",
                 pRspInfo ? pRspInfo->ErrorID : -1,
                 pRspInfo ? pRspInfo->ErrorMsg : "",
                 state->quote.client_quote_id);

    GatewayEvent ev{};
    ev.type = GatewayEventType::QuoteReject;
    ev.product_index = state->quote.product_index;
    ev.quote = state->quote;
    ev.quote.ack_ts = get_monotonic_ns();
    (void)callback_buf.try_push(ev);

    clear_quote_state(state->quote.client_quote_id);
}

void FEMASGateway::OnRspQuoteAction(CUstpFtdcQuoteActionField* pQuoteAction,
                                    CUstpFtdcRspInfoField* pRspInfo,
                                    int,
                                    bool) {
    if (pRspInfo && pRspInfo->ErrorID != 0) {
        OMM_LOG_WARN("femas", "quote cancel failed ErrorID={} Msg={} quote_sys_id={} quote_local_id={}",
                     pRspInfo->ErrorID, pRspInfo->ErrorMsg,
                     pQuoteAction ? pQuoteAction->QuoteSysID : "",
                     pQuoteAction ? pQuoteAction->UserQuoteLocalID : "");
    }
}

void FEMASGateway::OnErrRtnQuoteAction(CUstpFtdcQuoteActionField* pQuoteAction,
                                       CUstpFtdcRspInfoField* pRspInfo) {
    OMM_LOG_WARN("femas", "quote cancel error ErrorID={} Msg={} quote_sys_id={} quote_local_id={}",
                 pRspInfo ? pRspInfo->ErrorID : -1,
                 pRspInfo ? pRspInfo->ErrorMsg : "",
                 pQuoteAction ? pQuoteAction->QuoteSysID : "",
                 pQuoteAction ? pQuoteAction->UserQuoteLocalID : "");
}

void FEMASGateway::OnRspQryInstrument(CUstpFtdcRspInstrumentField* pInstrument,
                                      CUstpFtdcRspInfoField*,
                                      int,
                                      bool bIsLast) {
    std::lock_guard<std::mutex> lk(qry_mutex_);
    if (pInstrument && qry_out_ && *qry_count_ < qry_max_) {
        fill_instrument(qry_out_[*qry_count_], *pInstrument, *qry_count_);
        (*qry_count_)++;
    }
    if (bIsLast) {
        qry_done_ = true;
        qry_cv_.notify_one();
        OMM_LOG_INFO("femas", "instrument query complete count={}", *qry_count_);
    }
}

void FEMASGateway::fill_instrument(Instrument& out,
                                   const CUstpFtdcRspInstrumentField& src,
                                   uint16_t id) noexcept {
    out = Instrument{};
    out.instrument_id = id;
    out.underlying_id = INVALID_INSTRUMENT_ID;
    out.product_index = 0xFF;
    std::strncpy(out.code.data, src.InstrumentID, sizeof(out.code.data) - 1);
    std::strncpy(out.underlying_code.data, src.UnderlyingInstrID, sizeof(out.underlying_code.data) - 1);
    std::strncpy(out.exchange_id.data, src.ExchangeID, sizeof(out.exchange_id.data) - 1);
    out.tick_size = src.PriceTick;
    out.multiplier = static_cast<double>(src.VolumeMultiple);
    out.exchange = Exchange::CFFEX;

    if (src.OptionsType == USTP_FTDC_OT_CallOptions) {
        out.kind = InstrumentKind::Option;
        out.option_type = OptionType::Call;
        out.strike = src.StrikePrice;
    } else if (src.OptionsType == USTP_FTDC_OT_PutOptions) {
        out.kind = InstrumentKind::Option;
        out.option_type = OptionType::Put;
        out.strike = src.StrikePrice;
    } else {
        out.kind = InstrumentKind::Future;
    }
}

} // namespace omm
