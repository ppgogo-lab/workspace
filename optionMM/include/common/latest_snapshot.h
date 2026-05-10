#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace omm {

// Latest-value snapshot for one trivially-copyable object.
//
// This avoids C++ data races by storing payload bytes as atomic 64-bit words.
// The version counter is a consistency guard: odd means a writer is active,
// even means a complete payload has been published.
template<typename T>
class LatestSnapshot {
    static_assert(std::is_trivially_copyable_v<T>,
                  "LatestSnapshot requires a trivially copyable payload");
    static_assert(sizeof(T) % sizeof(uint64_t) == 0,
                  "LatestSnapshot payload size must be a multiple of 8 bytes");
    static_assert(alignof(T) >= alignof(uint64_t),
                  "LatestSnapshot payload alignment must be at least 8 bytes");

    static constexpr std::size_t kWordCount = sizeof(T) / sizeof(uint64_t);

public:
    /**
     * @brief LatestSnapshot.
     * @return None.
     */
    LatestSnapshot() = default;
    /**
     * @brief LatestSnapshot.
     * @param LatestSnapshot Parameter supplied by the caller.
     * @return None.
     */
    LatestSnapshot(const LatestSnapshot&) = delete;
    LatestSnapshot& operator=(const LatestSnapshot&) = delete;

    /**
     * @brief Publish.
     * @param value Parameter supplied by the caller.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    void publish(const T& value) noexcept {
        const uint64_t cur = version_.load(std::memory_order_relaxed);
        version_.store(cur + 1, std::memory_order_release);

        std::array<uint64_t, kWordCount> words{};
        std::memcpy(words.data(), &value, sizeof(T));
        for (std::size_t i = 0; i < kWordCount; ++i) {
            words_[i].store(words[i], std::memory_order_relaxed);
        }

        version_.store(cur + 2, std::memory_order_release);
    }

    /**
     * @brief Read.
     * @param out Parameter supplied by the caller.
     * @param max_attempts Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] bool read(T* out, int max_attempts = 4) const noexcept {
        if (out == nullptr) return false;

        std::array<uint64_t, kWordCount> words{};
        for (int attempt = 0; attempt < max_attempts; ++attempt) {
            const uint64_t v1 = version_.load(std::memory_order_acquire);
            if (v1 == 0 || (v1 & 1ULL) != 0) continue;

            for (std::size_t i = 0; i < kWordCount; ++i) {
                words[i] = words_[i].load(std::memory_order_relaxed);
            }

            const uint64_t v2 = version_.load(std::memory_order_acquire);
            if (v1 == v2 && (v2 & 1ULL) == 0) {
                std::memcpy(out, words.data(), sizeof(T));
                return true;
            }
        }

        return false;
    }

    /**
     * @brief Version.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] uint64_t version() const noexcept {
        return version_.load(std::memory_order_acquire);
    }

private:
    alignas(64) std::atomic<uint64_t> version_{0};
    alignas(64) std::array<std::atomic<uint64_t>, kWordCount> words_{};
};

template<typename T, std::size_t N>
class SnapshotArray {
public:
    /**
     * @brief SnapshotArray.
     * @return None.
     */
    SnapshotArray() = default;
    /**
     * @brief SnapshotArray.
     * @param SnapshotArray Parameter supplied by the caller.
     * @return None.
     */
    SnapshotArray(const SnapshotArray&) = delete;
    SnapshotArray& operator=(const SnapshotArray&) = delete;

    /**
     * @brief Publish.
     * @param index Parameter supplied by the caller.
     * @param value Parameter supplied by the caller.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    void publish(std::size_t index, const T& value) noexcept {
        if (index >= N) return;
        slots_[index].publish(value);
    }

    /**
     * @brief Read.
     * @param index Parameter supplied by the caller.
     * @param out Parameter supplied by the caller.
     * @param max_attempts Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] bool read(std::size_t index, T* out, int max_attempts = 4) const noexcept {
        if (index >= N) return false;
        return slots_[index].read(out, max_attempts);
    }

    /**
     * @brief Version.
     * @param index Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] uint64_t version(std::size_t index) const noexcept {
        if (index >= N) return 0;
        return slots_[index].version();
    }

private:
    alignas(64) std::array<LatestSnapshot<T>, N> slots_{};
};

} // namespace omm
