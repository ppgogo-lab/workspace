#pragma once

#include "common/types.h"
#include "common/config.h"
#include "risk/post_trade_risk.h"
#include "strategy/mm_params.h"

#include <thread>
#include <atomic>
#include <memory>
#include <string>

// Forward declarations to avoid pulling grpc headers into every TU
namespace grpc { class Server; }

namespace omm {

class TradingEngine;  // forward

// ─── GrpcMonitorServer ────────────────────────────────────────────────────────
// Runs a gRPC server on a dedicated side thread.
// Exposes streaming RPCs (Greeks, positions, ticks, risk alerts) and
// unary control RPCs (SetStrategyParams, Start/Stop, SetRiskThreshold, etc.).
//
// Thread model:
//   - start() spawns one thread that calls grpc::Server::Wait().
//   - gRPC completion queue threads handle individual RPCs.
//   - All writes to TradingEngine state go through AtomicMMParams (release store)
//     or PostTradeRisk (atomic breach flags) — no locks on the critical path.
//
// Usage:
//   GrpcMonitorServer srv("0.0.0.0:50051", engine);
//   srv.start();
//   ...
//   srv.stop();

class GrpcMonitorServer {
public:
    GrpcMonitorServer(const std::string& listen_addr, TradingEngine& engine);
    ~GrpcMonitorServer();

    // Non-copyable
    GrpcMonitorServer(const GrpcMonitorServer&)            = delete;
    GrpcMonitorServer& operator=(const GrpcMonitorServer&) = delete;

    void start();
    void stop() noexcept;

    [[nodiscard]] bool is_running() const noexcept {
        return running_.load(std::memory_order_relaxed);
    }

private:
    std::string      listen_addr_;
    TradingEngine&   engine_;

    std::unique_ptr<grpc::Server> server_;
    std::thread                   server_thread_;
    std::atomic<bool>             running_{false};
};

} // namespace omm
