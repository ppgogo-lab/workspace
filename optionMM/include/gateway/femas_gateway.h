#pragma once

#include "gateway/gateway.h"
#include "common/thread_utils.h"

// FEMAS SDK headers
#include "USTPFtdcTraderApi.h"

#include <atomic>
#include <string>
#include <cstring>
#include <mutex>
#include <condition_variable>

namespace omm {

// ─── FEMASGateway ─────────────────────────────────────────────────────────────
// Production gateway for CFFEX (equity index options) via FEMAS TraderAPI.
//
// Thread model: same as CTPGateway — see ctp_gateway.h for details.
//
// FEMAS differences from CTP:
//   - Native two-sided quote API: ReqQuoteInsert / OnRtnQuote (used for options MM).
//   - UserOrderLocalID is a 13-char string (same encoding as CTP OrderRef).
//   - Login does NOT require settlement confirmation.
//   - ExchangeID must be set explicitly on every order ("CFFEX").
//   - Topic subscriptions are explicit: SubscribePrivateTopic / SubscribePublicTopic.

class FEMASGateway : public IGateway, private CUstpFtdcTraderSpi {
public:
    FEMASGateway() = default;
    ~FEMASGateway() override { disconnect(); }

    FEMASGateway(const FEMASGateway&)            = delete;
    FEMASGateway& operator=(const FEMASGateway&) = delete;

    // ── IGateway ─────────────────────────────────────────────────────────────
    bool connect(const GatewayConfig& cfg) override;
    void disconnect() override;
    [[nodiscard]] bool is_connected() const noexcept override {
        return trading_ready_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] bool send_order(const Order& order) noexcept override;
    [[nodiscard]] bool send_quote(const Quote& quote) noexcept override;
    [[nodiscard]] bool cancel_order(OrderId id,
                                     uint16_t instrument_id) noexcept override;

    bool query_instruments(Instrument* out, uint16_t* count,
                            uint16_t max_count) override;

private:
    CUstpFtdcTraderApi* api_{nullptr};
    GatewayConfig       cfg_{};

    std::atomic<bool> trading_ready_{false};
    std::atomic<int>  request_id_{1};

    // Blocking instrument query
    std::mutex              qry_mutex_;
    std::condition_variable qry_cv_;
    Instrument*             qry_out_{nullptr};
    uint16_t*               qry_count_{nullptr};
    uint16_t                qry_max_{0};
    bool                    qry_done_{false};

    // UserOrderLocalID encoding (same scheme as CTP OrderRef)
    static void encode_local_id(char* buf, OrderId id) noexcept {
        std::snprintf(buf, 13, "%012llu", static_cast<unsigned long long>(id & 0xFFFFFFFFFFFFULL));
    }
    static OrderId decode_local_id(const char* buf) noexcept {
        return static_cast<OrderId>(std::strtoull(buf, nullptr, 10));
    }

    int next_req_id() noexcept {
        return request_id_.fetch_add(1, std::memory_order_relaxed);
    }

    // ── CUstpFtdcTraderSpi callbacks ──────────────────────────────────────────
    void OnFrontConnected() override;
    void OnFrontDisconnected(int nReason) override;
    void OnRspUserLogin(CUstpFtdcRspUserLoginField* pRspUserLogin,
                        CUstpFtdcRspInfoField* pRspInfo,
                        int nRequestID, bool bIsLast) override;
    void OnRspOrderInsert(CUstpFtdcInputOrderField* pInputOrder,
                          CUstpFtdcRspInfoField* pRspInfo,
                          int nRequestID, bool bIsLast) override;
    void OnRtnOrder(CUstpFtdcOrderField* pOrder) override;
    void OnRtnTrade(CUstpFtdcTradeField* pTrade) override;
    void OnErrRtnOrderInsert(CUstpFtdcInputOrderField* pInputOrder,
                              CUstpFtdcRspInfoField* pRspInfo) override;
    void OnRspOrderAction(CUstpFtdcOrderActionField* pOrderAction,
                          CUstpFtdcRspInfoField* pRspInfo,
                          int nRequestID, bool bIsLast) override;
    // Native two-sided quote callbacks
    void OnRspQuoteInsert(CUstpFtdcInputQuoteField* pInputQuote,
                          CUstpFtdcRspInfoField* pRspInfo,
                          int nRequestID, bool bIsLast) override;
    void OnRtnQuote(CUstpFtdcRtnQuoteField* pQuote) override;
    void OnErrRtnQuoteInsert(CUstpFtdcInputQuoteField* pInputQuote,
                              CUstpFtdcRspInfoField* pRspInfo) override;
    void OnRspQuoteAction(CUstpFtdcQuoteActionField* pQuoteAction,
                          CUstpFtdcRspInfoField* pRspInfo,
                          int nRequestID, bool bIsLast) override;
    void OnRspQryInstrument(CUstpFtdcRspInstrumentField* pInstrument,
                             CUstpFtdcRspInfoField* pRspInfo,
                             int nRequestID, bool bIsLast) override;

    static void fill_instrument(Instrument& out,
                                  const CUstpFtdcRspInstrumentField& src,
                                  uint16_t id) noexcept;
};

} // namespace omm
