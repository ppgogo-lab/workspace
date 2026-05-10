#pragma once

#include "pricing/vol_surface.h"

#include <algorithm>
#include <cmath>

namespace omm {

struct OrcWingParams {
    double ref_price{100.0};
    double atm_forward{100.0};
    double ssr{1.0};              // [0, 1], 1 = fully swimming skew
    double vol_ref{0.20};
    double slope_ref{-0.1};
    double vcr{0.0};
    double scr{0.0};
    double put_curv{0.05};
    double call_curv{0.05};
    double down_cutoff{-0.15};
    double up_cutoff{0.15};
    double down_smoothing{0.5};
    double up_smoothing{0.5};
    double expiry_T{1.0};
    bool   valid{false};
};

class OrcWingVolSurface : public IVolSurface {
public:
    OrcWingParams slices[MAX_EXPIRIES]{};
    int           n_slices{0};

    /**
     * @brief Effective forward.
     * @param p Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] static double effective_forward(const OrcWingParams& p) noexcept;
    /**
     * @brief Current vol.
     * @param p Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] static double current_vol(const OrcWingParams& p) noexcept;
    /**
     * @brief Current slope.
     * @param p Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] static double current_slope(const OrcWingParams& p) noexcept;
    /**
     * @brief Eval x.
     * @param p Parameter supplied by the caller.
     * @param x Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] static double eval_x(const OrcWingParams& p, double x) noexcept;
    /**
     * @brief Interpolate.
     * @param a Parameter supplied by the caller.
     * @param b Parameter supplied by the caller.
     * @param alpha Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] static OrcWingParams interpolate(const OrcWingParams& a,
                                                   const OrcWingParams& b,
                                                   double alpha) noexcept;

    /**
     * @brief Get vol.
     * @param k Parameter supplied by the caller.
     * @param T Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] double get_vol(double k, double T) const noexcept override;
    /**
     * @brief Get vol by strike.
     * @param F Parameter supplied by the caller.
     * @param K Parameter supplied by the caller.
     * @param T Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] double get_vol_by_strike(double F, double K, double T) const noexcept override;
    /**
     * @brief Get vols by strike.
     * @param F Parameter supplied by the caller.
     * @param K_arr Parameter supplied by the caller.
     * @param T_arr Parameter supplied by the caller.
     * @param sigma_out Parameter supplied by the caller.
     * @param count Parameter supplied by the caller.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    void get_vols_by_strike(double F,
                            const double* K_arr,
                            const double* T_arr,
                            double* sigma_out,
                            int count) const noexcept;
    /**
     * @brief Is valid.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] bool is_valid() const noexcept override {
        return n_slices > 0 && slices[0].valid;
    }
};

/**
 * @brief Fit orc wing slice.
 * @param strikes Parameter supplied by the caller.
 * @param market_vols Parameter supplied by the caller.
 * @param n Parameter supplied by the caller.
 * @param F Parameter supplied by the caller.
 * @param T Parameter supplied by the caller.
 * @param out Parameter supplied by the caller.
 * @param max_iter Parameter supplied by the caller.
 * @return Return value produced by the operation.
 * @note Noexcept API preserves hot-path failure and latency invariants.
 */
bool fit_orc_wing_slice(const double* strikes,
                        const double* market_vols,
                        int           n,
                        double        F,
                        double        T,
                        OrcWingParams& out,
                        int           max_iter = 100) noexcept;

/**
 * @brief Fit orc wing slice seeded.
 * @param strikes Parameter supplied by the caller.
 * @param market_vols Parameter supplied by the caller.
 * @param n Parameter supplied by the caller.
 * @param F Parameter supplied by the caller.
 * @param T Parameter supplied by the caller.
 * @param seed Parameter supplied by the caller.
 * @param out Parameter supplied by the caller.
 * @param max_iter Parameter supplied by the caller.
 * @return Return value produced by the operation.
 * @note Noexcept API preserves hot-path failure and latency invariants.
 */
bool fit_orc_wing_slice_seeded(const double* strikes,
                               const double* market_vols,
                               int           n,
                               double        F,
                               double        T,
                               const OrcWingParams* seed,
                               OrcWingParams& out,
                               int           max_iter = 100) noexcept;

} // namespace omm
