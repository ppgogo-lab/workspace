#include "pricing/wing.h"
#include <cmath>
#include <algorithm>

namespace omm {

// ─── Wing Levenberg-Marquardt fitter ─────────────────────────────────────────
// Minimizes sum-of-squared-errors in implied vol space:
//   residual_i = wing_iv(k_i; params) - market_vol_i
//
// The Wing model is linear in its 5 parameters, so the Jacobian is:
//   J(k) = [1, max(k,0), max(-k,0), max(k,0)^2, max(-k,0)^2]
// This means J^T J depends only on the strike points and is constant across
// all LM iterations — it is precomputed once before the loop.

bool fit_wing_slice(const double* strikes, const double* market_vols,
                    int n, double F, double T,
                    WingParams& out, int max_iter, double tol) noexcept {
    if (n < 5) return false;

    // Convert strikes to log-moneyness
    double k[MAX_STRIKES];
    for (int i = 0; i < n; ++i)
        k[i] = std::log(strikes[i] / F);

    // Initial guess: ATM_vol from vol at strike closest to F, slopes from
    // finite differences, curvatures from small positive default.
    WingParams p = out;
    p.expiry_T = T;

    // Find ATM vol (strike closest to F)
    double atm_vol = market_vols[0];
    double min_abs_k = std::fabs(k[0]);
    for (int i = 1; i < n; ++i) {
        if (std::fabs(k[i]) < min_abs_k) {
            min_abs_k = std::fabs(k[i]);
            atm_vol = market_vols[i];
        }
    }
    p.ATM_vol    = atm_vol;
    p.slope_call = -0.1;
    p.slope_put  = 0.1;
    p.curve_call = 0.01;
    p.curve_put  = 0.01;

    // Precompute J^T J (constant since Wing is linear in params)
    // J(k) = [1, kp, km, kp^2, km^2]  where kp=max(k,0), km=max(-k,0)
    double JtJ_base[5][5]{};
    for (int i = 0; i < n; ++i) {
        double kp = std::fmax(k[i], 0.0);
        double km = std::fmax(-k[i], 0.0);
        double J[5] = {1.0, kp, km, kp * kp, km * km};
        for (int a = 0; a < 5; ++a)
            for (int b = 0; b <= a; ++b)
                JtJ_base[a][b] += J[a] * J[b];
    }
    for (int a = 0; a < 5; ++a)
        for (int b = a + 1; b < 5; ++b)
            JtJ_base[a][b] = JtJ_base[b][a];

    double lambda = 1e-3;

    for (int iter = 0; iter < max_iter; ++iter) {
        // Compute residuals and J^T r
        double Jtr[5]{};
        double sse = 0.0;

        for (int i = 0; i < n; ++i) {
            double iv_m = WingVolSurface::wing_iv(p, k[i]);
            double r    = iv_m - market_vols[i];
            sse += r * r;
            double kp = std::fmax(k[i], 0.0);
            double km = std::fmax(-k[i], 0.0);
            double J[5] = {1.0, kp, km, kp * kp, km * km};
            for (int a = 0; a < 5; ++a)
                Jtr[a] += J[a] * r;
        }

        // Build augmented matrix: (JtJ_base + lambda*I) | -Jtr
        double A[5][6];
        for (int a = 0; a < 5; ++a) {
            for (int b = 0; b < 5; ++b) A[a][b] = JtJ_base[a][b];
            A[a][a] += lambda;
            A[a][5] = -Jtr[a];
        }

        // Gaussian elimination with partial pivoting
        for (int col = 0; col < 5; ++col) {
            int pivot = col;
            for (int row = col + 1; row < 5; ++row)
                if (std::fabs(A[row][col]) > std::fabs(A[pivot][col]))
                    pivot = row;
            for (int j = 0; j <= 5; ++j)
                std::swap(A[col][j], A[pivot][j]);
            if (std::fabs(A[col][col]) < 1e-14) continue;
            for (int row = 0; row < 5; ++row) {
                if (row == col) continue;
                double f = A[row][col] / A[col][col];
                for (int j = col; j <= 5; ++j)
                    A[row][j] -= f * A[col][j];
            }
        }
        double delta[5];
        for (int a = 0; a < 5; ++a)
            delta[a] = (std::fabs(A[a][a]) > 1e-14) ? A[a][5] / A[a][a] : 0.0;

        // Trial update with constraints
        WingParams q = p;
        q.ATM_vol    = std::fmax(q.ATM_vol    + delta[0], 1e-4);
        q.slope_call = std::fmax(-5.0, std::fmin(5.0, q.slope_call + delta[1]));
        q.slope_put  = std::fmax(-5.0, std::fmin(5.0, q.slope_put  + delta[2]));
        q.curve_call = std::fmax(q.curve_call + delta[3], 0.0);
        q.curve_put  = std::fmax(q.curve_put  + delta[4], 0.0);

        // Compute new SSE
        double new_sse = 0.0;
        for (int i = 0; i < n; ++i) {
            double r = WingVolSurface::wing_iv(q, k[i]) - market_vols[i];
            new_sse += r * r;
        }

        if (new_sse < sse) {
            p = q;
            lambda *= 0.3;
        } else {
            lambda *= 3.0;
        }

        double dnorm = std::sqrt(delta[0]*delta[0] + delta[1]*delta[1]
                                + delta[2]*delta[2] + delta[3]*delta[3]
                                + delta[4]*delta[4]);
        if (dnorm < tol) break;
    }

    out = p;
    out.valid = true;
    return true;
}

} // namespace omm