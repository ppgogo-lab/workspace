#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace omm {

namespace detail {

constexpr bool is_power_of_two(std::size_t value) noexcept {
    return value != 0 && (value & (value - 1)) == 0;
}

constexpr uint64_t mix64(uint64_t x) noexcept {
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

inline uint64_t fnv1a_bounded(const char* text, std::size_t max_len) noexcept {
    uint64_t h = 1469598103934665603ULL;
    if (text == nullptr) return h;
    for (std::size_t i = 0; i < max_len && text[i] != '\0'; ++i) {
        h ^= static_cast<unsigned char>(text[i]);
        h *= 1099511628211ULL;
    }
    return h;
}

inline bool str_equal_bounded(const char* lhs, const char* rhs, std::size_t max_len) noexcept {
    if (lhs == nullptr || rhs == nullptr) return false;
    for (std::size_t i = 0; i < max_len; ++i) {
        if (lhs[i] != rhs[i]) return false;
        if (lhs[i] == '\0') return true;
    }
    return true;
}

template <typename K>
constexpr uint64_t numeric_hash(K key) noexcept {
    static_assert(std::is_integral<K>::value, "FixedHashTable requires an integral key");
    return mix64(static_cast<uint64_t>(key));
}

} // namespace detail

template <typename K, typename V, std::size_t Capacity>
class FixedHashTable {
    static_assert(detail::is_power_of_two(Capacity), "Capacity must be a power of two");
    static_assert(std::is_integral<K>::value, "FixedHashTable requires an integral key");

public:
    [[nodiscard]] bool insert(K key, const V& value) noexcept {
        if (key == K{}) return false;
        std::size_t first_tombstone = Capacity;
        const std::size_t start = bucket(key);
        for (std::size_t probe = 0; probe < Capacity; ++probe) {
            const std::size_t idx = (start + probe) & (Capacity - 1);
            Slot& slot = slots_[idx];
            if (slot.state == State::Occupied) {
                if (slot.key == key) {
                    slot.value = value;
                    return true;
                }
                continue;
            }
            if (slot.state == State::Tombstone) {
                if (first_tombstone == Capacity) first_tombstone = idx;
                continue;
            }

            const std::size_t target = first_tombstone != Capacity ? first_tombstone : idx;
            occupy(target, key, value);
            return true;
        }
        if (first_tombstone != Capacity) {
            occupy(first_tombstone, key, value);
            return true;
        }
        return false;
    }

    [[nodiscard]] V* find(K key) noexcept {
        const std::size_t idx = find_index(key);
        return idx == Capacity ? nullptr : &slots_[idx].value;
    }

    [[nodiscard]] const V* find(K key) const noexcept {
        const std::size_t idx = find_index(key);
        return idx == Capacity ? nullptr : &slots_[idx].value;
    }

    bool erase(K key) noexcept {
        const std::size_t idx = find_index(key);
        if (idx == Capacity) return false;
        slots_[idx].state = State::Tombstone;
        --size_;
        return true;
    }

    void clear() noexcept {
        for (Slot& slot : slots_) slot.state = State::Empty;
        size_ = 0;
    }

    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }

    template <typename Fn>
    void for_each(Fn&& fn) noexcept {
        for (Slot& slot : slots_) {
            if (slot.state == State::Occupied) fn(slot.key, slot.value);
        }
    }

    template <typename Fn>
    void for_each(Fn&& fn) const noexcept {
        for (const Slot& slot : slots_) {
            if (slot.state == State::Occupied) fn(slot.key, slot.value);
        }
    }

private:
    enum class State : uint8_t { Empty, Occupied, Tombstone };

    struct Slot {
        State state{State::Empty};
        K key{};
        V value{};
    };

    [[nodiscard]] std::size_t bucket(K key) const noexcept {
        return static_cast<std::size_t>(detail::numeric_hash(key)) & (Capacity - 1);
    }

    [[nodiscard]] std::size_t find_index(K key) const noexcept {
        if (key == K{}) return Capacity;
        const std::size_t start = bucket(key);
        for (std::size_t probe = 0; probe < Capacity; ++probe) {
            const std::size_t idx = (start + probe) & (Capacity - 1);
            const Slot& slot = slots_[idx];
            if (slot.state == State::Empty) return Capacity;
            if (slot.state == State::Occupied && slot.key == key) return idx;
        }
        return Capacity;
    }

    void occupy(std::size_t idx, K key, const V& value) noexcept {
        Slot& slot = slots_[idx];
        slot.key = key;
        slot.value = value;
        slot.state = State::Occupied;
        ++size_;
    }

    std::array<Slot, Capacity> slots_{};
    std::size_t size_{0};
};

template <std::size_t KeySize, typename V, std::size_t Capacity>
class FixedStringHashTable {
    static_assert(KeySize > 1, "KeySize must include room for a null terminator");
    static_assert(detail::is_power_of_two(Capacity), "Capacity must be a power of two");

public:
    [[nodiscard]] bool insert(const char* key, const V& value) noexcept {
        if (key == nullptr || key[0] == '\0') return false;
        std::size_t first_tombstone = Capacity;
        const std::size_t start = bucket(key);
        for (std::size_t probe = 0; probe < Capacity; ++probe) {
            const std::size_t idx = (start + probe) & (Capacity - 1);
            Slot& slot = slots_[idx];
            if (slot.state == State::Occupied) {
                if (detail::str_equal_bounded(slot.key.data(), key, KeySize)) {
                    slot.value = value;
                    return true;
                }
                continue;
            }
            if (slot.state == State::Tombstone) {
                if (first_tombstone == Capacity) first_tombstone = idx;
                continue;
            }

            const std::size_t target = first_tombstone != Capacity ? first_tombstone : idx;
            occupy(target, key, value);
            return true;
        }
        if (first_tombstone != Capacity) {
            occupy(first_tombstone, key, value);
            return true;
        }
        return false;
    }

    [[nodiscard]] V* find(const char* key) noexcept {
        const std::size_t idx = find_index(key);
        return idx == Capacity ? nullptr : &slots_[idx].value;
    }

    [[nodiscard]] const V* find(const char* key) const noexcept {
        const std::size_t idx = find_index(key);
        return idx == Capacity ? nullptr : &slots_[idx].value;
    }

    bool erase(const char* key) noexcept {
        const std::size_t idx = find_index(key);
        if (idx == Capacity) return false;
        slots_[idx].state = State::Tombstone;
        --size_;
        return true;
    }

    void clear() noexcept {
        for (Slot& slot : slots_) slot.state = State::Empty;
        size_ = 0;
    }

    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }

private:
    enum class State : uint8_t { Empty, Occupied, Tombstone };

    struct Slot {
        State state{State::Empty};
        std::array<char, KeySize> key{};
        V value{};
    };

    [[nodiscard]] std::size_t bucket(const char* key) const noexcept {
        return static_cast<std::size_t>(detail::fnv1a_bounded(key, KeySize - 1)) & (Capacity - 1);
    }

    [[nodiscard]] std::size_t find_index(const char* key) const noexcept {
        if (key == nullptr || key[0] == '\0') return Capacity;
        const std::size_t start = bucket(key);
        for (std::size_t probe = 0; probe < Capacity; ++probe) {
            const std::size_t idx = (start + probe) & (Capacity - 1);
            const Slot& slot = slots_[idx];
            if (slot.state == State::Empty) return Capacity;
            if (slot.state == State::Occupied
                && detail::str_equal_bounded(slot.key.data(), key, KeySize)) {
                return idx;
            }
        }
        return Capacity;
    }

    void occupy(std::size_t idx, const char* key, const V& value) noexcept {
        Slot& slot = slots_[idx];
        std::strncpy(slot.key.data(), key, KeySize - 1);
        slot.key[KeySize - 1] = '\0';
        slot.value = value;
        slot.state = State::Occupied;
        ++size_;
    }

    std::array<Slot, Capacity> slots_{};
    std::size_t size_{0};
};

} // namespace omm
