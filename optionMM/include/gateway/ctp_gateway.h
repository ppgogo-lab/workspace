#pragma once

#include "gateway/gateway.h"
#include "common/thread_utils.h"

// CTP SDK headers
#include "ThostFtdcTraderApi.h"

#include <atomic>
#include <string>
#include <cstring>
#include <mutex>
#include <condition_variable>
#include <array>

namespace omm {

// ─── CTPGateway ───────────────────────────────────────────────────────────────
// Production gateway for SHFE / DCE / CZCE via CTP TraderAPI v6.7.2.
//
// Thread model:
//   connect() / disconnect() : called from main thread at startup/shutdown.
//   send_order / send_quote / cancel_order : called from gateway dispatcher thread.
//   CTP callbacks (OnRtn*, OnRsp*) : called from CTP internal thread.
//     → push GatewayEvents into callback_buf (SPSC, dispatcher reads).
//
// CTP quirks handled here:
//   - OrderRef is a 13-char string; we embed our 64-bit client_order_id
//     using the lower 12 decimal digits.
//   - Instrument query is blocking: ReqQryInstrument → OnRspQryInstrument
//     with bIsLast=true terminates the query.
//   - Login flow: ReqUserLogin → OnRspUserLogin → ReqSettlementInfoConfirm
//     → OnRspSettlementInfoConfirm → ready to trade.
//
// Not on critical path: CTP itself has ~100µs round-trip latency internally.
// We keep the IGateway interface as thin as possible to allow future swap to
// a lower-latency path (e.g. direct NIC bypass) without changing strategy code.

class CTPGateway : public IGateway, private CThostFtdcTraderSpi {
public:
    CTPGateway() = default;
    ~CTPGateway() override { disconnect(); }

    // Non-copyable
    CTPGateway(const CTPGateway&)            = delete;
    CTPGateway& operator=(const CTPGateway&) = delete;

    // ── IGateway ─────────────────────────────────────────────────────────────
    bool connect(const GatewayConfig& cfg) override;
    void disconnect() override;
    [[nodiscard]] bool is_connected() const noexcept override {
        return trading_ready_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] bool send_order(
            const Order& order,
            GatewayOrderRecoveryHandle* recovery = nullptr) noexcept override;
    [[nodiscard]] bool send_quote(
            const Quote& quote,
            GatewayQuoteRecoveryHandle* recovery = nullptr) noexcept override;
    [[nodiscard]] bool cancel_order(OrderId id,
                                     uint16_t instrument_id) noexcept override;
    [[nodiscard]] bool cancel_quote(QuoteId id,
                                    uint16_t instrument_id) noexcept override;
    [[nodiscard]] bool get_quote_recovery_handle(
            QuoteId id,
            GatewayQuoteRecoveryHandle* out) const noexcept override;

    bool query_instruments(Instrument* out, uint16_t* count,
                            uint16_t max_count) override;

private:
    // ── CTP API objects ───────────────────────────────────────────────────────
    CThostFtdcTraderApi* api_{nullptr};
    GatewayConfig        cfg_{};

    // ── State ─────────────────────────────────────────────────────────────────
    std::atomic<bool> trading_ready_{false};
    std::atomic<int>  request_id_{1};
    std::atomic<int>  order_ref_seq_{1};

    // ── Blocking query helpers ────────────────────────────────────────────────
    std::mutex              qry_mutex_;
    std::condition_variable qry_cv_;
    Instrument*             qry_out_{nullptr};
    uint16_t*               qry_count_{nullptr};
    uint16_t                qry_max_{0};
    bool                    qry_done_{false};

    struct SyntheticQuoteState {
        bool used{false};
        bool cancel_pending{false};
        bool reject_pending{false};
        Quote quote{};
        OrderId bid_order_id{0};
        OrderId ask_order_id{0};
        bool bid_active{false};
        bool ask_active{false};
    };
    mutable std::mutex quote_state_mutex_;
    std::array<SyntheticQuoteState, MAX_INSTRUMENTS> synthetic_quotes_{};

    // ── OrderRef ↔ client_order_id mapping ───────────────────────────────────
    // CTP uses a string OrderRef. We encode our uint64 into 12 decimal digits.
    static void encode_order_ref(char* buf, OrderId id) noexcept {
        // buf must be at least 13 bytes (CTP OrderRef is char[13])
        std::snprintf(buf, 13, "%012llu", static_cast<unsigned long long>(id & 0xFFFFFFFFFFFFULL));
    }
    static OrderId decode_order_ref(const char* buf) noexcept {
        return static_cast<OrderId>(std::strtoull(buf, nullptr, 10));
    }

    int next_req_id() noexcept {
        return request_id_.fetch_add(1, std::memory_order_relaxed);
    }

    // ── CThostFtdcTraderSpi callbacks ─────────────────────────────────────────
    void OnFrontConnected() override;
    void OnFrontDisconnected(int nReason) override;
    void OnRspUserLogin(CThostFtdcRspUserLoginField* pRspUserLogin,
                        CThostFtdcRspInfoField* pRspInfo,
                        int nRequestID, bool bIsLast) override;
    void OnRspSettlementInfoConfirm(
                        CThostFtdcSettlementInfoConfirmField* pSettlementInfoConfirm,
                        CThostFtdcRspInfoField* pRspInfo,
                        int nRequestID, bool bIsLast) override;
    void OnRspOrderInsert(CThostFtdcInputOrderField* pInputOrder,
                          CThostFtdcRspInfoField* pRspInfo,
                          int nRequestID, bool bIsLast) override;
    void OnRtnOrder(CThostFtdcOrderField* pOrder) override;
    void OnRtnTrade(CThostFtdcTradeField* pTrade) override;
    void OnErrRtnOrderInsert(CThostFtdcInputOrderField* pInputOrder,
                              CThostFtdcRspInfoField* pRspInfo) override;
    void OnRspOrderAction(CThostFtdcInputOrderActionField* pInputOrderAction,
                          CThostFtdcRspInfoField* pRspInfo,
                          int nRequestID, bool bIsLast) override;
    void OnRspQryInstrument(CThostFtdcInstrumentField* pInstrument,
                             CThostFtdcRspInfoField* pRspInfo,
                             int nRequestID, bool bIsLast) override;

    // Helper: populate our Instrument from CTP struct
    static void fill_instrument(Instrument& out,
                                  const CThostFtdcInstrumentField& src,
                                  uint16_t id) noexcept;
};

} // namespace omm
