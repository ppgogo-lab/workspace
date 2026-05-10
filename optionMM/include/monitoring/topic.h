#pragma once

#include <atomic>
#include <cstddef>
#include <type_traits>

namespace omm {

// Fixed-size lock-free history buffer for monitoring readers.
// Single writer, many polling readers. Writers never block; lagging readers may
// skip old entries if they fall behind the retained window.
template<typename T, std::size_t Capacity>
class MonitoringTopic {
    static_assert(std::is_trivially_copyable_v<T>,
        "MonitoringTopic requires trivially copyable payloads");
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0,
        "MonitoringTopic Capacity must be a power of 2");

    struct alignas(64) Entry {
        std::atomic<uint64_t> seq{0};
        T                     data{};
    };

public:
    /**
     * @brief MonitoringTopic.
     * @return None.
     */
    MonitoringTopic() = default;

    /**
     * @brief Publish.
     * @param item Parameter supplied by the caller.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    void publish(const T& item) noexcept {
        const uint64_t seq = next_seq_.fetch_add(1, std::memory_order_relaxed);
        Entry& entry = entries_[seq & (Capacity - 1)];
        entry.data = item;
        entry.seq.store(seq, std::memory_order_release);
        latest_seq_.store(seq, std::memory_order_release);
    }

    /**
     * @brief Read next.
     * @param cursor Parameter supplied by the caller.
     * @param out Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] bool read_next(uint64_t& cursor, T& out) const noexcept {
        uint64_t latest = latest_seq_.load(std::memory_order_acquire);
        if (latest == 0 || cursor >= latest) return false;

        uint64_t next = cursor + 1;
        if (latest >= Capacity && next <= latest - Capacity) {
            next = latest - Capacity + 1;
        }

        const Entry& entry = entries_[next & (Capacity - 1)];
        if (entry.seq.load(std::memory_order_acquire) != next) {
            cursor = latest;
            return false;
        }

        out = entry.data;
        cursor = next;
        return true;
    }

    /**
     * @brief Latest seq.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] uint64_t latest_seq() const noexcept {
        return latest_seq_.load(std::memory_order_acquire);
    }

private:
    alignas(64) Entry entries_[Capacity]{};
    alignas(64) std::atomic<uint64_t> next_seq_{1};
    alignas(64) std::atomic<uint64_t> latest_seq_{0};
};

} // namespace omm
