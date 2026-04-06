#include "gateway/femas_gateway.h"
#include "logger/logger.h"

#include <cstring>
#include <cstdio>
#include <chrono>

namespace omm {

// ─── connect / disconnect ─────────────────────────────────────────────────────

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
    if (api_) {
        api_->RegisterSpi(nullptr);
        api_->Release();
        api_ = nullptr;
        OMM_LOG_INFO("femas", "disconnected");
    }
}

// ─── send_order ───────────────────────────────────────────────────────────────

bool FEMASGateway::send_order(const Order& order) noexcept {
    if (!api_ || !trading_ready_.load(std::memory_order_relaxed)) return false;

    CUstpFtdcInputOrderField req{};
    std::strncpy(req.BrokerID,   cfg_.femas.broker_id, sizeof(req.BrokerID) - 1);
    std::strncpy(req.InvestorID, cfg_.femas.user_id,   sizeof(req.InvestorID) - 1);
    std::strncpy(req.ExchangeID, "CFFEX",             sizeof(req.ExchangeID) - 1);
    std::snprintf(req.InstrumentID, sizeof(req.InstrumentID), "%u", order.instrument_id);
    encode_local_id(req.UserOrderLocalID, order.client_order_id);

    req.OrderPriceType = (order.order_type == OrderType::Market)
                         ? USTP_FTDC_OPT_AnyPrice
                         : USTP_FTDC_OPT_LimitPrice;
    req.Direction      = (order.side == Side::Buy)
                         ? USTP_FTDC_D_Buy
                         : USTP_FTDC_D_Sell;
    req.OffsetFlag     = order.is_hedge ? USTP_FTDC_OF_Close : USTP_FTDC_OF_Open;
    req.HedgeFlag      = USTP_FTDC_CHF_Speculation;
    req.LimitPrice     = order.price;
    req.Volume         = order.volume;
    req.TimeCondition  = USTP_FTDC_TC_GFD;
    req.VolumeCondition = USTP_FTDC_VC_AV;
    req.MinVolume      = 1;
    req.ForceCloseReason = USTP_FTDC_FCR_NotForceClose;

    int ret = api_->ReqOrderInsert(&req, next_req_id());
    if (ret != 0) {
        OMM_LOG_WARN("femas", "ReqOrderInsert failed ret={} order_id={}", ret, order.client_order_id);
        return false;
    }
    return true;
}

// ─── send_quote ───────────────────────────────────────────────────────────────
// FEMAS has a native two-sided quote API — use it for options market making.

bool FEMASGateway::send_quote(const Quote& quote) noexcept {
    if (!api_ || !trading_ready_.load(std::memory_order_relaxed)) return false;

    CUstpFtdcInputQuoteField req{};
    std::strncpy(req.BrokerID,   cfg_.femas.broker_id, sizeof(req.BrokerID) - 1);
    std::strncpy(req.InvestorID, cfg_.femas.user_id,   sizeof(req.InvestorID) - 1);
    std::strncpy(req.ExchangeID, "CFFEX",             sizeof(req.ExchangeID) - 1);
    std::snprintf(req.InstrumentID, sizeof(req.InstrumentID), "%u", quote.instrument_id);
    encode_local_id(req.UserQuoteLocalID, quote.client_quote_id);

    req.AskOffsetFlag  = USTP_FTDC_OF_Open;
    req.BidOffsetFlag  = USTP_FTDC_OF_Open;
    req.AskHedgeFlag   = USTP_FTDC_CHF_MarketMaker;
    req.BidHedgeFlag   = USTP_FTDC_CHF_MarketMaker;
    req.AskPrice       = quote.ask_price;
    req.BidPrice       = quote.bid_price;
    req.AskVolume      = quote.ask_volume;
    req.BidVolume      = quote.bid_volume;

    int ret = api_->ReqQuoteInsert(&req, next_req_id());
    if (ret != 0) {
        OMM_LOG_WARN("femas", "ReqQuoteInsert failed ret={} quote_id={}", ret, quote.client_quote_id);
        return false;
    }
    return true;
}

// ─── cancel_order ─────────────────────────────────────────────────────────────

bool FEMASGateway::cancel_order(OrderId id, uint16_t instrument_id) noexcept {
    if (!api_ || !trading_ready_.load(std::memory_order_relaxed)) return false;

    CUstpFtdcOrderActionField req{};
    std::strncpy(req.BrokerID,   cfg_.femas.broker_id, sizeof(req.BrokerID) - 1);
    std::strncpy(req.InvestorID, cfg_.femas.user_id,   sizeof(req.InvestorID) - 1);
    std::strncpy(req.ExchangeID, "CFFEX",             sizeof(req.ExchangeID) - 1);
    encode_local_id(req.UserOrderLocalID, id);
    req.ActionFlag = USTP_FTDC_AF_Delete;

    int ret = api_->ReqOrderAction(&req, next_req_id());
    if (ret != 0) {
        OMM_LOG_WARN("femas", "ReqOrderAction failed ret={} order_id={}", ret, id);
        return false;
    }
    return true;
}

// ─── query_instruments ────────────────────────────────────────────────────────

bool FEMASGateway::query_instruments(Instrument* out, uint16_t* count,
                                      uint16_t max_count) {
    if (!api_ || !trading_ready_.load(std::memory_order_relaxed)) {
        *count = 0;
        return false;
    }

    {
        std::lock_guard<std::mutex> lk(qry_mutex_);
        qry_out_   = out;
        qry_count_ = count;
        qry_max_   = max_count;
        qry_done_  = false;
        *count     = 0;
    }

    CUstpFtdcQryInstrumentField req{};
    std::strncpy(req.ExchangeID, "CFFEX", sizeof(req.ExchangeID) - 1);
    api_->ReqQryInstrument(&req, next_req_id());

    std::unique_lock<std::mutex> lk(qry_mutex_);
    qry_cv_.wait_for(lk, std::chrono::seconds(30), [this] { return qry_done_; });
    return qry_done_;
}

// ─── SPI callbacks ────────────────────────────────────────────────────────────

void FEMASGateway::OnFrontConnected() {
    OMM_LOG_INFO("femas", "front connected — logging in");

    CUstpFtdcReqUserLoginField req{};
    std::strncpy(req.BrokerID, cfg_.femas.broker_id, sizeof(req.BrokerID) - 1);
    std::strncpy(req.UserID,   cfg_.femas.user_id,   sizeof(req.UserID) - 1);
    std::strncpy(req.Password, cfg_.femas.password,  sizeof(req.Password) - 1);
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
                                    int, bool) {
    if (pRspInfo && pRspInfo->ErrorID != 0) {
        OMM_LOG_ERROR("femas", "login failed ErrorID={} Msg={}", pRspInfo->ErrorID, pRspInfo->ErrorMsg);
        return;
    }
    trading_ready_.store(true, std::memory_order_release);
    OMM_LOG_INFO("femas", "login ok TradingDay={}", pLogin ? pLogin->TradingDay : "?");

    GatewayEvent ev{};
    ev.type = GatewayEventType::Connected;
    (void)callback_buf.try_push(ev);
}

void FEMASGateway::OnRspOrderInsert(CUstpFtdcInputOrderField* pOrder,
                                      CUstpFtdcRspInfoField* pRspInfo,
                                      int, bool) {
    if (!pOrder) return;
    OMM_LOG_WARN("femas", "order rejected ErrorID={} Msg={}",
                 pRspInfo ? pRspInfo->ErrorID : -1,
                 pRspInfo ? pRspInfo->ErrorMsg : "");

    GatewayEvent ev{};
    ev.type = GatewayEventType::OrderReject;
    ev.order.client_order_id = decode_local_id(pOrder->UserOrderLocalID);
    (void)callback_buf.try_push(ev);
}

void FEMASGateway::OnRtnOrder(CUstpFtdcOrderField* pOrder) {
    if (!pOrder) return;

    OrderId oid = decode_local_id(pOrder->UserOrderLocalID);

    if (pOrder->OrderStatus == USTP_FTDC_OS_Canceled) {
        GatewayEvent ev{};
        ev.type = GatewayEventType::OrderCancel;
        ev.order.client_order_id = oid;
        (void)callback_buf.try_push(ev);
    } else {
        GatewayEvent ev{};
        ev.type = GatewayEventType::OrderAck;
        ev.order.client_order_id = oid;
        ev.order.status          = OrderStatus::New;
        (void)callback_buf.try_push(ev);
    }
}

void FEMASGateway::OnRtnTrade(CUstpFtdcTradeField* pTrade) {
    if (!pTrade) return;

    GatewayEvent ev{};
    ev.type = GatewayEventType::OrderFill;
    ev.trade.client_order_id = decode_local_id(pTrade->UserOrderLocalID);
    ev.trade.fill_price      = pTrade->TradePrice;
    ev.trade.fill_volume     = pTrade->TradeVolume;
    ev.trade.side            = (pTrade->Direction == USTP_FTDC_D_Buy)
                               ? Side::Buy : Side::Sell;
    ev.trade.offset          = (pTrade->OffsetFlag == USTP_FTDC_OF_Open)
                               ? OffsetFlag::Open : OffsetFlag::Close;
    (void)callback_buf.try_push(ev);

    OMM_LOG_INFO("femas", "fill order_id={} side={} qty={} price={:.4f}",
                 ev.trade.client_order_id,
                 pTrade->Direction == USTP_FTDC_D_Buy ? "buy" : "sell",
                 pTrade->TradeVolume, pTrade->TradePrice);
}

void FEMASGateway::OnErrRtnOrderInsert(CUstpFtdcInputOrderField* pOrder,
                                         CUstpFtdcRspInfoField* pRspInfo) {
    if (!pOrder) return;
    OMM_LOG_WARN("femas", "order insert error ErrorID={} Msg={}",
                 pRspInfo ? pRspInfo->ErrorID : -1,
                 pRspInfo ? pRspInfo->ErrorMsg : "");

    GatewayEvent ev{};
    ev.type = GatewayEventType::OrderReject;
    ev.order.client_order_id = decode_local_id(pOrder->UserOrderLocalID);
    (void)callback_buf.try_push(ev);
}

void FEMASGateway::OnRspOrderAction(CUstpFtdcOrderActionField*,
                                      CUstpFtdcRspInfoField* pRspInfo,
                                      int, bool) {
    if (pRspInfo && pRspInfo->ErrorID != 0)
        OMM_LOG_WARN("femas", "cancel failed ErrorID={} Msg={}",
                     pRspInfo->ErrorID, pRspInfo->ErrorMsg);
}

void FEMASGateway::OnRspQuoteInsert(CUstpFtdcInputQuoteField* pQuote,
                                      CUstpFtdcRspInfoField* pRspInfo,
                                      int, bool) {
    if (!pQuote) return;
    if (pRspInfo && pRspInfo->ErrorID != 0) {
        OMM_LOG_WARN("femas", "quote rejected ErrorID={} Msg={}",
                     pRspInfo->ErrorID, pRspInfo->ErrorMsg);
        GatewayEvent ev{};
        ev.type = GatewayEventType::OrderReject;
        ev.order.client_order_id = decode_local_id(pQuote->UserQuoteLocalID);
        (void)callback_buf.try_push(ev);
    }
}

void FEMASGateway::OnRtnQuote(CUstpFtdcRtnQuoteField* pQuote) {
    if (!pQuote) return;

    GatewayEvent ev{};
    ev.type = GatewayEventType::QuoteAck;
    ev.quote.client_quote_id = decode_local_id(pQuote->UserQuoteLocalID);
    ev.quote.bid_price       = pQuote->BidPrice;
    ev.quote.ask_price       = pQuote->AskPrice;
    ev.quote.bid_volume      = pQuote->BidVolume;
    ev.quote.ask_volume      = pQuote->AskVolume;
    (void)callback_buf.try_push(ev);
}

void FEMASGateway::OnErrRtnQuoteInsert(CUstpFtdcInputQuoteField* pQuote,
                                         CUstpFtdcRspInfoField* pRspInfo) {
    if (!pQuote) return;
    OMM_LOG_WARN("femas", "quote insert error ErrorID={} Msg={}",
                 pRspInfo ? pRspInfo->ErrorID : -1,
                 pRspInfo ? pRspInfo->ErrorMsg : "");

    GatewayEvent ev{};
    ev.type = GatewayEventType::OrderReject;
    ev.order.client_order_id = decode_local_id(pQuote->UserQuoteLocalID);
    (void)callback_buf.try_push(ev);
}

void FEMASGateway::OnRspQuoteAction(CUstpFtdcQuoteActionField*,
                                      CUstpFtdcRspInfoField* pRspInfo,
                                      int, bool) {
    if (pRspInfo && pRspInfo->ErrorID != 0)
        OMM_LOG_WARN("femas", "quote cancel failed ErrorID={} Msg={}",
                     pRspInfo->ErrorID, pRspInfo->ErrorMsg);
}

void FEMASGateway::OnRspQryInstrument(CUstpFtdcRspInstrumentField* pInstrument,
                                        CUstpFtdcRspInfoField*,
                                        int, bool bIsLast) {
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

// ─── fill_instrument ─────────────────────────────────────────────────────────

void FEMASGateway::fill_instrument(Instrument& out,
                                     const CUstpFtdcRspInstrumentField& src,
                                     uint16_t id) noexcept {
    out = Instrument{};
    out.instrument_id = id;
    std::strncpy(out.code.data,            src.InstrumentID,       sizeof(out.code.data) - 1);
    std::strncpy(out.underlying_code.data, src.UnderlyingInstrID,  sizeof(out.underlying_code.data) - 1);
    std::strncpy(out.exchange_id.data,     src.ExchangeID,         sizeof(out.exchange_id.data) - 1);
    out.tick_size  = src.PriceTick;
    out.multiplier = static_cast<double>(src.VolumeMultiple);
    out.exchange   = Exchange::CFFEX;

    // FEMAS OptionsType: 'C'=Call, 'P'=Put, ' '=not option
    if (src.OptionsType == USTP_FTDC_OT_CallOptions) {
        out.kind        = InstrumentKind::Option;
        out.option_type = OptionType::Call;
        out.strike      = src.StrikePrice;
    } else if (src.OptionsType == USTP_FTDC_OT_PutOptions) {
        out.kind        = InstrumentKind::Option;
        out.option_type = OptionType::Put;
        out.strike      = src.StrikePrice;
    } else {
        out.kind = InstrumentKind::Future;
    }
}

} // namespace omm
