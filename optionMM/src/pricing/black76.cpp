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

static inline double safe_multiplier(double value) noexcept {
    return value > 1e-12 ? value : 1.0;
}

static inline double safe_trading_days(double value) noexcept {
    return value > 1e-12 ? value : 252.0;
}

Black76Result compute_from_precomputed_scalar(double F,
                                              double K,
                                              double T,
                                              double sqrt_T,
                                              double disc,
                                              double r,
                                              double sigma,
                                              bool   is_call,
                                              double option_multiplier,
                                              double future_multiplier,
                                              double trading_days_per_year) noexcept {
    const double Om = safe_multiplier(option_multiplier);
    const double Fm = safe_multiplier(future_multiplier);
    const double DY = safe_trading_days(trading_days_per_year);

    if (F < 1e-10 || K < 1e-10 || sigma < 1e-10 || T < 1e-10 || sqrt_T < 1e-10) {
        Black76Result z{};
        if (is_call) {
            z.price = disc * std::fmax(F - K, 0.0);
            z.std_delta = (F > K) ? disc : 0.0;
        } else {
            z.price = disc * std::fmax(K - F, 0.0);
            z.std_delta = (K > F) ? -disc : 0.0;
        }
        z.delta = z.std_delta * Om / Fm;
        z.delta_cash = z.std_delta * Om * F;
        return z;
    }

    const double sigma_sqrt_T = sigma * sqrt_T;
    const double d1 = (std::log(F / K) + 0.5 * sigma * sigma * T) / sigma_sqrt_T;
    const double d2 = d1 - sigma_sqrt_T;
    const double nd1 = norm_pdf(d1);
    const double s = is_call ? 1.0 : -1.0;
    const double Ns_d1 = norm_cdf(s * d1);
    const double Ns_d2 = norm_cdf(s * d2);

    Black76Result res{};
    // V = D * s * [F * N(s*d1) - K * N(s*d2)]
    res.price = disc * s * (F * Ns_d1 - K * Ns_d2);
    // Std delta = D * s * N(s*d1)
    res.std_delta = disc * s * Ns_d1;
    // Delta = Std delta * Om / Fm
    res.delta = res.std_delta * Om / Fm;
    // Delta cash = Std delta * Om * F
    res.delta_cash = res.std_delta * Om * F;
    // Std Gamma = D * n(d1) / [F * sigma * sqrt(T)]
    res.std_gamma = disc * nd1 / (F * sigma_sqrt_T);
    // Gamma = Std Gamma * Om * F * 0.01 / Fm
    res.gamma = res.std_gamma * Om * F * 0.01 / Fm;
    // Gamma cash = Std Gamma * Om * F * F * 0.01
    res.gamma_cash = res.std_gamma * Om * F * F * 0.01;
    // Vega = D * F * n(d1) * sqrt(T) * 0.01
    res.vega = disc * F * nd1 * sqrt_T * 0.01;
    // Vega cash = Vega * Om
    res.vega_cash = res.vega * Om;
    // Theta = 1 / DY * [-D * F * n(d1) * sigma / [2 * sqrt(T)] + r * V]
    res.theta = (-disc * F * nd1 * sigma / (2.0 * sqrt_T) + r * res.price) / DY;
    // Theta cash = Theta * Om
    res.theta_cash = res.theta * Om;
    // Rho = -T * V * 0.01
    res.rho = -T * res.price * 0.01;
    // Rho cash = Rho * Om
    res.rho_cash = res.rho * Om;
    // Vanna = D * n(d1) * (-d2 / sigma)
    res.vanna = disc * nd1 * (-d2 / sigma);
    // Volga = D * F * n(d1) * sqrt(T) * d1 * d2 / sigma * 0.01
    res.volga = disc * F * nd1 * sqrt_T * d1 * d2 / sigma * 0.01;
    // Charm = -D * n(d1) * [r / (sigma * sqrt(T)) - d2 / (2T)]
    res.charm = -disc * nd1 * (r / sigma_sqrt_T - d2 / (2.0 * T));
    return res;
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
                             double sigma, bool is_call,
                             double option_multiplier,
                             double future_multiplier,
                             double trading_days_per_year) noexcept {
    const double sqrt_T = T > 0.0 ? std::sqrt(T) : 0.0;
    const double disc = std::exp(-r * T);
    return compute_from_precomputed_scalar(F, K, T, sqrt_T, disc, r, sigma,
                                           is_call, option_multiplier,
                                           future_multiplier,
                                           trading_days_per_year);
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

void compute_batch_precomputed_scaled(const double*  F,
                                      const double*  K,
                                      const double*  T,
                                      const double*  sqrt_T,
                                      const double*  disc,
                                      const double*  sigma,
                                      const double*  option_multiplier,
                                      const double*  future_multiplier,
                                      const uint8_t* is_call,
                                      double         trading_days_per_year,
                                      Black76Result* out,
                                      int            count) noexcept {
    for (int i = 0; i < count; ++i) {
        const double r_val = (T[i] > 1e-10) ? -std::log(disc[i]) / T[i] : 0.0;
        const double Om = option_multiplier != nullptr ? option_multiplier[i] : 1.0;
        const double Fm = future_multiplier != nullptr ? future_multiplier[i] : 1.0;
        out[i] = compute_from_precomputed_scalar(F[i], K[i], T[i], sqrt_T[i], disc[i],
                                                 r_val, sigma[i], is_call[i] != 0,
                                                 Om, Fm, trading_days_per_year);
    }
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
            res.std_delta = (call && F[i] > K[i]) ? disc[i]
                          : (!call && K[i] > F[i]) ? -disc[i]
                          : 0.0;
            res.delta = res.std_delta;
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
            res.std_delta = disc[i] * norm_cdf(d1);
        } else {
            res.price = disc[i] * (K[i] * norm_cdf(-d2) - F[i] * norm_cdf(-d1));
            res.std_delta = -disc[i] * norm_cdf(-d1);
        }
        res.delta = res.std_delta;
        res.gamma = disc[i] * nd1 / (F[i] * sigma_sqrt_T) * F[i] * 0.01;
        res.vega = disc[i] * F[i] * nd1 * sqrt_T[i] * 0.01;
        res.vega_cash = res.vega;
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

namespace omm {
// Fused batch pricing: computes bid, mid, ask in single pass
// Reuses sigma lookup and d1/d2 computation (3× → 1×)
void compute_batch_quote_fused(const double* F_bid, const double* F_mid, const double* F_ask,
                               const double* K, const double* sqrt_T, const double* disc,
                               const double* sigma, const uint8_t* is_call,
                               Black76QuoteResult* bid_out, Black76QuoteResult* mid_out,
                               Black76QuoteResult* ask_out, int count) noexcept {
    compute_batch_quote_fused_scaled(F_bid, F_mid, F_ask, K, sqrt_T, disc,
                                     sigma, nullptr, nullptr, is_call,
                                     bid_out, mid_out, ask_out, count);
}

void compute_batch_quote_fused_scaled(const double* F_bid, const double* F_mid, const double* F_ask,
                                      const double* K, const double* sqrt_T, const double* disc,
                                      const double* sigma, const double* option_multiplier,
                                      const double* future_multiplier, const uint8_t* is_call,
                                      Black76QuoteResult* bid_out, Black76QuoteResult* mid_out,
                                      Black76QuoteResult* ask_out, int count) noexcept {
    for (int i = 0; i < count; ++i) {
        bid_out[i] = Black76QuoteResult{};
        mid_out[i] = Black76QuoteResult{};
        ask_out[i] = Black76QuoteResult{};
        const double F_m = F_mid[i];
        const double Om = safe_multiplier(option_multiplier != nullptr ? option_multiplier[i] : 1.0);
        const double Fm = safe_multiplier(future_multiplier != nullptr ? future_multiplier[i] : 1.0);

        if (F_m < 1e-10 || K[i] < 1e-10 || sigma[i] < 1e-10 || sqrt_T[i] < 1e-10) {
            const bool call = is_call[i] != 0;
            bid_out[i].price = disc[i] * (call ? std::fmax(F_bid[i] - K[i], 0.0)
                                               : std::fmax(K[i] - F_bid[i], 0.0));
            mid_out[i].price = disc[i] * (call ? std::fmax(F_m - K[i], 0.0)
                                               : std::fmax(K[i] - F_m, 0.0));
            ask_out[i].price = disc[i] * (call ? std::fmax(F_ask[i] - K[i], 0.0)
                                               : std::fmax(K[i] - F_ask[i], 0.0));
            bid_out[i].std_delta = (call && F_bid[i] > K[i]) ? disc[i]
                                  : (!call && K[i] > F_bid[i]) ? -disc[i] : 0.0;
            mid_out[i].std_delta = (call && F_m > K[i]) ? disc[i]
                                  : (!call && K[i] > F_m) ? -disc[i] : 0.0;
            ask_out[i].std_delta = (call && F_ask[i] > K[i]) ? disc[i]
                                  : (!call && K[i] > F_ask[i]) ? -disc[i] : 0.0;
            bid_out[i].delta = bid_out[i].std_delta * Om / Fm;
            mid_out[i].delta = mid_out[i].std_delta * Om / Fm;
            ask_out[i].delta = ask_out[i].std_delta * Om / Fm;
            continue;
        }

        const double sigma_sqrt_T = sigma[i] * sqrt_T[i];
        const bool call = is_call[i] != 0;

        const double d1_mid = (std::log(F_m / K[i]) + 0.5 * sigma[i] * sigma[i] * sqrt_T[i] * sqrt_T[i])
                            / sigma_sqrt_T;
        const double d2_mid = d1_mid - sigma_sqrt_T;
        const double nd1_mid = norm_pdf(d1_mid);

        if (call) {
            mid_out[i].price = disc[i] * (F_m * norm_cdf(d1_mid) - K[i] * norm_cdf(d2_mid));
            mid_out[i].std_delta = disc[i] * norm_cdf(d1_mid);
        } else {
            mid_out[i].price = disc[i] * (K[i] * norm_cdf(-d2_mid) - F_m * norm_cdf(-d1_mid));
            mid_out[i].std_delta = -disc[i] * norm_cdf(-d1_mid);
        }
        mid_out[i].delta = mid_out[i].std_delta * Om / Fm;
        mid_out[i].gamma = disc[i] * nd1_mid / (F_m * sigma_sqrt_T) * Om * F_m * 0.01 / Fm;
        mid_out[i].vega = disc[i] * F_m * nd1_mid * sqrt_T[i] * 0.01;
        mid_out[i].vega_cash = mid_out[i].vega * Om;

        const double d1_bid = (std::log(F_bid[i] / K[i]) + 0.5 * sigma[i] * sigma[i] * sqrt_T[i] * sqrt_T[i])
                            / sigma_sqrt_T;
        const double d2_bid = d1_bid - sigma_sqrt_T;

        if (call) {
            bid_out[i].price = disc[i] * (F_bid[i] * norm_cdf(d1_bid) - K[i] * norm_cdf(d2_bid));
            bid_out[i].std_delta = disc[i] * norm_cdf(d1_bid);
        } else {
            bid_out[i].price = disc[i] * (K[i] * norm_cdf(-d2_bid) - F_bid[i] * norm_cdf(-d1_bid));
            bid_out[i].std_delta = -disc[i] * norm_cdf(-d1_bid);
        }
        bid_out[i].delta = bid_out[i].std_delta * Om / Fm;

        const double d1_ask = (std::log(F_ask[i] / K[i]) + 0.5 * sigma[i] * sigma[i] * sqrt_T[i] * sqrt_T[i])
                            / sigma_sqrt_T;
        const double d2_ask = d1_ask - sigma_sqrt_T;

        if (call) {
            ask_out[i].price = disc[i] * (F_ask[i] * norm_cdf(d1_ask) - K[i] * norm_cdf(d2_ask));
            ask_out[i].std_delta = disc[i] * norm_cdf(d1_ask);
        } else {
            ask_out[i].price = disc[i] * (K[i] * norm_cdf(-d2_ask) - F_ask[i] * norm_cdf(-d1_ask));
            ask_out[i].std_delta = -disc[i] * norm_cdf(-d1_ask);
        }
        ask_out[i].delta = ask_out[i].std_delta * Om / Fm;
    }
}
} // namespace omm
