#include "pricing/cubic_spline.h"
#include <cstring>
#include <algorithm>

namespace omm {

// ─── Natural cubic spline fitting ────────────────────────────────────────────
// Solves the tridiagonal system for second derivatives using the Thomas algorithm.
// Input: n sorted (k, vol) pairs.
// Output: SplineSlice with knots, values, and second derivatives.

void fit_cubic_spline_slice(const double* k_values, const double* vol_values,
                             int n, double T, SplineSlice& out) noexcept {
    if (n < 2 || n > MAX_STRIKES) return;

    out.T = T;
    out.n = 0;
    out.valid = false;

    // Deduplicate knots: average vols at identical log-moneyness values
    for (int i = 0; i < n; ++i) {
        if (out.n > 0 && k_values[i] == out.k[out.n - 1]) {
            // Average with existing knot
            out.v[out.n - 1] = 0.5 * (out.v[out.n - 1] + vol_values[i]);
        } else {
            out.k[out.n] = k_values[i];
            out.v[out.n] = vol_values[i];
            out.c[out.n] = 0.0;
            ++out.n;
        }
    }
    int nn = out.n;  // effective number of unique knots

    if (nn == 2) {
        // Linear — second derivatives are zero (natural spline trivially satisfied)
        out.valid = true;
        return;
    }

    // Thomas algorithm for natural cubic spline
    // System: h[i-1]*c[i-1] + 2*(h[i-1]+h[i])*c[i] + h[i]*c[i+1] = rhs[i]
    // where h[i] = k[i+1] - k[i], natural BC: c[0] = c[n-1] = 0

    double h[MAX_STRIKES];
    for (int i = 0; i < nn - 1; ++i)
        h[i] = out.k[i + 1] - out.k[i];

    // Build RHS
    double rhs[MAX_STRIKES]{};
    for (int i = 1; i < nn - 1; ++i)
        rhs[i] = 6.0 * ((out.v[i + 1] - out.v[i]) / h[i]
                       - (out.v[i] - out.v[i - 1]) / h[i - 1]);

    // Forward sweep (Thomas)
    double diag[MAX_STRIKES]{};
    double upper[MAX_STRIKES]{};
    for (int i = 1; i < nn - 1; ++i) {
        diag[i]  = 2.0 * (h[i - 1] + h[i]);
        upper[i] = h[i];
    }
    // Natural BC: c[0] = c[n-1] = 0 — eliminate boundary rows
    for (int i = 2; i < nn - 1; ++i) {
        double factor = h[i - 1] / diag[i - 1];
        diag[i]  -= factor * upper[i - 1];
        rhs[i]   -= factor * rhs[i - 1];
    }
    // Back substitution
    out.c[nn - 1] = 0.0;
    for (int i = nn - 2; i >= 1; --i)
        out.c[i] = (std::fabs(diag[i]) > 1e-14)
                   ? (rhs[i] - upper[i] * out.c[i + 1]) / diag[i]
                   : 0.0;
    out.c[0] = 0.0;

    out.valid = true;
}

} // namespace omm
