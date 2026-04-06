#pragma once

#include <atomic>
#include <cstddef>
#include <cstring>
#include <type_traits>
#include <immintrin.h>  // _mm_pause

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
    SPSCRingBuffer() = default;

    // Non-copyable, non-movable: ring buffers are owned in-place by TradingEngine.
    SPSCRingBuffer(const SPSCRingBuffer&) = delete;
    SPSCRingBuffer& operator=(const SPSCRingBuffer&) = delete;

    // ── Producer interface (call from producer thread only) ──────────────────

    // Try to push one item. Returns false if the buffer is full (non-blocking).
    // The item is copied into the buffer slot before the head cursor is advanced,
    // ensuring the consumer never sees a partially-written slot.
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
    [[nodiscard]] bool try_pop(T& item) noexcept {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        // Acquire: synchronizes with producer's release-store to head_.
        if (tail == head_.load(std::memory_order_acquire))
            return false;   // empty
        item = buffer_[tail & MASK].data;
        // Release: makes our consumed position visible to the producer.
        tail_.store((tail + 1) & MASK, std::memory_order_release);
        return true;
    }

    // ── Shared (approximate, not linearizable) ───────────────────────────────

    // Returns an approximate item count. Not linearizable — only use for
    // monitoring/logging, never for correctness decisions.
    [[nodiscard]] std::size_t size_approx() const noexcept {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        return (head - tail) & MASK;
    }

    [[nodiscard]] bool empty_approx() const noexcept {
        return size_approx() == 0;
    }

    [[nodiscard]] static constexpr std::size_t capacity() noexcept {
        return Capacity - 1;   // one slot reserved
    }
};

// ─── Spin-wait helper for busy-poll loops ─────────────────────────────────────
// Insert in the "no work available" branch of a polling loop.
// On x86: PAUSE instruction — reduces power consumption, improves branch
// prediction recovery on HT, and prevents the CPU from burning the memory bus.
// On ARM: yield hint via __asm__ volatile("yield").
inline void spin_pause() noexcept {
#if defined(__x86_64__) || defined(__i386__)
    _mm_pause();
#elif defined(__aarch64__) || defined(__arm__)
    __asm__ volatile("yield" ::: "memory");
#else
    // No-op fallback
#endif
}

} // namespace omm
