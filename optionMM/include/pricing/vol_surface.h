#pragma once

#include "common/types.h"
#include "common/config.h"

#include <atomic>
#include <cstdint>
#include <cstring>

namespace omm {

// Maximum expiry slices per surface (e.g. monthly expiries for 2 years)
static constexpr int MAX_EXPIRIES = 24;
// Maximum strikes per expiry slice
static constexpr int MAX_STRIKES  = 256;

// ─── Abstract vol surface interface ──────────────────────────────────────────
// All methods are noexcept — called on the critical path from the pricer thread.
// Implementations must be safe to call concurrently from the pricer thread
// while the fitter thread is writing to the *inactive* buffer.
class IVolSurface {
public:
    // Get implied volatility by log-moneyness k = ln(K/F) and time-to-expiry T (years).
    /**
     * @brief Get vol.
     * @param k Parameter supplied by the caller.
     * @param T Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] virtual double get_vol(double k, double T) const noexcept = 0;

    // Convenience: get IV directly from forward F, strike K, and expiry T.
    /**
     * @brief Get vol by strike.
     * @param F Parameter supplied by the caller.
     * @param K Parameter supplied by the caller.
     * @param T Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] virtual double get_vol_by_strike(double F, double K,
                                                    double T) const noexcept = 0;

    // True if the surface has been fitted at least once (has valid parameters).
    /**
     * @brief Is valid.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] virtual bool is_valid() const noexcept = 0;

    /**
     * @brief IVolSurface.
     * @return None.
     */
    virtual ~IVolSurface() = default;
};

// ─── VolSurfaceManager ────────────────────────────────────────────────────────
// Thread-safe atomic snapshot mechanism:
//   - The pricer thread calls get() to obtain a stable read-only pointer.
//     It holds this pointer only for the duration of one tick computation.
//   - The fitter thread (off critical path) calls publish() after fitting new
//     parameters into the inactive buffer, atomically swapping it live.
//   - Zero allocation on either path after construction.
//
// Template parameter Surface must implement IVolSurface and be trivially
// copy-assignable (so parameters can be written without placement-new).
template<typename Surface>
class VolSurfaceManager {
    static_assert(std::is_base_of_v<IVolSurface, Surface>);

    // Two surface objects, ping-pong alternated.
    alignas(64) Surface bufs_[2];
    // Index of the currently active buffer (0 or 1).
    std::atomic<int> active_idx_{0};

public:
    /**
     * @brief VolSurfaceManager.
     * @return None.
     */
    VolSurfaceManager() = default;

    // Called by the pricer thread. acquire load ensures we see the fitter's
    // latest parameter writes before we read the surface contents.
    /**
     * @brief Get.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] const IVolSurface* get() const noexcept {
        int idx = active_idx_.load(std::memory_order_acquire);
        return &bufs_[idx];
    }

    // Called by the fitter thread. Returns a pointer to the inactive buffer
    // so the fitter can write new parameters into it.
    /**
     * @brief Get inactive.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] Surface* get_inactive() noexcept {
        int idx = active_idx_.load(std::memory_order_relaxed);
        return &bufs_[idx ^ 1];
    }

    // Called by the fitter thread after writing to get_inactive().
    // Release store makes new parameter writes visible to the pricer.
    /**
     * @brief Publish.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    void publish() noexcept {
        int idx = active_idx_.load(std::memory_order_relaxed);
        active_idx_.store(idx ^ 1, std::memory_order_release);
    }
};

} // namespace omm
