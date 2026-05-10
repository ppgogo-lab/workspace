#pragma once

#include "pricing/vol_surface.h"
#include <cmath>
#include <cstring>
#include <algorithm>

namespace omm {

// ─── Wing Model vol surface ──────────────────────────────────────────────────
// A parametric volatility smile model piecewise quadratic in log-moneyness.
//   iv(k) = ATM_vol + slope_call*max(k,0) + slope_put*max(-k,0)
//                   + curve_call*max(k,0)^2 + curve_put*max(-k,0)^2
// where k = ln(K/F) is log-moneyness.
//
// Parameters per slice:
//   ATM_vol    : at-the-money volatility level (>= 1e-4)
//   slope_call : linear slope for call wing (k > 0)
//   slope_put  : linear slope for put wing (k < 0)
//   curve_call : quadratic curvature for call wing (>= 0)
//   curve_put  : quadratic curvature for put wing (>= 0)
//
// Multi-expiry: one WingParams per expiry slice, linear interpolation in
// total variance space (w = iv^2 * T) like SVI.

struct WingParams {
    double ATM_vol{0.20};
    double slope_call{-0.1};
    double slope_put{0.1};
    double curve_call{0.05};
    double curve_put{0.05};
    double expiry_T{1.0};
    bool   valid{false};
};

class WingVolSurface : public IVolSurface {
public:
    WingParams slices[MAX_EXPIRIES]{};
    int        n_slices{0};

    // Evaluate implied volatility for a single Wing slice
    /**
     * @brief Wing iv.
     * @param p Parameter supplied by the caller.
     * @param k Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    static double wing_iv(const WingParams& p, double k) noexcept {
        double kp = std::fmax(k, 0.0);
        double km = std::fmax(-k, 0.0);
        double iv = p.ATM_vol
                  + p.slope_call * kp + p.slope_put * km
                  + p.curve_call * kp * kp + p.curve_put * km * km;
        return std::fmax(iv, 1e-6); // prevent <= 0
    }

    /**
     * @brief Get vol.
     * @param k Parameter supplied by the caller.
     * @param T Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] double get_vol(double k, double T) const noexcept override {
        if (n_slices == 0 || T < 1e-10) return 0.20;

        if (T <= slices[0].expiry_T) {
            return wing_iv(slices[0], k);
        }
        if (T >= slices[n_slices - 1].expiry_T) {
            return wing_iv(slices[n_slices - 1], k);
        }

        // Linear interpolation in total variance space
        for (int i = 0; i < n_slices - 1; ++i) {
            if (T >= slices[i].expiry_T && T <= slices[i + 1].expiry_T) {
                double t0 = slices[i].expiry_T, t1 = slices[i + 1].expiry_T;
                double alpha = (T - t0) / (t1 - t0);

                double v0 = wing_iv(slices[i], k);
                double v1 = wing_iv(slices[i + 1], k);

                double w0 = v0 * v0 * t0 * (T / t0); // total variance at T from slice i
                double w1 = v1 * v1 * t1 * (T / t1); // total variance at T from slice i+1

                double wT = (1.0 - alpha) * w0 + alpha * w1;
                return std::sqrt(std::fmax(wT, 0.0) / T);
            }
        }
        return 0.20;
    }

    /**
     * @brief Get vol by strike.
     * @param F Parameter supplied by the caller.
     * @param K Parameter supplied by the caller.
     * @param T Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] double get_vol_by_strike(double F, double K,
                                            double T) const noexcept override {
        if (F < 1e-10 || K < 1e-10) return 0.20;
        return get_vol(std::log(K / F), T);
    }

    /**
     * @brief Is valid.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] bool is_valid() const noexcept override {
        return n_slices > 0 && slices[0].valid;
    }
};

// ─── Wing fitter ─────────────────────────────────────────────────────────────
// Fits Wing parameters to a set of (strike, market_vol) pairs for one expiry.
// Uses Levenberg-Marquardt.
// Runs on the vol fitter side thread — NOT on the critical path.
/**
 * @brief Fit wing slice.
 * @param strikes Parameter supplied by the caller.
 * @param market_vols Parameter supplied by the caller.
 * @param n Parameter supplied by the caller.
 * @param F Parameter supplied by the caller.
 * @param T Parameter supplied by the caller.
 * @param out Parameter supplied by the caller.
 * @param max_iter Parameter supplied by the caller.
 * @param tol Parameter supplied by the caller.
 * @return Return value produced by the operation.
 * @note Noexcept API preserves hot-path failure and latency invariants.
 */
bool fit_wing_slice(const double* strikes,
                    const double* market_vols,
                    int           n,
                    double        F,
                    double        T,
                    WingParams&   out,
                    int           max_iter = 200,
                    double        tol      = 1e-8) noexcept;

} // namespace omm
