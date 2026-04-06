#pragma once

#include "feed/feed_handler.h"
#include "common/config.h"
#include <thread>

namespace omm {

// ─── MulticastFeedHandler ─────────────────────────────────────────────────────
// Receives market data via UDP multicast using recvmmsg for batched receives.
// If OpenOnload is available (detected at runtime via dlopen), the socket is
// accelerated via kernel bypass.
//
// Hot loop: pure spin on recvmmsg — no epoll, no blocking.
// Gap detection: tracks sequence_no per instrument; logs gaps via async spdlog.

class MulticastFeedHandler : public IFeedHandler {
public:
    MulticastFeedHandler(const MulticastConfig& cfg,
                         SPSCRingBuffer<MarketTick, 1024>* tick_buf) noexcept
        : IFeedHandler(tick_buf), cfg_(cfg) {}

    void start() override;
    void stop()  override;

    [[nodiscard]] bool     is_connected()  const noexcept override {
        return connected_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] uint64_t message_count() const noexcept override {
        return msg_count_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] uint64_t error_count()   const noexcept override {
        return err_count_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] uint64_t dropped_count() const noexcept override {
        return dropped_count_.load(std::memory_order_relaxed);
    }

    ~MulticastFeedHandler() override { stop(); }

private:
    MulticastConfig cfg_;
    std::thread     thread_;
    int             sock_{-1};

    void run_loop() noexcept;
    bool setup_socket() noexcept;
    void decode_and_push(const uint8_t* buf, int len,
                          int64_t recv_ts_ns) noexcept;
};

} // namespace omm
