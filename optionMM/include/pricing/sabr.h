#pragma once

#include "pricing/vol_surface.h"
#include <cmath>

namespace omm {

// ─── SABR vol surface ─────────────────────────────────────────────────────────
// Hagan et al. (2002) SABR model implied volatility approximation.
// Parameters per expiry slice: {alpha, beta, rho, nu}
//   alpha : initial volatility level (> 0)
//   beta  : CEV exponent [0, 1]; typically 0.5 (commodity) or 1.0 (equity index)
//           Fixed from config, not fitted.
//   rho   : correlation between asset and vol processes (-1 < rho < 1)
//   nu    : vol-of-vol (> 0)

struct SABRParams {
    double alpha{0.3};
    double beta{1.0};     // fixed from PricingConfig.sabr_beta
    double rho{-0.2};
    double nu{0.4};
    double expiry_T{1.0};
    double forward_F{1.0}; // forward price used during fitting
    bool   valid{false};
};

// Hagan 2002 SABR implied vol formula (lognormal approximation)
// Returns Black-76 implied volatility for forward F, strike K, expiry T.
/**
 * @brief Sabr vol.
 * @param p Parameter supplied by the caller.
 * @param F Parameter supplied by the caller.
 * @param K Parameter supplied by the caller.
 * @param T Parameter supplied by the caller.
 * @return Return value produced by the operation.
 * @note Noexcept API preserves hot-path failure and latency invariants.
 */
inline double sabr_vol(const SABRParams& p, double F, double K, double T) noexcept {
    if (T < 1e-10 || F < 1e-10 || K < 1e-10) return p.alpha;

    const double alpha = p.alpha, beta = p.beta, rho = p.rho, nu = p.nu;
    const double beta1 = 1.0 - beta;

    // ATM special case: F ≈ K
    if (std::fabs(F - K) < 1e-8 * F) {
        double FK_mid  = std::pow(F, beta1);
        double term1   = 1.0 + ((beta1 * beta1 * alpha * alpha) / (24.0 * FK_mid * FK_mid)
                               + 0.25 * rho * beta * nu * alpha / FK_mid
                               + (2.0 - 3.0 * rho * rho) * nu * nu / 24.0) * T;
        return (alpha / FK_mid) * term1;
    }

    double log_FK = std::log(F / K);
    double FK_beta = std::pow(F * K, beta1 * 0.5);

    // z and chi(z) terms
    double z   = (nu / alpha) * FK_beta * log_FK;
    double chi = std::log((std::sqrt(1.0 - 2.0 * rho * z + z * z) + z - rho)
                           / (1.0 - rho));

    double z_over_chi = (std::fabs(chi) < 1e-12) ? 1.0 : z / chi;

    // Expansion terms
    double A = alpha / (FK_beta * (1.0 + beta1 * beta1 / 24.0 * log_FK * log_FK
                                       + beta1 * beta1 * beta1 * beta1 / 1920.0
                                         * log_FK * log_FK * log_FK * log_FK));

    double B = 1.0 + ((beta1 * beta1 * alpha * alpha) / (24.0 * FK_beta * FK_beta)
                      + 0.25 * rho * beta * nu * alpha / FK_beta
                      + (2.0 - 3.0 * rho * rho) * nu * nu / 24.0) * T;

    return A * z_over_chi * B;
}

class SABRVolSurface : public IVolSurface {
public:
    SABRParams slices[MAX_EXPIRIES]{};
    int        n_slices{0};

    /**
     * @brief Get vol.
     * @param k Parameter supplied by the caller.
     * @param T Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] double get_vol(double k, double T) const noexcept override {
        // k = ln(K/F), so K = F*exp(k)
        // Use the actual forward F stored in params (fitted with that F)
        if (n_slices == 0 || T < 1e-10) return 0.20;

        // Find slice by T
        const SABRParams* p = &slices[n_slices - 1];
        for (int i = 0; i < n_slices - 1; ++i) {
            if (T <= slices[i + 1].expiry_T) { p = &slices[i]; break; }
        }
        // Use actual forward from params: K = F*exp(k)
        return sabr_vol(*p, p->forward_F, p->forward_F * std::exp(k), T);
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
        if (n_slices == 0 || T < 1e-10) return 0.20;

        // Find bracketing slices
        if (T <= slices[0].expiry_T)
            return sabr_vol(slices[0], F, K, T);
        if (T >= slices[n_slices - 1].expiry_T)
            return sabr_vol(slices[n_slices - 1], F, K, T);

        for (int i = 0; i < n_slices - 1; ++i) {
            if (T >= slices[i].expiry_T && T <= slices[i + 1].expiry_T) {
                double t0 = slices[i].expiry_T, t1 = slices[i + 1].expiry_T;
                double alpha = (T - t0) / (t1 - t0);
                double v0 = sabr_vol(slices[i],     F, K, T);
                double v1 = sabr_vol(slices[i + 1], F, K, T);
                return (1.0 - alpha) * v0 + alpha * v1;
            }
        }
        return 0.20;
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

// ─── SABR fitter ─────────────────────────────────────────────────────────────
// Fits {alpha, rho, nu} to market vols for one expiry slice.
// beta is fixed from config (not fitted). Uses gradient descent.
// Runs off the critical path on the vol fitter thread.
/**
 * @brief Fit sabr slice.
 * @param strikes Parameter supplied by the caller.
 * @param market_vols Parameter supplied by the caller.
 * @param n Parameter supplied by the caller.
 * @param F Parameter supplied by the caller.
 * @param T Parameter supplied by the caller.
 * @param beta Parameter supplied by the caller.
 * @param out Parameter supplied by the caller.
 * @param max_iter Parameter supplied by the caller.
 * @param tol Parameter supplied by the caller.
 * @return Return value produced by the operation.
 * @note Noexcept API preserves hot-path failure and latency invariants.
 */
bool fit_sabr_slice(const double* strikes,
                    const double* market_vols,
                    int           n,
                    double        F,
                    double        T,
                    double        beta,      // fixed from config
                    SABRParams&   out,
                    int           max_iter = 500,
                    double        tol      = 1e-8) noexcept;

} // namespace omm
