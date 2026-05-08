#include "pricing/black76.h"

#include <cmath>
#include <immintrin.h>

namespace omm {

namespace {

static inline __m512d erf_avx512(__m512d x_in) noexcept {
    const __m512d zero = _mm512_setzero_pd();
    const __m512d x = _mm512_max_pd(x_in, _mm512_sub_pd(zero, x_in));
    const __mmask8 neg_mask = _mm512_cmp_pd_mask(x_in, zero, _CMP_LT_OS);

    const __m512d p = _mm512_set1_pd(0.3275911);
    const __m512d t = _mm512_div_pd(_mm512_set1_pd(1.0),
                                    _mm512_fmadd_pd(p, x, _mm512_set1_pd(1.0)));

    const __m512d a5 = _mm512_set1_pd(1.061405429);
    const __m512d a4 = _mm512_set1_pd(-1.453152027);
    const __m512d a3 = _mm512_set1_pd(1.421413741);
    const __m512d a2 = _mm512_set1_pd(-0.284496736);
    const __m512d a1 = _mm512_set1_pd(0.254829592);

    __m512d poly = _mm512_fmadd_pd(a5, t, a4);
    poly = _mm512_fmadd_pd(poly, t, a3);
    poly = _mm512_fmadd_pd(poly, t, a2);
    poly = _mm512_fmadd_pd(poly, t, a1);
    poly = _mm512_mul_pd(poly, t);

    const __m512d x2 = _mm512_mul_pd(x, x);
    const __m512d neg_x2 = _mm512_sub_pd(zero, x2);
    alignas(64) double buf[8];
    alignas(64) double ebuf[8];
    _mm512_store_pd(buf, neg_x2);
    for (int i = 0; i < 8; ++i) {
        ebuf[i] = std::exp(buf[i]);
    }
    const __m512d exp_neg_x2 = _mm512_load_pd(ebuf);

    __m512d result = _mm512_sub_pd(_mm512_set1_pd(1.0),
                                   _mm512_mul_pd(poly, exp_neg_x2));
    return _mm512_mask_sub_pd(result, neg_mask, _mm512_setzero_pd(), result);
}

static inline __m512d norm_cdf_avx512(__m512d x) noexcept {
    const __m512d scaled = _mm512_mul_pd(x, _mm512_set1_pd(M_SQRT1_2));
    const __m512d erf_val = erf_avx512(scaled);
    return _mm512_mul_pd(_mm512_set1_pd(0.5),
                         _mm512_add_pd(_mm512_set1_pd(1.0), erf_val));
}

static inline __m512d norm_pdf_avx512(__m512d x) noexcept {
    static constexpr double INV_SQRT_2PI = 0.3989422804014327;
    const __m512d x2 = _mm512_mul_pd(x, x);
    const __m512d neg_half_x2 = _mm512_mul_pd(x2, _mm512_set1_pd(-0.5));
    alignas(64) double buf[8];
    alignas(64) double ebuf[8];
    _mm512_store_pd(buf, neg_half_x2);
    for (int i = 0; i < 8; ++i) {
        ebuf[i] = std::exp(buf[i]);
    }
    return _mm512_mul_pd(_mm512_load_pd(ebuf), _mm512_set1_pd(INV_SQRT_2PI));
}

void compute_8(const double* F, const double* K, const double* T,
               const double* r, const double* sigma,
               const uint8_t* is_call, Black76Result* out) noexcept {
    const __m512d vF = _mm512_loadu_pd(F);
    const __m512d vK = _mm512_loadu_pd(K);
    const __m512d vT = _mm512_loadu_pd(T);
    const __m512d vR = _mm512_loadu_pd(r);
    const __m512d vSig = _mm512_loadu_pd(sigma);

    const __m512d vSqrtT = _mm512_sqrt_pd(vT);
    const __m512d vSigSqrtT = _mm512_mul_pd(vSig, vSqrtT);

    const __m512d ratio = _mm512_div_pd(vF, vK);
    alignas(64) double ratio_buf[8];
    alignas(64) double log_buf[8];
    _mm512_store_pd(ratio_buf, ratio);
    for (int i = 0; i < 8; ++i) {
        log_buf[i] = std::log(ratio_buf[i]);
    }
    const __m512d log_FK = _mm512_load_pd(log_buf);

    const __m512d half_var = _mm512_mul_pd(
        _mm512_mul_pd(vSig, vSig),
        _mm512_mul_pd(_mm512_set1_pd(0.5), vT));
    const __m512d d1 = _mm512_div_pd(_mm512_add_pd(log_FK, half_var), vSigSqrtT);
    const __m512d d2 = _mm512_sub_pd(d1, vSigSqrtT);

    const __m512d neg_rT = _mm512_mul_pd(_mm512_sub_pd(_mm512_setzero_pd(), vR), vT);
    alignas(64) double dbuf[8];
    alignas(64) double discbuf[8];
    _mm512_store_pd(dbuf, neg_rT);
    for (int i = 0; i < 8; ++i) {
        discbuf[i] = std::exp(dbuf[i]);
    }
    const __m512d vDisc = _mm512_load_pd(discbuf);

    const __m512d Nd1 = norm_cdf_avx512(d1);
    const __m512d Nd2 = norm_cdf_avx512(d2);
    const __m512d nd1 = norm_pdf_avx512(d1);

    const __m512d call_price = _mm512_mul_pd(
        vDisc,
        _mm512_sub_pd(_mm512_mul_pd(vF, Nd1), _mm512_mul_pd(vK, Nd2)));
    const __m512d call_delta = _mm512_mul_pd(vDisc, Nd1);

    const __m512d one = _mm512_set1_pd(1.0);
    const __m512d Nnd1 = _mm512_sub_pd(one, Nd1);
    const __m512d Nnd2 = _mm512_sub_pd(one, Nd2);
    const __m512d put_price = _mm512_mul_pd(
        vDisc,
        _mm512_sub_pd(_mm512_mul_pd(vK, Nnd2), _mm512_mul_pd(vF, Nnd1)));
    const __m512d put_delta = _mm512_sub_pd(_mm512_setzero_pd(),
                                            _mm512_mul_pd(vDisc, Nnd1));

    const __m512d gamma = _mm512_div_pd(
        _mm512_mul_pd(vDisc, nd1),
        _mm512_mul_pd(vF, vSigSqrtT));
    const __m512d vega = _mm512_mul_pd(
        vDisc,
        _mm512_mul_pd(vF, _mm512_mul_pd(nd1, vSqrtT)));

    alignas(64) double prices_call[8];
    alignas(64) double prices_put[8];
    alignas(64) double deltas_call[8];
    alignas(64) double deltas_put[8];
    alignas(64) double gammas[8];
    alignas(64) double vegas[8];
    alignas(64) double d1_arr[8];
    alignas(64) double sig_arr[8];
    alignas(64) double sqrtT_arr[8];
    alignas(64) double disc_arr[8];
    alignas(64) double t_arr[8];

    _mm512_store_pd(prices_call, call_price);
    _mm512_store_pd(prices_put, put_price);
    _mm512_store_pd(deltas_call, call_delta);
    _mm512_store_pd(deltas_put, put_delta);
    _mm512_store_pd(gammas, gamma);
    _mm512_store_pd(vegas, vega);
    _mm512_store_pd(d1_arr, d1);
    _mm512_store_pd(sig_arr, vSig);
    _mm512_store_pd(sqrtT_arr, vSqrtT);
    _mm512_store_pd(disc_arr, vDisc);
    _mm512_store_pd(t_arr, vT);

    for (int i = 0; i < 8; ++i) {
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
                        + r[i] * price) / 252.0;
        out[i].rho = -t_arr[i] * price * 0.01;
        out[i] = compute_scalar(F[i], K[i], t_arr[i], r[i], sig_arr[i], call);
    }
}

void compute_8_precomputed(const double* F, const double* K,
                           const double* T, const double* sqrt_T,
                           const double* disc, const double* sigma,
                           const uint8_t* is_call,
                           Black76Result* out) noexcept {
    const __m512d vF = _mm512_loadu_pd(F);
    const __m512d vK = _mm512_loadu_pd(K);
    const __m512d vT = _mm512_loadu_pd(T);
    const __m512d vSig = _mm512_loadu_pd(sigma);
    const __m512d vSqrtT = _mm512_loadu_pd(sqrt_T);
    const __m512d vDisc = _mm512_loadu_pd(disc);

    const __m512d vSigSqrtT = _mm512_mul_pd(vSig, vSqrtT);

    const __m512d ratio = _mm512_div_pd(vF, vK);
    alignas(64) double ratio_buf[8];
    alignas(64) double log_buf[8];
    _mm512_store_pd(ratio_buf, ratio);
    for (int i = 0; i < 8; ++i) {
        log_buf[i] = std::log(ratio_buf[i]);
    }
    const __m512d log_FK = _mm512_load_pd(log_buf);

    const __m512d half_var = _mm512_mul_pd(
        _mm512_mul_pd(vSig, vSig),
        _mm512_mul_pd(_mm512_set1_pd(0.5), vT));
    const __m512d d1 = _mm512_div_pd(_mm512_add_pd(log_FK, half_var), vSigSqrtT);
    const __m512d d2 = _mm512_sub_pd(d1, vSigSqrtT);

    const __m512d Nd1 = norm_cdf_avx512(d1);
    const __m512d Nd2 = norm_cdf_avx512(d2);
    const __m512d nd1 = norm_pdf_avx512(d1);

    const __m512d call_price = _mm512_mul_pd(
        vDisc,
        _mm512_sub_pd(_mm512_mul_pd(vF, Nd1), _mm512_mul_pd(vK, Nd2)));
    const __m512d call_delta = _mm512_mul_pd(vDisc, Nd1);

    const __m512d one = _mm512_set1_pd(1.0);
    const __m512d Nnd1 = _mm512_sub_pd(one, Nd1);
    const __m512d Nnd2 = _mm512_sub_pd(one, Nd2);
    const __m512d put_price = _mm512_mul_pd(
        vDisc,
        _mm512_sub_pd(_mm512_mul_pd(vK, Nnd2), _mm512_mul_pd(vF, Nnd1)));
    const __m512d put_delta = _mm512_sub_pd(_mm512_setzero_pd(),
                                            _mm512_mul_pd(vDisc, Nnd1));

    const __m512d gamma = _mm512_div_pd(
        _mm512_mul_pd(vDisc, nd1),
        _mm512_mul_pd(vF, vSigSqrtT));
    const __m512d vega = _mm512_mul_pd(
        vDisc,
        _mm512_mul_pd(vF, _mm512_mul_pd(nd1, vSqrtT)));

    alignas(64) double prices_call[8];
    alignas(64) double prices_put[8];
    alignas(64) double deltas_call[8];
    alignas(64) double deltas_put[8];
    alignas(64) double gammas[8];
    alignas(64) double vegas[8];
    alignas(64) double d1_arr[8];
    alignas(64) double sig_arr[8];
    alignas(64) double sqrtT_arr[8];
    alignas(64) double disc_arr[8];
    alignas(64) double t_arr[8];

    _mm512_store_pd(prices_call, call_price);
    _mm512_store_pd(prices_put, put_price);
    _mm512_store_pd(deltas_call, call_delta);
    _mm512_store_pd(deltas_put, put_delta);
    _mm512_store_pd(gammas, gamma);
    _mm512_store_pd(vegas, vega);
    _mm512_store_pd(d1_arr, d1);
    _mm512_store_pd(sig_arr, vSig);
    _mm512_store_pd(sqrtT_arr, vSqrtT);
    _mm512_store_pd(disc_arr, vDisc);
    _mm512_store_pd(t_arr, vT);

    for (int i = 0; i < 8; ++i) {
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
                        + r_val * price) / 252.0;
        out[i].rho = -t_arr[i] * price * 0.01;
        out[i] = compute_scalar(F[i], K[i], t_arr[i], r_val, sig_arr[i], call);
    }
}

} // namespace

void compute_batch_avx512(const double* F, const double* K, const double* T,
                          const double* r, const double* sigma,
                          const uint8_t* is_call, Black76Result* out,
                          int count) noexcept {
    int i = 0;
    for (; i + 8 <= count; i += 8) {
        compute_8(F + i, K + i, T + i, r + i, sigma + i, is_call + i, out + i);
    }
    for (; i < count; ++i) {
        out[i] = compute_scalar(F[i], K[i], T[i], r[i], sigma[i], is_call[i] != 0);
    }
}

void compute_batch_avx512_precomputed(const double* F, const double* K,
                                      const double* T, const double* sqrt_T,
                                      const double* disc, const double* sigma,
                                      const uint8_t* is_call, Black76Result* out,
                                      int count) noexcept {
    int i = 0;
    for (; i + 8 <= count; i += 8) {
        compute_8_precomputed(F + i, K + i, T + i, sqrt_T + i, disc + i,
                              sigma + i, is_call + i, out + i);
    }
    for (; i < count; ++i) {
        const double r_val = (T[i] > 1e-10) ? -std::log(disc[i]) / T[i] : 0.0;
        out[i] = compute_scalar(F[i], K[i], T[i], r_val, sigma[i], is_call[i] != 0);
    }
}

} // namespace omm
