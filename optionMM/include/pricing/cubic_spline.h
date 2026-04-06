#pragma once

#include "pricing/vol_surface.h"
#include <cmath>
#include <algorithm>

namespace omm {

// ─── Natural cubic spline vol surface ────────────────────────────────────────
// Strike-space natural cubic spline per expiry slice.
// Bilinear interpolation in total variance (w = vol^2 * T) across expiries.
//
// "Natural" means zero second derivative at the boundary knots:
//   S''(x_0) = 0, S''(x_n) = 0
// This avoids extrapolation artifacts at the wings.

struct SplineSlice {
    double T{1.0};            // time to expiry
    double k[MAX_STRIKES]{};  // log-moneyness knot positions
    double v[MAX_STRIKES]{};  // implied vol at each knot
    double c[MAX_STRIKES]{};  // second derivatives (spline coefficients)
    int    n{0};              // number of knots
    bool   valid{false};
};

class CubicSplineVolSurface : public IVolSurface {
public:
    SplineSlice slices[MAX_EXPIRIES]{};
    int         n_slices{0};

    // Evaluate spline at log-moneyness k within one slice.
    static double eval_slice(const SplineSlice& s, double k) noexcept {
        if (s.n == 0) return 0.20;
        if (k <= s.k[0])     return s.v[0];
        if (k >= s.k[s.n-1]) return s.v[s.n-1];

        // Binary search for bracket
        int lo = 0, hi = s.n - 2;
        while (lo < hi) {
            int mid = (lo + hi + 1) / 2;
            if (s.k[mid] <= k) lo = mid; else hi = mid - 1;
        }

        double h  = s.k[lo + 1] - s.k[lo];
        double t  = (k - s.k[lo]) / h;
        double t1 = 1.0 - t;

        // Cubic Hermite form using natural spline second derivatives
        return t1 * s.v[lo] + t * s.v[lo + 1]
             + h * h / 6.0 * (t1 * (t1 * t1 - 1.0) * s.c[lo]
                              + t  * (t  * t  - 1.0) * s.c[lo + 1]);
    }

    [[nodiscard]] double get_vol(double k, double T) const noexcept override {
        if (n_slices == 0 || T < 1e-10) return 0.20;

        if (T <= slices[0].T)
            return eval_slice(slices[0], k);
        if (T >= slices[n_slices - 1].T)
            return eval_slice(slices[n_slices - 1], k);

        for (int i = 0; i < n_slices - 1; ++i) {
            if (T >= slices[i].T && T <= slices[i + 1].T) {
                double t0 = slices[i].T, t1 = slices[i + 1].T;
                double alpha = (T - t0) / (t1 - t0);
                // Interpolate in total variance space
                double v0 = eval_slice(slices[i],     k);
                double v1 = eval_slice(slices[i + 1], k);
                double w0 = v0 * v0 * t0, w1 = v1 * v1 * t1;
                double wT = (1.0 - alpha) * w0 + alpha * w1;
                return std::sqrt(std::fmax(wT, 0.0) / T);
            }
        }
        return 0.20;
    }

    [[nodiscard]] double get_vol_by_strike(double F, double K,
                                            double T) const noexcept override {
        if (F < 1e-10 || K < 1e-10) return 0.20;
        return get_vol(std::log(K / F), T);
    }

    [[nodiscard]] bool is_valid() const noexcept override {
        return n_slices > 0 && slices[0].valid;
    }
};

// ─── Cubic spline fitter ─────────────────────────────────────────────────────
// Fits a natural cubic spline to (log-moneyness, implied_vol) pairs.
// Solves the tridiagonal system for second derivatives using Thomas algorithm.
// Runs off the critical path.
//
// knots must be sorted in ascending order of k = ln(K/F).
void fit_cubic_spline_slice(const double*  k_values,    // log-moneyness array
                             const double*  vol_values,  // implied vol array
                             int            n,
                             double         T,
                             SplineSlice&   out) noexcept;

} // namespace omm
