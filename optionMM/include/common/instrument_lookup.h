#pragma once

#include "common/types.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace omm {

// Fixed-capacity code -> instrument_id lookup built once after gateway
// instrument discovery and then used on the feed/gateway hot path.
class InstrumentLookup {
public:
    /**
     * @brief Capacity.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    static constexpr std::size_t capacity() noexcept { return kCapacity; }

    /**
     * @brief Build.
     * @param instruments Parameter supplied by the caller.
     * @param n_instruments Parameter supplied by the caller.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    void build(const Instrument* instruments, uint16_t n_instruments) noexcept {
        instruments_ = instruments;
        n_instruments_ = n_instruments;
        entries_.fill(Entry{});

        if (!instruments_) return;
        for (uint16_t i = 0; i < n_instruments_; ++i) {
            if (instruments_[i].instrument_id == INVALID_INSTRUMENT_ID) continue;
            const std::string_view code = instruments_[i].code.view();
            if (code.empty()) continue;
            insert(code, i);
        }
    }

    /**
     * @brief Find.
     * @param code Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] uint16_t find(std::string_view code) const noexcept {
        if (!instruments_ || code.empty()) return INVALID_INSTRUMENT_ID;

        const uint64_t hash = hash_code(code);
        std::size_t idx = bucket(hash);
        for (std::size_t probe = 0; probe < kCapacity; ++probe) {
            const Entry& entry = entries_[idx];
            if (!entry.used) return INVALID_INSTRUMENT_ID;
            if (entry.hash == hash
                && entry.instrument_id < n_instruments_
                && instruments_[entry.instrument_id].code == code) {
                return entry.instrument_id;
            }
            idx = (idx + 1) & (kCapacity - 1);
        }

        return INVALID_INSTRUMENT_ID;
    }

private:
    struct Entry {
        uint64_t hash{0};
        uint16_t instrument_id{INVALID_INSTRUMENT_ID};
        bool used{false};
        uint8_t _pad[5]{};
    };

    static constexpr std::size_t kCapacity = 2048;
    static_assert((kCapacity & (kCapacity - 1)) == 0,
                  "InstrumentLookup capacity must be a power of 2");

    /**
     * @brief Hash code.
     * @param code Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    static uint64_t hash_code(std::string_view code) noexcept {
        uint64_t hash = 1469598103934665603ULL;
        for (unsigned char ch : code) {
            hash ^= static_cast<uint64_t>(ch);
            hash *= 1099511628211ULL;
        }
        return hash == 0 ? 1ULL : hash;
    }

    /**
     * @brief Bucket.
     * @param hash Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    static constexpr std::size_t bucket(uint64_t hash) noexcept {
        return static_cast<std::size_t>(hash) & (kCapacity - 1);
    }

    /**
     * @brief Insert.
     * @param code Parameter supplied by the caller.
     * @param instrument_id Parameter supplied by the caller.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    void insert(std::string_view code, uint16_t instrument_id) noexcept {
        const uint64_t hash = hash_code(code);
        std::size_t idx = bucket(hash);
        for (std::size_t probe = 0; probe < kCapacity; ++probe) {
            Entry& entry = entries_[idx];
            if (!entry.used) {
                entry.hash = hash;
                entry.instrument_id = instrument_id;
                entry.used = true;
                return;
            }
            if (entry.hash == hash
                && entry.instrument_id < n_instruments_
                && instruments_[entry.instrument_id].code == code) {
                entry.instrument_id = instrument_id;
                return;
            }
            idx = (idx + 1) & (kCapacity - 1);
        }
    }

    const Instrument* instruments_{nullptr};
    uint16_t n_instruments_{0};
    std::array<Entry, kCapacity> entries_{};
};

} // namespace omm
