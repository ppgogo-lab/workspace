#include "pricing/svi.h"
#include <cmath>
#include <algorithm>

namespace omm {

// ─── SVI Levenberg-Marquardt fitter ──────────────────────────────────────────
// Minimizes sum-of-squared-errors in implied vol space:
//   residual_i = iv_model(k_i, T; params) - market_vol_i
// Analytic Jacobian dr/dparams avoids finite differences.
//
// State vector x = {a, b, rho, m, sigma} (5 parameters).

static double svi_iv(const SVIParams& p, double k, double T) noexcept {
    double wk = SVIVolSurface::w(p, k);
    if (wk < 0.0 || T < 1e-10) return 0.0;
    return std::sqrt(wk / T);
}

// Jacobian of iv w.r.t. params {a, b, rho, m, sigma}
static void svi_jacobian(const SVIParams& p, double k, double T,
                          double* J) noexcept {
    double dk     = k - p.m;
    double sq     = std::sqrt(dk * dk + p.sigma * p.sigma);
    double wk     = SVIVolSurface::w(p, k);
    if (wk < 1e-14 || T < 1e-10) {
        for (int i = 0; i < 5; ++i) J[i] = 0.0;
        return;
    }
    double iv = std::sqrt(wk / T);
    double dw_div = 0.5 / (iv * T);  // d(iv)/d(w) = 1/(2*iv*T)

    // dw/da = 1
    J[0] = dw_div;
    // dw/db = rho*(k-m) + sqrt((k-m)^2 + sigma^2)
    J[1] = dw_div * (p.rho * dk + sq);
    // dw/drho = b*(k-m)
    J[2] = dw_div * (p.b * dk);
    // dw/dm = b*(-rho + (m-k)/sq)
    J[3] = dw_div * (p.b * (-p.rho - dk / sq));
    // dw/dsigma = b*sigma/sq
    J[4] = dw_div * (p.b * p.sigma / sq);
}

bool fit_svi_slice(const double* strikes, const double* market_vols,
                   int n, double F, double T,
                   SVIParams& out, int max_iter, double tol) noexcept {
    if (n < 5) return false;  // need at least 5 points for 5 parameters

    // Convert strikes to log-moneyness
    double k[MAX_STRIKES];
    for (int i = 0; i < n; ++i)
        k[i] = std::log(strikes[i] / F);

    // Initial parameters: flat vol surface matching average variance
    double avg_var = 0.0;
    for (int i = 0; i < n; ++i) avg_var += market_vols[i] * market_vols[i];
    avg_var /= n;

    SVIParams p = out;
    p.a = avg_var * T * 0.9;
    p.b = 0.1;
    p.rho = -0.2;
    p.m = 0.0;
    p.sigma = 0.2;
    p.expiry_T = T;

    double lambda = 1e-3;  // LM damping factor

    for (int iter = 0; iter < max_iter; ++iter) {
        // Compute residuals and JtJ, Jtr
        double JtJ[5][5]{};
        double Jtr[5]{};
        double sse = 0.0;

        for (int i = 0; i < n; ++i) {
            double iv_m = svi_iv(p, k[i], T);
            double r    = iv_m - market_vols[i];
            sse += r * r;
            double J[5];
            svi_jacobian(p, k[i], T, J);
            for (int a = 0; a < 5; ++a) {
                Jtr[a] += J[a] * r;
                for (int b = 0; b <= a; ++b)
                    JtJ[a][b] += J[a] * J[b];
            }
        }
        // Symmetrise
        for (int a = 0; a < 5; ++a)
            for (int b = a + 1; b < 5; ++b)
                JtJ[a][b] = JtJ[b][a];

        // Add LM damping
        for (int a = 0; a < 5; ++a)
            JtJ[a][a] += lambda;

        // Solve 5x5 linear system (JtJ + lambda*I) * delta = -Jtr
        // Gaussian elimination with partial pivoting
        double A[5][6];
        for (int a = 0; a < 5; ++a) {
            for (int b = 0; b < 5; ++b) A[a][b] = JtJ[a][b];
            A[a][5] = -Jtr[a];
        }
        for (int col = 0; col < 5; ++col) {
            // Pivot
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

        // Trial update
        SVIParams q = p;
        q.a += delta[0];
        q.b  = std::fmax(q.b + delta[1], 1e-6);
        q.rho = std::fmax(-0.9999, std::fmin(0.9999, q.rho + delta[2]));
        q.m  += delta[3];
        q.sigma = std::fmax(q.sigma + delta[4], 1e-6);

        // Compute new SSE
        double new_sse = 0.0;
        for (int i = 0; i < n; ++i) {
            double r = svi_iv(q, k[i], T) - market_vols[i];
            new_sse += r * r;
        }

        if (new_sse < sse) {
            p = q;
            lambda *= 0.3;
        } else {
            lambda *= 3.0;
        }

        if (std::sqrt(delta[0]*delta[0] + delta[1]*delta[1] + delta[2]*delta[2]
                      + delta[3]*delta[3] + delta[4]*delta[4]) < tol)
            break;
    }

    out = p;
    out.valid = true;
    return true;
}

} // namespace omm
