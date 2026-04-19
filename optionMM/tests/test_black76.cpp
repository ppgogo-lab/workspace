#include <gtest/gtest.h>

#include "pricing/black76.h"

#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

using namespace omm;

static constexpr double ABS_TOL = 1e-6;
static constexpr double SIMD_TOL = 1e-4;

using BatchFn = void (*)(
    const double*,
    const double*,
    const double*,
    const double*,
    const double*,
    const uint8_t*,
    Black76Result*,
    int) noexcept;

using BatchPrecomputedFn = void (*)(
    const double*,
    const double*,
    const double*,
    const double*,
    const double*,
    const double*,
    const uint8_t*,
    Black76Result*,
    int) noexcept;

static void fill_random_inputs(int count,
                               std::vector<double>& F,
                               std::vector<double>& K,
                               std::vector<double>& T,
                               std::vector<double>& r,
                               std::vector<double>& sigma,
                               std::vector<double>& sqrt_T,
                               std::vector<double>& disc,
                               std::vector<uint8_t>& is_call) {
    std::mt19937_64 rng(42);
    std::uniform_real_distribution<double> F_dist(80.0, 120.0);
    std::uniform_real_distribution<double> K_dist(70.0, 130.0);
    std::uniform_real_distribution<double> T_dist(0.05, 2.0);
    std::uniform_real_distribution<double> r_dist(0.01, 0.05);
    std::uniform_real_distribution<double> s_dist(0.10, 0.50);

    F.resize(count);
    K.resize(count);
    T.resize(count);
    r.resize(count);
    sigma.resize(count);
    sqrt_T.resize(count);
    disc.resize(count);
    is_call.resize(count);

    for (int i = 0; i < count; ++i) {
        F[i] = F_dist(rng);
        K[i] = K_dist(rng);
        T[i] = T_dist(rng);
        r[i] = r_dist(rng);
        sigma[i] = s_dist(rng);
        sqrt_T[i] = std::sqrt(T[i]);
        disc[i] = std::exp(-r[i] * T[i]);
        is_call[i] = (i % 2 == 0) ? 1u : 0u;
    }
}

static void expect_batch_matches_scalar(BatchFn fn, int count) {
    std::vector<double> F, K, T, r, sigma, sqrt_T, disc;
    std::vector<uint8_t> is_call;
    fill_random_inputs(count, F, K, T, r, sigma, sqrt_T, disc, is_call);

    std::vector<Black76Result> batch_out(count);
    std::vector<Black76Result> scalar_out(count);
    for (int i = 0; i < count; ++i) {
        scalar_out[i] = compute_scalar(F[i], K[i], T[i], r[i], sigma[i], is_call[i] != 0);
    }

    fn(F.data(), K.data(), T.data(), r.data(), sigma.data(), is_call.data(), batch_out.data(), count);

    for (int i = 0; i < count; ++i) {
        EXPECT_NEAR(batch_out[i].price, scalar_out[i].price, SIMD_TOL) << "price mismatch at i=" << i;
        EXPECT_NEAR(batch_out[i].delta, scalar_out[i].delta, SIMD_TOL) << "delta mismatch at i=" << i;
        EXPECT_NEAR(batch_out[i].gamma, scalar_out[i].gamma, SIMD_TOL) << "gamma mismatch at i=" << i;
        EXPECT_NEAR(batch_out[i].vega, scalar_out[i].vega, SIMD_TOL) << "vega mismatch at i=" << i;
        EXPECT_NEAR(batch_out[i].theta, scalar_out[i].theta, SIMD_TOL) << "theta mismatch at i=" << i;
        EXPECT_NEAR(batch_out[i].rho, scalar_out[i].rho, SIMD_TOL) << "rho mismatch at i=" << i;
    }
}

static void expect_batch_precomputed_matches_scalar(BatchPrecomputedFn fn, int count) {
    std::vector<double> F, K, T, r, sigma, sqrt_T, disc;
    std::vector<uint8_t> is_call;
    fill_random_inputs(count, F, K, T, r, sigma, sqrt_T, disc, is_call);

    std::vector<Black76Result> batch_out(count);
    std::vector<Black76Result> scalar_out(count);
    for (int i = 0; i < count; ++i) {
        scalar_out[i] = compute_scalar(F[i], K[i], T[i], r[i], sigma[i], is_call[i] != 0);
    }

    fn(F.data(), K.data(), T.data(), sqrt_T.data(), disc.data(), sigma.data(),
       is_call.data(), batch_out.data(), count);

    for (int i = 0; i < count; ++i) {
        EXPECT_NEAR(batch_out[i].price, scalar_out[i].price, SIMD_TOL) << "price mismatch at i=" << i;
        EXPECT_NEAR(batch_out[i].delta, scalar_out[i].delta, SIMD_TOL) << "delta mismatch at i=" << i;
        EXPECT_NEAR(batch_out[i].gamma, scalar_out[i].gamma, SIMD_TOL) << "gamma mismatch at i=" << i;
        EXPECT_NEAR(batch_out[i].vega, scalar_out[i].vega, SIMD_TOL) << "vega mismatch at i=" << i;
        EXPECT_NEAR(batch_out[i].theta, scalar_out[i].theta, SIMD_TOL) << "theta mismatch at i=" << i;
        EXPECT_NEAR(batch_out[i].rho, scalar_out[i].rho, SIMD_TOL) << "rho mismatch at i=" << i;
    }
}

TEST(Black76Scalar, ATMCall) {
    const auto res = compute_scalar(100.0, 100.0, 0.25, 0.03, 0.20, true);
    EXPECT_NEAR(res.price, 3.9580, 1e-3);
    EXPECT_NEAR(res.delta, 0.5161, 1e-3);
    EXPECT_GT(res.gamma, 0.0);
    EXPECT_GT(res.vega, 0.0);
    EXPECT_LT(res.theta, 0.0);
}

TEST(Black76Scalar, ATMPut) {
    const auto res = compute_scalar(100.0, 100.0, 0.25, 0.03, 0.20, false);
    EXPECT_NEAR(res.price, 3.9580, 1e-3);
    EXPECT_LT(res.delta, 0.0);
    EXPECT_GT(res.gamma, 0.0);
    EXPECT_GT(res.vega, 0.0);
    EXPECT_LT(res.theta, 0.0);
}

TEST(Black76Scalar, PutCallParity) {
    const double F = 105.0;
    const double K = 100.0;
    const double T = 0.5;
    const double r = 0.025;
    const double sigma = 0.25;
    const auto call = compute_scalar(F, K, T, r, sigma, true);
    const auto put = compute_scalar(F, K, T, r, sigma, false);
    const double disc = std::exp(-r * T);
    EXPECT_NEAR(call.price - put.price, (F - K) * disc, 1e-10);
}

TEST(Black76Scalar, PutCallParityOTM) {
    const double F = 90.0;
    const double K = 100.0;
    const double T = 1.0;
    const double r = 0.03;
    const double sigma = 0.30;
    const auto call = compute_scalar(F, K, T, r, sigma, true);
    const auto put = compute_scalar(F, K, T, r, sigma, false);
    const double disc = std::exp(-r * T);
    EXPECT_NEAR(call.price - put.price, (F - K) * disc, 1e-10);
}

TEST(Black76Scalar, NearZeroVol) {
    const auto res = compute_scalar(100.0, 100.0, 0.25, 0.03, 1e-11, true);
    EXPECT_FALSE(std::isnan(res.price));
    EXPECT_FALSE(std::isinf(res.price));
    EXPECT_FALSE(std::isnan(res.delta));
    EXPECT_FALSE(std::isinf(res.delta));
}

TEST(Black76Scalar, NearZeroT) {
    const auto res = compute_scalar(100.0, 100.0, 1e-11, 0.03, 0.20, true);
    EXPECT_FALSE(std::isnan(res.price));
    EXPECT_FALSE(std::isinf(res.price));
}

TEST(Black76Scalar, DeepITMCall) {
    const double F = 200.0;
    const double K = 100.0;
    const double T = 0.25;
    const double r = 0.03;
    const double sigma = 0.20;
    const auto res = compute_scalar(F, K, T, r, sigma, true);
    const double intrinsic = (F - K) * std::exp(-r * T);
    EXPECT_NEAR(res.price, intrinsic, 0.5);
    EXPECT_NEAR(res.delta, std::exp(-r * T), 0.01);
}

TEST(Black76Scalar, DeepOTMCall) {
    const auto res = compute_scalar(50.0, 200.0, 0.25, 0.03, 0.20, true);
    EXPECT_NEAR(res.price, 0.0, ABS_TOL);
    EXPECT_NEAR(res.delta, 0.0, ABS_TOL);
}

TEST(Black76Scalar, VegaSymmetry) {
    const auto call = compute_scalar(100.0, 95.0, 0.5, 0.03, 0.22, true);
    const auto put = compute_scalar(100.0, 95.0, 0.5, 0.03, 0.22, false);
    EXPECT_NEAR(call.vega, put.vega, 1e-10);
}

TEST(Black76Scalar, GammaSymmetry) {
    const auto call = compute_scalar(100.0, 95.0, 0.5, 0.03, 0.22, true);
    const auto put = compute_scalar(100.0, 95.0, 0.5, 0.03, 0.22, false);
    EXPECT_NEAR(call.gamma, put.gamma, 1e-10);
}

TEST(Black76Dispatch, PrefersHighestAvailableBackend) {
    const bool has_avx512 = black76_backend_available(Black76Backend::Avx512);
    const bool has_avx2 = black76_backend_available(Black76Backend::Avx2);
    const Black76Backend backend = black76_backend();

    if (has_avx512) {
        EXPECT_EQ(backend, Black76Backend::Avx512);
        EXPECT_STREQ(black76_backend_name(), "avx512");
    } else if (has_avx2) {
        EXPECT_EQ(backend, Black76Backend::Avx2);
        EXPECT_STREQ(black76_backend_name(), "avx2");
    } else {
        EXPECT_EQ(backend, Black76Backend::Scalar);
        EXPECT_STREQ(black76_backend_name(), "scalar");
    }
}

TEST(Black76Dispatch, BatchMatchesScalar) {
    expect_batch_matches_scalar(compute_batch, 19);
}

TEST(Black76Dispatch, BatchPrecomputedMatchesScalar) {
    expect_batch_precomputed_matches_scalar(compute_batch_precomputed, 19);
}

#ifdef OMM_BLACK76_AVX2_BACKEND
TEST(Black76AVX2, BatchMatchesScalar) {
    if (!black76_backend_available(Black76Backend::Avx2)) {
        GTEST_SKIP() << "AVX2 backend not available on this host";
    }
    expect_batch_matches_scalar(compute_batch_avx2, 20);
}

TEST(Black76AVX2, BatchPrecomputedMatchesScalar) {
    if (!black76_backend_available(Black76Backend::Avx2)) {
        GTEST_SKIP() << "AVX2 backend not available on this host";
    }
    expect_batch_precomputed_matches_scalar(compute_batch_avx2_precomputed, 20);
}

TEST(Black76AVX2, TailHandled) {
    if (!black76_backend_available(Black76Backend::Avx2)) {
        GTEST_SKIP() << "AVX2 backend not available on this host";
    }
    expect_batch_matches_scalar(compute_batch_avx2, 7);
}

TEST(Black76AVX2, BatchNoNaN) {
    if (!black76_backend_available(Black76Backend::Avx2)) {
        GTEST_SKIP() << "AVX2 backend not available on this host";
    }

    constexpr int N = 8;
    const double F[N] = {100, 100, 100, 100, 100, 100, 100, 100};
    const double K[N] = {80, 90, 100, 110, 120, 50, 200, 100};
    const double T[N] = {0.25, 0.5, 1.0, 0.1, 2.0, 0.25, 0.5, 0.001};
    const double r[N] = {0.03, 0.03, 0.03, 0.03, 0.03, 0.03, 0.03, 0.03};
    const double sigma[N] = {0.20, 0.25, 0.30, 0.15, 0.40, 0.20, 0.20, 0.20};
    const uint8_t call[N] = {1, 0, 1, 0, 1, 1, 0, 1};
    Black76Result out[N];

    compute_batch_avx2(F, K, T, r, sigma, call, out, N);
    for (int i = 0; i < N; ++i) {
        EXPECT_FALSE(std::isnan(out[i].price)) << "NaN price at i=" << i;
        EXPECT_FALSE(std::isinf(out[i].price)) << "Inf price at i=" << i;
        EXPECT_FALSE(std::isnan(out[i].delta)) << "NaN delta at i=" << i;
    }
}
#endif

#ifdef OMM_BLACK76_AVX512_BACKEND
TEST(Black76AVX512, BatchMatchesScalar) {
    if (!black76_backend_available(Black76Backend::Avx512)) {
        GTEST_SKIP() << "AVX-512 backend not available on this host";
    }
    expect_batch_matches_scalar(compute_batch_avx512, 24);
}

TEST(Black76AVX512, BatchPrecomputedMatchesScalar) {
    if (!black76_backend_available(Black76Backend::Avx512)) {
        GTEST_SKIP() << "AVX-512 backend not available on this host";
    }
    expect_batch_precomputed_matches_scalar(compute_batch_avx512_precomputed, 24);
}

TEST(Black76AVX512, TailHandled) {
    if (!black76_backend_available(Black76Backend::Avx512)) {
        GTEST_SKIP() << "AVX-512 backend not available on this host";
    }
    expect_batch_matches_scalar(compute_batch_avx512, 17);
}

TEST(Black76AVX512, BatchNoNaN) {
    if (!black76_backend_available(Black76Backend::Avx512)) {
        GTEST_SKIP() << "AVX-512 backend not available on this host";
    }

    constexpr int N = 16;
    const double F[N] = {100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100};
    const double K[N] = {80, 90, 100, 110, 120, 50, 200, 100, 85, 95, 105, 115, 125, 135, 145, 155};
    const double T[N] = {0.25, 0.5, 1.0, 0.1, 2.0, 0.25, 0.5, 0.001, 0.35, 0.6, 0.9, 1.2, 1.4, 1.6, 1.8, 0.2};
    const double r[N] = {0.03, 0.03, 0.03, 0.03, 0.03, 0.03, 0.03, 0.03, 0.025, 0.025, 0.025, 0.025, 0.02, 0.02, 0.02, 0.02};
    const double sigma[N] = {0.20, 0.25, 0.30, 0.15, 0.40, 0.20, 0.20, 0.20, 0.18, 0.21, 0.24, 0.27, 0.30, 0.33, 0.36, 0.39};
    const uint8_t call[N] = {1, 0, 1, 0, 1, 1, 0, 1, 1, 0, 1, 0, 1, 0, 1, 0};
    Black76Result out[N];

    compute_batch_avx512(F, K, T, r, sigma, call, out, N);
    for (int i = 0; i < N; ++i) {
        EXPECT_FALSE(std::isnan(out[i].price)) << "NaN price at i=" << i;
        EXPECT_FALSE(std::isinf(out[i].price)) << "Inf price at i=" << i;
        EXPECT_FALSE(std::isnan(out[i].delta)) << "NaN delta at i=" << i;
    }
}
#endif

TEST(ImpliedVol, RoundTripCall) {
    const double F = 100.0;
    const double K = 100.0;
    const double T = 0.25;
    const double r = 0.03;
    const double sigma = 0.20;
    const double price = compute_scalar(F, K, T, r, sigma, true).price;
    const double iv = implied_vol(price, F, K, T, r, true);
    EXPECT_NEAR(iv, sigma, 1e-5);
}

TEST(ImpliedVol, RoundTripPut) {
    const double F = 100.0;
    const double K = 95.0;
    const double T = 0.5;
    const double r = 0.02;
    const double sigma = 0.30;
    const double price = compute_scalar(F, K, T, r, sigma, false).price;
    const double iv = implied_vol(price, F, K, T, r, false);
    EXPECT_NEAR(iv, sigma, 1e-5);
}

TEST(ImpliedVol, BelowIntrinsicReturnsZero) {
    const double iv = implied_vol(0.0, 100.0, 50.0, 1.0, 0.03, true);
    EXPECT_NEAR(iv, 0.0, 1e-10);
}

TEST(ImpliedVol, HighVolSkew) {
    const double F = 5000.0;
    const double K = 4500.0;
    const double T = 0.083;
    const double r = 0.025;
    const double sigma = 0.40;
    const double price = compute_scalar(F, K, T, r, sigma, false).price;
    const double iv = implied_vol(price, F, K, T, r, false);
    EXPECT_NEAR(iv, sigma, 1e-5);
}
