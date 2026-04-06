#include "pricing/sabr.h"
#include <cmath>
#include <algorithm>

namespace omm {

// ─── SABR gradient descent fitter ────────────────────────────────────────────
// Fits {alpha, rho, nu} with beta fixed.
// Objective: minimize sum((sabr_vol(F,K_i,T; alpha,rho,nu) - market_vol_i)^2)
// Uses simple gradient descent with Armijo line search.

static double sabr_sse(const SABRParams& p,
                        const double* strikes, const double* market_vols,
                        int n, double F, double T) noexcept {
    double sse = 0.0;
    for (int i = 0; i < n; ++i) {
        double r = sabr_vol(p, F, strikes[i], T) - market_vols[i];
        sse += r * r;
    }
    return sse;
}

// Finite-difference gradient (off critical path — accuracy over speed)
static void sabr_gradient(const SABRParams& p,
                            const double* strikes, const double* market_vols,
                            int n, double F, double T,
                            double* grad) noexcept {
    const double eps = 1e-6;
    double sse0 = sabr_sse(p, strikes, market_vols, n, F, T);

    SABRParams q = p;
    q.alpha += eps;
    grad[0] = (sabr_sse(q, strikes, market_vols, n, F, T) - sse0) / eps;

    q = p; q.rho += eps;
    grad[1] = (sabr_sse(q, strikes, market_vols, n, F, T) - sse0) / eps;

    q = p; q.nu += eps;
    grad[2] = (sabr_sse(q, strikes, market_vols, n, F, T) - sse0) / eps;
}

bool fit_sabr_slice(const double* strikes, const double* market_vols,
                    int n, double F, double T, double beta,
                    SABRParams& out, int max_iter, double tol) noexcept {
    if (n < 3) return false;

    SABRParams p = out;
    p.beta     = beta;
    p.expiry_T = T;
    // Initial guess: alpha from ATM vol, rho/nu from typical values
    double avg_vol = 0.0;
    for (int i = 0; i < n; ++i) avg_vol += market_vols[i];
    p.alpha = avg_vol / n;
    p.rho   = -0.2;
    p.nu    = 0.4;

    double step = 0.01;

    for (int iter = 0; iter < max_iter; ++iter) {
        double grad[3];
        sabr_gradient(p, strikes, market_vols, n, F, T, grad);

        double gnorm = std::sqrt(grad[0]*grad[0] + grad[1]*grad[1] + grad[2]*grad[2]);
        if (gnorm < tol) break;

        // Normalise gradient direction
        double scale = step / gnorm;

        // Armijo backtracking line search
        double sse0 = sabr_sse(p, strikes, market_vols, n, F, T);
        double s = scale;
        for (int ls = 0; ls < 20; ++ls) {
            SABRParams q = p;
            q.alpha = std::fmax(p.alpha - s * grad[0], 1e-6);
            q.rho   = std::fmax(-0.9999, std::fmin(0.9999, p.rho - s * grad[1]));
            q.nu    = std::fmax(p.nu    - s * grad[2], 1e-6);
            double new_sse = sabr_sse(q, strikes, market_vols, n, F, T);
            if (new_sse < sse0) {
                p = q;
                break;
            }
            s *= 0.5;
        }
    }

    out = p;
    out.valid = true;
    return true;
}

} // namespace omm
