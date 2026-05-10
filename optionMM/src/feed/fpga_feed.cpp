#include "feed/fpga_feed.h"
#include "common/thread_utils.h"
#include "common/ring_buffer.h"

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <atomic>

namespace omm {

// ─── FPGA control block layout (first 64 bytes of mmap region) ───────────────
struct alignas(64) FPGACtrlBlock {
    std::atomic<uint64_t> write_seq{0};  // FPGA advances this after writing a record
    uint64_t              read_seq{0};   // host updates this after consuming a record
    uint32_t              record_size{sizeof(FPGARecord)};
    uint32_t              capacity{256}; // number of records in the ring
    uint8_t               _pad[32];
};

/**
 * @brief Implements Open device.
 * @return Return value produced by the operation.
 * @note Noexcept API preserves hot-path failure and latency invariants.
 */
bool FPGAFeedHandler::open_device() noexcept {
    fd_ = ::open(cfg_.device_path, O_RDWR);
    if (fd_ < 0) { err_count_++; return false; }

    // Map control block + record ring
    size_t map_size = sizeof(FPGACtrlBlock)
                    + 256 * sizeof(FPGARecord);  // default capacity
    mmap_base_ = ::mmap(nullptr, map_size,
                        PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
    if (mmap_base_ == MAP_FAILED) {
        mmap_base_ = nullptr;
        err_count_++;
        ::close(fd_); fd_ = -1;
        return false;
    }
    connected_.store(true, std::memory_order_release);
    return true;
}

/**
 * @brief Implements Close device.
 * @return None.
 * @note Noexcept API preserves hot-path failure and latency invariants.
 */
void FPGAFeedHandler::close_device() noexcept {
    if (mmap_base_) {
        ::munmap(mmap_base_,
                 sizeof(FPGACtrlBlock) + 256 * sizeof(FPGARecord));
        mmap_base_ = nullptr;
    }
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    connected_.store(false, std::memory_order_release);
}

/**
 * @brief Implements Decode record.
 * @param rec Parameter supplied by the caller.
 * @return None.
 * @note Noexcept API preserves hot-path failure and latency invariants.
 */
void FPGAFeedHandler::decode_record(const FPGARecord& rec) noexcept {
    uint16_t id = resolve_instrument(rec.instrument_code);
    if (id == INVALID_INSTRUMENT_ID) return;

    MarketTick tick{};
    tick.recv_ts_ns     = get_monotonic_ns();
    tick.exchange_ts_ns = static_cast<int64_t>(rec.hw_timestamp_ns);
    tick.instrument_id  = id;
    tick.last_price     = rec.last_price;
    tick.open_interest  = rec.open_interest;
    tick.volume         = rec.volume;
    tick.sequence_no    = rec.sequence_no;
    for (int i = 0; i < 5; ++i) {
        tick.bid_price[i]  = rec.bid_price[i];
        tick.ask_price[i]  = rec.ask_price[i];
        tick.bid_volume[i] = rec.bid_volume[i];
        tick.ask_volume[i] = rec.ask_volume[i];
    }

    if (!tick_buf_->try_push(to_top_of_book_tick(tick)))
        dropped_count_.fetch_add(1, std::memory_order_relaxed);
    else
        msg_count_.fetch_add(1, std::memory_order_relaxed);
}

/**
 * @brief Implements Run loop.
 * @return None.
 * @note Noexcept API preserves hot-path failure and latency invariants.
 */
void FPGAFeedHandler::run_loop() noexcept {
    if (!mmap_base_) return;

    auto* ctrl    = static_cast<FPGACtrlBlock*>(mmap_base_);
    auto* records = reinterpret_cast<FPGARecord*>(
                        static_cast<uint8_t*>(mmap_base_) + sizeof(FPGACtrlBlock));
    uint32_t capacity = ctrl->capacity;
    uint64_t local_seq = ctrl->read_seq;

    while (!stop_flag_.load(std::memory_order_relaxed)) {
        uint64_t write_seq = ctrl->write_seq.load(std::memory_order_acquire);
        if (local_seq == write_seq) {
            spin_pause();
            continue;
        }
        // Process all pending records
        while (local_seq != write_seq) {
            uint32_t slot = static_cast<uint32_t>(local_seq % capacity);
            decode_record(records[slot]);
            ++local_seq;
        }
        ctrl->read_seq = local_seq;  // update consumed pointer
    }
}

/**
 * @brief Implements Start.
 * @return None.
 */
void FPGAFeedHandler::start() {
    if (!open_device()) return;
    stop_flag_.store(false, std::memory_order_relaxed);
    thread_ = std::thread([this] { run_loop(); });
}

/**
 * @brief Implements Stop.
 * @return None.
 */
void FPGAFeedHandler::stop() {
    stop_flag_.store(true, std::memory_order_release);
    if (thread_.joinable()) thread_.join();
    close_device();
}

} // namespace omm
