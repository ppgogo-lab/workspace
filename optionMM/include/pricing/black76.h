#pragma once

#include <cstdint>
#include <cmath>
#include <immintrin.h>  // AVX2 intrinsics

namespace omm {

// ─── Black-76 result ──────────────────────────────────────────────────────────
// Layout matches Greeks in types.h (same field order, same types).
// Callers can memcpy into a Greeks struct after filling instrument_id/iv/T/calc_ts.
struct Black76Result {
    double price;
    double delta;   // ∂V/∂F
    double gamma;   // ∂²V/∂F²
    double vega;    // ∂V/∂σ
    double theta;   // ∂V/∂T  (per calendar day)
    double rho;     // ∂V/∂r
};

// ─── Scalar pricer ────────────────────────────────────────────────────────────
// Reference implementation. Used as fallback and in tests.
// Returns all-zeros if sigma < 1e-10 or T < 1e-10 (prevents NaN on critical path).
[[nodiscard]] Black76Result compute_scalar(
    double F,       // forward price
    double K,       // strike
    double T,       // time to expiry in years
    double r,       // risk-free rate (annualised)
    double sigma,   // implied volatility
    bool   is_call  // true = call, false = put
) noexcept;

// ─── AVX2 batch pricer ────────────────────────────────────────────────────────
// Processes `count` options in batches of 4 using 256-bit SIMD.
// All input arrays must have at least `count` elements.
// Output array `out` must have at least `count` elements.
// Arrays do NOT need to be aligned (uses _mm256_loadu_pd).
// Compiled only when __AVX2__ is defined (set via -mavx2).
#ifdef __AVX2__
void compute_batch_avx2(
    const double*  F,
    const double*  K,
    const double*  T,
    const double*  r,
    const double*  sigma,
    const uint8_t* is_call,   // 1 = call, 0 = put
    Black76Result* out,
    int            count
) noexcept;

// Optimized batch pricer with pre-computed sqrt(T) and exp(-r*T).
// Used when T and r are constant across the batch (same expiry, same rate).
void compute_batch_avx2_precomputed(
    const double*  F,
    const double*  K,
    const double*  T,
    const double*  sqrt_T,    // pre-computed sqrt(T) per option
    const double*  disc,      // pre-computed exp(-r*T) per option
    const double*  sigma,
    const uint8_t* is_call,
    Black76Result* out,
    int            count
) noexcept;
#endif

// ─── Implied volatility inversion ────────────────────────────────────────────
// Bisection on Black-76 price → IV. Returns 0.0 if market_price is below
// intrinsic or the bid-ask spread is too wide to invert reliably.
// Precision: converges to within tol in at most ~52 iterations (64-bit bisection).
// Not on the critical path — called only by the vol fitter side thread.
[[nodiscard]] double implied_vol(
    double market_price,  // option mid-price
    double F,             // forward price
    double K,             // strike
    double T,             // time to expiry in years
    double r,             // risk-free rate
    bool   is_call,       // true = call, false = put
    double tol = 1e-7     // convergence tolerance on IV
) noexcept;

// ─── Dispatch helpers ─────────────────────────────────────────────────────────
// Calls AVX2 batch if available, otherwise falls back to scalar loop.
inline void compute_batch(
    const double*  F,
    const double*  K,
    const double*  T,
    const double*  r,
    const double*  sigma,
    const uint8_t* is_call,
    Black76Result* out,
    int            count
) noexcept {
#ifdef __AVX2__
    compute_batch_avx2(F, K, T, r, sigma, is_call, out, count);
#else
    for (int i = 0; i < count; ++i)
        out[i] = compute_scalar(F[i], K[i], T[i], r[i], sigma[i], is_call[i] != 0);
#endif
}

// Dispatch for precomputed sqrt(T) and disc variant.
inline void compute_batch_precomputed(
    const double*  F,
    const double*  K,
    const double*  T,
    const double*  sqrt_T,
    const double*  disc,
    const double*  sigma,
    const uint8_t* is_call,
    Black76Result* out,
    int            count
) noexcept {
#ifdef __AVX2__
    compute_batch_avx2_precomputed(F, K, T, sqrt_T, disc, sigma, is_call, out, count);
#else
    for (int i = 0; i < count; ++i) {
        double r_val = (T[i] > 1e-10) ? -std::log(disc[i]) / T[i] : 0.0;
        out[i] = compute_scalar(F[i], K[i], T[i], r_val, sigma[i], is_call[i] != 0);
    }
#endif
}

} // namespace omm
