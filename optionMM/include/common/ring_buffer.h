#pragma once

#include <atomic>
#include <cstddef>
#include <cstring>
#include <type_traits>
#include <thread>
#include <chrono>
#include <immintrin.h>  // _mm_pause, _mm_prefetch

namespace omm {

// ─── SPSC Lock-Free Ring Buffer ───────────────────────────────────────────────
// Single-Producer Single-Consumer. Each pipeline stage has exactly one producer
// thread and one consumer thread — this is enforced by design (see trading_engine).
//
// Properties:
//   - Zero dynamic allocation: buffer is a fixed-size array member
//   - Zero OS primitives: no mutex, no condition variable, no futex
//   - Cache-line separated head/tail to eliminate false sharing
//   - Slot-level cache-line alignment to prevent adjacent-slot false sharing
//   - T must be trivially copyable (enforced by static_assert)
//   - Capacity must be a power of 2 (enforced by static_assert)
//   - Capacity - 1 usable slots (one slot reserved to distinguish full from empty)
//
// Memory ordering:
//   Producer: acquire-reads tail (full check), release-writes head (publish)
//   Consumer: acquire-reads head (available check), release-writes tail (consumed)
//   This is the minimum necessary for correctness on x86 (TSO) and ARM (weak).
//
// Usage:
//   SPSCRingBuffer<MarketTick, 1024> buf;
//   buf.try_push(tick);   // from feed thread
//   buf.try_pop(tick);    // from pricer thread

template<typename T, std::size_t Capacity>
class SPSCRingBuffer {
    static_assert(std::is_trivially_copyable_v<T>,
        "SPSCRingBuffer: T must be trivially copyable (no heap, no vtable)");
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0,
        "SPSCRingBuffer: Capacity must be a power of 2, minimum 2");

    static constexpr std::size_t MASK = Capacity - 1;

    // Each slot is cache-line aligned to prevent adjacent slots sharing a line.
    // Without this, writing slot[i] would invalidate slot[i+1] in the consumer's
    // L1 cache, causing unnecessary cache coherence traffic.
    struct alignas(64) Slot {
        T data;
        // Implicit padding to 64 bytes if sizeof(T) < 64.
        // For types larger than 64 bytes (e.g. MarketTick = 192 bytes) the slot
        // spans multiple cache lines — this is unavoidable and acceptable because
        // only one side writes any given slot at a time (SPSC invariant).
    };

    // Producer-owned cursor. alignas(64) ensures it occupies its own cache line,
    // preventing false sharing with tail_ (which the consumer writes).
    alignas(64) std::atomic<std::size_t> head_{0};

    // Consumer-owned cursor. Separate cache line from head_.
    alignas(64) std::atomic<std::size_t> tail_{0};

    // Buffer. Aligned to 64 bytes so slot[0] starts on a cache line boundary.
    alignas(64) Slot buffer_[Capacity];

public:
    /**
     * @brief SPSCRingBuffer.
     * @return None.
     */
    SPSCRingBuffer() = default;

    // Non-copyable, non-movable: ring buffers are owned in-place by TradingEngine.
    /**
     * @brief SPSCRingBuffer.
     * @param SPSCRingBuffer Parameter supplied by the caller.
     * @return None.
     */
    SPSCRingBuffer(const SPSCRingBuffer&) = delete;
    SPSCRingBuffer& operator=(const SPSCRingBuffer&) = delete;

    // ── Producer interface (call from producer thread only) ──────────────────

    // Try to push one item. Returns false if the buffer is full (non-blocking).
    // The item is copied into the buffer slot before the head cursor is advanced,
    // ensuring the consumer never sees a partially-written slot.
    /**
     * @brief Try push.
     * @param item Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] bool try_push(const T& item) noexcept {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t next = (head + 1) & MASK;
        // Acquire: synchronizes with consumer's release-store to tail_.
        // Ensures we see the consumer's latest consumed position.
        if (next == tail_.load(std::memory_order_acquire))
            return false;   // full
        buffer_[head & MASK].data = item;
        // Release: makes the written slot visible to the consumer before
        // the consumer sees the updated head cursor.
        head_.store(next, std::memory_order_release);
        return true;
    }

    // Push N items atomically: writes all slots, then ONE release-store to publish.
    // Returns false without pushing anything if there is not enough space for all N items.
    // NOTE: producer-side only; N must be >= 1.
    /**
     * @brief Try push batch.
     * @param items Parameter supplied by the caller.
     * @param count Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] bool try_push_batch(const T* items, int count) noexcept {
        if (count <= 0) return true;
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t tail = tail_.load(std::memory_order_acquire);
        // Check space: need count slots available
        const std::size_t avail = (tail - head - 1) & MASK;  // free slots (excluding reserved)
        if (static_cast<std::size_t>(count) > avail) return false;
        // Write all items without any fence
        for (int i = 0; i < count; ++i)
            buffer_[(head + i) & MASK].data = items[i];
        // Single release-store publishes all N items to the consumer atomically
        head_.store((head + count) & MASK, std::memory_order_release);
        return true;
    }

    // ── Consumer interface (call from consumer thread only) ──────────────────

    // Try to pop one item. Returns false if the buffer is empty (non-blocking).
    // Prefetches next slot to reduce cache miss latency.
    /**
     * @brief Try pop.
     * @param item Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] bool try_pop(T& item) noexcept {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        // Acquire: synchronizes with producer's release-store to head_.
        const std::size_t head = head_.load(std::memory_order_acquire);
        if (tail == head)
            return false;   // empty

        // Prefetch next slot (if available) to reduce cache miss latency
        const std::size_t next_tail = (tail + 1) & MASK;
        if (next_tail != head) {
            #if defined(__x86_64__) || defined(__i386__)
                _mm_prefetch(reinterpret_cast<const char*>(&buffer_[next_tail & MASK]), _MM_HINT_T0);
            #elif defined(__aarch64__) || defined(__arm__)
                __builtin_prefetch(&buffer_[next_tail & MASK], 0, 3);
            #endif
        }

        item = buffer_[tail & MASK].data;
        // Release: makes our consumed position visible to the producer.
        tail_.store(next_tail, std::memory_order_release);
        return true;
    }

    // Pop up to max_count items atomically: reads all available slots, then ONE release-store.
    // Returns the actual number of items popped (0 if empty, up to max_count if available).
    // Prefetches all slots in batch to reduce cache miss latency.
    // NOTE: consumer-side only; max_count must be >= 1.
    /**
     * @brief Try pop batch.
     * @param items Parameter supplied by the caller.
     * @param max_count Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] int try_pop_batch(T* items, int max_count) noexcept {
        if (max_count <= 0) return 0;
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        const std::size_t head = head_.load(std::memory_order_acquire);
        // Check available items
        const std::size_t avail = (head - tail) & MASK;
        if (avail == 0) return 0;  // empty
        // Pop min(avail, max_count) items
        const int count = (avail < static_cast<std::size_t>(max_count))
                          ? static_cast<int>(avail)
                          : max_count;

        // Prefetch all slots in batch to reduce cache miss latency
        #if defined(__x86_64__) || defined(__i386__)
            for (int i = 0; i < count; ++i) {
                _mm_prefetch(reinterpret_cast<const char*>(&buffer_[(tail + i) & MASK]), _MM_HINT_T0);
            }
        #elif defined(__aarch64__) || defined(__arm__)
            for (int i = 0; i < count; ++i) {
                __builtin_prefetch(&buffer_[(tail + i) & MASK], 0, 3);
            }
        #endif

        // Read all items without any fence
        for (int i = 0; i < count; ++i)
            items[i] = buffer_[(tail + i) & MASK].data;
        // Single release-store updates tail atomically for all N items
        tail_.store((tail + count) & MASK, std::memory_order_release);
        return count;
    }

    // ── Shared (approximate, not linearizable) ───────────────────────────────

    // Returns an approximate item count. Not linearizable — only use for
    // monitoring/logging, never for correctness decisions.
    /**
     * @brief Size approx.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] std::size_t size_approx() const noexcept {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        return (head - tail) & MASK;
    }

    /**
     * @brief Empty approx.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] bool empty_approx() const noexcept {
        return size_approx() == 0;
    }

    /**
     * @brief Capacity.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] static constexpr std::size_t capacity() noexcept {
        return Capacity - 1;   // one slot reserved
    }
};

// ─── Spin-wait helper for busy-poll loops ─────────────────────────────────────
// Insert in the "no work available" branch of a polling loop.
// On x86: PAUSE instruction — reduces power consumption, improves branch
// prediction recovery on HT, and prevents the CPU from burning the memory bus.
// On ARM: yield hint via __asm__ volatile("yield").
/**
 * @brief Spin pause.
 * @return None.
 * @note Noexcept API preserves hot-path failure and latency invariants.
 */
inline void spin_pause() noexcept {
#if defined(__x86_64__) || defined(__i386__)
    _mm_pause();
#elif defined(__aarch64__) || defined(__arm__)
    __asm__ volatile("yield" ::: "memory");
#else
    // No-op fallback
#endif
}

// Adaptive spin-pause with exponential backoff
// Reduces CPU overhead when idle while maintaining low latency when active
/**
 * @brief Adaptive spin pause.
 * @param spin_count Parameter supplied by the caller.
 * @return None.
 * @note Noexcept API preserves hot-path failure and latency invariants.
 */
inline void adaptive_spin_pause(int& spin_count) noexcept {
    constexpr int kFastSpins = 100;      // ~100ns of spinning (fast path)
    constexpr int kYieldSpins = 1000;    // ~1μs before sleep (medium path)

    if (spin_count < kFastSpins) {
        // Fast path: CPU pause instruction (~1ns)
        #if defined(__x86_64__) || defined(__i386__)
            _mm_pause();
        #elif defined(__aarch64__) || defined(__arm__)
            __asm__ volatile("yield" ::: "memory");
        #endif
        ++spin_count;
    } else if (spin_count < kYieldSpins) {
        // Medium path: yield to scheduler (~100ns-1μs)
        std::this_thread::yield();
        ++spin_count;
    } else {
        // Slow path: brief sleep to reduce CPU usage (~1μs)
        std::this_thread::sleep_for(std::chrono::microseconds(1));
        spin_count = 0;  // Reset counter
    }
}

} // namespace omm
