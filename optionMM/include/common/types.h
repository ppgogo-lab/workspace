#pragma once

#include <cstdint>
#include <cstring>
#include <array>
#include <string_view>
#include <immintrin.h>   // _MM_SET_FLUSH_ZERO_MODE

namespace omm {

// ─── Constants ───────────────────────────────────────────────────────────────
static constexpr uint16_t MAX_INSTRUMENTS = 1024;
static constexpr uint16_t INVALID_INSTRUMENT_ID = 0xFFFF;
static constexpr uint8_t  MAX_PRODUCTS    = 32;
static constexpr uint8_t  MAX_ARBITRAGE_STRATEGIES_PER_PRODUCT = 4;
static constexpr uint16_t MAX_OPEN_ORDERS = 4096;
static constexpr uint16_t MAX_USERS = 64;
static constexpr uint16_t MAX_BOOKS = 128;
static constexpr uint8_t  MAX_EXCHANGE_CALENDARS = 8;
static constexpr uint16_t MAX_EXCHANGE_CALENDAR_DAYS = 4096;
static constexpr uint8_t  MAX_TRADING_SESSIONS_PER_EXCHANGE = 8;

// ─── Primitive aliases ────────────────────────────────────────────────────────
// Price stays as double. Set FTZ/DAZ flags at startup (see main.cpp) to prevent
// denormal-number performance penalty. Do NOT use fixed-point: Black-76 requires
// floating-point math and converting tick-to-fixed→float→fixed adds latency.
using Price     = double;
using Volume    = int32_t;
using OrderId   = uint64_t;
using QuoteId   = uint64_t;
using Timestamp = int64_t;   // nanoseconds since epoch (CLOCK_MONOTONIC_RAW)
using UserId    = uint32_t;
using BookId    = uint32_t;

static constexpr UserId INVALID_USER_ID = 0;
static constexpr BookId INVALID_BOOK_ID = 0;

// ─── Fixed-length string (no heap allocation) ────────────────────────────────
// Used only in non-hot-path structs (Instrument registry, config, logging).
// Hot-path structs use uint16_t instrument_id instead.
template<std::size_t N>
struct FixedStr {
    char data[N]{};

    FixedStr() = default;

    explicit FixedStr(std::string_view sv) noexcept {
        std::size_t len = std::min(sv.size(), N - 1);
        std::memcpy(data, sv.data(), len);
        data[len] = '\0';
    }

    std::string_view view() const noexcept { return {data, std::strlen(data)}; }

    bool operator==(const FixedStr& o) const noexcept {
        return std::strncmp(data, o.data, N) == 0;
    }
    bool operator==(std::string_view sv) const noexcept {
        return view() == sv;
    }
    bool empty() const noexcept { return data[0] == '\0'; }
};

using InstrumentCode = FixedStr<32>;   // exchange instrument code, e.g. "cu2501-C-75000"
using ExchangeId     = FixedStr<16>;   // exchange code, e.g. "SHFE"
using AccountId      = FixedStr<32>;   // trading account

// ─── Enumerations (explicit uint8_t to control struct packing) ───────────────
enum class Exchange : uint8_t {
    SHFE = 0,
    DCE = 1,
    CZCE = 2,
    CFFEX = 3,
    GFEX = 4,
    Unknown = 255
};
enum class OptionType  : uint8_t { Call, Put };
enum class Side        : uint8_t { Buy, Sell };
enum class OffsetFlag  : uint8_t { Open, Close, CloseToday, CloseYesterday };
enum class OrderType   : uint8_t { Limit, FAK, FOK, Market };
enum class OrderStatus : uint8_t { New, PartialFilled, Filled, Cancelled, Rejected };
enum class InstrumentKind : uint8_t { Future, Option };
enum class ArbitrageStrategyType : uint8_t { None = 0, PCP = 1 };

// ─── Instrument (registry entry, off hot path) ───────────────────────────────
// Populated at startup: futures from config, options from gateway query.
// Indexed by uint16_t instrument_id for O(1) lookup everywhere.
// Cache-line aligned for efficient lookup in hot path.
struct alignas(64) Instrument {
    InstrumentCode code;                // exchange code, e.g. "cu2501-C-75000"
    InstrumentCode underlying_code;     // underlying futures code, e.g. "cu2501"
    ExchangeId     exchange_id;
    Exchange       exchange;
    InstrumentKind kind;
    OptionType     option_type;         // valid only if kind == Option
    double         strike;              // valid only if kind == Option
    double         multiplier;          // contract multiplier (e.g. 5 for IF)
    double         tick_size;           // minimum price movement
    int32_t        expiry_date;         // YYYYMMDD (e.g. 20251226)
    int64_t        expiry_epoch_ns;     // expiry as nanoseconds from same epoch as trading clock
    uint16_t       instrument_id;       // our internal integer ID (assigned at startup)
    uint16_t       underlying_id;       // internal ID of the underlying future
    uint8_t        product_index;       // which strategy thread owns this instrument
    uint8_t        _pad[3];
};
static_assert(sizeof(Instrument) <= 192);

// ─── Market data tick ─────────────────────────────────────────────────────────
// Hot path: written by feed handler, read by pricer thread.
// instrument_id is uint16_t — feed decoder maps exchange code → id exactly once.
struct alignas(64) TopOfBookTick {
    int64_t  recv_ts_ns;
    int64_t  exchange_ts_ns;
    uint16_t instrument_id;
    uint8_t  _pad0[6];
    double   last_price;
    double   bid_price[1];
    double   ask_price[1];
    int32_t  bid_volume[1];
    int32_t  ask_volume[1];
    uint64_t sequence_no;
};
static_assert(sizeof(TopOfBookTick) == 64);
static_assert(alignof(TopOfBookTick) == 64);

struct alignas(64) MarketTick {
    int64_t  recv_ts_ns;                // hardware or software receive timestamp
    int64_t  exchange_ts_ns;            // exchange-reported timestamp
    uint16_t instrument_id;             // O(1) array index into instrument registry
    uint8_t  _pad0[6];
    double   last_price;
    double   bid_price[5];
    double   ask_price[5];
    int32_t  bid_volume[5];
    int32_t  ask_volume[5];
    double   open_interest;
    int64_t  volume;
    double   open_price;
    double   high_price;
    double   low_price;
    double   pre_settlement;
    double   pre_close;
    uint64_t sequence_no;               // exchange sequence number for gap detection
};
static_assert(sizeof(MarketTick) == 256);
static_assert(alignof(MarketTick) == 64);

[[nodiscard]] inline TopOfBookTick to_top_of_book_tick(const MarketTick& src) noexcept {
    TopOfBookTick dst{};
    dst.recv_ts_ns = src.recv_ts_ns;
    dst.exchange_ts_ns = src.exchange_ts_ns;
    dst.instrument_id = src.instrument_id;
    dst.last_price = src.last_price;
    dst.bid_price[0] = src.bid_price[0];
    dst.ask_price[0] = src.ask_price[0];
    dst.bid_volume[0] = src.bid_volume[0];
    dst.ask_volume[0] = src.ask_volume[0];
    dst.sequence_no = src.sequence_no;
    return dst;
}

// ─── Greeks + theoretical price (output of Black-76 pricer) ──────────────────
struct alignas(64) Greeks {
    uint16_t instrument_id;
    uint8_t  _pad[6];
    double   theo_price;
    double   std_delta; // D * s * N(s*d1)
    double   delta;     // std_delta * option_multiplier / future_multiplier
    double   delta_cash;
    double   std_gamma;
    double   gamma;
    double   gamma_cash;
    double   vega;      // per 1 vol point
    double   vega_cash;
    double   theta;     // per trading day
    double   theta_cash;
    double   rho;       // per 1% rate move
    double   rho_cash;  // rho * option_multiplier
    double   vanna;
    double   volga;
    double   charm;
    double   iv;        // implied volatility used
    double   T;         // time to expiry in years at calculation time
    int64_t  calc_ts_ns;
};
static_assert(sizeof(Greeks) == 192);
static_assert(alignof(Greeks) == 64);

// ─── Pricing signal (pricer → strategy thread) ───────────────────────────────
// Keep this compact. Full Greeks remain in greeks_snapshot_ for risk/monitoring,
// but the strategy hot path only needs a small subset.
enum PricingFlags : uint16_t {
    PricingFlagNone = 0,
    PricingFlagHasUnderlyingRef = 1u << 0,
};

struct alignas(64) PricingSignal {
    uint16_t instrument_id{INVALID_INSTRUMENT_ID};
    uint16_t underlying_id{INVALID_INSTRUMENT_ID};
    uint16_t flags{PricingFlagNone};
    uint16_t _pad0{0};
    uint64_t sequence_no{0};
    int64_t  calc_ts_ns{0};
    double   theo_bid{0.0};
    double   theo_ask{0.0};
    float    std_delta{0.0F};
    float    delta{0.0F};
    float    vega{0.0F};
    float    underlying_ref_bid{0.0F};
    float    underlying_ref_ask{0.0F};
    uint8_t  _pad1[4]{};
};
static_assert(sizeof(PricingSignal) == 64);
static_assert(alignof(PricingSignal) == 64);

// ─── Timer event (timer thread → strategy thread) ────────────────────────────
enum class TimerEventType : uint8_t {
    HedgeCheck,      // evaluate delta, fire hedge if threshold exceeded
    QuoteRefresh,    // re-quote all instruments without waiting for new tick
    SessionOpen,     // market session opened — enable quoting
    SessionClose,    // market session closing — cancel quotes, stop quoting
    RecalcGreeks,    // recalculate theta/T for all instruments (near expiry)
};

struct alignas(16) TimerEvent {
    int64_t        trigger_ts_ns;
    TimerEventType type;
    uint8_t        _pad[7];
};
static_assert(sizeof(TimerEvent) == 16);

// ─── Order ────────────────────────────────────────────────────────────────────
struct alignas(64) Order {
    OrderId    client_order_id;
    OrderId    exchange_order_id{0};
    uint16_t   instrument_id;
    uint8_t    product_index;
    uint8_t    _pad0{0};
    BookId     book_id{INVALID_BOOK_ID};
    AccountId  account_id;
    ExchangeId exchange_id;
    Side       side;
    OffsetFlag offset;
    OrderType  order_type;
    OrderStatus status{OrderStatus::New};
    uint8_t    _pad1[4];
    double     price;
    Volume     volume;
    Volume     filled_volume{0};
    double     avg_fill_price{0.0};
    Timestamp  send_ts{0};
    Timestamp  ack_ts{0};
    bool       is_manual{false};
    bool       is_hedge{false};
    bool       is_cancel{false};
    uint8_t    _pad2[5];
};

// ─── Quote (two-sided order) ──────────────────────────────────────────────────
struct alignas(64) Quote {
    QuoteId    client_quote_id;
    QuoteId    exchange_quote_id{0};
    uint16_t   instrument_id;
    uint8_t    product_index;
    uint8_t    _pad0{0};
    BookId     book_id{INVALID_BOOK_ID};
    AccountId  account_id;
    ExchangeId exchange_id;
    OffsetFlag bid_offset;
    OffsetFlag ask_offset;
    uint8_t    _pad1[6];
    double     bid_price;
    double     ask_price;
    Volume     bid_volume;
    Volume     ask_volume;
    OrderStatus bid_status{OrderStatus::New};
    OrderStatus ask_status{OrderStatus::New};
    uint8_t    _pad2[2];
    Timestamp  send_ts{0};
    Timestamp  ack_ts{0};
};

// ─── Trade (fill report) ──────────────────────────────────────────────────────
struct alignas(64) Trade {
    OrderId    client_order_id;
    uint64_t   trade_id;
    uint16_t   instrument_id;
    uint8_t    product_index;
    uint8_t    _pad0{0};
    BookId     book_id{INVALID_BOOK_ID};
    AccountId  account_id;
    ExchangeId exchange_id;
    Side       side;
    OffsetFlag offset;
    uint8_t    _pad1[6];
    double     fill_price;
    Volume     fill_volume;
    uint32_t   _pad2;
    Timestamp  fill_ts;
};

enum class ArbIntentKind : uint8_t {
    SubmitOrder = 0,
    CancelOrder = 1,
};

struct alignas(64) ArbIntent {
    Order                 order{};
    ArbitrageStrategyType strategy_type{ArbitrageStrategyType::None};
    ArbIntentKind         kind{ArbIntentKind::SubmitOrder};
    bool                  cleanup{false};
    uint8_t               _pad0[5]{};
    Timestamp             intent_ts{0};
    double                edge_ticks{0.0};
    uint16_t              call_instrument_id{INVALID_INSTRUMENT_ID};
    uint16_t              put_instrument_id{INVALID_INSTRUMENT_ID};
    uint16_t              future_instrument_id{INVALID_INSTRUMENT_ID};
    uint16_t              _pad1{0};
};

struct alignas(32) ArbMarketTrigger {
    uint16_t  instrument_id{INVALID_INSTRUMENT_ID};
    uint8_t   product_index{0xFF};
    uint8_t   _pad0[5]{};
    uint64_t  sequence_no{0};
    Timestamp recv_ts_ns{0};
};

// ─── Position ─────────────────────────────────────────────────────────────────
// Cache-line aligned to prevent false sharing in position arrays.
struct alignas(64) Position {
    uint16_t instrument_id;
    uint8_t  product_index;
    uint8_t  _pad[5];
    int32_t  net_position{0};
    int32_t  long_position{0};
    int32_t  short_position{0};
    int32_t  long_today{0};
    int32_t  short_today{0};
    double   avg_long_price{0.0};
    double   avg_short_price{0.0};
    double   realized_pnl{0.0};
};

struct BookPosition {
    BookId   book_id{INVALID_BOOK_ID};
    uint16_t instrument_id{INVALID_INSTRUMENT_ID};
    uint8_t  product_index{0xFF};
    uint8_t  _pad0{0};
    int32_t  net_position{0};
    int32_t  long_position{0};
    int32_t  short_position{0};
    int32_t  long_today{0};
    int32_t  short_today{0};
    double   avg_long_price{0.0};
    double   avg_short_price{0.0};
    double   realized_pnl{0.0};
};

struct BookPortfolioGreeks {
    BookId    book_id{INVALID_BOOK_ID};
    uint8_t   product_index{0xFF};  // 0xFF = aggregate across all products
    uint8_t   _pad0[3]{};
    double    net_delta{0.0};
    double    net_gamma{0.0};
    double    net_vega{0.0};
    double    net_theta{0.0};
    double    pnl_realized{0.0};
    double    pnl_unrealized{0.0};
    Timestamp calc_ts{0};
};

// ─── Portfolio-level Greeks (aggregated across all instruments) ───────────────
struct PortfolioGreeks {
    double net_delta{0.0};
    double net_gamma{0.0};
    double net_vega{0.0};
    double net_theta{0.0};
    double pnl_realized{0.0};
    double pnl_unrealized{0.0};
    Timestamp calc_ts{0};
};

// ─── FP environment setup (call once in main, before any pricing) ─────────────
inline void setup_fp_environment() noexcept {
    // Flush-To-Zero: FP results that would be denormal become 0 instead.
    // Denormals-Are-Zero: denormal inputs treated as 0.
    // Without these, a near-zero volatility or deep-OTM option causes a
    // 50-100x slowdown in FP operations (hardware microcode assist).
    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
}

enum class SystemAlertType : uint8_t {
    QuoteCancelGiveUp = 0,
};

struct SystemAlert {
    Timestamp       ts_ns{0};
    uint16_t        instrument_id{INVALID_INSTRUMENT_ID};
    uint8_t         product_index{0xFF};
    SystemAlertType type{SystemAlertType::QuoteCancelGiveUp};
    char            message[128]{};
};

} // namespace omm
