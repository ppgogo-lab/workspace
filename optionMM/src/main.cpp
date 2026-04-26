#include "common/config.h"
#include "common/types.h"
#include "engine/trading_engine.h"
#include "monitoring/grpc_server.h"
#include "logger/logger.h"
#include "gateway/sim_gateway.h"
#include "gateway/femas_gateway.h"
#include "feed/multicast_feed.h"
#include "feed/fpga_feed.h"
#include "feed/femas_feed.h"
#include "feed/sim_feed.h"
#include "sim/sim_instruments.h"

#include <csignal>
#include <cstdio>
#include <cstring>
#include <array>
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

    auto cfg = std::make_unique<omm::SystemConfig>();
    try {
        *cfg = omm::load_config(argv[1]);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Config error: %s\n", e.what());
        return 1;
    }

    omm::OmmLogger::init("logs/optionmm.log", /*stdout=*/true);
    OMM_LOG_INFO("startup", "optionMM starting");

    const bool sim_mode = cfg->feed.type == omm::FeedType::Sim
                       || cfg->gateway.type == omm::GatewayType::Sim;
    if (sim_mode && cfg->pricing.vol_method != omm::VolMethod::OrcWing) {
        OMM_LOG_WARN("startup", "sim mode forcing pricing.vol_surface.method=orcWing");
        cfg->pricing.vol_method = omm::VolMethod::OrcWing;
    }
    if ((cfg->feed.type == omm::FeedType::Sim) != (cfg->gateway.type == omm::GatewayType::Sim)) {
        std::fprintf(stderr, "Config error: feed.type=sim and gateway.type=sim must be used together\n");
        return 1;
    }

    // Select gateway from config
    std::unique_ptr<omm::IGateway> gw;
    omm::SimGateway* sim_gw = nullptr;
    switch (cfg->gateway.type) {
        case omm::GatewayType::FEMAS:
            gw = std::make_unique<omm::FEMASGateway>();
            OMM_LOG_INFO("startup", "gateway: FEMAS front={}", cfg->gateway.femas.front_addr);
            break;
        case omm::GatewayType::Sim: {
            auto sim_gateway = std::make_unique<omm::SimGateway>();
            sim_gateway->set_sim_config(cfg->sim);
            auto sim_instruments = std::make_unique<std::array<omm::Instrument, omm::MAX_INSTRUMENTS>>();
            const uint16_t sim_count =
                omm::build_sim_instruments(*cfg, sim_instruments->data(), omm::MAX_INSTRUMENTS);
            for (uint16_t i = 0; i < sim_count; ++i) {
                sim_gateway->add_instrument((*sim_instruments)[i]);
            }
            OMM_LOG_INFO("startup", "gateway: sim instruments={}", sim_count);
            sim_gw = sim_gateway.get();
            gw = std::move(sim_gateway);
            break;
        }
    }
    if (!gw->connect(cfg->gateway)) {
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
    switch (cfg->feed.type) {
        case omm::FeedType::Multicast:
            feed = std::make_unique<omm::MulticastFeedHandler>(cfg->feed.multicast, nullptr);
            OMM_LOG_INFO("startup", "feed: multicast iface={}", cfg->feed.multicast.interface);
            break;
        case omm::FeedType::FPGA:
            feed = std::make_unique<omm::FPGAFeedHandler>(cfg->feed.fpga, nullptr);
            OMM_LOG_INFO("startup", "feed: FPGA device={}", cfg->feed.fpga.device_path);
            break;
        case omm::FeedType::FEMAS:
            fill_if_empty(cfg->feed.femas.front_addr, sizeof(cfg->feed.femas.front_addr),
                          cfg->gateway.femas.front_addr);
            fill_if_empty(cfg->feed.femas.broker_id, sizeof(cfg->feed.femas.broker_id),
                          cfg->gateway.femas.broker_id);
            fill_if_empty(cfg->feed.femas.user_id, sizeof(cfg->feed.femas.user_id),
                          cfg->gateway.femas.user_id);
            fill_if_empty(cfg->feed.femas.password, sizeof(cfg->feed.femas.password),
                          cfg->gateway.femas.password);

            feed = std::make_unique<omm::FEMASFeedHandler>(cfg->feed.femas, nullptr);
            OMM_LOG_INFO("startup", "feed: FEMAS front={} topic_id={}",
                         cfg->feed.femas.front_addr, cfg->feed.femas.topic_id);
            break;
        case omm::FeedType::Sim:
            feed = std::make_unique<omm::SimFeedHandler>(cfg->sim, nullptr, sim_gw);
            OMM_LOG_INFO("startup", "feed: sim tick_interval_ms={} seed={}",
                         cfg->sim.tick_interval_ms, cfg->sim.random_seed);
            break;
    }

    auto engine = std::make_unique<omm::TradingEngine>(*cfg, std::move(gw), std::move(feed));

    std::string grpc_addr = cfg->monitoring.grpc_listen_addr;
    auto monitor = std::make_unique<omm::GrpcMonitorServer>(grpc_addr, *engine);

    std::signal(SIGINT,  sig_handler);
    std::signal(SIGTERM, sig_handler);

    engine->start();
    monitor->start();

    std::fprintf(stdout, "optionMM running. gRPC monitor on %s\n", grpc_addr.c_str());

    while (!g_stop.load(std::memory_order_relaxed)) {
        struct timespec ts{0, 50'000'000};  // 50ms
        nanosleep(&ts, nullptr);
    }

    monitor->stop();
    engine->stop();
    OMM_LOG_INFO("shutdown", "optionMM stopped");
    omm::OmmLogger::shutdown();
    return 0;
}
