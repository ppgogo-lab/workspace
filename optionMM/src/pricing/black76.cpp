#include "pricing/black76.h"

#include <cmath>
#include <cstring>
#include <immintrin.h>

namespace omm {

// ─── Scalar helpers ───────────────────────────────────────────────────────────

// Cumulative standard normal distribution using erfc for accuracy.
// N(x) = erfc(-x / sqrt(2)) / 2
static inline double norm_cdf(double x) noexcept {
    return 0.5 * std::erfc(-x * M_SQRT1_2);
}

// Standard normal PDF: n(x) = exp(-x^2/2) / sqrt(2*pi)
static inline double norm_pdf(double x) noexcept {
    static constexpr double INV_SQRT_2PI = 0.3989422804014327;
    return INV_SQRT_2PI * std::exp(-0.5 * x * x);
}

// ─── Scalar pricer ────────────────────────────────────────────────────────────

Black76Result compute_scalar(double F, double K, double T, double r,
                              double sigma, bool is_call) noexcept {
    // Guard against degenerate inputs — return zero Greeks, not NaN
    if (sigma < 1e-10 || T < 1e-10) {
        Black76Result z{};
        // Intrinsic value only
        double disc = std::exp(-r * T);
        if (is_call)
            z.price = disc * std::fmax(F - K, 0.0);
        else
            z.price = disc * std::fmax(K - F, 0.0);
        z.delta = (is_call && F > K) ? disc : (!is_call && K > F) ? -disc : 0.0;
        return z;
    }

    double sqrt_T   = std::sqrt(T);
    double sigma_sqT = sigma * sqrt_T;
    double d1 = (std::log(F / K) + 0.5 * sigma * sigma * T) / sigma_sqT;
    double d2 = d1 - sigma_sqT;
    double disc = std::exp(-r * T);

    double Nd1, Nd2, nd1;
    if (is_call) {
        Nd1 = norm_cdf(d1);
        Nd2 = norm_cdf(d2);
        nd1 = norm_pdf(d1);

        Black76Result res;
        res.price = disc * (F * Nd1 - K * Nd2);
        res.delta = disc * Nd1;
        res.gamma = disc * nd1 / (F * sigma_sqT);
        res.vega  = disc * F * nd1 * sqrt_T;
        // Theta: -(F*n(d1)*sigma)/(2*sqrt(T)) - r*(F*N(d1) - K*N(d2))*disc
        // Per calendar day (divide by 365)
        res.theta = (-disc * F * nd1 * sigma / (2.0 * sqrt_T)
                     - r * res.price) / 365.0;
        res.rho   = -T * res.price;
        return res;
    } else {
        double Nnd1 = norm_cdf(-d1);
        double Nnd2 = norm_cdf(-d2);
        nd1 = norm_pdf(d1);

        Black76Result res;
        res.price = disc * (K * Nnd2 - F * Nnd1);
        res.delta = -disc * Nnd1;
        res.gamma = disc * nd1 / (F * sigma_sqT);
        res.vega  = disc * F * nd1 * sqrt_T;
        res.theta = (-disc * F * nd1 * sigma / (2.0 * sqrt_T)
                     - r * res.price) / 365.0;
        res.rho   = -T * res.price;
        return res;
    }
}

// ─── AVX2 batch pricer ────────────────────────────────────────────────────────
// Uses a minimax polynomial approximation for erf() accurate to ~1e-7
// over the range used in Black-76 (d1, d2 typically in [-4, 4]).
// Reference: Abramowitz & Stegun 7.1.26 approximation.
#ifdef __AVX2__

// Polynomial approximation of erf(x) for x >= 0, |x| <= 4.
// erf(x) ≈ 1 - (a1*t + a2*t^2 + a3*t^3 + a4*t^4 + a5*t^5) * exp(-x^2)
// where t = 1/(1 + 0.3275911*x)
// Max error: 1.5e-7
static inline __m256d erf_avx2(__m256d x_in) noexcept {
    // Compute abs(x): clear the sign bit by ANDing with 0x7FFFFFFFFFFFFFFF
    __m256d sign_bit = _mm256_set1_pd(-0.0);  // all bits 1 except sign = 0x8000...
    __m256d x = _mm256_andnot_pd(sign_bit, x_in);  // abs: ~sign_bit & x_in = clear sign bit
    // neg_mask: all-ones lanes where x_in < 0
    __m256d zero = _mm256_setzero_pd();
    __m256d neg_mask = _mm256_cmp_pd(x_in, zero, _CMP_LT_OS);

    __m256d p  = _mm256_set1_pd(0.3275911);
    __m256d t  = _mm256_div_pd(_mm256_set1_pd(1.0),
                                _mm256_fmadd_pd(p, x, _mm256_set1_pd(1.0)));

    // Horner evaluation of polynomial in t
    __m256d a5 = _mm256_set1_pd( 1.061405429);
    __m256d a4 = _mm256_set1_pd(-1.453152027);
    __m256d a3 = _mm256_set1_pd( 1.421413741);
    __m256d a2 = _mm256_set1_pd(-0.284496736);
    __m256d a1 = _mm256_set1_pd( 0.254829592);

    __m256d poly = _mm256_fmadd_pd(a5, t, a4);
    poly = _mm256_fmadd_pd(poly, t, a3);
    poly = _mm256_fmadd_pd(poly, t, a2);
    poly = _mm256_fmadd_pd(poly, t, a1);
    poly = _mm256_mul_pd(poly, t);

    // exp(-x^2)
    __m256d x2   = _mm256_mul_pd(x, x);
    __m256d neg_x2 = _mm256_sub_pd(zero, x2);
    // Use std::exp lane-by-lane (no AVX2 exp intrinsic in base ISA)
    alignas(32) double buf[4];
    _mm256_store_pd(buf, neg_x2);
    alignas(32) double ebuf[4] = { std::exp(buf[0]), std::exp(buf[1]),
                                    std::exp(buf[2]), std::exp(buf[3]) };
    __m256d exp_neg_x2 = _mm256_load_pd(ebuf);

    __m256d result = _mm256_sub_pd(_mm256_set1_pd(1.0),
                                    _mm256_mul_pd(poly, exp_neg_x2));

    // Restore sign: erf(-x) = -erf(x)
    __m256d neg_sign = _mm256_and_pd(neg_mask, _mm256_set1_pd(-0.0));
    return _mm256_xor_pd(result, neg_sign);
}

// norm_cdf(x) = 0.5 * erfc(-x/sqrt(2)) = 0.5 * (1 + erf(x/sqrt(2)))
static inline __m256d norm_cdf_avx2(__m256d x) noexcept {
    static const double INV_SQRT2 = M_SQRT1_2;  // 1/sqrt(2)
    __m256d scaled = _mm256_mul_pd(x, _mm256_set1_pd(INV_SQRT2));
    __m256d erf_val = erf_avx2(scaled);
    return _mm256_mul_pd(_mm256_set1_pd(0.5),
                          _mm256_add_pd(_mm256_set1_pd(1.0), erf_val));
}

// norm_pdf(x) = exp(-x^2/2) / sqrt(2*pi)
static inline __m256d norm_pdf_avx2(__m256d x) noexcept {
    static constexpr double INV_SQRT_2PI = 0.3989422804014327;
    __m256d x2     = _mm256_mul_pd(x, x);
    __m256d neg_h  = _mm256_mul_pd(x2, _mm256_set1_pd(-0.5));
    alignas(32) double buf[4], ebuf[4];
    _mm256_store_pd(buf, neg_h);
    for (int i = 0; i < 4; ++i) ebuf[i] = std::exp(buf[i]);
    return _mm256_mul_pd(_mm256_load_pd(ebuf), _mm256_set1_pd(INV_SQRT_2PI));
}

// Process 4 options with AVX2 SIMD
static void compute_4(const double* F, const double* K, const double* T,
                       const double* r, const double* sigma,
                       const uint8_t* is_call, Black76Result* out) noexcept {
    __m256d vF     = _mm256_loadu_pd(F);
    __m256d vK     = _mm256_loadu_pd(K);
    __m256d vT     = _mm256_loadu_pd(T);
    __m256d vR     = _mm256_loadu_pd(r);
    __m256d vSig   = _mm256_loadu_pd(sigma);

    __m256d vSqrtT = _mm256_sqrt_pd(vT);
    __m256d vSigSqrtT = _mm256_mul_pd(vSig, vSqrtT);

    // d1 = (log(F/K) + 0.5*sigma^2*T) / (sigma*sqrt(T))
    __m256d ratio = _mm256_div_pd(vF, vK);
    alignas(32) double lbuf[4], rbuf[4];
    _mm256_store_pd(lbuf, ratio);
    for (int i = 0; i < 4; ++i) rbuf[i] = std::log(lbuf[i]);
    __m256d log_FK = _mm256_load_pd(rbuf);

    __m256d half_var = _mm256_mul_pd(
        _mm256_mul_pd(vSig, vSig),
        _mm256_mul_pd(_mm256_set1_pd(0.5), vT)
    );
    __m256d d1 = _mm256_div_pd(_mm256_add_pd(log_FK, half_var), vSigSqrtT);
    __m256d d2 = _mm256_sub_pd(d1, vSigSqrtT);

    // disc = exp(-r*T)
    __m256d neg_rT = _mm256_mul_pd(_mm256_sub_pd(_mm256_setzero_pd(), vR), vT);
    alignas(32) double dbuf[4], discbuf[4];
    _mm256_store_pd(dbuf, neg_rT);
    for (int i = 0; i < 4; ++i) discbuf[i] = std::exp(dbuf[i]);
    __m256d vDisc = _mm256_load_pd(discbuf);

    __m256d Nd1 = norm_cdf_avx2(d1);
    __m256d Nd2 = norm_cdf_avx2(d2);
    __m256d nd1 = norm_pdf_avx2(d1);

    // call: price = disc*(F*Nd1 - K*Nd2), delta = disc*Nd1
    __m256d call_price = _mm256_mul_pd(vDisc,
        _mm256_sub_pd(_mm256_mul_pd(vF, Nd1), _mm256_mul_pd(vK, Nd2)));
    __m256d call_delta = _mm256_mul_pd(vDisc, Nd1);

    // put: price = disc*(K*Nnd2 - F*Nnd1), delta = -disc*Nnd1
    __m256d one = _mm256_set1_pd(1.0);
    __m256d Nnd1 = _mm256_sub_pd(one, Nd1);
    __m256d Nnd2 = _mm256_sub_pd(one, Nd2);
    __m256d put_price = _mm256_mul_pd(vDisc,
        _mm256_sub_pd(_mm256_mul_pd(vK, Nnd2), _mm256_mul_pd(vF, Nnd1)));
    __m256d put_delta = _mm256_sub_pd(_mm256_setzero_pd(),
                                       _mm256_mul_pd(vDisc, Nnd1));

    // gamma = disc * nd1 / (F * sigma * sqrt(T))  — same for call and put
    __m256d gamma = _mm256_div_pd(
        _mm256_mul_pd(vDisc, nd1),
        _mm256_mul_pd(vF, vSigSqrtT)
    );

    // vega = disc * F * nd1 * sqrt(T)  — same for call and put
    __m256d vega = _mm256_mul_pd(vDisc,
        _mm256_mul_pd(vF, _mm256_mul_pd(nd1, vSqrtT)));

    // Extract and write per-option, applying call/put selection
    alignas(32) double prices_call[4], prices_put[4];
    alignas(32) double deltas_call[4], deltas_put[4];
    alignas(32) double gammas[4], vegas[4];
    alignas(32) double d1_arr[4], d2_arr[4];
    alignas(32) double sig_arr[4], sqrtT_arr[4], disc_arr[4], vT_arr[4];

    _mm256_store_pd(prices_call, call_price);
    _mm256_store_pd(prices_put,  put_price);
    _mm256_store_pd(deltas_call, call_delta);
    _mm256_store_pd(deltas_put,  put_delta);
    _mm256_store_pd(gammas, gamma);
    _mm256_store_pd(vegas,  vega);
    _mm256_store_pd(d1_arr, d1);
    _mm256_store_pd(d2_arr, d2);  // unused below but consistent
    _mm256_store_pd(sig_arr,   vSig);
    _mm256_store_pd(sqrtT_arr, vSqrtT);
    _mm256_store_pd(disc_arr,  vDisc);
    _mm256_store_pd(vT_arr,    vT);

    for (int i = 0; i < 4; ++i) {
        bool call = (is_call[i] != 0);
        double p = call ? prices_call[i] : prices_put[i];
        out[i].price = p;
        out[i].delta = call ? deltas_call[i] : deltas_put[i];
        out[i].gamma = gammas[i];
        out[i].vega  = vegas[i];
        // theta = (-disc*F*nd1*sigma / (2*sqrt(T)) - r*price) / 365
        double nd1_s  = std::exp(-0.5 * d1_arr[i] * d1_arr[i]) * 0.3989422804014327;
        out[i].theta = (-disc_arr[i] * F[i] * nd1_s * sig_arr[i]
                         / (2.0 * sqrtT_arr[i])
                        - r[i] * p) / 365.0;
        out[i].rho = -vT_arr[i] * p;
    }
}

void compute_batch_avx2(const double* F, const double* K, const double* T,
                         const double* r, const double* sigma,
                         const uint8_t* is_call, Black76Result* out,
                         int count) noexcept {
    int i = 0;
    for (; i + 4 <= count; i += 4)
        compute_4(F+i, K+i, T+i, r+i, sigma+i, is_call+i, out+i);
    // Tail: scalar fallback for remainder
    for (; i < count; ++i)
        out[i] = compute_scalar(F[i], K[i], T[i], r[i], sigma[i], is_call[i] != 0);
}

#endif // __AVX2__

// ─── Implied volatility (bisection) ──────────────────────────────────────────

double implied_vol(double market_price, double F, double K, double T,
                   double r, bool is_call, double tol) noexcept {
    if (F < 1e-10 || K < 1e-10 || T < 1e-10) return 0.0;

    // Check against intrinsic value — if market_price is below intrinsic, bail
    double disc = std::exp(-r * T);
    double intrinsic = is_call ? std::fmax(disc * (F - K), 0.0)
                               : std::fmax(disc * (K - F), 0.0);
    if (market_price <= intrinsic + 1e-10) return 0.0;

    // Bisection bounds: [lo, hi] in vol space
    double lo = 1e-5, hi = 5.0;  // 0.001% to 500% annualised vol

    // Quick sanity: price at hi must exceed market_price
    if (compute_scalar(F, K, T, r, hi, is_call).price < market_price) return 0.0;

    for (int i = 0; i < 100; ++i) {
        double mid = 0.5 * (lo + hi);
        double p   = compute_scalar(F, K, T, r, mid, is_call).price;
        if (p < market_price)
            lo = mid;
        else
            hi = mid;
        if (hi - lo < tol) break;
    }
    return 0.5 * (lo + hi);
}

} // namespace omm
