#include "gateway/ctp_gateway.h"
#include "logger/logger.h"

#include <cstring>
#include <cstdio>
#include <chrono>
#include <thread>

namespace omm {

// ─── connect / disconnect ─────────────────────────────────────────────────────

bool CTPGateway::connect(const GatewayConfig& cfg) {
    cfg_ = cfg;

    // Create API instance; flow_path is a directory for CTP flow files
    api_ = CThostFtdcTraderApi::CreateFtdcTraderApi("./ctp_flow/");
    if (!api_) {
        OMM_LOG_ERROR("ctp", "CreateFtdcTraderApi failed");
        return false;
    }

    api_->RegisterSpi(this);
    api_->SubscribePrivateTopic(THOST_TERT_QUICK);
    api_->SubscribePublicTopic(THOST_TERT_QUICK);
    api_->RegisterFront(const_cast<char*>(cfg_.ctp.front_addr));
    api_->Init();

    OMM_LOG_INFO("ctp", "connecting to front={}", cfg_.ctp.front_addr);
    return true;  // actual readiness signalled via OnRspSettlementInfoConfirm
}

void CTPGateway::disconnect() {
    trading_ready_.store(false, std::memory_order_release);
    if (api_) {
        api_->RegisterSpi(nullptr);
        api_->Release();
        api_ = nullptr;
        OMM_LOG_INFO("ctp", "disconnected");
    }
}

// ─── send_order ───────────────────────────────────────────────────────────────

bool CTPGateway::send_order(const Order& order) noexcept {
    if (!api_ || !trading_ready_.load(std::memory_order_relaxed)) return false;

    CThostFtdcInputOrderField req{};
    std::strncpy(req.BrokerID,    cfg_.ctp.broker_id, sizeof(req.BrokerID) - 1);
    std::strncpy(req.InvestorID,  cfg_.ctp.user_id,   sizeof(req.InvestorID) - 1);

    // Map instrument_id → exchange instrument code via the instrument registry
    // (stored in InstrumentID field of our Order — caller must set it)
    // For now we use a placeholder; production wires instrument registry lookup here.
    std::snprintf(req.InstrumentID, sizeof(req.InstrumentID), "%u", order.instrument_id);

    encode_order_ref(req.OrderRef, order.client_order_id);

    req.OrderPriceType = (order.order_type == OrderType::Market)
                         ? THOST_FTDC_OPT_AnyPrice
                         : THOST_FTDC_OPT_LimitPrice;
    req.Direction      = (order.side == Side::Buy)
                         ? THOST_FTDC_D_Buy
                         : THOST_FTDC_D_Sell;
    req.CombOffsetFlag[0] = order.is_hedge ? THOST_FTDC_OF_Close : THOST_FTDC_OF_Open;
    req.CombHedgeFlag[0]  = THOST_FTDC_HF_Speculation;
    req.LimitPrice        = order.price;
    req.VolumeTotalOriginal = order.volume;
    req.TimeCondition     = THOST_FTDC_TC_GFD;
    req.VolumeCondition   = THOST_FTDC_VC_AV;
    req.MinVolume         = 1;
    req.ContingentCondition = THOST_FTDC_CC_Immediately;
    req.ForceCloseReason  = THOST_FTDC_FCC_NotForceClose;
    req.IsAutoSuspend     = 0;

    int ret = api_->ReqOrderInsert(&req, next_req_id());
    if (ret != 0) {
        OMM_LOG_WARN("ctp", "ReqOrderInsert failed ret={} order_id={}", ret, order.client_order_id);
        return false;
    }
    return true;
}

// ─── send_quote ───────────────────────────────────────────────────────────────
// CTP does not have a native two-sided quote API for commodity options.
// We simulate it by sending two separate limit orders (bid + ask).

bool CTPGateway::send_quote(const Quote& quote) noexcept {
    if (!api_ || !trading_ready_.load(std::memory_order_relaxed)) return false;

    bool ok = true;

    if (quote.bid_volume > 0) {
        Order bid{};
        bid.client_order_id = quote.client_quote_id;
        bid.instrument_id   = quote.instrument_id;
        bid.product_index   = quote.product_index;
        bid.side            = Side::Buy;
        bid.order_type      = OrderType::Limit;
        bid.price           = quote.bid_price;
        bid.volume          = quote.bid_volume;
        ok &= send_order(bid);
    }

    if (quote.ask_volume > 0) {
        Order ask{};
        ask.client_order_id = quote.client_quote_id | (1ULL << 32);  // distinguish ask leg
        ask.instrument_id   = quote.instrument_id;
        ask.product_index   = quote.product_index;
        ask.side            = Side::Sell;
        ask.order_type      = OrderType::Limit;
        ask.price           = quote.ask_price;
        ask.volume          = quote.ask_volume;
        ok &= send_order(ask);
    }

    // Push QuoteAck immediately (CTP acks each leg separately via OnRtnOrder)
    GatewayEvent ev{};
    ev.type          = GatewayEventType::QuoteAck;
    ev.product_index = quote.product_index;
    ev.quote         = quote;
    (void)callback_buf.try_push(ev);

    return ok;
}

// ─── cancel_order ─────────────────────────────────────────────────────────────

bool CTPGateway::cancel_order(OrderId id, uint16_t instrument_id) noexcept {
    if (!api_ || !trading_ready_.load(std::memory_order_relaxed)) return false;

    CThostFtdcInputOrderActionField req{};
    std::strncpy(req.BrokerID,   cfg_.ctp.broker_id, sizeof(req.BrokerID) - 1);
    std::strncpy(req.InvestorID, cfg_.ctp.user_id,   sizeof(req.InvestorID) - 1);
    encode_order_ref(req.OrderRef, id);
    req.ActionFlag = THOST_FTDC_AF_Delete;

    int ret = api_->ReqOrderAction(&req, next_req_id());
    if (ret != 0) {
        OMM_LOG_WARN("ctp", "ReqOrderAction failed ret={} order_id={}", ret, id);
        return false;
    }
    return true;
}

// ─── query_instruments ────────────────────────────────────────────────────────

bool CTPGateway::query_instruments(Instrument* out, uint16_t* count,
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

    CThostFtdcQryInstrumentField req{};
    api_->ReqQryInstrument(&req, next_req_id());

    // Block until OnRspQryInstrument signals completion (bIsLast=true)
    std::unique_lock<std::mutex> lk(qry_mutex_);
    qry_cv_.wait_for(lk, std::chrono::seconds(30), [this] { return qry_done_; });

    return qry_done_;
}

// ─── CTP SPI callbacks ────────────────────────────────────────────────────────

void CTPGateway::OnFrontConnected() {
    OMM_LOG_INFO("ctp", "front connected — logging in");

    CThostFtdcReqUserLoginField req{};
    std::strncpy(req.BrokerID, cfg_.ctp.broker_id, sizeof(req.BrokerID) - 1);
    std::strncpy(req.UserID,   cfg_.ctp.user_id,   sizeof(req.UserID) - 1);
    std::strncpy(req.Password, cfg_.ctp.password,  sizeof(req.Password) - 1);
    api_->ReqUserLogin(&req, next_req_id());
}

void CTPGateway::OnFrontDisconnected(int nReason) {
    trading_ready_.store(false, std::memory_order_release);
    OMM_LOG_WARN("ctp", "front disconnected reason={}", nReason);

    GatewayEvent ev{};
    ev.type = GatewayEventType::Disconnected;
    (void)callback_buf.try_push(ev);
}

void CTPGateway::OnRspUserLogin(CThostFtdcRspUserLoginField* pLogin,
                                  CThostFtdcRspInfoField* pRspInfo,
                                  int, bool) {
    if (pRspInfo && pRspInfo->ErrorID != 0) {
        OMM_LOG_ERROR("ctp", "login failed ErrorID={} Msg={}", pRspInfo->ErrorID, pRspInfo->ErrorMsg);
        return;
    }
    OMM_LOG_INFO("ctp", "login ok TradingDay={}", pLogin ? pLogin->TradingDay : "?");

    // Confirm settlement info to unlock trading
    CThostFtdcSettlementInfoConfirmField req{};
    std::strncpy(req.BrokerID,   cfg_.ctp.broker_id, sizeof(req.BrokerID) - 1);
    std::strncpy(req.InvestorID, cfg_.ctp.user_id,   sizeof(req.InvestorID) - 1);
    api_->ReqSettlementInfoConfirm(&req, next_req_id());
}

void CTPGateway::OnRspSettlementInfoConfirm(
        CThostFtdcSettlementInfoConfirmField*,
        CThostFtdcRspInfoField* pRspInfo,
        int, bool) {
    if (pRspInfo && pRspInfo->ErrorID != 0) {
        OMM_LOG_ERROR("ctp", "settlement confirm failed ErrorID={}", pRspInfo->ErrorID);
        return;
    }
    trading_ready_.store(true, std::memory_order_release);
    OMM_LOG_INFO("ctp", "settlement confirmed — trading ready");

    GatewayEvent ev{};
    ev.type = GatewayEventType::Connected;
    (void)callback_buf.try_push(ev);
}

void CTPGateway::OnRspOrderInsert(CThostFtdcInputOrderField* pOrder,
                                    CThostFtdcRspInfoField* pRspInfo,
                                    int, bool) {
    if (!pOrder) return;
    OMM_LOG_WARN("ctp", "order rejected ErrorID={} Msg={}",
                 pRspInfo ? pRspInfo->ErrorID : -1,
                 pRspInfo ? pRspInfo->ErrorMsg : "");

    GatewayEvent ev{};
    ev.type = GatewayEventType::OrderReject;
    ev.order.client_order_id = decode_order_ref(pOrder->OrderRef);
    (void)callback_buf.try_push(ev);
}

void CTPGateway::OnRtnOrder(CThostFtdcOrderField* pOrder) {
    if (!pOrder) return;

    OrderId oid = decode_order_ref(pOrder->OrderRef);

    if (pOrder->OrderStatus == THOST_FTDC_OST_Canceled) {
        GatewayEvent ev{};
        ev.type = GatewayEventType::OrderCancel;
        ev.order.client_order_id = oid;
        (void)callback_buf.try_push(ev);
    } else if (pOrder->OrderSubmitStatus == THOST_FTDC_OSS_InsertSubmitted ||
               pOrder->OrderSubmitStatus == THOST_FTDC_OSS_Accepted) {
        GatewayEvent ev{};
        ev.type = GatewayEventType::OrderAck;
        ev.order.client_order_id    = oid;
        ev.order.exchange_order_id  = std::strtoull(pOrder->OrderSysID, nullptr, 10);
        ev.order.status             = OrderStatus::New;
        (void)callback_buf.try_push(ev);
    }
}

void CTPGateway::OnRtnTrade(CThostFtdcTradeField* pTrade) {
    if (!pTrade) return;

    GatewayEvent ev{};
    ev.type = GatewayEventType::OrderFill;
    ev.trade.client_order_id = decode_order_ref(pTrade->OrderRef);
    ev.trade.fill_price      = pTrade->Price;
    ev.trade.fill_volume     = pTrade->Volume;
    ev.trade.side            = (pTrade->Direction == THOST_FTDC_D_Buy)
                               ? Side::Buy : Side::Sell;
    ev.trade.offset          = (pTrade->OffsetFlag == THOST_FTDC_OF_Open)
                               ? OffsetFlag::Open : OffsetFlag::Close;
    (void)callback_buf.try_push(ev);

    OMM_LOG_INFO("ctp", "fill order_id={} side={} qty={} price={:.4f}",
                 ev.trade.client_order_id,
                 pTrade->Direction == THOST_FTDC_D_Buy ? "buy" : "sell",
                 pTrade->Volume, pTrade->Price);
}

void CTPGateway::OnErrRtnOrderInsert(CThostFtdcInputOrderField* pOrder,
                                       CThostFtdcRspInfoField* pRspInfo) {
    if (!pOrder) return;
    OMM_LOG_WARN("ctp", "order insert error ErrorID={} Msg={}",
                 pRspInfo ? pRspInfo->ErrorID : -1,
                 pRspInfo ? pRspInfo->ErrorMsg : "");

    GatewayEvent ev{};
    ev.type = GatewayEventType::OrderReject;
    ev.order.client_order_id = decode_order_ref(pOrder->OrderRef);
    (void)callback_buf.try_push(ev);
}

void CTPGateway::OnRspOrderAction(CThostFtdcInputOrderActionField*,
                                    CThostFtdcRspInfoField* pRspInfo,
                                    int, bool) {
    if (pRspInfo && pRspInfo->ErrorID != 0)
        OMM_LOG_WARN("ctp", "cancel failed ErrorID={} Msg={}",
                     pRspInfo->ErrorID, pRspInfo->ErrorMsg);
}

void CTPGateway::OnRspQryInstrument(CThostFtdcInstrumentField* pInstrument,
                                      CThostFtdcRspInfoField*,
                                      int, bool bIsLast) {
    std::lock_guard<std::mutex> lk(qry_mutex_);
    if (pInstrument && qry_out_ && *qry_count_ < qry_max_) {
        fill_instrument(qry_out_[*qry_count_], *pInstrument, *qry_count_);
        (*qry_count_)++;
    }
    if (bIsLast) {
        qry_done_ = true;
        qry_cv_.notify_one();
        OMM_LOG_INFO("ctp", "instrument query complete count={}", *qry_count_);
    }
}

// ─── fill_instrument ─────────────────────────────────────────────────────────

void CTPGateway::fill_instrument(Instrument& out,
                                   const CThostFtdcInstrumentField& src,
                                   uint16_t id) noexcept {
    out = Instrument{};
    out.instrument_id = id;
    std::strncpy(out.code.data,            src.InstrumentID,       sizeof(out.code.data) - 1);
    std::strncpy(out.underlying_code.data, src.UnderlyingInstrID,  sizeof(out.underlying_code.data) - 1);
    std::strncpy(out.exchange_id.data,     src.ExchangeID,         sizeof(out.exchange_id.data) - 1);
    out.tick_size   = src.PriceTick;
    out.multiplier  = static_cast<double>(src.VolumeMultiple);
    out.strike      = src.StrikePrice;

    // ProductClass: '2'=Options, '1'=Futures (CTP char codes)
    if (src.ProductClass == THOST_FTDC_PC_Options) {
        out.kind        = InstrumentKind::Option;
        out.option_type = (src.OptionsType == THOST_FTDC_CP_CallOptions)
                          ? OptionType::Call : OptionType::Put;
    } else {
        out.kind = InstrumentKind::Future;
    }

    // Exchange enum
    if      (std::strncmp(src.ExchangeID, "SHFE", 4) == 0) out.exchange = Exchange::SHFE;
    else if (std::strncmp(src.ExchangeID, "DCE",  3) == 0) out.exchange = Exchange::DCE;
    else if (std::strncmp(src.ExchangeID, "CZCE", 4) == 0) out.exchange = Exchange::CZCE;
    else                                                     out.exchange = Exchange::Unknown;

    // Expiry date: CTP ExpireDate is "YYYYMMDD"
    out.expiry_date = std::atoi(src.ExpireDate);
}

} // namespace omm
