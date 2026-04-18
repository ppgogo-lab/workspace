#include "monitoring/grpc_server.h"
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
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace omm {

namespace {

constexpr uint32_t kAllProducts = 0xFFu;
constexpr double kNsPerYear = 365.0 * 24.0 * 3600.0 * 1e9;
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
}

void populate_tick(const MarketTick& tick, omm::proto::Tick* msg) {
    msg->set_instrument_id(tick.instrument_id);
    msg->set_last_price(tick.last_price);
    msg->set_bid_price(tick.bid_price[0]);
    msg->set_ask_price(tick.ask_price[0]);
    msg->set_bid_volume(tick.bid_volume[0]);
    msg->set_ask_volume(tick.ask_volume[0]);
    msg->set_exchange_ts_ns(tick.exchange_ts_ns);
    msg->set_recv_ts_ns(tick.recv_ts_ns);
}

double current_expiry_t(const Instrument& opt) noexcept {
    double T = (opt.expiry_epoch_ns - get_monotonic_ns()) / kNsPerYear;
    return std::max(1e-4, T);
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
    case StrategyQuoteMonitorState::ReplacePending:
        return omm::proto::MM_QUOTE_REPLACE_PENDING;
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
                       omm::proto::MMSuppressReason reason) {
    if (reasons == nullptr) return;
    for (const int existing : *reasons) {
        if (existing == static_cast<int>(reason)) return;
    }
    reasons->Add(static_cast<int>(reason));
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

} // namespace

class TradingMonitorServiceImpl final : public omm::proto::TradingMonitor::Service {
public:
    explicit TradingMonitorServiceImpl(TradingEngine& engine) : engine_(engine) {}

    grpc::Status SetStrategyParams(
            grpc::ServerContext*,
            const omm::proto::SetStrategyParamsRequest* req,
            omm::proto::SetStrategyParamsResponse* resp) override
    {
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
        if (p.has_hedge_delta_threshold()) snap.hedge_delta_threshold = p.hedge_delta_threshold();
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
        OMM_LOG_INFO(
            "grpc",
            "SetStrategyParams product={} qv={} max_pos={} warn_pos={} base_half={} hedge_delta={} vega={} enabled={}",
            idx,
            snap.quote_volume,
            snap.max_position,
            snap.warning_position,
            snap.base_half_spread_ticks,
            snap.hedge_delta_threshold,
            snap.product_vega_threshold,
            (int)snap.enabled);
        resp->set_ok(true);
        return grpc::Status::OK;
    }

    grpc::Status StartStrategy(
            grpc::ServerContext*,
            const omm::proto::StartStopRequest* req,
            omm::proto::StartStopResponse* resp) override
    {
        set_enabled(req->product_index(), true);
        OMM_LOG_INFO("grpc", "StartStrategy product={}", req->product_index());
        resp->set_ok(true);
        return grpc::Status::OK;
    }

    grpc::Status StopStrategy(
            grpc::ServerContext*,
            const omm::proto::StartStopRequest* req,
            omm::proto::StartStopResponse* resp) override
    {
        set_enabled(req->product_index(), false);
        OMM_LOG_INFO("grpc", "StopStrategy product={}", req->product_index());
        resp->set_ok(true);
        return grpc::Status::OK;
    }

    grpc::Status SetRiskThreshold(
            grpc::ServerContext*,
            const omm::proto::SetRiskThresholdRequest* req,
            omm::proto::SetRiskThresholdResponse* resp) override
    {
        const auto& t = req->threshold();
        engine_.post_risk_mutable().set_limits(
            t.max_net_position(), t.max_delta(), t.max_gamma(), t.max_vega());
        OMM_LOG_INFO("grpc", "SetRiskThreshold pos={} delta={} gamma={} vega={}",
                     t.max_net_position(), t.max_delta(), t.max_gamma(), t.max_vega());
        resp->set_ok(true);
        return grpc::Status::OK;
    }

    grpc::Status SendManualOrder(
            grpc::ServerContext*,
            const omm::proto::ManualOrderRequest* req,
            omm::proto::ManualOrderResponse* resp) override
    {
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

        bool ok = engine_.submit_manual_order(o);
        OMM_LOG_INFO("grpc", "SendManualOrder instr={} side={} qty={} price={:.4f} ok={}",
                     o.instrument_id, req->side(), o.volume, o.price, (int)ok);
        resp->set_ok(ok);
        resp->set_order_id(o.client_order_id);
        return grpc::Status::OK;
    }

    grpc::Status CancelOrder(
            grpc::ServerContext*,
            const omm::proto::CancelOrderRequest* req,
            omm::proto::CancelOrderResponse* resp) override
    {
        bool ok = engine_.cancel_order(req->order_id(),
                                       static_cast<uint16_t>(req->instrument_id()));
        OMM_LOG_INFO("grpc", "CancelOrder order_id={} ok={}", req->order_id(), (int)ok);
        resp->set_ok(ok);
        return grpc::Status::OK;
    }

    grpc::Status GetSnapshot(
            grpc::ServerContext*,
            const omm::proto::SnapshotRequest*,
            omm::proto::SnapshotResponse* resp) override
    {
        const auto* greeks_snap = engine_.greeks_snapshot();
        for (int i = 0; i < engine_.n_instruments(); ++i) {
            const auto& g = greeks_snap[i];
            if (g.instrument_id == INVALID_INSTRUMENT_ID) continue;
            auto* pg = resp->add_greeks();
            pg->set_instrument_id(g.instrument_id);
            pg->set_theo_price(g.theo_price);
            pg->set_delta(g.delta);
            pg->set_gamma(g.gamma);
            pg->set_vega(g.vega);
            pg->set_theta(g.theta);
            pg->set_rho(g.rho);
            pg->set_iv(g.iv);
            pg->set_t(g.T);
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
            mp->set_hedge_delta_threshold(snap.hedge_delta_threshold);
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
        }
        return grpc::Status::OK;
    }

    grpc::Status StreamGreeks(
            grpc::ServerContext* ctx,
            const omm::proto::StreamRequest* req,
            grpc::ServerWriter<omm::proto::Greeks>* writer) override
    {
        while (!ctx->IsCancelled()) {
            const auto* snap = engine_.greeks_snapshot();
            for (int i = 0; i < engine_.n_instruments(); ++i) {
                const auto& g = snap[i];
                if (g.instrument_id == INVALID_INSTRUMENT_ID) continue;
                if (!instrument_in_scope(engine_, req->product_index(), g.instrument_id)) continue;
                omm::proto::Greeks msg;
                msg.set_instrument_id(g.instrument_id);
                msg.set_theo_price(g.theo_price);
                msg.set_delta(g.delta);
                msg.set_gamma(g.gamma);
                msg.set_vega(g.vega);
                msg.set_theta(g.theta);
                msg.set_rho(g.rho);
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
        while (!ctx->IsCancelled()) {
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
        std::array<uint64_t, MAX_PRODUCTS> cursors{};
        for (int i = 0; i < engine_.product_count() && i < static_cast<int>(MAX_PRODUCTS); ++i) {
            cursors[i] = engine_.monitor_alerts(i).latest_seq();
        }
        bool sent_position_breach = false;
        bool sent_delta_breach = false;
        bool sent_gamma_breach = false;
        bool sent_vega_breach = false;
        while (!ctx->IsCancelled()) {
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
        uint64_t cursor = engine_.monitor_ticks().latest_seq();
        MarketTick tick{};
        while (!ctx->IsCancelled()) {
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
        uint64_t cursor = engine_.monitor_orders().latest_seq();
        Order order{};
        while (!ctx->IsCancelled()) {
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
        uint64_t cursor = engine_.monitor_quotes().latest_seq();
        Quote quote{};
        while (!ctx->IsCancelled()) {
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
        uint64_t cursor = engine_.monitor_trades().latest_seq();
        Trade trade{};
        while (!ctx->IsCancelled()) {
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
        while (!ctx->IsCancelled()) {
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
                    F = engine_.tick_snapshot()[i].last_price;
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
                    const double T = current_expiry_t(opt);
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

    void set_enabled(int product_index, bool enabled) {
        if (product_index < 0) {
            for (int i = 0; i < engine_.product_count(); ++i)
                engine_.mm_params(i).enabled.store(enabled, std::memory_order_release);
        } else if (product_index < engine_.product_count()) {
            engine_.mm_params(product_index).enabled.store(enabled, std::memory_order_release);
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
