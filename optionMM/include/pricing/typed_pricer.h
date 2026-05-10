#pragma once

#include "pricing/black76.h"
#include "pricing/svi.h"
#include "pricing/orc_wing.h"
#include "common/types.h"

#include <cmath>

namespace omm {

// Typed pricer eliminates virtual dispatch by using templates
// Provides 8-12 cycles/option speedup vs IVolSurface* virtual calls
template <typename VolSurface>
class TypedPricer {
public:
    /**
     * @brief TypedPricer.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    explicit TypedPricer(const VolSurface* surf) noexcept : surf_(surf) {}

    // Batch compute volatilities for options
    // Uses cached expiry slice for SVI, direct calls for others
    /**
     * @brief Compute batch vols.
     * @param log_k_arr Parameter supplied by the caller.
     * @param T_arr Parameter supplied by the caller.
     * @param slice_idx_arr Parameter supplied by the caller.
     * @param sigma_out Parameter supplied by the caller.
     * @param count Parameter supplied by the caller.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    void compute_batch_vols(
        const double* log_k_arr,      // log(K/F) for each option
        const double* T_arr,           // Time to expiry for each option
        const int8_t* slice_idx_arr,   // Cached expiry slice index (-1 if not cached)
        double* sigma_out,             // Output volatilities
        int count) const noexcept;

    // Specialized for OrcWing (uses strike-based lookup)
    /**
     * @brief Compute batch vols by strike.
     * @param F Parameter supplied by the caller.
     * @param K_arr Parameter supplied by the caller.
     * @param T_arr Parameter supplied by the caller.
     * @param sigma_out Parameter supplied by the caller.
     * @param count Parameter supplied by the caller.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    void compute_batch_vols_by_strike(
        double F,                      // Forward price
        const double* K_arr,           // Strike prices
        const double* T_arr,           // Time to expiry
        double* sigma_out,             // Output volatilities
        int count) const noexcept;

private:
    const VolSurface* surf_;
};

// Template specialization for SVIVolSurface (uses cached slices)
template <>
inline void TypedPricer<SVIVolSurface>::compute_batch_vols(
    const double* log_k_arr,
    const double* T_arr,
    const int8_t* slice_idx_arr,
    double* sigma_out,
    int count) const noexcept
{
    for (int i = 0; i < count; ++i) {
        if (slice_idx_arr[i] >= 0) {
            sigma_out[i] = surf_->get_vol_cached(log_k_arr[i], T_arr[i], slice_idx_arr[i]);
        } else {
            sigma_out[i] = surf_->get_vol(log_k_arr[i], T_arr[i]);
        }
    }
}

// Template specialization for OrcWingVolSurface (uses strike-based lookup)
template <>
inline void TypedPricer<OrcWingVolSurface>::compute_batch_vols_by_strike(
    double F,
    const double* K_arr,
    const double* T_arr,
    double* sigma_out,
    int count) const noexcept
{
    surf_->get_vols_by_strike(F, K_arr, T_arr, sigma_out, count);
}

} // namespace omm
