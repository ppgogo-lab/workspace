#pragma once

#include "gateway/gateway.h"
#include "common/thread_utils.h"
#include "femas/api_wrapper.h"

#include "USTPFtdcTraderApi.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>

namespace omm {

class FEMASGateway : public IGateway, private CUstpFtdcTraderSpi {
public:
    FEMASGateway() = default;
    ~FEMASGateway() override { disconnect(); }

    FEMASGateway(const FEMASGateway&) = delete;
    FEMASGateway& operator=(const FEMASGateway&) = delete;

    bool connect(const GatewayConfig& cfg) override;
    void disconnect() override;
    [[nodiscard]] bool is_connected() const noexcept override {
        return trading_ready_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] bool send_order(const Order& order) noexcept override;
    [[nodiscard]] bool send_quote(const Quote& quote) noexcept override;
    [[nodiscard]] bool cancel_order(OrderId id, uint16_t instrument_id) noexcept override;
    [[nodiscard]] bool cancel_quote(QuoteId id, uint16_t instrument_id) noexcept override;
    [[nodiscard]] bool supports_quote_replace() const noexcept override { return true; }

    bool query_instruments(Instrument* out, uint16_t* count, uint16_t max_count) override;

private:
    struct OrderState {
        bool       used{false};
        bool       acked{false};
        bool       is_quote_leg{false};
        OrderId    client_order_id{0};
        QuoteId    client_quote_id{0};
        uint16_t   instrument_id{INVALID_INSTRUMENT_ID};
        uint8_t    product_index{0xFF};
        Side       side{Side::Buy};
        OffsetFlag offset{OffsetFlag::Open};
        double     price{0.0};
        Volume     volume{0};
        char       exchange_local_id[16]{};
        char       order_sys_id[32]{};
    };

    struct QuoteState {
        bool    used{false};
        bool    acked{false};
        Quote   quote{};
        char    quote_local_id[16]{};
        char    quote_sys_id[32]{};
        char    bid_local_id[16]{};
        char    ask_local_id[16]{};
        char    bid_order_sys_id[32]{};
        char    ask_order_sys_id[32]{};
    };

    IFemasTraderApi* api_{nullptr};
    GatewayConfig cfg_{};

    std::atomic<bool> trading_ready_{false};
    std::atomic<int> request_id_{1};
    std::atomic<uint64_t> local_id_seq_{1};

    std::mutex qry_mutex_;
    std::condition_variable qry_cv_;
    Instrument* qry_out_{nullptr};
    uint16_t* qry_count_{nullptr};
    uint16_t qry_max_{0};
    bool qry_done_{false};

    std::mutex state_mutex_;
    std::array<OrderState, MAX_OPEN_ORDERS> order_states_{};
    std::array<QuoteState, MAX_OPEN_ORDERS / 2> quote_states_{};

    static void encode_local_id(char* buf, uint64_t id) noexcept {
        std::snprintf(buf, 13, "%012llu", static_cast<unsigned long long>(id & 0xFFFFFFFFFFFFULL));
    }
    static uint64_t decode_local_id(const char* buf) noexcept {
        return static_cast<uint64_t>(std::strtoull(buf, nullptr, 10));
    }

    int next_req_id() noexcept {
        return request_id_.fetch_add(1, std::memory_order_relaxed);
    }
    uint64_t next_local_id() noexcept {
        return local_id_seq_.fetch_add(1, std::memory_order_relaxed);
    }
    [[nodiscard]] const char* exchange_id() const noexcept {
        return "CFFEX";
    }

    static OffsetFlag decode_offset(char femas_offset) noexcept;
    static char encode_offset(OffsetFlag offset) noexcept;
    static OrderStatus decode_order_status(char femas_status) noexcept;
    static uint64_t parse_numeric_id(const char* text) noexcept;

    OrderState* alloc_order_state() noexcept;
    QuoteState* alloc_quote_state() noexcept;
    OrderState* find_order_by_local_id(const char* local_id) noexcept;
    OrderState* find_order_by_sys_id(const char* order_sys_id) noexcept;
    QuoteState* find_quote_by_client_id(QuoteId quote_id) noexcept;
    QuoteState* find_quote_by_local_id(const char* local_id) noexcept;
    QuoteState* find_quote_by_sys_id(const char* quote_sys_id) noexcept;
    void clear_order_state(const char* local_id) noexcept;
    void clear_quote_state(QuoteId quote_id) noexcept;
    void push_order_event(GatewayEventType type, const OrderState& state,
                          OrderStatus status = OrderStatus::New,
                          Volume filled_volume = 0) noexcept;
    void push_trade_event(GatewayEventType type, const OrderState& state,
                          const CUstpFtdcTradeField& trade) noexcept;

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
    void OnRspQuoteInsert(CUstpFtdcInputQuoteField* pInputQuote,
                          CUstpFtdcRspInfoField* pRspInfo,
                          int nRequestID, bool bIsLast) override;
    void OnRtnQuote(CUstpFtdcRtnQuoteField* pQuote) override;
    void OnErrRtnQuoteInsert(CUstpFtdcInputQuoteField* pInputQuote,
                             CUstpFtdcRspInfoField* pRspInfo) override;
    void OnRspQuoteAction(CUstpFtdcQuoteActionField* pQuoteAction,
                          CUstpFtdcRspInfoField* pRspInfo,
                          int nRequestID, bool bIsLast) override;
    void OnErrRtnQuoteAction(CUstpFtdcQuoteActionField* pQuoteAction,
                             CUstpFtdcRspInfoField* pRspInfo) override;
    void OnRspQryInstrument(CUstpFtdcRspInstrumentField* pInstrument,
                            CUstpFtdcRspInfoField* pRspInfo,
                            int nRequestID, bool bIsLast) override;

    static void fill_instrument(Instrument& out,
                                const CUstpFtdcRspInstrumentField& src,
                                uint16_t id) noexcept;
};

} // namespace omm
