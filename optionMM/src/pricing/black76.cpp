#include "pricing/black76.h"

#include <cmath>

namespace omm {

namespace {

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

struct Black76Dispatch {
    Black76Backend     backend;
    BatchFn            batch;
    BatchPrecomputedFn precomputed;
};

static inline double norm_cdf(double x) noexcept {
    return 0.5 * std::erfc(-x * M_SQRT1_2);
}

static inline double norm_pdf(double x) noexcept {
    static constexpr double INV_SQRT_2PI = 0.3989422804014327;
    return INV_SQRT_2PI * std::exp(-0.5 * x * x);
}

void compute_batch_scalar_impl(const double*  F,
                               const double*  K,
                               const double*  T,
                               const double*  r,
                               const double*  sigma,
                               const uint8_t* is_call,
                               Black76Result* out,
                               int            count) noexcept {
    for (int i = 0; i < count; ++i) {
        out[i] = compute_scalar(F[i], K[i], T[i], r[i], sigma[i], is_call[i] != 0);
    }
}

void compute_batch_scalar_precomputed_impl(const double*  F,
                                           const double*  K,
                                           const double*  T,
                                           const double*  sqrt_T,
                                           const double*  disc,
                                           const double*  sigma,
                                           const uint8_t* is_call,
                                           Black76Result* out,
                                           int            count) noexcept {
    (void)sqrt_T;
    for (int i = 0; i < count; ++i) {
        const double r_val = (T[i] > 1e-10) ? -std::log(disc[i]) / T[i] : 0.0;
        out[i] = compute_scalar(F[i], K[i], T[i], r_val, sigma[i], is_call[i] != 0);
    }
}

bool cpu_supports_avx2_fma() noexcept {
#if (defined(__x86_64__) || defined(__i386__)) && (defined(__GNUC__) || defined(__clang__))
    static const bool supported = []() noexcept {
        __builtin_cpu_init();
        return __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");
    }();
    return supported;
#else
    return false;
#endif
}

bool cpu_supports_avx512f_fma() noexcept {
#if (defined(__x86_64__) || defined(__i386__)) && (defined(__GNUC__) || defined(__clang__))
    static const bool supported = []() noexcept {
        __builtin_cpu_init();
        return __builtin_cpu_supports("avx512f") && __builtin_cpu_supports("fma");
    }();
    return supported;
#else
    return false;
#endif
}

Black76Dispatch detect_dispatch() noexcept {
#ifdef OMM_BLACK76_AVX512_BACKEND
    if (cpu_supports_avx512f_fma()) {
        return {
            Black76Backend::Avx512,
            compute_batch_avx512,
            compute_batch_avx512_precomputed,
        };
    }
#endif

#ifdef OMM_BLACK76_AVX2_BACKEND
    if (cpu_supports_avx2_fma()) {
        return {
            Black76Backend::Avx2,
            compute_batch_avx2,
            compute_batch_avx2_precomputed,
        };
    }
#endif

    return {
        Black76Backend::Scalar,
        compute_batch_scalar_impl,
        compute_batch_scalar_precomputed_impl,
    };
}

const Black76Dispatch& get_dispatch() noexcept {
    static const Black76Dispatch dispatch = detect_dispatch();
    return dispatch;
}

} // namespace

const char* black76_backend_name() noexcept {
    switch (black76_backend()) {
        case Black76Backend::Avx512: return "avx512";
        case Black76Backend::Avx2: return "avx2";
        case Black76Backend::Scalar: return "scalar";
    }
    return "unknown";
}

Black76Backend black76_backend() noexcept {
    return get_dispatch().backend;
}

bool black76_backend_available(Black76Backend backend) noexcept {
    switch (backend) {
        case Black76Backend::Scalar:
            return true;
        case Black76Backend::Avx2:
#ifdef OMM_BLACK76_AVX2_BACKEND
            return cpu_supports_avx2_fma();
#else
            return false;
#endif
        case Black76Backend::Avx512:
#ifdef OMM_BLACK76_AVX512_BACKEND
            return cpu_supports_avx512f_fma();
#else
            return false;
#endif
    }
    return false;
}

Black76Result compute_scalar(double F, double K, double T, double r,
                             double sigma, bool is_call) noexcept {
    if (sigma < 1e-10 || T < 1e-10) {
        Black76Result z{};
        const double disc = std::exp(-r * T);
        if (is_call) {
            z.price = disc * std::fmax(F - K, 0.0);
        } else {
            z.price = disc * std::fmax(K - F, 0.0);
        }
        z.delta = (is_call && F > K) ? disc : (!is_call && K > F) ? -disc : 0.0;
        return z;
    }

    const double sqrt_T = std::sqrt(T);
    const double sigma_sqT = sigma * sqrt_T;
    const double d1 = (std::log(F / K) + 0.5 * sigma * sigma * T) / sigma_sqT;
    const double d2 = d1 - sigma_sqT;
    const double disc = std::exp(-r * T);

    const double nd1 = norm_pdf(d1);
    Black76Result res{};
    if (is_call) {
        const double Nd1 = norm_cdf(d1);
        const double Nd2 = norm_cdf(d2);
        res.price = disc * (F * Nd1 - K * Nd2);
        res.delta = disc * Nd1;
    } else {
        const double Nnd1 = norm_cdf(-d1);
        const double Nnd2 = norm_cdf(-d2);
        res.price = disc * (K * Nnd2 - F * Nnd1);
        res.delta = -disc * Nnd1;
    }

    res.gamma = disc * nd1 / (F * sigma_sqT);
    res.vega  = disc * F * nd1 * sqrt_T;
    res.theta = (-disc * F * nd1 * sigma / (2.0 * sqrt_T) - r * res.price) / 365.0;
    res.rho   = -T * res.price;
    return res;
}

void compute_batch(const double*  F,
                   const double*  K,
                   const double*  T,
                   const double*  r,
                   const double*  sigma,
                   const uint8_t* is_call,
                   Black76Result* out,
                   int            count) noexcept {
    get_dispatch().batch(F, K, T, r, sigma, is_call, out, count);
}

void compute_batch_precomputed(const double*  F,
                               const double*  K,
                               const double*  T,
                               const double*  sqrt_T,
                               const double*  disc,
                               const double*  sigma,
                               const uint8_t* is_call,
                               Black76Result* out,
                               int            count) noexcept {
    get_dispatch().precomputed(F, K, T, sqrt_T, disc, sigma, is_call, out, count);
}

void compute_batch_quote_precomputed(const double*  F,
                                     const double*  K,
                                     const double*  sqrt_T,
                                     const double*  disc,
                                     const double*  sigma,
                                     const uint8_t* is_call,
                                     Black76QuoteResult* out,
                                     int            count) noexcept {
    for (int i = 0; i < count; ++i) {
        Black76QuoteResult res{};
        if (F[i] < 1e-10 || K[i] < 1e-10 || sigma[i] < 1e-10 || sqrt_T[i] < 1e-10) {
            const bool call = is_call[i] != 0;
            res.price = disc[i] * (call ? std::fmax(F[i] - K[i], 0.0)
                                        : std::fmax(K[i] - F[i], 0.0));
            res.delta = (call && F[i] > K[i]) ? disc[i]
                      : (!call && K[i] > F[i]) ? -disc[i]
                      : 0.0;
            out[i] = res;
            continue;
        }

        const double sigma_sqrt_T = sigma[i] * sqrt_T[i];
        const double d1 = (std::log(F[i] / K[i]) + 0.5 * sigma[i] * sigma[i] * sqrt_T[i] * sqrt_T[i])
                        / sigma_sqrt_T;
        const double d2 = d1 - sigma_sqrt_T;
        const double nd1 = norm_pdf(d1);
        if (is_call[i] != 0) {
            res.price = disc[i] * (F[i] * norm_cdf(d1) - K[i] * norm_cdf(d2));
            res.delta = disc[i] * norm_cdf(d1);
        } else {
            res.price = disc[i] * (K[i] * norm_cdf(-d2) - F[i] * norm_cdf(-d1));
            res.delta = -disc[i] * norm_cdf(-d1);
        }
        res.gamma = disc[i] * nd1 / (F[i] * sigma_sqrt_T);
        res.vega = disc[i] * F[i] * nd1 * sqrt_T[i];
        out[i] = res;
    }
}

double implied_vol(double market_price, double F, double K, double T,
                   double r, bool is_call, double tol) noexcept {
    if (F < 1e-10 || K < 1e-10 || T < 1e-10) {
        return 0.0;
    }

    const double disc = std::exp(-r * T);
    const double intrinsic = is_call ? std::fmax(disc * (F - K), 0.0)
                                     : std::fmax(disc * (K - F), 0.0);
    if (market_price <= intrinsic + 1e-10) {
        return 0.0;
    }

    double lo = 1e-5;
    double hi = 5.0;
    if (compute_scalar(F, K, T, r, hi, is_call).price < market_price) {
        return 0.0;
    }

    for (int i = 0; i < 100; ++i) {
        const double mid = 0.5 * (lo + hi);
        const double p = compute_scalar(F, K, T, r, mid, is_call).price;
        if (p < market_price) {
            lo = mid;
        } else {
            hi = mid;
        }
        if (hi - lo < tol) {
            break;
        }
    }
    return 0.5 * (lo + hi);
}

} // namespace omm
