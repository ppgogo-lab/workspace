#pragma once

#include "feed/feed_handler.h"
#include "common/config.h"
#include <thread>

namespace omm {

// ─── FPGAFeedHandler ──────────────────────────────────────────────────────────
// Receives hardware-timestamped market data from an FPGA via memory-mapped I/O.
// The FPGA DMA engine writes fixed-size records into a shared memory region and
// advances a sequence counter. The feed thread spins on the counter.
//
// Device interface:
//   - mmap /dev/fpgaX → shared ring buffer of FPGARecord structs
//   - First 64 bytes: control block (write_seq, read_seq, record_size, capacity)
//   - Remaining bytes: circular array of FPGARecord
//
// This implementation provides the interface and stub; the actual FPGA protocol
// is vendor-specific and filled in at integration time.

struct FPGARecord {
    uint64_t hw_timestamp_ns;   // hardware receive timestamp
    uint64_t sequence_no;
    char     instrument_code[32];
    double   last_price;
    double   bid_price[5];
    double   ask_price[5];
    int32_t  bid_volume[5];
    int32_t  ask_volume[5];
    double   open_interest;
    int64_t  volume;
    uint8_t  _pad[8];
};

class FPGAFeedHandler : public IFeedHandler {
public:
    FPGAFeedHandler(const FpgaConfig& cfg,
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

    ~FPGAFeedHandler() override { stop(); }

private:
    FpgaConfig  cfg_;
    std::thread thread_;
    void*       mmap_base_{nullptr};
    int         fd_{-1};

    void run_loop() noexcept;
    bool open_device() noexcept;
    void close_device() noexcept;
    void decode_record(const FPGARecord& rec) noexcept;
};

} // namespace omm
