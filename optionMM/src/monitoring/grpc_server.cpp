#include "monitoring/grpc_server.h"
#include "engine/trading_engine.h"
#include "strategy/mm_params.h"
#include "common/config.h"
#include "logger/logger.h"

#include "proto/gen/trading.grpc.pb.h"
#include "proto/gen/trading.pb.h"

#include <grpcpp/grpcpp.h>
#include <grpcpp/server_builder.h>

#include <chrono>

namespace omm {

// ─── Service implementation ───────────────────────────────────────────────────

class TradingMonitorServiceImpl final : public omm::proto::TradingMonitor::Service {
public:
    explicit TradingMonitorServiceImpl(TradingEngine& engine) : engine_(engine) {}

    // ── Control RPCs ─────────────────────────────────────────────────────────

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
        MMParamsConfig snap{};
        snap.bid_spread            = p.bid_spread();
        snap.ask_spread            = p.ask_spread();
        snap.hedge_delta_threshold = p.hedge_delta_threshold();
        snap.quote_volume          = p.quote_volume();
        snap.max_position          = p.max_position();
        snap.enabled               = p.enabled();
        engine_.mm_params(idx).apply(snap);
        OMM_LOG_INFO("grpc", "SetStrategyParams product={} bid={} ask={} enabled={}",
                     idx, snap.bid_spread, snap.ask_spread, (int)snap.enabled);
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
        o.is_manual       = true;
        o.is_hedge        = false;

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
        }

        auto* pg = resp->mutable_portfolio();
        const auto& port = engine_.post_risk().portfolio_greeks();
        pg->set_total_delta(port.net_delta);
        pg->set_total_gamma(port.net_gamma);
        pg->set_total_vega(port.net_vega);
        pg->set_total_theta(port.net_theta);

        for (int i = 0; i < engine_.product_count(); ++i) {
            MMParamsConfig snap = engine_.mm_params(i).snapshot();
            auto* mp = resp->add_mm_params();
            mp->set_bid_spread(snap.bid_spread);
            mp->set_ask_spread(snap.ask_spread);
            mp->set_hedge_delta_threshold(snap.hedge_delta_threshold);
            mp->set_quote_volume(snap.quote_volume);
            mp->set_max_position(snap.max_position);
            mp->set_enabled(snap.enabled);
        }
        return grpc::Status::OK;
    }

    // ── Streaming RPCs ────────────────────────────────────────────────────────

    grpc::Status StreamGreeks(
            grpc::ServerContext* ctx,
            const omm::proto::StreamRequest*,
            grpc::ServerWriter<omm::proto::Greeks>* writer) override
    {
        while (!ctx->IsCancelled()) {
            const auto* snap = engine_.greeks_snapshot();
            for (int i = 0; i < engine_.n_instruments(); ++i) {
                const auto& g = snap[i];
                if (g.instrument_id == INVALID_INSTRUMENT_ID) continue;
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
            const omm::proto::StreamRequest*,
            grpc::ServerWriter<omm::proto::Position>* writer) override
    {
        while (!ctx->IsCancelled()) {
            const auto* pos_snap = engine_.post_risk().positions();
            for (int i = 0; i < engine_.n_instruments(); ++i) {
                const auto& pos = pos_snap[i];
                if (pos.net_position == 0) continue;
                omm::proto::Position msg;
                msg.set_instrument_id(static_cast<uint32_t>(i));
                msg.set_net_position(pos.net_position);
                msg.set_avg_price(pos.avg_long_price);
                msg.set_realized_pnl(pos.realized_pnl);
                if (!writer->Write(msg)) return grpc::Status::OK;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        return grpc::Status::OK;
    }

    grpc::Status StreamRiskAlerts(
            grpc::ServerContext* ctx,
            const omm::proto::StreamRequest*,
            grpc::ServerWriter<omm::proto::RiskAlert>* writer) override
    {
        while (!ctx->IsCancelled()) {
            const auto& pr = engine_.post_risk();
            auto send = [&](omm::proto::RiskAlert::AlertType t, const char* msg) {
                omm::proto::RiskAlert alert;
                alert.set_type(t);
                alert.set_message(msg);
                alert.set_ts_ns(0);
                (void)writer->Write(alert);
            };
            if (pr.position_breach()) send(omm::proto::RiskAlert::POSITION_BREACH, "position limit breached");
            if (pr.delta_breach())    send(omm::proto::RiskAlert::DELTA_BREACH,    "delta limit breached");
            if (pr.gamma_breach())    send(omm::proto::RiskAlert::GAMMA_BREACH,    "gamma limit breached");
            if (pr.vega_breach())     send(omm::proto::RiskAlert::VEGA_BREACH,     "vega limit breached");
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        return grpc::Status::OK;
    }

    // Tick/Order/Quote/VolSurface streams: stubbed pending fan-out infrastructure
    grpc::Status StreamTicks(grpc::ServerContext* ctx, const omm::proto::StreamRequest*,
            grpc::ServerWriter<omm::proto::Tick>*) override {
        while (!ctx->IsCancelled()) std::this_thread::sleep_for(std::chrono::seconds(1));
        return grpc::Status::OK;
    }
    grpc::Status StreamOrders(grpc::ServerContext* ctx, const omm::proto::StreamRequest*,
            grpc::ServerWriter<omm::proto::OrderUpdate>*) override {
        while (!ctx->IsCancelled()) std::this_thread::sleep_for(std::chrono::seconds(1));
        return grpc::Status::OK;
    }
    grpc::Status StreamQuotes(grpc::ServerContext* ctx, const omm::proto::StreamRequest*,
            grpc::ServerWriter<omm::proto::QuoteUpdate>*) override {
        while (!ctx->IsCancelled()) std::this_thread::sleep_for(std::chrono::seconds(1));
        return grpc::Status::OK;
    }
    grpc::Status StreamVolSurface(grpc::ServerContext* ctx, const omm::proto::StreamRequest*,
            grpc::ServerWriter<omm::proto::VolSurface>*) override {
        while (!ctx->IsCancelled()) std::this_thread::sleep_for(std::chrono::seconds(1));
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

// ─── GrpcMonitorServer ────────────────────────────────────────────────────────

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
