#include "common/config.h"
#include "common/types.h"
#include "engine/trading_engine.h"
#include "monitoring/grpc_server.h"
#include "logger/logger.h"
#include "gateway/sim_gateway.h"
#include "gateway/ctp_gateway.h"
#include "gateway/femas_gateway.h"
#include "feed/multicast_feed.h"
#include "feed/fpga_feed.h"
#include "feed/femas_feed.h"

#include <csignal>
#include <cstdio>
#include <cstring>
#include <memory>

namespace {
std::atomic<bool> g_stop{false};
void sig_handler(int) { g_stop.store(true, std::memory_order_relaxed); }
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::fprintf(stderr, "Usage: optionmm <config.yaml>\n");
        return 1;
    }

    omm::SystemConfig cfg;
    try {
        cfg = omm::load_config(argv[1]);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Config error: %s\n", e.what());
        return 1;
    }

    omm::OmmLogger::init("logs/optionmm.log", /*stdout=*/true);
    OMM_LOG_INFO("startup", "optionMM starting");

    // Select gateway from config
    std::unique_ptr<omm::IGateway> gw;
    switch (cfg.gateway.type) {
        case omm::GatewayType::CTP:
            gw = std::make_unique<omm::CTPGateway>();
            OMM_LOG_INFO("startup", "gateway: CTP front={}", cfg.gateway.ctp.front_addr);
            break;
        case omm::GatewayType::FEMAS:
            gw = std::make_unique<omm::FEMASGateway>();
            OMM_LOG_INFO("startup", "gateway: FEMAS front={}", cfg.gateway.femas.front_addr);
            break;
    }
    if (!gw->connect(cfg.gateway)) {
        std::fprintf(stderr, "Gateway connect failed\n");
        return 1;
    }

    // Select feed handler from config (tick_buf wired by TradingEngine constructor)
    std::unique_ptr<omm::IFeedHandler> feed;
    auto fill_if_empty = [](char* dst, std::size_t dst_size, const char* src) {
        if (dst[0] != '\0' || !src) return;
        std::strncpy(dst, src, dst_size - 1);
        dst[dst_size - 1] = '\0';
    };
    switch (cfg.feed.type) {
        case omm::FeedType::Multicast:
            feed = std::make_unique<omm::MulticastFeedHandler>(cfg.feed.multicast, nullptr);
            OMM_LOG_INFO("startup", "feed: multicast iface={}", cfg.feed.multicast.interface);
            break;
        case omm::FeedType::FPGA:
            feed = std::make_unique<omm::FPGAFeedHandler>(cfg.feed.fpga, nullptr);
            OMM_LOG_INFO("startup", "feed: FPGA device={}", cfg.feed.fpga.device_path);
            break;
        case omm::FeedType::FEMAS:
            fill_if_empty(cfg.feed.femas.front_addr, sizeof(cfg.feed.femas.front_addr),
                          cfg.gateway.femas.front_addr);
            fill_if_empty(cfg.feed.femas.broker_id, sizeof(cfg.feed.femas.broker_id),
                          cfg.gateway.femas.broker_id);
            fill_if_empty(cfg.feed.femas.user_id, sizeof(cfg.feed.femas.user_id),
                          cfg.gateway.femas.user_id);
            fill_if_empty(cfg.feed.femas.password, sizeof(cfg.feed.femas.password),
                          cfg.gateway.femas.password);

            feed = std::make_unique<omm::FEMASFeedHandler>(cfg.feed.femas, nullptr);
            OMM_LOG_INFO("startup", "feed: FEMAS front={} topic_id={}",
                         cfg.feed.femas.front_addr, cfg.feed.femas.topic_id);
            break;
    }

    omm::TradingEngine engine(cfg, std::move(gw), std::move(feed));

    std::string grpc_addr = cfg.monitoring.grpc_listen_addr;
    omm::GrpcMonitorServer monitor(grpc_addr, engine);

    std::signal(SIGINT,  sig_handler);
    std::signal(SIGTERM, sig_handler);

    engine.start();
    monitor.start();

    std::fprintf(stdout, "optionMM running. gRPC monitor on %s\n", grpc_addr.c_str());

    while (!g_stop.load(std::memory_order_relaxed)) {
        struct timespec ts{0, 50'000'000};  // 50ms
        nanosleep(&ts, nullptr);
    }

    monitor.stop();
    engine.stop();
    OMM_LOG_INFO("shutdown", "optionMM stopped");
    omm::OmmLogger::shutdown();
    return 0;
}
