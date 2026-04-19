#include "pricing/black76.h"

#include <cmath>
#include <immintrin.h>

namespace omm {

namespace {

static inline __m256d erf_avx2(__m256d x_in) noexcept {
    const __m256d sign_bit = _mm256_set1_pd(-0.0);
    const __m256d x = _mm256_andnot_pd(sign_bit, x_in);
    const __m256d zero = _mm256_setzero_pd();
    const __m256d neg_mask = _mm256_cmp_pd(x_in, zero, _CMP_LT_OS);

    const __m256d p = _mm256_set1_pd(0.3275911);
    const __m256d t = _mm256_div_pd(_mm256_set1_pd(1.0),
                                    _mm256_fmadd_pd(p, x, _mm256_set1_pd(1.0)));

    const __m256d a5 = _mm256_set1_pd(1.061405429);
    const __m256d a4 = _mm256_set1_pd(-1.453152027);
    const __m256d a3 = _mm256_set1_pd(1.421413741);
    const __m256d a2 = _mm256_set1_pd(-0.284496736);
    const __m256d a1 = _mm256_set1_pd(0.254829592);

    __m256d poly = _mm256_fmadd_pd(a5, t, a4);
    poly = _mm256_fmadd_pd(poly, t, a3);
    poly = _mm256_fmadd_pd(poly, t, a2);
    poly = _mm256_fmadd_pd(poly, t, a1);
    poly = _mm256_mul_pd(poly, t);

    const __m256d x2 = _mm256_mul_pd(x, x);
    const __m256d neg_x2 = _mm256_sub_pd(zero, x2);
    alignas(32) double buf[4];
    alignas(32) double ebuf[4];
    _mm256_store_pd(buf, neg_x2);
    for (int i = 0; i < 4; ++i) {
        ebuf[i] = std::exp(buf[i]);
    }
    const __m256d exp_neg_x2 = _mm256_load_pd(ebuf);

    const __m256d result = _mm256_sub_pd(_mm256_set1_pd(1.0),
                                         _mm256_mul_pd(poly, exp_neg_x2));
    const __m256d neg_sign = _mm256_and_pd(neg_mask, _mm256_set1_pd(-0.0));
    return _mm256_xor_pd(result, neg_sign);
}

static inline __m256d norm_cdf_avx2(__m256d x) noexcept {
    const __m256d scaled = _mm256_mul_pd(x, _mm256_set1_pd(M_SQRT1_2));
    const __m256d erf_val = erf_avx2(scaled);
    return _mm256_mul_pd(_mm256_set1_pd(0.5),
                         _mm256_add_pd(_mm256_set1_pd(1.0), erf_val));
}

static inline __m256d norm_pdf_avx2(__m256d x) noexcept {
    static constexpr double INV_SQRT_2PI = 0.3989422804014327;
    const __m256d x2 = _mm256_mul_pd(x, x);
    const __m256d neg_half_x2 = _mm256_mul_pd(x2, _mm256_set1_pd(-0.5));
    alignas(32) double buf[4];
    alignas(32) double ebuf[4];
    _mm256_store_pd(buf, neg_half_x2);
    for (int i = 0; i < 4; ++i) {
        ebuf[i] = std::exp(buf[i]);
    }
    return _mm256_mul_pd(_mm256_load_pd(ebuf), _mm256_set1_pd(INV_SQRT_2PI));
}

void compute_4(const double* F, const double* K, const double* T,
               const double* r, const double* sigma,
               const uint8_t* is_call, Black76Result* out) noexcept {
    const __m256d vF = _mm256_loadu_pd(F);
    const __m256d vK = _mm256_loadu_pd(K);
    const __m256d vT = _mm256_loadu_pd(T);
    const __m256d vR = _mm256_loadu_pd(r);
    const __m256d vSig = _mm256_loadu_pd(sigma);

    const __m256d vSqrtT = _mm256_sqrt_pd(vT);
    const __m256d vSigSqrtT = _mm256_mul_pd(vSig, vSqrtT);

    const __m256d ratio = _mm256_div_pd(vF, vK);
    alignas(32) double ratio_buf[4];
    alignas(32) double log_buf[4];
    _mm256_store_pd(ratio_buf, ratio);
    for (int i = 0; i < 4; ++i) {
        log_buf[i] = std::log(ratio_buf[i]);
    }
    const __m256d log_FK = _mm256_load_pd(log_buf);

    const __m256d half_var = _mm256_mul_pd(
        _mm256_mul_pd(vSig, vSig),
        _mm256_mul_pd(_mm256_set1_pd(0.5), vT));
    const __m256d d1 = _mm256_div_pd(_mm256_add_pd(log_FK, half_var), vSigSqrtT);
    const __m256d d2 = _mm256_sub_pd(d1, vSigSqrtT);

    const __m256d neg_rT = _mm256_mul_pd(_mm256_sub_pd(_mm256_setzero_pd(), vR), vT);
    alignas(32) double dbuf[4];
    alignas(32) double discbuf[4];
    _mm256_store_pd(dbuf, neg_rT);
    for (int i = 0; i < 4; ++i) {
        discbuf[i] = std::exp(dbuf[i]);
    }
    const __m256d vDisc = _mm256_load_pd(discbuf);

    const __m256d Nd1 = norm_cdf_avx2(d1);
    const __m256d Nd2 = norm_cdf_avx2(d2);
    const __m256d nd1 = norm_pdf_avx2(d1);

    const __m256d call_price = _mm256_mul_pd(
        vDisc,
        _mm256_sub_pd(_mm256_mul_pd(vF, Nd1), _mm256_mul_pd(vK, Nd2)));
    const __m256d call_delta = _mm256_mul_pd(vDisc, Nd1);

    const __m256d one = _mm256_set1_pd(1.0);
    const __m256d Nnd1 = _mm256_sub_pd(one, Nd1);
    const __m256d Nnd2 = _mm256_sub_pd(one, Nd2);
    const __m256d put_price = _mm256_mul_pd(
        vDisc,
        _mm256_sub_pd(_mm256_mul_pd(vK, Nnd2), _mm256_mul_pd(vF, Nnd1)));
    const __m256d put_delta = _mm256_sub_pd(_mm256_setzero_pd(),
                                            _mm256_mul_pd(vDisc, Nnd1));

    const __m256d gamma = _mm256_div_pd(
        _mm256_mul_pd(vDisc, nd1),
        _mm256_mul_pd(vF, vSigSqrtT));
    const __m256d vega = _mm256_mul_pd(
        vDisc,
        _mm256_mul_pd(vF, _mm256_mul_pd(nd1, vSqrtT)));

    alignas(32) double prices_call[4];
    alignas(32) double prices_put[4];
    alignas(32) double deltas_call[4];
    alignas(32) double deltas_put[4];
    alignas(32) double gammas[4];
    alignas(32) double vegas[4];
    alignas(32) double d1_arr[4];
    alignas(32) double sig_arr[4];
    alignas(32) double sqrtT_arr[4];
    alignas(32) double disc_arr[4];
    alignas(32) double t_arr[4];

    _mm256_store_pd(prices_call, call_price);
    _mm256_store_pd(prices_put, put_price);
    _mm256_store_pd(deltas_call, call_delta);
    _mm256_store_pd(deltas_put, put_delta);
    _mm256_store_pd(gammas, gamma);
    _mm256_store_pd(vegas, vega);
    _mm256_store_pd(d1_arr, d1);
    _mm256_store_pd(sig_arr, vSig);
    _mm256_store_pd(sqrtT_arr, vSqrtT);
    _mm256_store_pd(disc_arr, vDisc);
    _mm256_store_pd(t_arr, vT);

    for (int i = 0; i < 4; ++i) {
        const bool call = (is_call[i] != 0);
        const double price = call ? prices_call[i] : prices_put[i];
        const double nd1_scalar =
            std::exp(-0.5 * d1_arr[i] * d1_arr[i]) * 0.3989422804014327;

        out[i].price = price;
        out[i].delta = call ? deltas_call[i] : deltas_put[i];
        out[i].gamma = gammas[i];
        out[i].vega = vegas[i];
        out[i].theta = (-disc_arr[i] * F[i] * nd1_scalar * sig_arr[i]
                         / (2.0 * sqrtT_arr[i])
                        - r[i] * price) / 365.0;
        out[i].rho = -t_arr[i] * price;
    }
}

void compute_4_precomputed(const double* F, const double* K,
                           const double* T, const double* sqrt_T,
                           const double* disc, const double* sigma,
                           const uint8_t* is_call,
                           Black76Result* out) noexcept {
    const __m256d vF = _mm256_loadu_pd(F);
    const __m256d vK = _mm256_loadu_pd(K);
    const __m256d vT = _mm256_loadu_pd(T);
    const __m256d vSig = _mm256_loadu_pd(sigma);
    const __m256d vSqrtT = _mm256_loadu_pd(sqrt_T);
    const __m256d vDisc = _mm256_loadu_pd(disc);

    const __m256d vSigSqrtT = _mm256_mul_pd(vSig, vSqrtT);

    const __m256d ratio = _mm256_div_pd(vF, vK);
    alignas(32) double ratio_buf[4];
    alignas(32) double log_buf[4];
    _mm256_store_pd(ratio_buf, ratio);
    for (int i = 0; i < 4; ++i) {
        log_buf[i] = std::log(ratio_buf[i]);
    }
    const __m256d log_FK = _mm256_load_pd(log_buf);

    const __m256d half_var = _mm256_mul_pd(
        _mm256_mul_pd(vSig, vSig),
        _mm256_mul_pd(_mm256_set1_pd(0.5), vT));
    const __m256d d1 = _mm256_div_pd(_mm256_add_pd(log_FK, half_var), vSigSqrtT);
    const __m256d d2 = _mm256_sub_pd(d1, vSigSqrtT);

    const __m256d Nd1 = norm_cdf_avx2(d1);
    const __m256d Nd2 = norm_cdf_avx2(d2);
    const __m256d nd1 = norm_pdf_avx2(d1);

    const __m256d call_price = _mm256_mul_pd(
        vDisc,
        _mm256_sub_pd(_mm256_mul_pd(vF, Nd1), _mm256_mul_pd(vK, Nd2)));
    const __m256d call_delta = _mm256_mul_pd(vDisc, Nd1);

    const __m256d one = _mm256_set1_pd(1.0);
    const __m256d Nnd1 = _mm256_sub_pd(one, Nd1);
    const __m256d Nnd2 = _mm256_sub_pd(one, Nd2);
    const __m256d put_price = _mm256_mul_pd(
        vDisc,
        _mm256_sub_pd(_mm256_mul_pd(vK, Nnd2), _mm256_mul_pd(vF, Nnd1)));
    const __m256d put_delta = _mm256_sub_pd(_mm256_setzero_pd(),
                                            _mm256_mul_pd(vDisc, Nnd1));

    const __m256d gamma = _mm256_div_pd(
        _mm256_mul_pd(vDisc, nd1),
        _mm256_mul_pd(vF, vSigSqrtT));
    const __m256d vega = _mm256_mul_pd(
        vDisc,
        _mm256_mul_pd(vF, _mm256_mul_pd(nd1, vSqrtT)));

    alignas(32) double prices_call[4];
    alignas(32) double prices_put[4];
    alignas(32) double deltas_call[4];
    alignas(32) double deltas_put[4];
    alignas(32) double gammas[4];
    alignas(32) double vegas[4];
    alignas(32) double d1_arr[4];
    alignas(32) double sig_arr[4];
    alignas(32) double sqrtT_arr[4];
    alignas(32) double disc_arr[4];
    alignas(32) double t_arr[4];

    _mm256_store_pd(prices_call, call_price);
    _mm256_store_pd(prices_put, put_price);
    _mm256_store_pd(deltas_call, call_delta);
    _mm256_store_pd(deltas_put, put_delta);
    _mm256_store_pd(gammas, gamma);
    _mm256_store_pd(vegas, vega);
    _mm256_store_pd(d1_arr, d1);
    _mm256_store_pd(sig_arr, vSig);
    _mm256_store_pd(sqrtT_arr, vSqrtT);
    _mm256_store_pd(disc_arr, vDisc);
    _mm256_store_pd(t_arr, vT);

    for (int i = 0; i < 4; ++i) {
        const bool call = (is_call[i] != 0);
        const double price = call ? prices_call[i] : prices_put[i];
        const double nd1_scalar =
            std::exp(-0.5 * d1_arr[i] * d1_arr[i]) * 0.3989422804014327;
        const double r_val = (t_arr[i] > 1e-10) ? -std::log(disc_arr[i]) / t_arr[i] : 0.0;

        out[i].price = price;
        out[i].delta = call ? deltas_call[i] : deltas_put[i];
        out[i].gamma = gammas[i];
        out[i].vega = vegas[i];
        out[i].theta = (-disc_arr[i] * F[i] * nd1_scalar * sig_arr[i]
                         / (2.0 * sqrtT_arr[i])
                        - r_val * price) / 365.0;
        out[i].rho = -t_arr[i] * price;
    }
}

} // namespace

void compute_batch_avx2(const double* F, const double* K, const double* T,
                        const double* r, const double* sigma,
                        const uint8_t* is_call, Black76Result* out,
                        int count) noexcept {
    int i = 0;
    for (; i + 4 <= count; i += 4) {
        compute_4(F + i, K + i, T + i, r + i, sigma + i, is_call + i, out + i);
    }
    for (; i < count; ++i) {
        out[i] = compute_scalar(F[i], K[i], T[i], r[i], sigma[i], is_call[i] != 0);
    }
}

void compute_batch_avx2_precomputed(const double* F, const double* K,
                                    const double* T, const double* sqrt_T,
                                    const double* disc, const double* sigma,
                                    const uint8_t* is_call, Black76Result* out,
                                    int count) noexcept {
    int i = 0;
    for (; i + 4 <= count; i += 4) {
        compute_4_precomputed(F + i, K + i, T + i, sqrt_T + i, disc + i,
                              sigma + i, is_call + i, out + i);
    }
    for (; i < count; ++i) {
        const double r_val = (T[i] > 1e-10) ? -std::log(disc[i]) / T[i] : 0.0;
        out[i] = compute_scalar(F[i], K[i], T[i], r_val, sigma[i], is_call[i] != 0);
    }
}

} // namespace omm
