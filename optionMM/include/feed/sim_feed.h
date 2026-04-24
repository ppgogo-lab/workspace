#pragma once

#include "feed/feed_handler.h"
#include "common/config.h"

#include <thread>

namespace omm {

class SimGateway;

class SimFeedHandler : public IFeedHandler {
public:
    SimFeedHandler(const SimConfig& cfg,
                   SPSCRingBuffer<TopOfBookTick, 1024>* tick_buf,
                   SimGateway* sim_gateway) noexcept
        : IFeedHandler(tick_buf), cfg_(cfg), sim_gateway_(sim_gateway) {}

    void start() override;
    void stop() override;

    [[nodiscard]] bool is_connected() const noexcept override {
        return connected_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] uint64_t message_count() const noexcept override {
        return msg_count_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] uint64_t error_count() const noexcept override {
        return err_count_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] uint64_t dropped_count() const noexcept override {
        return dropped_count_.load(std::memory_order_relaxed);
    }

    ~SimFeedHandler() override { stop(); }

private:
    SimConfig   cfg_;
    SimGateway* sim_gateway_{nullptr};
    std::thread thread_;

    void run_loop() noexcept;
};

} // namespace omm
