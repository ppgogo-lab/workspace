#include "monitoring/grpc_server.h"
#include "common/auth.h"
#include "engine/trading_engine.h"
#include "strategy/mm_params.h"
#include "common/config.h"
#include "common/thread_utils.h"
#include "logger/logger.h"

#include "trading.grpc.pb.h"
#include "trading.pb.h"

#include <grpcpp/grpcpp.h>
#include <grpcpp/server_builder.h>

#include <google/protobuf/repeated_field.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace omm {

namespace {

constexpr uint32_t kAllProducts = 0xFFu;
constexpr uint32_t kSuppressStaleTheo = 1u << 0;
constexpr uint32_t kSuppressInvalidMarket = 1u << 1;
constexpr uint32_t kSuppressPosition = 1u << 2;
constexpr uint32_t kSuppressRisk = 1u << 3;
constexpr uint32_t kSuppressThrottle = 1u << 5;
constexpr uint32_t kSuppressUnderlyingShock = 1u << 6;
constexpr uint32_t kSuppressProductExposure = 1u << 7;
constexpr uint32_t kSuppressCancelStuck = 1u << 8;

bool all_products(uint32_t product_index) noexcept {
    return product_index == kAllProducts;
}

bool product_in_scope(uint32_t request_product_index, uint32_t product_index) noexcept {
    return all_products(request_product_index) || request_product_index == product_index;
}

bool instrument_in_scope(const TradingEngine& engine,
                         uint32_t request_product_index,
                         uint32_t instrument_id) noexcept {
    if (all_products(request_product_index)) return true;
    if (instrument_id >= static_cast<uint32_t>(engine.n_instruments())) return false;
    return engine.instruments()[instrument_id].product_index == request_product_index;
}

const char* order_status_name(OrderStatus status) noexcept {
    switch (status) {
    case OrderStatus::New: return "New";
    case OrderStatus::PartialFilled: return "PartialFilled";
    case OrderStatus::Filled: return "Filled";
    case OrderStatus::Cancelled: return "Cancelled";
    case OrderStatus::Rejected: return "Rejected";
    }
    return "Unknown";
}

const char* side_name(Side side) noexcept {
    switch (side) {
    case Side::Buy: return "Buy";
    case Side::Sell: return "Sell";
    }
    return "";
}

const char* instrument_kind_name(InstrumentKind kind) noexcept {
    switch (kind) {
    case InstrumentKind::Future: return "Future";
    case InstrumentKind::Option: return "Option";
    }
    return "Unknown";
}

const char* option_type_name(const Instrument& instr) noexcept {
    if (instr.kind != InstrumentKind::Option) return "";
    return instr.option_type == OptionType::Call ? "Call" : "Put";
}

const char* base_offset_type_name(BaseOffsetType type) noexcept {
    switch (type) {
    case BaseOffsetType::Tick: return "tick";
    case BaseOffsetType::Price: return "price";
    case BaseOffsetType::Percentage: return "percentage";
    }
    return "price";
}

bool parse_base_offset_type(std::string_view type, BaseOffsetType* out) noexcept {
    if (out == nullptr) return false;
    if (type == "tick" || type == "Tick") {
        *out = BaseOffsetType::Tick;
        return true;
    }
    if (type == "price" || type == "Price") {
        *out = BaseOffsetType::Price;
        return true;
    }
    if (type == "percentage" || type == "Percentage") {
        *out = BaseOffsetType::Percentage;
        return true;
    }
    return false;
}

void populate_product_pricing(uint32_t product_index,
                              const ProductPricingConfig& pricing,
                              omm::proto::ProductPricingParams* msg) {
    msg->set_product_index(product_index);
    msg->set_base_offset_type(base_offset_type_name(pricing.base_offset_type));
    msg->set_base_offset_value(pricing.base_offset_value);
}

omm::proto::ArbitrageStrategyType arb_strategy_type_to_proto(ArbitrageStrategyType type) noexcept {
    switch (type) {
    case ArbitrageStrategyType::PCP:
        return omm::proto::ARB_STRATEGY_PCP;
    case ArbitrageStrategyType::None:
    default:
        return omm::proto::ARB_STRATEGY_NONE;
    }
}

bool arb_strategy_type_from_proto(omm::proto::ArbitrageStrategyType type,
                                  ArbitrageStrategyType* out) noexcept {
    if (out == nullptr) return false;
    switch (type) {
    case omm::proto::ARB_STRATEGY_PCP:
        *out = ArbitrageStrategyType::PCP;
        return true;
    case omm::proto::ARB_STRATEGY_NONE:
    default:
        *out = ArbitrageStrategyType::None;
        return false;
    }
}

omm::proto::PcpOpportunityDirection pcp_direction_to_proto(PCPMonitorDirection dir) noexcept {
    switch (dir) {
    case PCPMonitorDirection::LongSyntheticShortFuture:
        return omm::proto::PCP_DIR_LONG_SYNTH_SHORT_FUTURE;
    case PCPMonitorDirection::ShortSyntheticLongFuture:
        return omm::proto::PCP_DIR_SHORT_SYNTH_LONG_FUTURE;
    case PCPMonitorDirection::None:
    default:
        return omm::proto::PCP_DIR_NONE;
    }
}

void populate_user_info(const PersistedUser& user, omm::proto::UserInfo* msg) {
    if (msg == nullptr) return;
    msg->set_user_id(user.user_id);
    msg->set_username(user.username);
    msg->set_display_name(user.display_name);
    msg->set_active(user.active);
    msg->set_default_book_id(user.default_book_id);
}

void populate_book_info(const PersistedBook& book, omm::proto::BookInfo* msg) {
    if (msg == nullptr) return;
    msg->set_book_id(book.book_id);
    msg->set_book_code(book.book_code);
    msg->set_display_name(book.display_name);
    msg->set_active(book.active);
    msg->set_description(book.description);
}

class SessionManager {
public:
    struct Session {
        std::string token;
        PersistedUser user{};
    };

    [[nodiscard]] Session create_session(const PersistedUser& user) {
        Session session{};
        session.token = generate_session_token();
        session.user = user;
        std::lock_guard<std::mutex> lock(mutex_);
        sessions_[session.token] = session.user;
        return session;
    }

    [[nodiscard]] bool get_session(std::string_view token, PersistedUser* out_user) const {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = sessions_.find(std::string(token));
        if (it == sessions_.end()) return false;
        if (out_user != nullptr) *out_user = it->second;
        return true;
    }

    [[nodiscard]] bool is_active(std::string_view token) const {
        return get_session(token, nullptr);
    }

    [[nodiscard]] bool revoke(std::string_view token, bool* out_zero_sessions) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = sessions_.find(std::string(token));
        if (it == sessions_.end()) return false;
        sessions_.erase(it);
        if (out_zero_sessions != nullptr) {
            *out_zero_sessions = sessions_.empty();
        }
        return true;
    }

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, PersistedUser> sessions_;
};

void populate_order_update(const TradingEngine& engine, const Order& order, omm::proto::OrderUpdate* msg) {
    msg->set_client_order_id(order.client_order_id);
    msg->set_instrument_id(order.instrument_id);
    msg->set_status(order_status_name(order.status));
    msg->set_fill_price(order.avg_fill_price);
    msg->set_fill_volume(order.filled_volume);
    msg->set_ts_ns(order.ack_ts != 0 ? order.ack_ts : order.send_ts);
    if (!order.exchange_id.empty()) {
        msg->set_exchange_id(std::string(order.exchange_id.view()));
    } else if (order.instrument_id < static_cast<uint16_t>(engine.n_instruments())) {
        msg->set_exchange_id(std::string(engine.instruments()[order.instrument_id].exchange_id.view()));
    }
    msg->set_side(side_name(order.side));
    msg->set_price(order.price);
    msg->set_volume(order.volume);
    msg->set_book_id(order.book_id);
}

void populate_trade_update(const Trade& trade, omm::proto::OrderUpdate* msg) {
    msg->set_client_order_id(trade.client_order_id);
    msg->set_instrument_id(trade.instrument_id);
    msg->set_status("Filled");
    msg->set_fill_price(trade.fill_price);
    msg->set_fill_volume(trade.fill_volume);
    msg->set_ts_ns(trade.fill_ts);
    msg->set_exchange_id(std::string(trade.exchange_id.view()));
    msg->set_side(side_name(trade.side));
    msg->set_price(trade.fill_price);
    msg->set_volume(trade.fill_volume);
    msg->set_exchange_trade_id(trade.trade_id);
    msg->set_book_id(trade.book_id);
}

void populate_quote_update(const Quote& quote,
                           std::string_view status,
                           int64_t ts_ns,
                           omm::proto::QuoteUpdate* msg) {
    msg->set_client_quote_id(quote.client_quote_id);
    msg->set_instrument_id(quote.instrument_id);
    msg->set_bid_price(quote.bid_price);
    msg->set_ask_price(quote.ask_price);
    msg->set_bid_volume(quote.bid_volume);
    msg->set_ask_volume(quote.ask_volume);
    msg->set_status(std::string(status));
    msg->set_ts_ns(ts_ns);
    msg->set_book_id(quote.book_id);
}

void populate_tick(const TopOfBookTick& tick, omm::proto::Tick* msg) {
    msg->set_instrument_id(tick.instrument_id);
    msg->set_last_price(tick.last_price);
    msg->set_bid_price(tick.bid_price[0]);
    msg->set_ask_price(tick.ask_price[0]);
    msg->set_bid_volume(tick.bid_volume[0]);
    msg->set_ask_volume(tick.ask_volume[0]);
    msg->set_exchange_ts_ns(tick.exchange_ts_ns);
    msg->set_recv_ts_ns(tick.recv_ts_ns);
}

omm::proto::RiskAlert::AlertType alert_type_to_proto(SystemAlertType type) noexcept {
    switch (type) {
    case SystemAlertType::QuoteCancelGiveUp:
        return omm::proto::RiskAlert::QUOTE_CANCEL_GIVE_UP;
    }
    return omm::proto::RiskAlert::QUOTE_CANCEL_GIVE_UP;
}

omm::proto::MMQuoteState quote_state_to_proto(StrategyQuoteMonitorState state) noexcept {
    switch (state) {
    case StrategyQuoteMonitorState::Idle:
        return omm::proto::MM_QUOTE_IDLE;
    case StrategyQuoteMonitorState::Live:
        return omm::proto::MM_QUOTE_LIVE;
    case StrategyQuoteMonitorState::AckPending:
        return omm::proto::MM_QUOTE_ACK_PENDING;
    case StrategyQuoteMonitorState::CancelPending:
        return omm::proto::MM_QUOTE_CANCEL_PENDING;
    case StrategyQuoteMonitorState::CancelFailed:
        return omm::proto::MM_QUOTE_CANCEL_FAILED;
    case StrategyQuoteMonitorState::Suppressed:
        return omm::proto::MM_QUOTE_SUPPRESSED;
    }
    return omm::proto::MM_QUOTE_IDLE;
}

void add_reason_unique(google::protobuf::RepeatedField<int>* reasons,
                       int reason) {
    if (reasons == nullptr) return;
    for (const int existing : *reasons) {
        if (existing == reason) return;
    }
    reasons->Add(reason);
}

void append_product_reasons(const ProductMonitorState& state,
                            google::protobuf::RepeatedField<int>* reasons) {
    if (!state.strategy_enabled) {
        add_reason_unique(reasons, omm::proto::MM_REASON_DISABLED);
    }
    if (!state.session_open) {
        add_reason_unique(reasons, omm::proto::MM_REASON_SESSION_CLOSED);
    }
    if (state.risk_breach) {
        add_reason_unique(reasons, omm::proto::MM_REASON_RISK_LIMIT);
    }
    if (state.exposure_breached) {
        add_reason_unique(reasons, omm::proto::MM_REASON_PRODUCT_EXPOSURE);
    }
    if (state.underlying_shock_suppressed) {
        add_reason_unique(reasons, omm::proto::MM_REASON_UNDERLYING_SHOCK);
    }
}

void append_instrument_reasons(uint32_t suppress_flags,
                               google::protobuf::RepeatedField<int>* reasons) {
    if (suppress_flags & kSuppressStaleTheo) {
        add_reason_unique(reasons, omm::proto::MM_REASON_STALE_THEO);
    }
    if (suppress_flags & kSuppressInvalidMarket) {
        add_reason_unique(reasons, omm::proto::MM_REASON_INVALID_MARKET);
    }
    if (suppress_flags & kSuppressPosition) {
        add_reason_unique(reasons, omm::proto::MM_REASON_POSITION_LIMIT);
    }
    if (suppress_flags & kSuppressRisk) {
        add_reason_unique(reasons, omm::proto::MM_REASON_RISK_LIMIT);
    }
    if (suppress_flags & kSuppressThrottle) {
        add_reason_unique(reasons, omm::proto::MM_REASON_THROTTLE);
    }
    if (suppress_flags & kSuppressUnderlyingShock) {
        add_reason_unique(reasons, omm::proto::MM_REASON_UNDERLYING_SHOCK);
    }
    if (suppress_flags & kSuppressProductExposure) {
        add_reason_unique(reasons, omm::proto::MM_REASON_PRODUCT_EXPOSURE);
    }
    if (suppress_flags & kSuppressCancelStuck) {
        add_reason_unique(reasons, omm::proto::MM_REASON_CANCEL_STUCK);
    }
}

void append_arb_reasons(uint32_t suppress_flags,
                        google::protobuf::RepeatedField<int>* reasons) {
    if (suppress_flags & ArbSuppressDisabled) {
        add_reason_unique(reasons, omm::proto::ARB_REASON_DISABLED);
    }
    if (suppress_flags & ArbSuppressNoPairs) {
        add_reason_unique(reasons, omm::proto::ARB_REASON_NO_PAIRS);
    }
    if (suppress_flags & ArbSuppressInvalidMarket) {
        add_reason_unique(reasons, omm::proto::ARB_REASON_INVALID_MARKET);
    }
    if (suppress_flags & ArbSuppressCooldown) {
        add_reason_unique(reasons, omm::proto::ARB_REASON_COOLDOWN);
    }
    if (suppress_flags & ArbSuppressIntentBackpressure) {
        add_reason_unique(reasons, omm::proto::ARB_REASON_INTENT_BACKPRESSURE);
    }
    if (suppress_flags & ArbSuppressLiveOrders) {
        add_reason_unique(reasons, omm::proto::ARB_REASON_LIVE_ORDERS);
    }
    if (suppress_flags & ArbSuppressCleanupPending) {
        add_reason_unique(reasons, omm::proto::ARB_REASON_CLEANUP_PENDING);
    }
}

} // namespace

class TradingMonitorServiceImpl final : public omm::proto::TradingMonitor::Service {
public:
    explicit TradingMonitorServiceImpl(TradingEngine& engine) : engine_(engine) {}

    grpc::Status Login(
            grpc::ServerContext*,
            const omm::proto::LoginRequest* req,
            omm::proto::LoginResponse* resp) override
    {
        const PersistedUser* user = find_user(req->username());
        if (user == nullptr || !user->active || !password_matches(*user, req->password())) {
            resp->set_ok(false);
            resp->set_message("invalid username or password");
            return grpc::Status::OK;
        }

        SessionManager::Session session{};
        try {
            session = session_manager_.create_session(*user);
        } catch (const std::exception& e) {
            return grpc::Status(grpc::StatusCode::INTERNAL, e.what());
        }
        engine_.note_session_activated();
        resp->set_ok(true);
        resp->set_session_token(session.token);
        populate_user_info(session.user, resp->mutable_user());
        return grpc::Status::OK;
    }

    grpc::Status Logout(
            grpc::ServerContext* ctx,
            const omm::proto::LogoutRequest*,
            omm::proto::LogoutResponse* resp) override
    {
        std::string token;
        PersistedUser user{};
        grpc::Status auth_status = authenticate(ctx, &user, &token);
        if (!auth_status.ok()) return auth_status;

        bool zero_sessions = false;
        if (!session_manager_.revoke(token, &zero_sessions)) {
            return grpc::Status(grpc::StatusCode::UNAUTHENTICATED, "invalid session");
        }
        if (zero_sessions) {
            engine_.shutdown_on_zero_sessions();
        }
        resp->set_ok(true);
        resp->set_triggered_shutdown(zero_sessions);
        return grpc::Status::OK;
    }

    grpc::Status WhoAmI(
            grpc::ServerContext* ctx,
            const omm::proto::WhoAmIRequest*,
            omm::proto::WhoAmIResponse* resp) override
    {
        PersistedUser user{};
        grpc::Status auth_status = authenticate(ctx, &user, nullptr);
        if (!auth_status.ok()) return auth_status;
        resp->set_ok(true);
        populate_user_info(user, resp->mutable_user());
        return grpc::Status::OK;
    }

    grpc::Status SetStrategyParams(
            grpc::ServerContext* ctx,
            const omm::proto::SetStrategyParamsRequest* req,
            omm::proto::SetStrategyParamsResponse* resp) override
    {
        grpc::Status auth_status = authenticate(ctx, nullptr, nullptr);
        if (!auth_status.ok()) return auth_status;
        int idx = static_cast<int>(req->product_index());
        if (idx < 0 || idx >= engine_.product_count()) {
            resp->set_ok(false);
            resp->set_message("invalid product_index");
            return grpc::Status::OK;
        }
        const auto& p = req->params();
        MMParamsConfig snap = engine_.mm_params(idx).snapshot();
        if (p.has_bid_spread()) snap.bid_spread = p.bid_spread();
        if (p.has_ask_spread()) snap.ask_spread = p.ask_spread();
        if (p.has_product_delta_threshold()) snap.product_delta_threshold = p.product_delta_threshold();
        if (p.has_quote_volume()) snap.quote_volume = p.quote_volume();
        if (p.has_max_position()) snap.max_position = p.max_position();
        if (p.has_enabled()) snap.enabled = p.enabled();
        if (p.has_product_vega_threshold()) snap.product_vega_threshold = p.product_vega_threshold();
        if (p.has_min_quote_interval_ms()) snap.min_quote_interval_ms = p.min_quote_interval_ms();
        if (p.has_warning_position()) snap.warning_position = p.warning_position();
        if (p.has_base_half_spread_ticks()) snap.base_half_spread_ticks = p.base_half_spread_ticks();
        if (p.has_min_half_spread_ticks()) snap.min_half_spread_ticks = p.min_half_spread_ticks();
        if (p.has_max_half_spread_ticks()) snap.max_half_spread_ticks = p.max_half_spread_ticks();
        if (p.has_inventory_skew_per_lot_ticks()) {
            snap.inventory_skew_per_lot_ticks = p.inventory_skew_per_lot_ticks();
        }
        if (p.has_follow_weight()) snap.follow_weight = p.follow_weight();
        if (p.has_requote_price_epsilon_ticks()) {
            snap.requote_price_epsilon_ticks = p.requote_price_epsilon_ticks();
        }
        if (p.has_market_width_widen_threshold_ticks()) {
            snap.market_width_widen_threshold_ticks = p.market_width_widen_threshold_ticks();
        }
        if (p.has_underlying_move_widen_threshold_ticks()) {
            snap.underlying_move_widen_threshold_ticks = p.underlying_move_widen_threshold_ticks();
        }
        if (p.has_use_one_sided_at_limits()) {
            snap.use_one_sided_at_limits = p.use_one_sided_at_limits();
        }
        engine_.mm_params(idx).apply(snap);
        engine_.persist_mm_params_update(idx, snap);
        OMM_LOG_INFO(
            "grpc",
            "SetStrategyParams product={} qv={} max_pos={} warn_pos={} base_half={} product_delta={} vega={} enabled={}",
            idx,
            snap.quote_volume,
            snap.max_position,
            snap.warning_position,
            snap.base_half_spread_ticks,
            snap.product_delta_threshold,
            snap.product_vega_threshold,
            (int)snap.enabled);
        resp->set_ok(true);
        return grpc::Status::OK;
    }

    grpc::Status SetProductPricingParams(
            grpc::ServerContext* ctx,
            const omm::proto::SetProductPricingParamsRequest* req,
            omm::proto::SetProductPricingParamsResponse* resp) override
    {
        grpc::Status auth_status = authenticate(ctx, nullptr, nullptr);
        if (!auth_status.ok()) return auth_status;

        const auto& params = req->params();
        const int idx = static_cast<int>(params.product_index());
        if (idx < 0 || idx >= engine_.product_count()) {
            resp->set_ok(false);
            resp->set_message("invalid product_index");
            return grpc::Status::OK;
        }

        ProductPricingConfig pricing = engine_.product_pricing(idx);
        if (!params.base_offset_type().empty()
            && !parse_base_offset_type(params.base_offset_type(), &pricing.base_offset_type)) {
            resp->set_ok(false);
            resp->set_message("base_offset_type must be tick/price/percentage");
            return grpc::Status::OK;
        }
        pricing.base_offset_value = params.base_offset_value();
        if (!engine_.set_product_pricing(idx, pricing)) {
            resp->set_ok(false);
            resp->set_message("failed to update product pricing");
            return grpc::Status::OK;
        }

        OMM_LOG_INFO(
            "grpc",
            "SetProductPricingParams product={} base_offset_type={} base_offset_value={}",
            idx,
            base_offset_type_name(pricing.base_offset_type),
            pricing.base_offset_value);
        resp->set_ok(true);
        return grpc::Status::OK;
    }

    grpc::Status StartStrategy(
            grpc::ServerContext* ctx,
            const omm::proto::StartStopRequest* req,
            omm::proto::StartStopResponse* resp) override
    {
        grpc::Status auth_status = authenticate(ctx, nullptr, nullptr);
        if (!auth_status.ok()) return auth_status;
        set_enabled(req->product_index(), true);
        OMM_LOG_INFO("grpc", "StartStrategy product={}", req->product_index());
        resp->set_ok(true);
        return grpc::Status::OK;
    }

    grpc::Status StopStrategy(
            grpc::ServerContext* ctx,
            const omm::proto::StartStopRequest* req,
            omm::proto::StartStopResponse* resp) override
    {
        grpc::Status auth_status = authenticate(ctx, nullptr, nullptr);
        if (!auth_status.ok()) return auth_status;
        set_enabled(req->product_index(), false);
        OMM_LOG_INFO("grpc", "StopStrategy product={}", req->product_index());
        resp->set_ok(true);
        return grpc::Status::OK;
    }

    grpc::Status SetArbStrategyParams(
            grpc::ServerContext* ctx,
            const omm::proto::SetArbStrategyParamsRequest* req,
            omm::proto::SetArbStrategyParamsResponse* resp) override
    {
        grpc::Status auth_status = authenticate(ctx, nullptr, nullptr);
        if (!auth_status.ok()) return auth_status;
        ArbitrageStrategyType type = ArbitrageStrategyType::None;
        if (!arb_strategy_type_from_proto(req->id().strategy_type(), &type)) {
            resp->set_ok(false);
            resp->set_message("invalid arbitrage strategy_type");
            return grpc::Status::OK;
        }
        const int idx = static_cast<int>(req->id().product_index());
        AtomicArbParams* params = engine_.arbitrage_params(idx, type);
        if (params == nullptr) {
            resp->set_ok(false);
            resp->set_message("arbitrage strategy not configured");
            return grpc::Status::OK;
        }

        ArbParamsConfig snap = params->snapshot();
        const auto& p = req->params();
        if (p.has_min_edge_ticks()) snap.min_edge_ticks = p.min_edge_ticks();
        if (p.has_cooldown_ms()) snap.cooldown_ms = p.cooldown_ms();
        if (p.has_scan_interval_ms()) snap.scan_interval_ms = p.scan_interval_ms();
        if (p.has_cleanup_timeout_ms()) snap.cleanup_timeout_ms = p.cleanup_timeout_ms();
        if (p.has_max_order_volume()) snap.max_order_volume = p.max_order_volume();
        if (p.has_max_live_orders()) snap.max_live_orders = p.max_live_orders();
        if (p.has_cleanup_on_partial()) snap.cleanup_on_partial = p.cleanup_on_partial();
        if (p.has_enabled()) snap.enabled = p.enabled();
        params->apply(snap);
        engine_.persist_arb_params_update(idx, type, snap);

        OMM_LOG_INFO("grpc",
                     "SetArbStrategyParams product={} type={} edge_ticks={} max_order_volume={} enabled={}",
                     idx,
                     static_cast<int>(type),
                     snap.min_edge_ticks,
                     snap.max_order_volume,
                     static_cast<int>(snap.enabled));
        resp->set_ok(true);
        return grpc::Status::OK;
    }

    grpc::Status StartArbStrategy(
            grpc::ServerContext* ctx,
            const omm::proto::ArbStartStopRequest* req,
            omm::proto::ArbStartStopResponse* resp) override
    {
        grpc::Status auth_status = authenticate(ctx, nullptr, nullptr);
        if (!auth_status.ok()) return auth_status;
        ArbitrageStrategyType type = ArbitrageStrategyType::None;
        if (!arb_strategy_type_from_proto(req->id().strategy_type(), &type)) {
            resp->set_ok(false);
            resp->set_message("invalid arbitrage strategy_type");
            return grpc::Status::OK;
        }
        const bool ok =
            engine_.set_arbitrage_enabled(static_cast<int>(req->id().product_index()), type, true);
        resp->set_ok(ok);
        if (!ok) {
            resp->set_message("arbitrage strategy not configured");
        }
        OMM_LOG_INFO("grpc", "StartArbStrategy product={} type={}",
                     req->id().product_index(),
                     static_cast<int>(type));
        return grpc::Status::OK;
    }

    grpc::Status StopArbStrategy(
            grpc::ServerContext* ctx,
            const omm::proto::ArbStartStopRequest* req,
            omm::proto::ArbStartStopResponse* resp) override
    {
        grpc::Status auth_status = authenticate(ctx, nullptr, nullptr);
        if (!auth_status.ok()) return auth_status;
        ArbitrageStrategyType type = ArbitrageStrategyType::None;
        if (!arb_strategy_type_from_proto(req->id().strategy_type(), &type)) {
            resp->set_ok(false);
            resp->set_message("invalid arbitrage strategy_type");
            return grpc::Status::OK;
        }
        const bool ok =
            engine_.set_arbitrage_enabled(static_cast<int>(req->id().product_index()), type, false);
        resp->set_ok(ok);
        if (!ok) {
            resp->set_message("arbitrage strategy not configured");
        }
        OMM_LOG_INFO("grpc", "StopArbStrategy product={} type={}",
                     req->id().product_index(),
                     static_cast<int>(type));
        return grpc::Status::OK;
    }

    grpc::Status SetRiskThreshold(
            grpc::ServerContext* ctx,
            const omm::proto::SetRiskThresholdRequest* req,
            omm::proto::SetRiskThresholdResponse* resp) override
    {
        grpc::Status auth_status = authenticate(ctx, nullptr, nullptr);
        if (!auth_status.ok()) return auth_status;
        const auto& t = req->threshold();
        engine_.post_risk_mutable().set_limits(
            t.max_net_position(), t.max_delta(), t.max_gamma(), t.max_vega());
        engine_.persist_risk_limits_update(engine_.post_risk().limits());
        OMM_LOG_INFO("grpc", "SetRiskThreshold pos={} delta={} gamma={} vega={}",
                     t.max_net_position(), t.max_delta(), t.max_gamma(), t.max_vega());
        resp->set_ok(true);
        return grpc::Status::OK;
    }

    grpc::Status SendManualOrder(
            grpc::ServerContext* ctx,
            const omm::proto::ManualOrderRequest* req,
            omm::proto::ManualOrderResponse* resp) override
    {
        PersistedUser user{};
        grpc::Status auth_status = authenticate(ctx, &user, nullptr);
        if (!auth_status.ok()) return auth_status;
        Order o{};
        o.instrument_id   = static_cast<uint16_t>(req->instrument_id());
        o.side            = (req->side() == "sell") ? Side::Sell : Side::Buy;
        o.order_type      = OrderType::Limit;
        o.price           = req->price();
        o.volume          = req->volume();
        o.client_order_id = engine_.next_manual_order_id();
        o.send_ts         = get_monotonic_ns();
        o.is_manual       = true;
        o.is_hedge        = false;
        if (o.instrument_id < static_cast<uint16_t>(engine_.n_instruments())) {
            const Instrument& instr = engine_.instruments()[o.instrument_id];
            o.product_index = instr.product_index;
            o.exchange_id = instr.exchange_id;
        }
        const BookId requested_book = req->has_book_id()
            ? static_cast<BookId>(req->book_id())
            : user.default_book_id;
        const PersistedBook* book = find_book(requested_book);
        if (book == nullptr || !book->active || requested_book == INVALID_BOOK_ID) {
            resp->set_ok(false);
            resp->set_message("manual order requires a valid active book");
            return grpc::Status::OK;
        }
        o.book_id = requested_book;

        bool ok = engine_.submit_manual_order(o);
        OMM_LOG_INFO("grpc", "SendManualOrder instr={} side={} qty={} price={:.4f} ok={}",
                     o.instrument_id, req->side(), o.volume, o.price, (int)ok);
        resp->set_ok(ok);
        resp->set_order_id(o.client_order_id);
        resp->set_book_id(o.book_id);
        return grpc::Status::OK;
    }

    grpc::Status CancelOrder(
            grpc::ServerContext* ctx,
            const omm::proto::CancelOrderRequest* req,
            omm::proto::CancelOrderResponse* resp) override
    {
        grpc::Status auth_status = authenticate(ctx, nullptr, nullptr);
        if (!auth_status.ok()) return auth_status;
        bool ok = engine_.cancel_order(req->order_id(),
                                       static_cast<uint16_t>(req->instrument_id()));
        OMM_LOG_INFO("grpc", "CancelOrder order_id={} ok={}", req->order_id(), (int)ok);
        resp->set_ok(ok);
        return grpc::Status::OK;
    }

    grpc::Status CancelQuote(
            grpc::ServerContext* ctx,
            const omm::proto::CancelQuoteRequest* req,
            omm::proto::CancelQuoteResponse* resp) override
    {
        grpc::Status auth_status = authenticate(ctx, nullptr, nullptr);
        if (!auth_status.ok()) return auth_status;
        bool ok = engine_.cancel_quote(req->quote_id(),
                                       static_cast<uint16_t>(req->instrument_id()));
        OMM_LOG_INFO("grpc", "CancelQuote quote_id={} ok={}", req->quote_id(), (int)ok);
        resp->set_ok(ok);
        return grpc::Status::OK;
    }

    grpc::Status GetSnapshot(
            grpc::ServerContext* ctx,
            const omm::proto::SnapshotRequest*,
            omm::proto::SnapshotResponse* resp) override
    {
        PersistedUser user{};
        grpc::Status auth_status = authenticate(ctx, &user, nullptr);
        if (!auth_status.ok()) return auth_status;
        for (int i = 0; i < engine_.n_instruments(); ++i) {
            Greeks g{};
            if (!engine_.read_greeks_snapshot(static_cast<uint16_t>(i), &g)) continue;
            if (g.instrument_id == INVALID_INSTRUMENT_ID) continue;
            auto* pg = resp->add_greeks();
            pg->set_instrument_id(g.instrument_id);
            pg->set_theo_price(g.theo_price);
            pg->set_std_delta(g.std_delta);
            pg->set_delta(g.delta);
            pg->set_delta_cash(g.delta_cash);
            pg->set_std_gamma(g.std_gamma);
            pg->set_gamma(g.gamma);
            pg->set_gamma_cash(g.gamma_cash);
            pg->set_vega(g.vega);
            pg->set_vega_cash(g.vega_cash);
            pg->set_theta(g.theta);
            pg->set_theta_cash(g.theta_cash);
            pg->set_rho(g.rho);
            pg->set_rho_cash(g.rho_cash);
            pg->set_vanna(g.vanna);
            pg->set_volga(g.volga);
            pg->set_charm(g.charm);
            pg->set_iv(g.iv);
            pg->set_t(g.T);
            pg->set_calc_ts_ns(g.calc_ts_ns);
        }

        const auto* pos_snap = engine_.post_risk().positions();
        for (int i = 0; i < engine_.n_instruments(); ++i) {
            const auto& pos = pos_snap[i];
            if (pos.net_position == 0) continue;
            auto* pp = resp->add_positions();
            pp->set_instrument_id(static_cast<uint32_t>(i));
            pp->set_net_position(pos.net_position);
            pp->set_avg_price(pos.avg_long_price);
            pp->set_realized_pnl(pos.realized_pnl);
            pp->set_unrealized_pnl(0.0);
        }

        auto* pg = resp->mutable_portfolio();
        const auto& port = engine_.post_risk().portfolio_greeks();
        pg->set_total_delta(port.net_delta);
        pg->set_total_gamma(port.net_gamma);
        pg->set_total_vega(port.net_vega);
        pg->set_total_theta(port.net_theta);

        const auto& soft_limits = engine_.post_risk().limits();
        auto* risk_state = resp->mutable_risk_state();
        auto* threshold = risk_state->mutable_threshold();
        threshold->set_max_net_position(soft_limits.max_net_position);
        threshold->set_max_delta(soft_limits.max_delta);
        threshold->set_max_gamma(soft_limits.max_gamma);
        threshold->set_max_vega(soft_limits.max_vega);
        risk_state->set_position_breach(engine_.post_risk().position_breach());
        risk_state->set_delta_breach(engine_.post_risk().delta_breach());
        risk_state->set_gamma_breach(engine_.post_risk().gamma_breach());
        risk_state->set_vega_breach(engine_.post_risk().vega_breach());

        for (int i = 0; i < engine_.product_count(); ++i) {
            MMParamsConfig snap = engine_.mm_params(i).snapshot();
            auto* mp = resp->add_mm_params();
            mp->set_bid_spread(snap.bid_spread);
            mp->set_ask_spread(snap.ask_spread);
            mp->set_product_delta_threshold(snap.product_delta_threshold);
            mp->set_quote_volume(snap.quote_volume);
            mp->set_max_position(snap.max_position);
            mp->set_product_vega_threshold(snap.product_vega_threshold);
            mp->set_min_quote_interval_ms(snap.min_quote_interval_ms);
            mp->set_warning_position(snap.warning_position);
            mp->set_base_half_spread_ticks(snap.base_half_spread_ticks);
            mp->set_min_half_spread_ticks(snap.min_half_spread_ticks);
            mp->set_max_half_spread_ticks(snap.max_half_spread_ticks);
            mp->set_inventory_skew_per_lot_ticks(snap.inventory_skew_per_lot_ticks);
            mp->set_follow_weight(snap.follow_weight);
            mp->set_requote_price_epsilon_ticks(snap.requote_price_epsilon_ticks);
            mp->set_market_width_widen_threshold_ticks(snap.market_width_widen_threshold_ticks);
            mp->set_underlying_move_widen_threshold_ticks(
                snap.underlying_move_widen_threshold_ticks);
            mp->set_use_one_sided_at_limits(snap.use_one_sided_at_limits);
            mp->set_enabled(snap.enabled);

            populate_product_pricing(static_cast<uint32_t>(i),
                                     engine_.product_pricing(i),
                                     resp->add_product_pricing_params());
        }

        for (int i = 0; i < engine_.product_count(); ++i) {
            for (int slot = 0; slot < engine_.arbitrage_strategy_count(i); ++slot) {
                const ArbitrageStrategyType type = engine_.arbitrage_strategy_type(i, slot);
                if (type == ArbitrageStrategyType::None) continue;

                ArbParamsConfig arb_params{};
                if (engine_.arbitrage_params_snapshot(i, type, &arb_params)) {
                    auto* entry = resp->add_arb_params();
                    entry->set_product_index(i);
                    entry->set_strategy_type(arb_strategy_type_to_proto(type));
                    auto* params = entry->mutable_params();
                    params->set_min_edge_ticks(arb_params.min_edge_ticks);
                    params->set_cooldown_ms(arb_params.cooldown_ms);
                    params->set_scan_interval_ms(arb_params.scan_interval_ms);
                    params->set_cleanup_timeout_ms(arb_params.cleanup_timeout_ms);
                    params->set_max_order_volume(arb_params.max_order_volume);
                    params->set_max_live_orders(arb_params.max_live_orders);
                    params->set_cleanup_on_partial(arb_params.cleanup_on_partial);
                    params->set_enabled(arb_params.enabled);
                }

                ArbStrategyMonitorState arb_state{};
                if (engine_.arbitrage_strategy_state(i, type, &arb_state)) {
                    auto* state = resp->add_arb_strategy_states();
                    state->set_product_index(arb_state.product_index);
                    state->set_strategy_type(arb_strategy_type_to_proto(arb_state.strategy_type));
                    state->set_enabled(arb_state.enabled);
                    state->set_running(arb_state.running);
                    state->set_cleanup_active(arb_state.cleanup_active);
                    state->set_live_orders(arb_state.live_orders);
                    state->set_pair_count(arb_state.pair_count);
                    state->set_active_call_id(arb_state.active_call_id);
                    state->set_active_put_id(arb_state.active_put_id);
                    state->set_active_future_id(arb_state.active_future_id);
                    state->set_last_edge_ticks(arb_state.last_edge_ticks);
                    state->set_last_trigger_edge_ticks(arb_state.last_trigger_edge_ticks);
                    state->set_last_eval_ts_ns(arb_state.last_eval_ts_ns);
                    state->set_last_trigger_ts_ns(arb_state.last_trigger_ts_ns);
                    append_arb_reasons(arb_state.suppress_flags, state->mutable_reasons());
                }

                if (type == ArbitrageStrategyType::PCP) {
                    std::array<PCPPairMonitorState, MAX_INSTRUMENTS> pcp_rows{};
                    const int pcp_count = engine_.arbitrage_pcp_monitor_states(
                        i, type, pcp_rows.data(), MAX_INSTRUMENTS);
                    for (int row_idx = 0; row_idx < pcp_count; ++row_idx) {
                        const auto& row = pcp_rows[static_cast<std::size_t>(row_idx)];
                        auto* msg = resp->add_pcp_opportunities();
                        msg->set_product_index(row.product_index);
                        msg->set_strategy_type(arb_strategy_type_to_proto(row.strategy_type));
                        msg->set_call_id(row.call_id);
                        msg->set_put_id(row.put_id);
                        msg->set_future_id(row.future_id);
                        msg->set_expiry_date(row.expiry_date);
                        msg->set_strike(row.strike);
                        msg->set_market_valid(row.market_valid);
                        msg->set_selected(row.selected);
                        msg->set_discount_factor(row.discount_factor);
                        msg->set_synthetic_bid(row.synthetic_bid);
                        msg->set_synthetic_ask(row.synthetic_ask);
                        msg->set_future_bid(row.future_bid);
                        msg->set_future_ask(row.future_ask);
                        msg->set_long_synth_edge_ticks(row.long_synth_edge_ticks);
                        msg->set_short_synth_edge_ticks(row.short_synth_edge_ticks);
                        msg->set_best_edge_ticks(row.best_edge_ticks);
                        msg->set_best_direction(pcp_direction_to_proto(row.best_direction));
                        msg->set_best_volume(row.best_volume);
                        msg->set_eval_ts_ns(row.eval_ts_ns);
                    }
                }
            }
        }

        for (int i = 0; i < engine_.product_count(); ++i) {
            ProductMonitorState product_state{};
            if (!engine_.product_monitor_state(i, &product_state)) continue;

            auto* ps = resp->add_product_states();
            ps->set_product_index(product_state.product_index);
            ps->set_strategy_enabled(product_state.strategy_enabled);
            ps->set_session_open(product_state.session_open);
            ps->set_product_suppressed(product_state.product_suppressed);
            append_product_reasons(product_state, ps->mutable_reasons());

            std::array<InstrumentMonitorState, MAX_INSTRUMENTS> instrument_states{};
            const int instrument_count =
                engine_.instrument_monitor_states(i, instrument_states.data(), MAX_INSTRUMENTS);
            for (int j = 0; j < instrument_count; ++j) {
                const auto& state = instrument_states[static_cast<std::size_t>(j)];
                auto* is = resp->add_instrument_states();
                is->set_instrument_id(state.instrument_id);
                is->set_product_index(state.product_index);
                is->set_quote_state(quote_state_to_proto(state.quote_state));
                is->set_net_position(state.net_position);
                is->set_cancel_attempts(state.cancel_attempts);
                is->set_last_quote_ts_ns(state.last_quote_ts_ns);
                append_instrument_reasons(state.suppress_flags, is->mutable_reasons());
                append_product_reasons(product_state, is->mutable_reasons());
            }
        }

        for (int i = 0; i < engine_.n_instruments(); ++i) {
            const Instrument& instr = engine_.instruments()[i];
            if (instr.instrument_id == INVALID_INSTRUMENT_ID) continue;
            auto* pi = resp->add_instruments();
            pi->set_instrument_id(instr.instrument_id);
            pi->set_code(std::string(instr.code.view()));
            pi->set_underlying_code(std::string(instr.underlying_code.view()));
            pi->set_kind(instrument_kind_name(instr.kind));
            pi->set_option_type(option_type_name(instr));
            pi->set_strike(instr.strike);
            pi->set_product_index(instr.product_index);
            pi->set_underlying_id(instr.underlying_id);
            pi->set_exchange_id(std::string(instr.exchange_id.view()));
            pi->set_expiry_date(instr.expiry_date);
            pi->set_tick_size(instr.tick_size);
            pi->set_multiplier(instr.multiplier);
        }

        populate_user_info(user, resp->mutable_current_user());
        for (const auto& book : engine_.identity_state().books) {
            populate_book_info(book, resp->add_books());
        }

        std::vector<BookPosition> book_positions(
            engine_.identity_state().books.size()
            * static_cast<std::size_t>(std::max(1, engine_.n_instruments())));
        const int book_position_count =
            engine_.book_positions_snapshot(book_positions.data(),
                                            static_cast<int>(book_positions.size()));
        for (int i = 0; i < book_position_count; ++i) {
            const BookPosition& pos = book_positions[static_cast<std::size_t>(i)];
            auto* msg = resp->add_book_positions();
            msg->set_book_id(pos.book_id);
            msg->set_instrument_id(pos.instrument_id);
            msg->set_product_index(pos.product_index);
            msg->set_net_position(pos.net_position);
            msg->set_long_position(pos.long_position);
            msg->set_short_position(pos.short_position);
            msg->set_avg_long_price(pos.avg_long_price);
            msg->set_avg_short_price(pos.avg_short_price);
            msg->set_realized_pnl(pos.realized_pnl);
        }

        std::vector<BookPortfolioGreeks> book_portfolios(
            engine_.identity_state().books.size()
            * static_cast<std::size_t>(MAX_PRODUCTS + 1));
        const int book_portfolio_count =
            engine_.book_portfolios_snapshot(book_portfolios.data(),
                                             static_cast<int>(book_portfolios.size()));
        for (int i = 0; i < book_portfolio_count; ++i) {
            const BookPortfolioGreeks& portfolio = book_portfolios[static_cast<std::size_t>(i)];
            auto* msg = resp->add_book_portfolios();
            msg->set_book_id(portfolio.book_id);
            msg->set_product_index(portfolio.product_index);
            msg->set_total_delta(portfolio.net_delta);
            msg->set_total_gamma(portfolio.net_gamma);
            msg->set_total_vega(portfolio.net_vega);
            msg->set_total_theta(portfolio.net_theta);
            msg->set_realized_pnl(portfolio.pnl_realized);
            msg->set_unrealized_pnl(portfolio.pnl_unrealized);
            msg->set_calc_ts_ns(portfolio.calc_ts);
        }
        return grpc::Status::OK;
    }

    grpc::Status StreamGreeks(
            grpc::ServerContext* ctx,
            const omm::proto::StreamRequest* req,
            grpc::ServerWriter<omm::proto::Greeks>* writer) override
    {
        std::string token;
        grpc::Status auth_status = authenticate(ctx, nullptr, &token);
        if (!auth_status.ok()) return auth_status;
        while (!ctx->IsCancelled()) {
            if (!session_manager_.is_active(token)) {
                return grpc::Status(grpc::StatusCode::UNAUTHENTICATED, "session expired");
            }
            for (int i = 0; i < engine_.n_instruments(); ++i) {
                Greeks g{};
                if (!engine_.read_greeks_snapshot(static_cast<uint16_t>(i), &g)) continue;
                if (g.instrument_id == INVALID_INSTRUMENT_ID) continue;
                if (!instrument_in_scope(engine_, req->product_index(), g.instrument_id)) continue;
                omm::proto::Greeks msg;
                msg.set_instrument_id(g.instrument_id);
                msg.set_theo_price(g.theo_price);
                msg.set_std_delta(g.std_delta);
                msg.set_delta(g.delta);
                msg.set_delta_cash(g.delta_cash);
                msg.set_std_gamma(g.std_gamma);
                msg.set_gamma(g.gamma);
                msg.set_gamma_cash(g.gamma_cash);
                msg.set_vega(g.vega);
                msg.set_vega_cash(g.vega_cash);
                msg.set_theta(g.theta);
                msg.set_theta_cash(g.theta_cash);
                msg.set_rho(g.rho);
                msg.set_rho_cash(g.rho_cash);
                msg.set_vanna(g.vanna);
                msg.set_volga(g.volga);
                msg.set_charm(g.charm);
                msg.set_iv(g.iv);
                msg.set_t(g.T);
                msg.set_calc_ts_ns(g.calc_ts_ns);
                if (!writer->Write(msg)) return grpc::Status::OK;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        return grpc::Status::OK;
    }

    grpc::Status StreamPositions(
            grpc::ServerContext* ctx,
            const omm::proto::StreamRequest* req,
            grpc::ServerWriter<omm::proto::Position>* writer) override
    {
        std::string token;
        grpc::Status auth_status = authenticate(ctx, nullptr, &token);
        if (!auth_status.ok()) return auth_status;
        while (!ctx->IsCancelled()) {
            if (!session_manager_.is_active(token)) {
                return grpc::Status(grpc::StatusCode::UNAUTHENTICATED, "session expired");
            }
            const auto* pos_snap = engine_.post_risk().positions();
            for (int i = 0; i < engine_.n_instruments(); ++i) {
                const auto& pos = pos_snap[i];
                if (pos.net_position == 0) continue;
                if (!instrument_in_scope(engine_, req->product_index(), i)) continue;
                omm::proto::Position msg;
                msg.set_instrument_id(static_cast<uint32_t>(i));
                msg.set_net_position(pos.net_position);
                msg.set_avg_price(pos.avg_long_price);
                msg.set_realized_pnl(pos.realized_pnl);
                msg.set_unrealized_pnl(0.0);
                if (!writer->Write(msg)) return grpc::Status::OK;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        return grpc::Status::OK;
    }

    grpc::Status StreamRiskAlerts(
            grpc::ServerContext* ctx,
            const omm::proto::StreamRequest* req,
            grpc::ServerWriter<omm::proto::RiskAlert>* writer) override
    {
        std::string token;
        grpc::Status auth_status = authenticate(ctx, nullptr, &token);
        if (!auth_status.ok()) return auth_status;
        std::array<uint64_t, MAX_PRODUCTS> cursors{};
        for (int i = 0; i < engine_.product_count() && i < static_cast<int>(MAX_PRODUCTS); ++i) {
            cursors[i] = engine_.monitor_alerts(i).latest_seq();
        }
        bool sent_position_breach = false;
        bool sent_delta_breach = false;
        bool sent_gamma_breach = false;
        bool sent_vega_breach = false;
        while (!ctx->IsCancelled()) {
            if (!session_manager_.is_active(token)) {
                return grpc::Status(grpc::StatusCode::UNAUTHENTICATED, "session expired");
            }
            const auto& pr = engine_.post_risk();
            auto send = [&](omm::proto::RiskAlert::AlertType t, const char* msg) {
                omm::proto::RiskAlert alert;
                alert.set_type(t);
                alert.set_message(msg);
                alert.set_ts_ns(get_monotonic_ns());
                return writer->Write(alert);
            };
            const bool position_breach = pr.position_breach();
            const bool delta_breach = pr.delta_breach();
            const bool gamma_breach = pr.gamma_breach();
            const bool vega_breach = pr.vega_breach();

            if (position_breach && !sent_position_breach
                && !send(omm::proto::RiskAlert::POSITION_BREACH, "position limit breached")) {
                return grpc::Status::OK;
            }
            if (delta_breach && !sent_delta_breach
                && !send(omm::proto::RiskAlert::DELTA_BREACH, "delta limit breached")) {
                return grpc::Status::OK;
            }
            if (gamma_breach && !sent_gamma_breach
                && !send(omm::proto::RiskAlert::GAMMA_BREACH, "gamma limit breached")) {
                return grpc::Status::OK;
            }
            if (vega_breach && !sent_vega_breach
                && !send(omm::proto::RiskAlert::VEGA_BREACH, "vega limit breached")) {
                return grpc::Status::OK;
            }
            sent_position_breach = position_breach;
            sent_delta_breach = delta_breach;
            sent_gamma_breach = gamma_breach;
            sent_vega_breach = vega_breach;

            bool wrote_custom_alert = false;
            for (int i = 0; i < engine_.product_count() && i < static_cast<int>(MAX_PRODUCTS); ++i) {
                SystemAlert alert{};
                while (engine_.monitor_alerts(i).read_next(cursors[i], alert)) {
                    if (!product_in_scope(req->product_index(), alert.product_index)) continue;
                    omm::proto::RiskAlert msg;
                    msg.set_type(alert_type_to_proto(alert.type));
                    msg.set_message(alert.message);
                    msg.set_ts_ns(alert.ts_ns);
                    if (!writer->Write(msg)) return grpc::Status::OK;
                    wrote_custom_alert = true;
                }
            }
            if (!wrote_custom_alert
                && !position_breach && !delta_breach && !gamma_breach && !vega_breach) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                continue;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        return grpc::Status::OK;
    }

    grpc::Status StreamTicks(
            grpc::ServerContext* ctx,
            const omm::proto::StreamRequest* req,
            grpc::ServerWriter<omm::proto::Tick>* writer) override
    {
        std::string token;
        grpc::Status auth_status = authenticate(ctx, nullptr, &token);
        if (!auth_status.ok()) return auth_status;
        uint64_t cursor = engine_.monitor_ticks().latest_seq();
        TopOfBookTick tick{};
        while (!ctx->IsCancelled()) {
            if (!session_manager_.is_active(token)) {
                return grpc::Status(grpc::StatusCode::UNAUTHENTICATED, "session expired");
            }
            bool wrote = false;
            while (engine_.monitor_ticks().read_next(cursor, tick)) {
                if (!instrument_in_scope(engine_, req->product_index(), tick.instrument_id)) continue;
                omm::proto::Tick msg;
                populate_tick(tick, &msg);
                if (!writer->Write(msg)) return grpc::Status::OK;
                wrote = true;
            }
            if (!wrote) std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        return grpc::Status::OK;
    }

    grpc::Status StreamOrders(
            grpc::ServerContext* ctx,
            const omm::proto::StreamRequest* req,
            grpc::ServerWriter<omm::proto::OrderUpdate>* writer) override
    {
        std::string token;
        grpc::Status auth_status = authenticate(ctx, nullptr, &token);
        if (!auth_status.ok()) return auth_status;
        uint64_t cursor = engine_.monitor_orders().latest_seq();
        Order order{};
        while (!ctx->IsCancelled()) {
            if (!session_manager_.is_active(token)) {
                return grpc::Status(grpc::StatusCode::UNAUTHENTICATED, "session expired");
            }
            bool wrote = false;
            while (engine_.monitor_orders().read_next(cursor, order)) {
                if (!product_in_scope(req->product_index(), order.product_index)) continue;
                omm::proto::OrderUpdate msg;
                populate_order_update(engine_, order, &msg);
                if (!writer->Write(msg)) return grpc::Status::OK;
                wrote = true;
            }
            if (!wrote) std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        return grpc::Status::OK;
    }

    grpc::Status StreamQuotes(
            grpc::ServerContext* ctx,
            const omm::proto::StreamRequest* req,
            grpc::ServerWriter<omm::proto::QuoteUpdate>* writer) override
    {
        std::string token;
        grpc::Status auth_status = authenticate(ctx, nullptr, &token);
        if (!auth_status.ok()) return auth_status;
        uint64_t cursor = engine_.monitor_quotes().latest_seq();
        Quote quote{};
        while (!ctx->IsCancelled()) {
            if (!session_manager_.is_active(token)) {
                return grpc::Status(grpc::StatusCode::UNAUTHENTICATED, "session expired");
            }
            bool wrote = false;
            while (engine_.monitor_quotes().read_next(cursor, quote)) {
                if (!product_in_scope(req->product_index(), quote.product_index)) continue;
                omm::proto::QuoteUpdate msg;
                populate_quote_update(quote,
                                      quote.ack_ts != 0 ? "Acked" : "New",
                                      quote.ack_ts != 0 ? quote.ack_ts : quote.send_ts,
                                      &msg);
                if (!writer->Write(msg)) return grpc::Status::OK;
                wrote = true;
            }
            if (!wrote) std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        return grpc::Status::OK;
    }

    grpc::Status StreamTrades(
            grpc::ServerContext* ctx,
            const omm::proto::StreamRequest* req,
            grpc::ServerWriter<omm::proto::OrderUpdate>* writer) override
    {
        std::string token;
        grpc::Status auth_status = authenticate(ctx, nullptr, &token);
        if (!auth_status.ok()) return auth_status;
        uint64_t cursor = engine_.monitor_trades().latest_seq();
        Trade trade{};
        while (!ctx->IsCancelled()) {
            if (!session_manager_.is_active(token)) {
                return grpc::Status(grpc::StatusCode::UNAUTHENTICATED, "session expired");
            }
            bool wrote = false;
            while (engine_.monitor_trades().read_next(cursor, trade)) {
                if (!product_in_scope(req->product_index(), trade.product_index)) continue;
                omm::proto::OrderUpdate msg;
                populate_trade_update(trade, &msg);
                if (!writer->Write(msg)) return grpc::Status::OK;
                wrote = true;
            }
            if (!wrote) std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        return grpc::Status::OK;
    }

    grpc::Status StreamVolSurface(
            grpc::ServerContext* ctx,
            const omm::proto::StreamRequest* req,
            grpc::ServerWriter<omm::proto::VolSurface>* writer) override
    {
        std::string token;
        grpc::Status auth_status = authenticate(ctx, nullptr, &token);
        if (!auth_status.ok()) return auth_status;
        while (!ctx->IsCancelled()) {
            if (!session_manager_.is_active(token)) {
                return grpc::Status(grpc::StatusCode::UNAUTHENTICATED, "session expired");
            }
            for (int p = 0; p < engine_.product_count(); ++p) {
                if (!product_in_scope(req->product_index(), p)) continue;

                const IVolSurface* surf = nullptr;
                if (engine_.orc_wing_surface(p).get()->is_valid()) {
                    surf = engine_.orc_wing_surface(p).get();
                } else if (engine_.wing_surface(p).get()->is_valid()) {
                    surf = engine_.wing_surface(p).get();
                } else {
                    surf = engine_.svi_surface(p).get();
                }
                if (!surf || !surf->is_valid()) continue;

                double F = 0.0;
                for (int i = 0; i < engine_.n_instruments(); ++i) {
                    const Instrument& instr = engine_.instruments()[i];
                    if (instr.product_index != static_cast<uint8_t>(p) ||
                        instr.kind != InstrumentKind::Future) {
                        continue;
                    }
                    TopOfBookTick tick{};
                    if (!engine_.read_tick_snapshot(static_cast<uint16_t>(i), &tick)) continue;
                    F = tick.last_price;
                    if (F > 0.0) break;
                }
                if (F <= 0.0) F = 100.0;

                omm::proto::VolSurface msg;
                msg.set_product_index(static_cast<uint32_t>(p));
                msg.set_fit_ts_ns(get_monotonic_ns());

                std::map<int32_t, std::map<double, double>> vols_by_expiry;
                std::map<int32_t, double> expiry_t_by_date;
                for (uint16_t oi = 0; oi < engine_.option_count(p); ++oi) {
                    const uint16_t opt_id = engine_.option_id(p, oi);
                    const Instrument& opt = engine_.instruments()[opt_id];
                    const double T = engine_.option_time_to_expiry_years(opt);
                    expiry_t_by_date[opt.expiry_date] = T;
                    vols_by_expiry[opt.expiry_date][opt.strike] =
                        surf->get_vol_by_strike(F, opt.strike, T);
                }

                for (const auto& [expiry_date, vol_by_strike] : vols_by_expiry) {
                    if (vol_by_strike.size() < 2) continue;
                    auto* slice = msg.add_slices();
                    slice->set_expiry_t(expiry_t_by_date[expiry_date]);
                    for (const auto& [strike, vol] : vol_by_strike) {
                        slice->add_strikes(strike);
                        slice->add_vols(vol);
                    }
                }

                if (msg.slices_size() > 0 && !writer->Write(msg)) {
                    return grpc::Status::OK;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        return grpc::Status::OK;
    }

private:
    TradingEngine& engine_;
    SessionManager session_manager_{};

    [[nodiscard]] static std::string_view trim_bearer_prefix(std::string_view value) noexcept {
        constexpr std::string_view kBearer = "Bearer ";
        constexpr std::string_view kBearerLower = "bearer ";
        if (value.rfind(kBearer, 0) == 0) return value.substr(kBearer.size());
        if (value.rfind(kBearerLower, 0) == 0) return value.substr(kBearerLower.size());
        return {};
    }

    [[nodiscard]] std::optional<std::string> extract_token(grpc::ServerContext* ctx) const {
        if (ctx == nullptr) return std::nullopt;
        const auto& metadata = ctx->client_metadata();
        const auto it = metadata.find("authorization");
        if (it == metadata.end()) return std::nullopt;
        const std::string_view raw(it->second.data(), it->second.length());
        const std::string_view token = trim_bearer_prefix(raw);
        if (token.empty()) return std::nullopt;
        return std::string(token);
    }

    grpc::Status authenticate(grpc::ServerContext* ctx,
                              PersistedUser* out_user,
                              std::string* out_token) const {
        const std::optional<std::string> token = extract_token(ctx);
        if (!token.has_value()) {
            return grpc::Status(grpc::StatusCode::UNAUTHENTICATED, "missing bearer token");
        }
        PersistedUser user{};
        if (!session_manager_.get_session(*token, &user)) {
            return grpc::Status(grpc::StatusCode::UNAUTHENTICATED, "invalid session");
        }
        if (out_user != nullptr) *out_user = user;
        if (out_token != nullptr) *out_token = *token;
        return grpc::Status::OK;
    }

    [[nodiscard]] const PersistedUser* find_user(std::string_view username) const noexcept {
        for (const auto& user : engine_.identity_state().users) {
            if (username == user.username) return &user;
        }
        return nullptr;
    }

    [[nodiscard]] const PersistedBook* find_book(BookId book_id) const noexcept {
        for (const auto& book : engine_.identity_state().books) {
            if (book.book_id == book_id) return &book;
        }
        return nullptr;
    }

    [[nodiscard]] static bool password_matches(const PersistedUser& user,
                                               std::string_view password) noexcept {
        if (user.password_hash[0] == '\0') return false;
        if (password_hash_encoded(user.password_hash)) {
            return verify_password(password, user.password_hash);
        }
        return password == user.password_hash;
    }

    void set_enabled(int product_index, bool enabled) {
        if (product_index < 0) {
            for (int i = 0; i < engine_.product_count(); ++i) {
                engine_.mm_params(i).enabled.store(enabled, std::memory_order_release);
                engine_.persist_mm_params_update(i, engine_.mm_params(i).snapshot());
            }
        } else if (product_index < engine_.product_count()) {
            engine_.mm_params(product_index).enabled.store(enabled, std::memory_order_release);
            engine_.persist_mm_params_update(product_index,
                                             engine_.mm_params(product_index).snapshot());
        }
    }
};

GrpcMonitorServer::GrpcMonitorServer(const std::string& listen_addr,
                                     TradingEngine& engine)
    : listen_addr_(listen_addr), engine_(engine) {}

GrpcMonitorServer::~GrpcMonitorServer() { stop(); }

void GrpcMonitorServer::start() {
    auto svc = std::make_unique<TradingMonitorServiceImpl>(engine_);

    grpc::ServerBuilder builder;
    builder.AddListeningPort(listen_addr_, grpc::InsecureServerCredentials());
    builder.RegisterService(svc.release());
    server_ = builder.BuildAndStart();

    OMM_LOG_INFO("grpc", "gRPC server listening on {}", listen_addr_);
    running_.store(true, std::memory_order_release);
    server_thread_ = std::thread([this] {
        server_->Wait();
        running_.store(false, std::memory_order_release);
    });
}

void GrpcMonitorServer::stop() noexcept {
    if (server_) {
        OMM_LOG_INFO("grpc", "gRPC server shutting down");
        server_->Shutdown();
        if (server_thread_.joinable()) server_thread_.join();
        server_.reset();
    }
}

} // namespace omm
