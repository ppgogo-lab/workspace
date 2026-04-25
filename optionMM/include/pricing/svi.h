#pragma once

#include "pricing/vol_surface.h"
#include <cmath>
#include <cstring>

namespace omm {

// ─── SVI (Stochastic Volatility Inspired) vol surface ────────────────────────
// Gatheral (2004) raw SVI parameterisation per expiry slice:
//   w(k) = a + b * (rho*(k - m) + sqrt((k - m)^2 + sigma^2))
// where k = ln(K/F) is log-moneyness, w(k) is total implied variance.
// Implied vol: iv(k,T) = sqrt(w(k) / T)
//
// Parameters per slice: {a, b, rho, m, sigma}
//   a     : overall variance level (vertical shift)
//   b     : slope / wing steepness (>= 0)
//   rho   : skew / asymmetry (-1 < rho < 1)
//   m     : ATM shift (location of minimum variance)
//   sigma : curvature / smile width (> 0)
//
// Multi-expiry: one SVIParams per expiry slice, linear interpolation in T.

struct SVIParams {
    double a{0.04};
    double b{0.1};
    double rho{-0.3};
    double m{0.0};
    double sigma{0.1};
    double expiry_T{1.0};   // time to expiry in years for this slice
    bool   valid{false};
};

class SVIVolSurface : public IVolSurface {
public:
    SVIParams slices[MAX_EXPIRIES]{};
    int       n_slices{0};

    // Evaluate total variance w(k) for a single SVI slice
    static double w(const SVIParams& p, double k) noexcept {
        double dk = k - p.m;
        return p.a + p.b * (p.rho * dk + std::sqrt(dk * dk + p.sigma * p.sigma));
    }

    [[nodiscard]] double get_vol(double k, double T) const noexcept override {
        if (n_slices == 0 || T < 1e-10) return 0.20;  // fallback flat vol

        // Find bracketing slices for T
        if (T <= slices[0].expiry_T) {
            double wk = w(slices[0], k);
            return std::sqrt(std::fmax(wk, 0.0) / slices[0].expiry_T);
        }
        if (T >= slices[n_slices - 1].expiry_T) {
            double wk = w(slices[n_slices - 1], k);
            return std::sqrt(std::fmax(wk, 0.0) / slices[n_slices - 1].expiry_T);
        }
        // Linear interpolation in total variance space
        for (int i = 0; i < n_slices - 1; ++i) {
            if (T >= slices[i].expiry_T && T <= slices[i + 1].expiry_T) {
                double t0 = slices[i].expiry_T, t1 = slices[i + 1].expiry_T;
                double alpha = (T - t0) / (t1 - t0);
                double w0 = w(slices[i],     k) * (T / t0);  // scale to T
                double w1 = w(slices[i + 1], k) * (T / t1);
                double wT = (1.0 - alpha) * w0 + alpha * w1;
                return std::sqrt(std::fmax(wT, 0.0) / T);
            }
        }
        return 0.20;  // should not reach here
    }

    // Find expiry slice index for a given T (for caching)
    // Returns -1 if T is out of bounds, otherwise returns the lower slice index
    [[nodiscard]] int find_expiry_slice_index(double T) const noexcept {
        if (n_slices == 0 || T < 1e-10) return -1;
        if (T <= slices[0].expiry_T) return 0;
        if (T >= slices[n_slices - 1].expiry_T) return n_slices - 1;

        // Binary search for bracketing slices
        int lo = 0, hi = n_slices - 1;
        while (lo < hi - 1) {
            int mid = (lo + hi) / 2;
            if (T < slices[mid].expiry_T) {
                hi = mid;
            } else {
                lo = mid;
            }
        }
        return lo;  // Return lower bracket index
    }

    // Fast vol lookup using cached slice index (eliminates linear scan)
    [[nodiscard]] double get_vol_cached(double k, double T, int slice_idx) const noexcept {
        if (slice_idx < 0 || slice_idx >= n_slices || T < 1e-10) return 0.20;

        // Edge cases
        if (T <= slices[0].expiry_T) {
            double wk = w(slices[0], k);
            return std::sqrt(std::fmax(wk, 0.0) / slices[0].expiry_T);
        }
        if (T >= slices[n_slices - 1].expiry_T) {
            double wk = w(slices[n_slices - 1], k);
            return std::sqrt(std::fmax(wk, 0.0) / slices[n_slices - 1].expiry_T);
        }

        // Use cached slice index for interpolation
        int i = slice_idx;
        if (i >= n_slices - 1) i = n_slices - 2;  // Ensure valid bracket

        double t0 = slices[i].expiry_T, t1 = slices[i + 1].expiry_T;
        double alpha = (T - t0) / (t1 - t0);
        double w0 = w(slices[i],     k) * (T / t0);
        double w1 = w(slices[i + 1], k) * (T / t1);
        double wT = (1.0 - alpha) * w0 + alpha * w1;
        return std::sqrt(std::fmax(wT, 0.0) / T);
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

// ─── SVI fitter ───────────────────────────────────────────────────────────────
// Fits SVI parameters to a set of (strike, market_vol) pairs for one expiry.
// Uses Levenberg-Marquardt with analytic Jacobian.
// Runs on the vol fitter side thread — NOT on the critical path.
//
// Returns true on convergence, false if max iterations exceeded.
bool fit_svi_slice(const double* strikes,   // array of strike prices
                   const double* market_vols, // array of market implied vols
                   int           n,           // number of points
                   double        F,           // forward price
                   double        T,           // time to expiry
                   SVIParams&    out,         // output parameters
                   int           max_iter = 200,
                   double        tol      = 1e-8) noexcept;

} // namespace omm
