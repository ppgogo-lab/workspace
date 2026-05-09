#include "gateway/femas_gateway.h"
#include "logger/logger.h"

#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <ctime>

namespace omm {

namespace {

constexpr char kMarketMakerHedge = USTP_FTDC_CHF_MarketMaker;
constexpr char kSpecHedge = USTP_FTDC_CHF_Speculation;

Exchange parse_exchange_id(const char* exchange_id) noexcept {
    if (!exchange_id) return Exchange::Unknown;
    if (std::strncmp(exchange_id, "SHFE", 4) == 0) return Exchange::SHFE;
    if (std::strncmp(exchange_id, "DCE", 3) == 0) return Exchange::DCE;
    if (std::strncmp(exchange_id, "CZCE", 4) == 0) return Exchange::CZCE;
    if (std::strncmp(exchange_id, "CFFEX", 5) == 0) return Exchange::CFFEX;
    if (std::strncmp(exchange_id, "GFEX", 4) == 0) return Exchange::GFEX;
    return Exchange::Unknown;
}

int64_t estimate_expiry_epoch_ns(const char* expire_date) noexcept {
    if (!expire_date || std::strlen(expire_date) != 8) return 0;

    int year = 0;
    int month = 0;
    int day = 0;
    if (std::sscanf(expire_date, "%4d%2d%2d", &year, &month, &day) != 3) return 0;

    std::tm expiry_tm{};
    expiry_tm.tm_year = year - 1900;
    expiry_tm.tm_mon = month - 1;
    expiry_tm.tm_mday = day;
    expiry_tm.tm_hour = 15;

    const std::time_t expiry_wall = std::mktime(&expiry_tm);
    if (expiry_wall == static_cast<std::time_t>(-1)) return 0;

    const auto now_wall = std::chrono::system_clock::to_time_t(
        std::chrono::system_clock::now());
    const int64_t delta_sec = std::max<int64_t>(0, expiry_wall - now_wall);
    return get_monotonic_ns() + delta_sec * 1'000'000'000LL;
}

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
        if (!state.used.load(std::memory_order_relaxed)) {
            // Reset fields manually (can't use assignment with atomic)
            state.used.store(true, std::memory_order_relaxed);
            state.acked = false;
            state.is_quote_leg = false;
            state.client_order_id = 0;
            state.client_quote_id = 0;
            state.instrument_id = INVALID_INSTRUMENT_ID;
            state.product_index = 0xFF;
            state.side = Side::Buy;
            state.offset = OffsetFlag::Open;
            state.price = 0.0;
            state.volume = 0;
            std::memset(state.exchange_local_id, 0, sizeof(state.exchange_local_id));
            std::memset(state.order_sys_id, 0, sizeof(state.order_sys_id));
            return &state;
        }
    }
    return nullptr;
}

// Lock-free allocation for send path: uses atomic CAS to claim slots without mutex.
// This eliminates contention between send path (dispatcher thread) and callback path
// (gateway internal thread). The hint provides a starting point to reduce collisions.
FEMASGateway::OrderState* FEMASGateway::alloc_order_state_lockfree() noexcept {
    const uint32_t start_hint = next_order_slot_hint_.fetch_add(1, std::memory_order_relaxed);
    // Try MAX_OPEN_ORDERS slots starting from hint (wraps around)
    for (uint32_t attempt = 0; attempt < MAX_OPEN_ORDERS; ++attempt) {
        const uint32_t slot = (start_hint + attempt) % MAX_OPEN_ORDERS;
        OrderState* state = &order_states_[slot];
        bool expected = false;
        // Atomic CAS: only succeeds if slot is free, no lock needed
        if (state->used.compare_exchange_strong(expected, true,
                                                std::memory_order_acquire,
                                                std::memory_order_relaxed)) {
            // Reset state fields (keep 'used' as true)
            state->acked = false;
            state->is_quote_leg = false;
            state->client_order_id = 0;
            state->client_quote_id = 0;
            state->instrument_id = INVALID_INSTRUMENT_ID;
            state->product_index = 0xFF;
            state->side = Side::Buy;
            state->offset = OffsetFlag::Open;
            state->price = 0.0;
            state->volume = 0;
            std::memset(state->exchange_local_id, 0, sizeof(state->exchange_local_id));
            std::memset(state->order_sys_id, 0, sizeof(state->order_sys_id));
            return state;
        }
    }
    return nullptr;  // All slots full
}

FEMASGateway::QuoteState* FEMASGateway::alloc_quote_state() noexcept {
    for (auto& state : quote_states_) {
        if (!state.used.load(std::memory_order_relaxed)) {
            // Reset fields manually (can't use assignment with atomic)
            state.used.store(true, std::memory_order_relaxed);
            state.acked = false;
            state.quote = Quote{};
            std::memset(state.quote_local_id, 0, sizeof(state.quote_local_id));
            std::memset(state.quote_sys_id, 0, sizeof(state.quote_sys_id));
            std::memset(state.bid_local_id, 0, sizeof(state.bid_local_id));
            std::memset(state.ask_local_id, 0, sizeof(state.ask_local_id));
            std::memset(state.bid_order_sys_id, 0, sizeof(state.bid_order_sys_id));
            std::memset(state.ask_order_sys_id, 0, sizeof(state.ask_order_sys_id));
            return &state;
        }
    }
    return nullptr;
}

// Lock-free allocation for send path (quotes)
FEMASGateway::QuoteState* FEMASGateway::alloc_quote_state_lockfree() noexcept {
    const uint32_t start_hint = next_quote_slot_hint_.fetch_add(1, std::memory_order_relaxed);
    const uint32_t max_slots = static_cast<uint32_t>(quote_states_.size());
    for (uint32_t attempt = 0; attempt < max_slots; ++attempt) {
        const uint32_t slot = (start_hint + attempt) % max_slots;
        QuoteState* state = &quote_states_[slot];
        bool expected = false;
        if (state->used.compare_exchange_strong(expected, true,
                                                std::memory_order_acquire,
                                                std::memory_order_relaxed)) {
            // Reset state fields (keep 'used' as true)
            state->acked = false;
            state->quote = Quote{};
            std::memset(state->quote_local_id, 0, sizeof(state->quote_local_id));
            std::memset(state->quote_sys_id, 0, sizeof(state->quote_sys_id));
            std::memset(state->bid_local_id, 0, sizeof(state->bid_local_id));
            std::memset(state->ask_local_id, 0, sizeof(state->ask_local_id));
            std::memset(state->bid_order_sys_id, 0, sizeof(state->bid_order_sys_id));
            std::memset(state->ask_order_sys_id, 0, sizeof(state->ask_order_sys_id));
            return state;
        }
    }
    return nullptr;
}

std::size_t FEMASGateway::order_index(const OrderState* state) const noexcept {
    return static_cast<std::size_t>(state - order_states_.data());
}

std::size_t FEMASGateway::quote_index(const QuoteState* state) const noexcept {
    return static_cast<std::size_t>(state - quote_states_.data());
}

void FEMASGateway::index_order_state(OrderState* state) noexcept {
    if (!state || !state->used) return;
    const std::size_t idx = order_index(state);
    if (!state->is_quote_leg && state->client_order_id != 0) {
        (void)order_client_index_.insert(state->client_order_id, idx);
    }
    if (state->exchange_local_id[0]) {
        (void)order_local_index_.insert(state->exchange_local_id, idx);
    }
    if (state->order_sys_id[0]) {
        (void)order_sys_index_.insert(state->order_sys_id, idx);
    }
}

void FEMASGateway::index_quote_state(QuoteState* state) noexcept {
    if (!state || !state->used) return;
    const std::size_t idx = quote_index(state);
    if (state->quote.client_quote_id != 0) {
        (void)quote_client_index_.insert(state->quote.client_quote_id, idx);
    }
    if (state->quote_local_id[0]) {
        (void)quote_local_index_.insert(state->quote_local_id, idx);
    }
    if (state->quote_sys_id[0]) {
        (void)quote_sys_index_.insert(state->quote_sys_id, idx);
    }
}

void FEMASGateway::unindex_order_state(OrderState* state) noexcept {
    if (!state) return;
    if (!state->is_quote_leg && state->client_order_id != 0) {
        order_client_index_.erase(state->client_order_id);
    }
    if (state->exchange_local_id[0]) {
        order_local_index_.erase(state->exchange_local_id);
    }
    if (state->order_sys_id[0]) {
        order_sys_index_.erase(state->order_sys_id);
    }
}

void FEMASGateway::unindex_quote_state(QuoteState* state) noexcept {
    if (!state) return;
    if (state->quote.client_quote_id != 0) {
        quote_client_index_.erase(state->quote.client_quote_id);
    }
    if (state->quote_local_id[0]) {
        quote_local_index_.erase(state->quote_local_id);
    }
    if (state->quote_sys_id[0]) {
        quote_sys_index_.erase(state->quote_sys_id);
    }
}

FEMASGateway::OrderState* FEMASGateway::find_order_by_local_id(const char* local_id) noexcept {
    if (!local_id || !local_id[0]) return nullptr;
    const std::size_t* idx = order_local_index_.find(local_id);
    if (idx != nullptr && *idx < order_states_.size()) return &order_states_[*idx];
    return nullptr;
}

FEMASGateway::OrderState* FEMASGateway::find_order_by_sys_id(const char* order_sys_id) noexcept {
    if (!order_sys_id || !order_sys_id[0]) return nullptr;
    const std::size_t* idx = order_sys_index_.find(order_sys_id);
    if (idx != nullptr && *idx < order_states_.size()) return &order_states_[*idx];
    return nullptr;
}

FEMASGateway::QuoteState* FEMASGateway::find_quote_by_client_id(QuoteId quote_id) noexcept {
    const std::size_t* idx = quote_client_index_.find(quote_id);
    if (idx != nullptr && *idx < quote_states_.size()) return &quote_states_[*idx];
    return nullptr;
}

FEMASGateway::QuoteState* FEMASGateway::find_quote_by_local_id(const char* local_id) noexcept {
    if (!local_id || !local_id[0]) return nullptr;
    const std::size_t* idx = quote_local_index_.find(local_id);
    if (idx != nullptr && *idx < quote_states_.size()) return &quote_states_[*idx];
    return nullptr;
}

FEMASGateway::QuoteState* FEMASGateway::find_quote_by_sys_id(const char* quote_sys_id) noexcept {
    if (!quote_sys_id || !quote_sys_id[0]) return nullptr;
    const std::size_t* idx = quote_sys_index_.find(quote_sys_id);
    if (idx != nullptr && *idx < quote_states_.size()) return &quote_states_[*idx];
    return nullptr;
}

void FEMASGateway::clear_order_state(const char* local_id) noexcept {
    if (OrderState* state = find_order_by_local_id(local_id)) {
        unindex_order_state(state);
        // Reset fields manually (can't use assignment with atomic)
        state->used.store(false, std::memory_order_relaxed);
        state->acked = false;
        state->is_quote_leg = false;
        state->client_order_id = 0;
        state->client_quote_id = 0;
        state->instrument_id = INVALID_INSTRUMENT_ID;
        state->product_index = 0xFF;
        std::memset(state->exchange_local_id, 0, sizeof(state->exchange_local_id));
        std::memset(state->order_sys_id, 0, sizeof(state->order_sys_id));
    }
}

void FEMASGateway::clear_quote_state(QuoteId quote_id) noexcept {
    QuoteState* quote = find_quote_by_client_id(quote_id);
    if (!quote) return;
    clear_order_state(quote->bid_local_id);
    clear_order_state(quote->ask_local_id);
    unindex_quote_state(quote);
    // Reset fields manually (can't use assignment with atomic)
    quote->used.store(false, std::memory_order_relaxed);
    quote->acked = false;
    quote->quote = Quote{};
    std::memset(quote->quote_local_id, 0, sizeof(quote->quote_local_id));
    std::memset(quote->quote_sys_id, 0, sizeof(quote->quote_sys_id));
    std::memset(quote->bid_local_id, 0, sizeof(quote->bid_local_id));
    std::memset(quote->ask_local_id, 0, sizeof(quote->ask_local_id));
    std::memset(quote->bid_order_sys_id, 0, sizeof(quote->bid_order_sys_id));
    std::memset(quote->ask_order_sys_id, 0, sizeof(quote->ask_order_sys_id));
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

    api_ = create_femas_trader_api(cfg_.femas.front_addr);
    if (!api_) {
        OMM_LOG_ERROR("femas", "CreateFtdcTraderApi failed front={}", cfg_.femas.front_addr);
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
        std::unique_lock<std::shared_mutex> lk(state_rw_lock_);
        // Reset all states manually (can't use assignment with atomic)
        for (auto& state : order_states_) {
            state.used.store(false, std::memory_order_relaxed);
            state.acked = false;
            state.is_quote_leg = false;
            state.client_order_id = 0;
            state.client_quote_id = 0;
            state.instrument_id = INVALID_INSTRUMENT_ID;
            state.product_index = 0xFF;
            std::memset(state.exchange_local_id, 0, sizeof(state.exchange_local_id));
            std::memset(state.order_sys_id, 0, sizeof(state.order_sys_id));
        }
        for (auto& state : quote_states_) {
            state.used.store(false, std::memory_order_relaxed);
            state.acked = false;
            state.quote = Quote{};
            std::memset(state.quote_local_id, 0, sizeof(state.quote_local_id));
            std::memset(state.quote_sys_id, 0, sizeof(state.quote_sys_id));
            std::memset(state.bid_local_id, 0, sizeof(state.bid_local_id));
            std::memset(state.ask_local_id, 0, sizeof(state.ask_local_id));
            std::memset(state.bid_order_sys_id, 0, sizeof(state.bid_order_sys_id));
            std::memset(state.ask_order_sys_id, 0, sizeof(state.ask_order_sys_id));
        }
        order_client_index_.clear();
        order_local_index_.clear();
        order_sys_index_.clear();
        quote_client_index_.clear();
        quote_local_index_.clear();
        quote_sys_index_.clear();
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

    // Lock-free state allocation: no mutex needed, uses atomic CAS
    // This eliminates contention with callback thread that reads state
    OrderState* state = alloc_order_state_lockfree();
    if (!state) {
        OMM_LOG_WARN("femas", "order state table full");
        return false;
    }

    // Populate state fields (no lock needed - we own this slot exclusively)
    state->client_order_id = order.client_order_id;
    state->instrument_id = order.instrument_id;
    state->product_index = order.product_index;
    state->side = order.side;
    state->offset = order.offset;
    state->price = order.price;
    state->volume = order.volume;
    std::strncpy(state->exchange_local_id, req.UserOrderLocalID, sizeof(state->exchange_local_id) - 1);

    // Index state for callback lookups (still needs lock, but much shorter critical section)
    {
        std::unique_lock<std::shared_mutex> lk(state_rw_lock_);
        index_order_state(state);
    }

    const int ret = api_->ReqOrderInsert(&req, next_req_id());
    if (ret != 0) {
        std::unique_lock<std::shared_mutex> lk(state_rw_lock_);
        clear_order_state(local_id);
        OMM_LOG_WARN("femas", "ReqOrderInsert failed ret={} order_id={}", ret, order.client_order_id);
        return false;
    }
    return true;
}

bool FEMASGateway::send_quote(const Quote& quote,
                              OrderId* bid_order_id_out,
                              OrderId* ask_order_id_out) noexcept {
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

    // Lock-free state allocation for quote and its two legs
    QuoteState* quote_state = alloc_quote_state_lockfree();
    OrderState* bid_state = alloc_order_state_lockfree();
    OrderState* ask_state = alloc_order_state_lockfree();
    if (!quote_state || !bid_state || !ask_state) {
        // Rollback: release any allocated slots
        if (quote_state) quote_state->used.store(false, std::memory_order_release);
        if (bid_state) bid_state->used.store(false, std::memory_order_release);
        if (ask_state) ask_state->used.store(false, std::memory_order_release);
        OMM_LOG_WARN("femas", "quote state table full");
        return false;
    }

    // Populate state fields (no lock needed - we own these slots exclusively)
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

    // Index states for callback lookups (still needs lock, but much shorter critical section)
    {
        std::unique_lock<std::shared_mutex> lk(state_rw_lock_);
        index_quote_state(quote_state);
        index_order_state(bid_state);
        index_order_state(ask_state);
    }

    const int ret = api_->ReqQuoteInsert(&req, next_req_id());
    if (ret != 0) {
        std::unique_lock<std::shared_mutex> lk(state_rw_lock_);
        clear_quote_state(quote.client_quote_id);
        OMM_LOG_WARN("femas", "ReqQuoteInsert failed ret={} quote_id={}", ret, quote.client_quote_id);
        return false;
    }

    // Return synthetic bid/ask order IDs for quote leg tracking
    if (bid_order_id_out != nullptr) {
        *bid_order_id_out = quote.client_quote_id;
    }
    if (ask_order_id_out != nullptr) {
        *ask_order_id_out = quote.client_quote_id | (1ULL << 47);
    }

    return true;
}

bool FEMASGateway::cancel_order(OrderId id, uint16_t instrument_id) noexcept {
    if (!api_ || !trading_ready_.load(std::memory_order_relaxed)) return false;

    CUstpFtdcQuoteActionField quote_req{};
    bool has_quote_cancel = false;
    {
        std::shared_lock<std::shared_mutex> lk(state_rw_lock_);
        if (QuoteState* quote = find_quote_by_client_id(id)) {
            std::strncpy(quote_req.BrokerID, cfg_.femas.broker_id, sizeof(quote_req.BrokerID) - 1);
            std::strncpy(quote_req.ExchangeID, exchange_id(), sizeof(quote_req.ExchangeID) - 1);
            std::strncpy(quote_req.InvestorID, cfg_.femas.user_id, sizeof(quote_req.InvestorID) - 1);
            std::strncpy(quote_req.UserID, cfg_.femas.user_id, sizeof(quote_req.UserID) - 1);
            std::strncpy(quote_req.UserQuoteLocalID,
                         quote->quote_local_id,
                         sizeof(quote_req.UserQuoteLocalID) - 1);
            if (quote->quote_sys_id[0]) {
                std::strncpy(quote_req.QuoteSysID,
                             quote->quote_sys_id,
                             sizeof(quote_req.QuoteSysID) - 1);
            }
            encode_local_id(quote_req.UserQuoteActionLocalID, next_local_id());
            quote_req.ActionFlag = USTP_FTDC_AF_Delete;
            has_quote_cancel = true;
        }
    }
    if (has_quote_cancel) {
        const int ret = api_->ReqQuoteAction(&quote_req, next_req_id());
        if (ret != 0) {
            OMM_LOG_WARN("femas", "ReqQuoteAction failed ret={} quote_id={}", ret, id);
            return false;
        }
        return true;
    }

    CUstpFtdcOrderActionField req{};
    bool has_order_cancel = false;
    {
        std::shared_lock<std::shared_mutex> lk(state_rw_lock_);
        const std::size_t* idx = order_client_index_.find(id);
        if (idx != nullptr && *idx < order_states_.size()) {
            const OrderState* order_state = &order_states_[*idx];
            if (order_state->used.load(std::memory_order_relaxed)
                && !order_state->is_quote_leg
                && order_state->client_order_id == id) {
                std::strncpy(req.BrokerID, cfg_.femas.broker_id, sizeof(req.BrokerID) - 1);
                std::strncpy(req.ExchangeID, exchange_id(), sizeof(req.ExchangeID) - 1);
                std::strncpy(req.InvestorID, cfg_.femas.user_id, sizeof(req.InvestorID) - 1);
                std::strncpy(req.UserID, cfg_.femas.user_id, sizeof(req.UserID) - 1);
                std::strncpy(req.UserOrderLocalID,
                             order_state->exchange_local_id,
                             sizeof(req.UserOrderLocalID) - 1);
                if (order_state->order_sys_id[0]) {
                    std::strncpy(req.OrderSysID,
                                 order_state->order_sys_id,
                                 sizeof(req.OrderSysID) - 1);
                }
                encode_local_id(req.UserOrderActionLocalID, next_local_id());
                req.ActionFlag = USTP_FTDC_AF_Delete;
                has_order_cancel = true;
            }
        }
    }
    if (!has_order_cancel) {
        OMM_LOG_WARN("femas", "cancel_order missing state order_id={} instrument_id={}", id, instrument_id);
        return false;
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

    std::unique_lock<std::shared_mutex> lk(state_rw_lock_);
    OrderState* state = find_order_by_local_id(pOrder->UserOrderLocalID);
    if (!state) return;

    if (pOrder->OrderSysID[0]) {
        if (state->order_sys_id[0]) order_sys_index_.erase(state->order_sys_id);
        std::strncpy(state->order_sys_id, pOrder->OrderSysID, sizeof(state->order_sys_id) - 1);
        (void)order_sys_index_.insert(state->order_sys_id, order_index(state));
    }

    if (pRspInfo && pRspInfo->ErrorID != 0) {
        OMM_LOG_WARN("femas", "order rejected ErrorID={} Msg={} order_id={}",
                     pRspInfo->ErrorID, pRspInfo->ErrorMsg, state->client_order_id);
        push_order_event(GatewayEventType::OrderReject, *state, OrderStatus::Rejected);
        unindex_order_state(state);
        // Reset fields manually (can't use assignment with atomic)
        state->used.store(false, std::memory_order_relaxed);
        state->acked = false;
        state->is_quote_leg = false;
        state->client_order_id = 0;
        state->client_quote_id = 0;
        state->instrument_id = INVALID_INSTRUMENT_ID;
        state->product_index = 0xFF;
        std::memset(state->exchange_local_id, 0, sizeof(state->exchange_local_id));
        std::memset(state->order_sys_id, 0, sizeof(state->order_sys_id));
    }
}

void FEMASGateway::OnRtnOrder(CUstpFtdcOrderField* pOrder) {
    if (!pOrder) return;

    // Copy data for deferred logging (reduces lock hold time)
    bool should_log_warn = false;
    char local_id_copy[32] = {};
    char sys_id_copy[32] = {};

    {
        std::unique_lock<std::shared_mutex> lk(state_rw_lock_);
        OrderState* state = find_order_by_local_id(pOrder->UserOrderLocalID);
        if (!state) state = find_order_by_sys_id(pOrder->OrderSysID);
        if (!state) {
            should_log_warn = true;
            std::strncpy(local_id_copy, pOrder->UserOrderLocalID, sizeof(local_id_copy) - 1);
            std::strncpy(sys_id_copy, pOrder->OrderSysID, sizeof(sys_id_copy) - 1);
            // Release lock before logging
        } else {
            if (pOrder->OrderSysID[0]) {
                if (state->order_sys_id[0]) order_sys_index_.erase(state->order_sys_id);
                std::strncpy(state->order_sys_id, pOrder->OrderSysID, sizeof(state->order_sys_id) - 1);
                (void)order_sys_index_.insert(state->order_sys_id, order_index(state));
            }

            const OrderStatus status = decode_order_status(pOrder->OrderStatus);
            if (!state->acked && status != OrderStatus::Cancelled) {
                state->acked = true;
                push_order_event(GatewayEventType::OrderAck, *state, status, pOrder->VolumeTraded);
            } else if (status == OrderStatus::Cancelled) {
                push_order_event(GatewayEventType::OrderCancel, *state, status, pOrder->VolumeTraded);
                if (!state->is_quote_leg) {
                    unindex_order_state(state);
                    // Reset fields manually (can't use assignment with atomic)
                    state->used.store(false, std::memory_order_relaxed);
                    state->acked = false;
                    state->is_quote_leg = false;
                    state->client_order_id = 0;
                    state->client_quote_id = 0;
                    state->instrument_id = INVALID_INSTRUMENT_ID;
                    state->product_index = 0xFF;
                    std::memset(state->exchange_local_id, 0, sizeof(state->exchange_local_id));
                    std::memset(state->order_sys_id, 0, sizeof(state->order_sys_id));
                }
            }
        }
    }  // Lock released here

    // Deferred logging (outside lock)
    if (should_log_warn) {
        OMM_LOG_WARN("femas", "unmatched OnRtnOrder local_id={} sys_id={}",
                     local_id_copy, sys_id_copy);
    }
}

void FEMASGateway::OnRtnTrade(CUstpFtdcTradeField* pTrade) {
    if (!pTrade) return;

    // Copy data for deferred logging (reduces lock hold time)
    bool should_log_warn = false;
    bool should_log_debug = false;
    char local_id_copy[32] = {};
    char sys_id_copy[32] = {};
    uint64_t order_id_copy = 0;
    bool is_quote_leg_copy = false;
    char direction_copy = 0;
    int volume_copy = 0;
    double price_copy = 0.0;

    {
        std::unique_lock<std::shared_mutex> lk(state_rw_lock_);
        OrderState* state = find_order_by_local_id(pTrade->UserOrderLocalID);
        if (!state) state = find_order_by_sys_id(pTrade->OrderSysID);
        if (!state) {
            should_log_warn = true;
            std::strncpy(local_id_copy, pTrade->UserOrderLocalID, sizeof(local_id_copy) - 1);
            std::strncpy(sys_id_copy, pTrade->OrderSysID, sizeof(sys_id_copy) - 1);
            // Release lock before logging
        } else {
            if (pTrade->OrderSysID[0]) {
                if (state->order_sys_id[0]) order_sys_index_.erase(state->order_sys_id);
                std::strncpy(state->order_sys_id, pTrade->OrderSysID, sizeof(state->order_sys_id) - 1);
                (void)order_sys_index_.insert(state->order_sys_id, order_index(state));
            }

            push_trade_event(state->is_quote_leg ? GatewayEventType::QuoteFill : GatewayEventType::OrderFill,
                             *state, *pTrade);

            // Copy data for deferred debug logging
            should_log_debug = true;
            order_id_copy = state->is_quote_leg ? state->client_quote_id : state->client_order_id;
            is_quote_leg_copy = state->is_quote_leg;
            direction_copy = pTrade->Direction;
            volume_copy = pTrade->TradeVolume;
            price_copy = pTrade->TradePrice;
        }
    }  // Lock released here

    // Deferred logging (outside lock)
    if (should_log_warn) {
        OMM_LOG_WARN("femas", "unmatched OnRtnTrade local_id={} sys_id={}",
                     local_id_copy, sys_id_copy);
    }
    if (should_log_debug) {
        OMM_LOG_DEBUG("femas", "fill order_id={} quote_leg={} side={} qty={} price={:.4f}",
                      order_id_copy, is_quote_leg_copy ? 1 : 0,
                      direction_copy == USTP_FTDC_D_Buy ? "buy" : "sell",
                      volume_copy, price_copy);
    }
}

void FEMASGateway::OnErrRtnOrderInsert(CUstpFtdcInputOrderField* pOrder,
                                       CUstpFtdcRspInfoField* pRspInfo) {
    if (!pOrder) return;

    // Copy data for deferred logging (reduces lock hold time)
    bool should_log = false;
    int error_id = 0;
    char error_msg[128] = {};
    uint64_t order_id_copy = 0;

    {
        std::unique_lock<std::shared_mutex> lk(state_rw_lock_);
        OrderState* state = find_order_by_local_id(pOrder->UserOrderLocalID);
        if (!state) return;

        should_log = true;
        error_id = pRspInfo ? pRspInfo->ErrorID : -1;
        if (pRspInfo && pRspInfo->ErrorMsg[0]) {
            std::strncpy(error_msg, pRspInfo->ErrorMsg, sizeof(error_msg) - 1);
        }
        order_id_copy = state->client_order_id;

        push_order_event(GatewayEventType::OrderReject, *state, OrderStatus::Rejected);
        unindex_order_state(state);
        // Reset fields manually (can't use assignment with atomic)
        state->used.store(false, std::memory_order_relaxed);
        state->acked = false;
        state->is_quote_leg = false;
        state->client_order_id = 0;
        state->client_quote_id = 0;
        state->instrument_id = INVALID_INSTRUMENT_ID;
        state->product_index = 0xFF;
        std::memset(state->exchange_local_id, 0, sizeof(state->exchange_local_id));
        std::memset(state->order_sys_id, 0, sizeof(state->order_sys_id));
    }  // Lock released here

    // Deferred logging (outside lock)
    if (should_log) {
        OMM_LOG_WARN("femas", "order insert error ErrorID={} Msg={} order_id={}",
                     error_id, error_msg, order_id_copy);
    }
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

    // Copy data for deferred logging (reduces lock hold time)
    bool should_log_warn = false;
    int error_id = 0;
    char error_msg[128] = {};
    uint64_t quote_id_copy = 0;

    {
        std::unique_lock<std::shared_mutex> lk(state_rw_lock_);
        QuoteState* state = find_quote_by_local_id(pQuote->UserQuoteLocalID);
        if (!state) return;

        if (pQuote->QuoteSysID[0]) {
            if (state->quote_sys_id[0]) quote_sys_index_.erase(state->quote_sys_id);
            std::strncpy(state->quote_sys_id, pQuote->QuoteSysID, sizeof(state->quote_sys_id) - 1);
            (void)quote_sys_index_.insert(state->quote_sys_id, quote_index(state));
        }
        if (pRspInfo && pRspInfo->ErrorID != 0) {
            should_log_warn = true;
            error_id = pRspInfo->ErrorID;
            std::strncpy(error_msg, pRspInfo->ErrorMsg, sizeof(error_msg) - 1);
            quote_id_copy = state->quote.client_quote_id;

            GatewayEvent ev{};
            ev.type = GatewayEventType::QuoteReject;
            ev.product_index = state->quote.product_index;
            ev.quote = state->quote;
            ev.quote.ack_ts = get_monotonic_ns();
            (void)callback_buf.try_push(ev);

            clear_quote_state(state->quote.client_quote_id);
        }
    }  // Lock released here

    // Deferred logging (outside lock)
    if (should_log_warn) {
        OMM_LOG_WARN("femas", "quote rejected ErrorID={} Msg={} quote_id={}",
                     error_id, error_msg, quote_id_copy);
    }
}

void FEMASGateway::OnRtnQuote(CUstpFtdcRtnQuoteField* pQuote) {
    if (!pQuote) return;

    // Copy data for deferred logging (reduces lock hold time)
    bool should_log_warn = false;
    char local_id_copy[32] = {};
    char sys_id_copy[32] = {};

    {
        std::unique_lock<std::shared_mutex> lk(state_rw_lock_);
        QuoteState* state = find_quote_by_local_id(pQuote->UserQuoteLocalID);
        if (!state) state = find_quote_by_sys_id(pQuote->QuoteSysID);
        if (!state) {
            should_log_warn = true;
            std::strncpy(local_id_copy, pQuote->UserQuoteLocalID, sizeof(local_id_copy) - 1);
            std::strncpy(sys_id_copy, pQuote->QuoteSysID, sizeof(sys_id_copy) - 1);
            // Release lock before logging
        } else {
            if (state->quote_sys_id[0]) quote_sys_index_.erase(state->quote_sys_id);
            std::strncpy(state->quote_sys_id, pQuote->QuoteSysID, sizeof(state->quote_sys_id) - 1);
            if (state->quote_sys_id[0]) (void)quote_sys_index_.insert(state->quote_sys_id, quote_index(state));
            std::strncpy(state->bid_order_sys_id, pQuote->BidOrderSysID, sizeof(state->bid_order_sys_id) - 1);
            std::strncpy(state->ask_order_sys_id, pQuote->AskOrderSysID, sizeof(state->ask_order_sys_id) - 1);
            if (OrderState* bid_state = find_order_by_local_id(state->bid_local_id)) {
                if (bid_state->order_sys_id[0]) order_sys_index_.erase(bid_state->order_sys_id);
                std::strncpy(bid_state->order_sys_id, pQuote->BidOrderSysID, sizeof(bid_state->order_sys_id) - 1);
                if (bid_state->order_sys_id[0]) (void)order_sys_index_.insert(bid_state->order_sys_id, order_index(bid_state));
            }
            if (OrderState* ask_state = find_order_by_local_id(state->ask_local_id)) {
                if (ask_state->order_sys_id[0]) order_sys_index_.erase(ask_state->order_sys_id);
                std::strncpy(ask_state->order_sys_id, pQuote->AskOrderSysID, sizeof(ask_state->order_sys_id) - 1);
                if (ask_state->order_sys_id[0]) (void)order_sys_index_.insert(ask_state->order_sys_id, order_index(ask_state));
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
    }  // Lock released here

    // Deferred logging (outside lock)
    if (should_log_warn) {
        OMM_LOG_WARN("femas", "unmatched OnRtnQuote local_id={} sys_id={}",
                     local_id_copy, sys_id_copy);
    }
}

void FEMASGateway::OnErrRtnQuoteInsert(CUstpFtdcInputQuoteField* pQuote,
                                       CUstpFtdcRspInfoField* pRspInfo) {
    if (!pQuote) return;

    // Copy data for deferred logging (reduces lock hold time)
    bool should_log = false;
    int error_id = 0;
    char error_msg[128] = {};
    uint64_t quote_id_copy = 0;

    {
        std::unique_lock<std::shared_mutex> lk(state_rw_lock_);
        QuoteState* state = find_quote_by_local_id(pQuote->UserQuoteLocalID);
        if (!state) return;

        should_log = true;
        error_id = pRspInfo ? pRspInfo->ErrorID : -1;
        if (pRspInfo && pRspInfo->ErrorMsg[0]) {
            std::strncpy(error_msg, pRspInfo->ErrorMsg, sizeof(error_msg) - 1);
        }
        quote_id_copy = state->quote.client_quote_id;

        GatewayEvent ev{};
        ev.type = GatewayEventType::QuoteReject;
        ev.product_index = state->quote.product_index;
        ev.quote = state->quote;
        ev.quote.ack_ts = get_monotonic_ns();

        (void)callback_buf.try_push(ev);

        clear_quote_state(state->quote.client_quote_id);
    }  // Lock released here

    // Deferred logging (outside lock)
    if (should_log) {
        OMM_LOG_WARN("femas", "quote insert error ErrorID={} Msg={} quote_id={}",
                     error_id, error_msg, quote_id_copy);
    }
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
    out.exchange = parse_exchange_id(src.ExchangeID);
    out.expiry_date = std::atoi(src.ExpireDate);
    out.expiry_epoch_ns = estimate_expiry_epoch_ns(src.ExpireDate);

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
