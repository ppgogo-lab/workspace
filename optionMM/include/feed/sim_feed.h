#pragma once

#include "feed/feed_handler.h"
#include "common/config.h"

#include <thread>

namespace omm {

class SimGateway;

class SimFeedHandler : public IFeedHandler {
public:
    /**
     * @brief SimFeedHandler.
     * @param cfg Parameter supplied by the caller.
     * @param tick_buf Parameter supplied by the caller.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    SimFeedHandler(const SimConfig& cfg,
                   SPSCRingBuffer<TopOfBookTick, 1024>* tick_buf,
                   SimGateway* sim_gateway) noexcept
        : IFeedHandler(tick_buf), cfg_(cfg), sim_gateway_(sim_gateway) {}

    /**
     * @brief Start.
     * @return None.
     */
    void start() override;
    /**
     * @brief Stop.
     * @return None.
     */
    void stop() override;

    /**
     * @brief Is connected.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] bool is_connected() const noexcept override {
        return connected_.load(std::memory_order_relaxed);
    }
    /**
     * @brief Message count.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] uint64_t message_count() const noexcept override {
        return msg_count_.load(std::memory_order_relaxed);
    }
    /**
     * @brief Error count.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] uint64_t error_count() const noexcept override {
        return err_count_.load(std::memory_order_relaxed);
    }
    /**
     * @brief Dropped count.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] uint64_t dropped_count() const noexcept override {
        return dropped_count_.load(std::memory_order_relaxed);
    }

    /**
     * @brief SimFeedHandler.
     * @return None.
     */
    ~SimFeedHandler() override { stop(); }

private:
    SimConfig   cfg_;
    SimGateway* sim_gateway_{nullptr};
    std::thread thread_;

    void run_loop() noexcept;
};

} // namespace omm
