#include <gtest/gtest.h>
#include "pricing/black76.h"

#include <cmath>
#include <cstdint>
#include <random>

using namespace omm;

// Tolerance for scalar vs reference comparisons
static constexpr double ABS_TOL = 1e-6;
// Tolerance for SIMD vs scalar comparisons.
// A&S 7.1.26 polynomial erf approximation has max error ~1.5e-7 per erf call;
// two norm_cdf calls (each using erf) compound to ~1e-5 absolute price error.
static constexpr double SIMD_TOL = 1e-4;

// ─── Scalar: ATM call reference values ───────────────────────────────────────
// Black-76: F=100, K=100, T=0.25, r=0.03, sigma=0.20
// ATM call = ATM put (put-call parity: (F-K)*disc = 0 when F=K)
// price ≈ 3.9580, delta ≈ 0.5161 (disc * N(d1), d1 = sigma*sqrt(T)/2 > 0)
TEST(Black76Scalar, ATMCall) {
    auto res = compute_scalar(100.0, 100.0, 0.25, 0.03, 0.20, true);
    EXPECT_NEAR(res.price, 3.9580, 1e-3);
    EXPECT_NEAR(res.delta, 0.5161, 1e-3);
    EXPECT_GT(res.gamma, 0.0);
    EXPECT_GT(res.vega,  0.0);
    EXPECT_LT(res.theta, 0.0);  // theta is always negative for long options
}

// ─── Scalar: ATM put reference values ────────────────────────────────────────
// Black-76 ATM: call price == put price (F=K, so (F-K)*disc = 0)
TEST(Black76Scalar, ATMPut) {
    auto res = compute_scalar(100.0, 100.0, 0.25, 0.03, 0.20, false);
    EXPECT_NEAR(res.price, 3.9580, 1e-3);
    EXPECT_LT(res.delta, 0.0);   // put delta is negative
    EXPECT_GT(res.gamma, 0.0);
    EXPECT_GT(res.vega,  0.0);
    EXPECT_LT(res.theta, 0.0);
}

// ─── Put-call parity ─────────────────────────────────────────────────────────
// call - put = (F - K) * exp(-r*T)
TEST(Black76Scalar, PutCallParity) {
    const double F = 105.0, K = 100.0, T = 0.5, r = 0.025, sigma = 0.25;
    auto call = compute_scalar(F, K, T, r, sigma, true);
    auto put  = compute_scalar(F, K, T, r, sigma, false);
    double disc = std::exp(-r * T);
    double parity = (F - K) * disc;
    EXPECT_NEAR(call.price - put.price, parity, 1e-10);
}

// ─── Put-call parity: OTM ────────────────────────────────────────────────────
TEST(Black76Scalar, PutCallParityOTM) {
    const double F = 90.0, K = 100.0, T = 1.0, r = 0.03, sigma = 0.30;
    auto call = compute_scalar(F, K, T, r, sigma, true);
    auto put  = compute_scalar(F, K, T, r, sigma, false);
    double disc = std::exp(-r * T);
    EXPECT_NEAR(call.price - put.price, (F - K) * disc, 1e-10);
}

// ─── Near-zero vol: no NaN/inf ────────────────────────────────────────────────
TEST(Black76Scalar, NearZeroVol) {
    auto res = compute_scalar(100.0, 100.0, 0.25, 0.03, 1e-11, true);
    EXPECT_FALSE(std::isnan(res.price));
    EXPECT_FALSE(std::isinf(res.price));
    EXPECT_FALSE(std::isnan(res.delta));
    EXPECT_FALSE(std::isinf(res.delta));
}

// ─── Near-zero T: no NaN/inf ─────────────────────────────────────────────────
TEST(Black76Scalar, NearZeroT) {
    auto res = compute_scalar(100.0, 100.0, 1e-11, 0.03, 0.20, true);
    EXPECT_FALSE(std::isnan(res.price));
    EXPECT_FALSE(std::isinf(res.price));
}

// ─── Deep ITM call: price ≈ intrinsic value ───────────────────────────────────
TEST(Black76Scalar, DeepITMCall) {
    // F >> K: call price ≈ (F - K) * disc
    const double F = 200.0, K = 100.0, T = 0.25, r = 0.03, sigma = 0.20;
    auto res = compute_scalar(F, K, T, r, sigma, true);
    double intrinsic = (F - K) * std::exp(-r * T);
    EXPECT_NEAR(res.price, intrinsic, 0.5);  // within 50 cents
    EXPECT_NEAR(res.delta, std::exp(-r * T), 0.01);  // delta ≈ disc
}

// ─── Deep OTM call: price ≈ 0 ────────────────────────────────────────────────
TEST(Black76Scalar, DeepOTMCall) {
    auto res = compute_scalar(50.0, 200.0, 0.25, 0.03, 0.20, true);
    EXPECT_NEAR(res.price, 0.0, 1e-6);
    EXPECT_NEAR(res.delta, 0.0, 1e-6);
}

// ─── Vega symmetry: call vega == put vega ────────────────────────────────────
TEST(Black76Scalar, VegaSymmetry) {
    const double F = 100.0, K = 95.0, T = 0.5, r = 0.03, sigma = 0.22;
    auto call = compute_scalar(F, K, T, r, sigma, true);
    auto put  = compute_scalar(F, K, T, r, sigma, false);
    EXPECT_NEAR(call.vega, put.vega, 1e-10);
}

// ─── Gamma symmetry: call gamma == put gamma ─────────────────────────────────
TEST(Black76Scalar, GammaSymmetry) {
    const double F = 100.0, K = 95.0, T = 0.5, r = 0.03, sigma = 0.22;
    auto call = compute_scalar(F, K, T, r, sigma, true);
    auto put  = compute_scalar(F, K, T, r, sigma, false);
    EXPECT_NEAR(call.gamma, put.gamma, 1e-10);
}

// ─── AVX2 batch matches scalar ────────────────────────────────────────────────
#ifdef __AVX2__
TEST(Black76AVX2, BatchMatchesScalar) {
    constexpr int N = 20;
    std::mt19937_64 rng(42);
    std::uniform_real_distribution<double> F_dist(80.0, 120.0);
    std::uniform_real_distribution<double> K_dist(70.0, 130.0);
    std::uniform_real_distribution<double> T_dist(0.05, 2.0);
    std::uniform_real_distribution<double> r_dist(0.01, 0.05);
    std::uniform_real_distribution<double> s_dist(0.10, 0.50);

    double F[N], K[N], T[N], r[N], sigma[N];
    uint8_t is_call[N];
    Black76Result batch_out[N];
    Black76Result scalar_out[N];

    for (int i = 0; i < N; ++i) {
        F[i]     = F_dist(rng);
        K[i]     = K_dist(rng);
        T[i]     = T_dist(rng);
        r[i]     = r_dist(rng);
        sigma[i] = s_dist(rng);
        is_call[i] = (i % 2 == 0) ? 1 : 0;
        scalar_out[i] = compute_scalar(F[i], K[i], T[i], r[i], sigma[i], is_call[i] != 0);
    }

    compute_batch_avx2(F, K, T, r, sigma, is_call, batch_out, N);

    for (int i = 0; i < N; ++i) {
        EXPECT_NEAR(batch_out[i].price, scalar_out[i].price, SIMD_TOL)
            << "price mismatch at i=" << i;
        EXPECT_NEAR(batch_out[i].delta, scalar_out[i].delta, SIMD_TOL)
            << "delta mismatch at i=" << i;
        EXPECT_NEAR(batch_out[i].gamma, scalar_out[i].gamma, SIMD_TOL)
            << "gamma mismatch at i=" << i;
        EXPECT_NEAR(batch_out[i].vega,  scalar_out[i].vega,  SIMD_TOL)
            << "vega mismatch at i=" << i;
    }
}

TEST(Black76AVX2, BatchNonMultipleOf4) {
    // Test that the tail (count % 4 != 0) is handled correctly
    constexpr int N = 7;
    double F[N], K[N], T[N], r[N], sigma[N];
    uint8_t is_call[N];
    Black76Result batch_out[N], scalar_out[N];

    for (int i = 0; i < N; ++i) {
        F[i] = 100.0; K[i] = 100.0; T[i] = 0.25;
        r[i] = 0.03; sigma[i] = 0.20;
        is_call[i] = 1;
        scalar_out[i] = compute_scalar(F[i], K[i], T[i], r[i], sigma[i], true);
    }
    compute_batch_avx2(F, K, T, r, sigma, is_call, batch_out, N);
    for (int i = 0; i < N; ++i)
        EXPECT_NEAR(batch_out[i].price, scalar_out[i].price, SIMD_TOL);
}

TEST(Black76AVX2, BatchNoNaN) {
    // Verify no NaN/inf in batch output for a range of inputs
    constexpr int N = 8;
    double F[N]     = {100, 100, 100, 100, 100, 100, 100, 100};
    double K[N]     = { 80,  90, 100, 110, 120,  50, 200, 100};
    double T[N]     = {0.25, 0.5, 1.0, 0.1, 2.0, 0.25, 0.5, 0.001};
    double r[N]     = {0.03, 0.03, 0.03, 0.03, 0.03, 0.03, 0.03, 0.03};
    double sig[N]   = {0.20, 0.25, 0.30, 0.15, 0.40, 0.20, 0.20, 0.20};
    uint8_t call[N] = {1, 0, 1, 0, 1, 1, 0, 1};
    Black76Result out[N];
    compute_batch_avx2(F, K, T, r, sig, call, out, N);
    for (int i = 0; i < N; ++i) {
        EXPECT_FALSE(std::isnan(out[i].price)) << "NaN price at i=" << i;
        EXPECT_FALSE(std::isinf(out[i].price)) << "Inf price at i=" << i;
        EXPECT_FALSE(std::isnan(out[i].delta)) << "NaN delta at i=" << i;
    }
}
#endif // __AVX2__

// ─── implied_vol: round-trip (price → IV → price matches) ────────────────────
TEST(ImpliedVol, RoundTripCall) {
    const double F = 100.0, K = 100.0, T = 0.25, r = 0.03, sigma = 0.20;
    double price = compute_scalar(F, K, T, r, sigma, true).price;
    double iv    = implied_vol(price, F, K, T, r, true);
    EXPECT_NEAR(iv, sigma, 1e-5);
}

TEST(ImpliedVol, RoundTripPut) {
    const double F = 100.0, K = 95.0, T = 0.5, r = 0.02, sigma = 0.30;
    double price = compute_scalar(F, K, T, r, sigma, false).price;
    double iv    = implied_vol(price, F, K, T, r, false);
    EXPECT_NEAR(iv, sigma, 1e-5);
}

TEST(ImpliedVol, BelowIntrinsicReturnsZero) {
    // Deep ITM call, price below intrinsic → should return 0
    double iv = implied_vol(0.0, 100.0, 50.0, 1.0, 0.03, true);
    EXPECT_NEAR(iv, 0.0, 1e-10);
}

TEST(ImpliedVol, HighVolSkew) {
    // OTM put with steep skew: 40% vol
    const double F = 5000.0, K = 4500.0, T = 0.083, r = 0.025, sigma = 0.40;
    double price = compute_scalar(F, K, T, r, sigma, false).price;
    double iv    = implied_vol(price, F, K, T, r, false);
    EXPECT_NEAR(iv, sigma, 1e-5);
}
