#pragma once

#include "common/instrument_lookup.h"
#include "common/types.h"
#include "common/ring_buffer.h"
#include <atomic>
#include <cstdint>

namespace omm {

// ─── IFeedHandler ─────────────────────────────────────────────────────────────
// Abstract interface for market data ingestion.
// One concrete implementation is selected at startup based on FeedConfig.
//
// Ownership:
//   - The TradingEngine owns the tick_buf ring buffer.
//   - The feed handler receives a raw pointer at construction and writes to it.
//   - The feed thread spins inside run_loop() — never yields, never sleeps.
//
// Thread model:
//   - start() spawns the feed thread (internally).
//   - stop() signals the thread to exit and joins it.
//   - All other methods are thread-safe (atomic reads).

class IFeedHandler {
public:
    /**
     * @brief IFeedHandler.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    explicit IFeedHandler(SPSCRingBuffer<TopOfBookTick, 1024>* tick_buf) noexcept
        : tick_buf_(tick_buf) {}

    /**
     * @brief Start.
     * @return None.
     */
    virtual void start() = 0;
    /**
     * @brief Stop.
     * @return None.
     */
    virtual void stop()  = 0;

    /**
     * @brief Is connected.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] virtual bool     is_connected()    const noexcept = 0;
    /**
     * @brief Message count.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] virtual uint64_t message_count()   const noexcept = 0;
    /**
     * @brief Error count.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] virtual uint64_t error_count()     const noexcept = 0;
    /**
     * @brief Dropped count.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] virtual uint64_t dropped_count()   const noexcept = 0;

    /**
     * @brief IFeedHandler.
     * @return None.
     */
    virtual ~IFeedHandler() = default;

protected:
    SPSCRingBuffer<TopOfBookTick, 1024>* tick_buf_;
    std::atomic<bool>    stop_flag_{false};
    std::atomic<bool>    connected_{false};
    std::atomic<uint64_t> msg_count_{0};
    std::atomic<uint64_t> err_count_{0};
    std::atomic<uint64_t> dropped_count_{0};

    // Instrument code → internal ID mapping (set by engine at startup)
    // O(1) lookup: instrument_code_to_id[hash] = instrument_id
    // For now a simple linear scan; replace with hash map in production.
    const Instrument* instruments_{nullptr};
    uint16_t          n_instruments_{0};
    InstrumentLookup  instrument_lookup_{};

    // Map an exchange instrument code string to an internal instrument_id.
    // Returns INVALID_INSTRUMENT_ID if not found.
    [[nodiscard]] uint16_t resolve_instrument(const char* code) const noexcept {
        return instrument_lookup_.find(code ? std::string_view(code) : std::string_view{});
    }

public:
    /**
     * @brief Set instruments.
     * @param instruments Parameter supplied by the caller.
     * @param n Parameter supplied by the caller.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    void set_instruments(const Instrument* instruments,
                         uint16_t n) noexcept {
        instruments_  = instruments;
        n_instruments_ = n;
        instrument_lookup_.build(instruments_, n_instruments_);
    }

    // Called by TradingEngine after it takes ownership of the feed,
    // so feed handlers can be constructed before the engine's tick_buf exists.
    /**
     * @brief Set tick buf.
     * @param buf Parameter supplied by the caller.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    void set_tick_buf(SPSCRingBuffer<TopOfBookTick, 1024>* buf) noexcept {
        tick_buf_ = buf;
    }

    /**
     * @brief Instrument count.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] uint16_t instrument_count() const noexcept {
        return n_instruments_;
    }
};

} // namespace omm
