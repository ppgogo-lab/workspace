#pragma once

#include "common/fixed_hash_table.h"
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
#include <shared_mutex>

namespace omm {

class FEMASGateway : public IGateway, private CUstpFtdcTraderSpi {
public:
    /**
     * @brief FEMASGateway.
     * @return None.
     */
    FEMASGateway() = default;
    /**
     * @brief FEMASGateway.
     * @return None.
     */
    ~FEMASGateway() override { disconnect(); }

    /**
     * @brief FEMASGateway.
     * @param FEMASGateway Parameter supplied by the caller.
     * @return None.
     */
    FEMASGateway(const FEMASGateway&) = delete;
    FEMASGateway& operator=(const FEMASGateway&) = delete;

    /**
     * @brief Connect.
     * @param cfg Parameter supplied by the caller.
     * @return Return value produced by the operation.
     */
    bool connect(const GatewayConfig& cfg) override;
    /**
     * @brief Disconnect.
     * @return None.
     */
    void disconnect() override;
    /**
     * @brief Is connected.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] bool is_connected() const noexcept override {
        return trading_ready_.load(std::memory_order_relaxed);
    }

    /**
     * @brief Send order.
     * @param order Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] bool send_order(const Order& order) noexcept override;
    /**
     * @brief Send quote.
     * @param quote Parameter supplied by the caller.
     * @param bid_order_id_out Parameter supplied by the caller.
     * @param ask_order_id_out Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] bool send_quote(
            const Quote& quote,
            OrderId* bid_order_id_out = nullptr,
            OrderId* ask_order_id_out = nullptr) noexcept override;
    /**
     * @brief Cancel order.
     * @param id Parameter supplied by the caller.
     * @param instrument_id Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] bool cancel_order(OrderId id, uint16_t instrument_id) noexcept override;
    /**
     * @brief Cancel quote.
     * @param id Parameter supplied by the caller.
     * @param instrument_id Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] bool cancel_quote(QuoteId id, uint16_t instrument_id) noexcept override;
    /**
     * @brief Supports quote replace.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] bool supports_quote_replace() const noexcept override { return true; }

    /**
     * @brief Query instruments.
     * @param out Parameter supplied by the caller.
     * @param count Parameter supplied by the caller.
     * @param max_count Parameter supplied by the caller.
     * @return Return value produced by the operation.
     */
    bool query_instruments(Instrument* out, uint16_t* count, uint16_t max_count) override;

private:
    struct OrderState {
        std::atomic<bool> used{false};  // Atomic for lock-free allocation
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
        std::atomic<bool> used{false};  // Atomic for lock-free allocation
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
    // Lock-free slot allocation: round-robin starting point for finding free slots
    std::atomic<uint32_t> next_order_slot_hint_{0};
    std::atomic<uint32_t> next_quote_slot_hint_{0};

    std::mutex qry_mutex_;
    std::condition_variable qry_cv_;
    Instrument* qry_out_{nullptr};
    uint16_t* qry_count_{nullptr};
    uint16_t qry_max_{0};
    bool qry_done_{false};

    // Read-write lock for state tables: allows concurrent reads (lookups) but exclusive writes (indexing/updates)
    // This eliminates reader-reader contention in callback path while maintaining safety
    mutable std::shared_mutex state_rw_lock_;
    std::array<OrderState, MAX_OPEN_ORDERS> order_states_{};
    std::array<QuoteState, MAX_OPEN_ORDERS / 2> quote_states_{};
    FixedHashTable<OrderId, std::size_t, MAX_OPEN_ORDERS * 2> order_client_index_{};
    FixedStringHashTable<16, std::size_t, MAX_OPEN_ORDERS * 2> order_local_index_{};
    FixedStringHashTable<32, std::size_t, MAX_OPEN_ORDERS * 2> order_sys_index_{};
    FixedHashTable<QuoteId, std::size_t, MAX_OPEN_ORDERS> quote_client_index_{};
    FixedStringHashTable<16, std::size_t, MAX_OPEN_ORDERS> quote_local_index_{};
    FixedStringHashTable<32, std::size_t, MAX_OPEN_ORDERS> quote_sys_index_{};

    /**
     * @brief Encode local id.
     * @param buf Parameter supplied by the caller.
     * @param id Parameter supplied by the caller.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    static void encode_local_id(char* buf, uint64_t id) noexcept {
        std::snprintf(buf, 13, "%012llu", static_cast<unsigned long long>(id & 0xFFFFFFFFFFFFULL));
    }
    /**
     * @brief Decode local id.
     * @param buf Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    static uint64_t decode_local_id(const char* buf) noexcept {
        return static_cast<uint64_t>(std::strtoull(buf, nullptr, 10));
    }

    /**
     * @brief Next req id.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    int next_req_id() noexcept {
        return request_id_.fetch_add(1, std::memory_order_relaxed);
    }
    /**
     * @brief Next local id.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    uint64_t next_local_id() noexcept {
        return local_id_seq_.fetch_add(1, std::memory_order_relaxed);
    }
    /**
     * @brief Exchange id.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] const char* exchange_id() const noexcept {
        return "CFFEX";
    }

    /**
     * @brief Decode offset.
     * @param femas_offset Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    static OffsetFlag decode_offset(char femas_offset) noexcept;
    /**
     * @brief Encode offset.
     * @param offset Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    static char encode_offset(OffsetFlag offset) noexcept;
    /**
     * @brief Decode order status.
     * @param femas_status Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    static OrderStatus decode_order_status(char femas_status) noexcept;
    /**
     * @brief Parse numeric id.
     * @param text Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    static uint64_t parse_numeric_id(const char* text) noexcept;

    /**
     * @brief Alloc order state.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    OrderState* alloc_order_state() noexcept;
    /**
     * @brief Alloc order state lockfree.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    OrderState* alloc_order_state_lockfree() noexcept;  // Lock-free version for send path
    /**
     * @brief Alloc quote state.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    QuoteState* alloc_quote_state() noexcept;
    /**
     * @brief Alloc quote state lockfree.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    QuoteState* alloc_quote_state_lockfree() noexcept;  // Lock-free version for send path
    /**
     * @brief Order index.
     * @param state Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] std::size_t order_index(const OrderState* state) const noexcept;
    /**
     * @brief Quote index.
     * @param state Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] std::size_t quote_index(const QuoteState* state) const noexcept;
    /**
     * @brief Index order state.
     * @param state Parameter supplied by the caller.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    void index_order_state(OrderState* state) noexcept;
    /**
     * @brief Index quote state.
     * @param state Parameter supplied by the caller.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    void index_quote_state(QuoteState* state) noexcept;
    /**
     * @brief Unindex order state.
     * @param state Parameter supplied by the caller.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    void unindex_order_state(OrderState* state) noexcept;
    /**
     * @brief Unindex quote state.
     * @param state Parameter supplied by the caller.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    void unindex_quote_state(QuoteState* state) noexcept;
    /**
     * @brief Find order by local id.
     * @param local_id Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    OrderState* find_order_by_local_id(const char* local_id) noexcept;
    /**
     * @brief Find order by sys id.
     * @param order_sys_id Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    OrderState* find_order_by_sys_id(const char* order_sys_id) noexcept;
    /**
     * @brief Find quote by client id.
     * @param quote_id Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    QuoteState* find_quote_by_client_id(QuoteId quote_id) noexcept;
    /**
     * @brief Find quote by local id.
     * @param local_id Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    QuoteState* find_quote_by_local_id(const char* local_id) noexcept;
    /**
     * @brief Find quote by sys id.
     * @param quote_sys_id Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    QuoteState* find_quote_by_sys_id(const char* quote_sys_id) noexcept;
    /**
     * @brief Clear order state.
     * @param local_id Parameter supplied by the caller.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    void clear_order_state(const char* local_id) noexcept;
    /**
     * @brief Clear quote state.
     * @param quote_id Parameter supplied by the caller.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    void clear_quote_state(QuoteId quote_id) noexcept;
    /**
     * @brief Push order event.
     * @param type Parameter supplied by the caller.
     * @param state Parameter supplied by the caller.
     * @param status Parameter supplied by the caller.
     * @param filled_volume Parameter supplied by the caller.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    void push_order_event(GatewayEventType type, const OrderState& state,
                          OrderStatus status = OrderStatus::New,
                          Volume filled_volume = 0) noexcept;
    /**
     * @brief Push trade event.
     * @param type Parameter supplied by the caller.
     * @param state Parameter supplied by the caller.
     * @param trade Parameter supplied by the caller.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    void push_trade_event(GatewayEventType type, const OrderState& state,
                          const CUstpFtdcTradeField& trade) noexcept;

    /**
     * @brief OnFrontConnected.
     * @return None.
     */
    void OnFrontConnected() override;
    /**
     * @brief OnFrontDisconnected.
     * @param nReason Parameter supplied by the caller.
     * @return None.
     */
    void OnFrontDisconnected(int nReason) override;
    /**
     * @brief OnRspUserLogin.
     * @param pRspUserLogin Parameter supplied by the caller.
     * @param pRspInfo Parameter supplied by the caller.
     * @param nRequestID Parameter supplied by the caller.
     * @param bIsLast Parameter supplied by the caller.
     * @return None.
     */
    void OnRspUserLogin(CUstpFtdcRspUserLoginField* pRspUserLogin,
                        CUstpFtdcRspInfoField* pRspInfo,
                        int nRequestID, bool bIsLast) override;
    /**
     * @brief OnRspOrderInsert.
     * @param pInputOrder Parameter supplied by the caller.
     * @param pRspInfo Parameter supplied by the caller.
     * @param nRequestID Parameter supplied by the caller.
     * @param bIsLast Parameter supplied by the caller.
     * @return None.
     */
    void OnRspOrderInsert(CUstpFtdcInputOrderField* pInputOrder,
                          CUstpFtdcRspInfoField* pRspInfo,
                          int nRequestID, bool bIsLast) override;
    /**
     * @brief OnRtnOrder.
     * @param pOrder Parameter supplied by the caller.
     * @return None.
     */
    void OnRtnOrder(CUstpFtdcOrderField* pOrder) override;
    /**
     * @brief OnRtnTrade.
     * @param pTrade Parameter supplied by the caller.
     * @return None.
     */
    void OnRtnTrade(CUstpFtdcTradeField* pTrade) override;
    /**
     * @brief OnErrRtnOrderInsert.
     * @param pInputOrder Parameter supplied by the caller.
     * @param pRspInfo Parameter supplied by the caller.
     * @return None.
     */
    void OnErrRtnOrderInsert(CUstpFtdcInputOrderField* pInputOrder,
                             CUstpFtdcRspInfoField* pRspInfo) override;
    /**
     * @brief OnRspOrderAction.
     * @param pOrderAction Parameter supplied by the caller.
     * @param pRspInfo Parameter supplied by the caller.
     * @param nRequestID Parameter supplied by the caller.
     * @param bIsLast Parameter supplied by the caller.
     * @return None.
     */
    void OnRspOrderAction(CUstpFtdcOrderActionField* pOrderAction,
                          CUstpFtdcRspInfoField* pRspInfo,
                          int nRequestID, bool bIsLast) override;
    /**
     * @brief OnRspQuoteInsert.
     * @param pInputQuote Parameter supplied by the caller.
     * @param pRspInfo Parameter supplied by the caller.
     * @param nRequestID Parameter supplied by the caller.
     * @param bIsLast Parameter supplied by the caller.
     * @return None.
     */
    void OnRspQuoteInsert(CUstpFtdcInputQuoteField* pInputQuote,
                          CUstpFtdcRspInfoField* pRspInfo,
                          int nRequestID, bool bIsLast) override;
    /**
     * @brief OnRtnQuote.
     * @param pQuote Parameter supplied by the caller.
     * @return None.
     */
    void OnRtnQuote(CUstpFtdcRtnQuoteField* pQuote) override;
    /**
     * @brief OnErrRtnQuoteInsert.
     * @param pInputQuote Parameter supplied by the caller.
     * @param pRspInfo Parameter supplied by the caller.
     * @return None.
     */
    void OnErrRtnQuoteInsert(CUstpFtdcInputQuoteField* pInputQuote,
                             CUstpFtdcRspInfoField* pRspInfo) override;
    /**
     * @brief OnRspQuoteAction.
     * @param pQuoteAction Parameter supplied by the caller.
     * @param pRspInfo Parameter supplied by the caller.
     * @param nRequestID Parameter supplied by the caller.
     * @param bIsLast Parameter supplied by the caller.
     * @return None.
     */
    void OnRspQuoteAction(CUstpFtdcQuoteActionField* pQuoteAction,
                          CUstpFtdcRspInfoField* pRspInfo,
                          int nRequestID, bool bIsLast) override;
    /**
     * @brief OnErrRtnQuoteAction.
     * @param pQuoteAction Parameter supplied by the caller.
     * @param pRspInfo Parameter supplied by the caller.
     * @return None.
     */
    void OnErrRtnQuoteAction(CUstpFtdcQuoteActionField* pQuoteAction,
                             CUstpFtdcRspInfoField* pRspInfo) override;
    /**
     * @brief OnRspQryInstrument.
     * @param pInstrument Parameter supplied by the caller.
     * @param pRspInfo Parameter supplied by the caller.
     * @param nRequestID Parameter supplied by the caller.
     * @param bIsLast Parameter supplied by the caller.
     * @return None.
     */
    void OnRspQryInstrument(CUstpFtdcRspInstrumentField* pInstrument,
                            CUstpFtdcRspInfoField* pRspInfo,
                            int nRequestID, bool bIsLast) override;

    /**
     * @brief Fill instrument.
     * @param out Parameter supplied by the caller.
     * @param src Parameter supplied by the caller.
     * @param id Parameter supplied by the caller.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    static void fill_instrument(Instrument& out,
                                const CUstpFtdcRspInstrumentField& src,
                                uint16_t id) noexcept;
};

} // namespace omm
