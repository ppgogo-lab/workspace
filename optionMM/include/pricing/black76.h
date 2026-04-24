#pragma once

#include <cmath>
#include <cstdint>

namespace omm {

struct Black76Result {
    double price;
    double delta;
    double gamma;
    double vega;
    double theta;
    double rho;
};

struct Black76QuoteResult {
    double price;
    double delta;
    double gamma;
    double vega;
};

enum class Black76Backend : uint8_t {
    Scalar = 0,
    Avx2,
    Avx512,
};

[[nodiscard]] const char* black76_backend_name() noexcept;
[[nodiscard]] Black76Backend black76_backend() noexcept;
[[nodiscard]] bool black76_backend_available(Black76Backend backend) noexcept;

[[nodiscard]] Black76Result compute_scalar(
    double F,
    double K,
    double T,
    double r,
    double sigma,
    bool   is_call
) noexcept;

void compute_batch(
    const double*  F,
    const double*  K,
    const double*  T,
    const double*  r,
    const double*  sigma,
    const uint8_t* is_call,
    Black76Result* out,
    int            count
) noexcept;

void compute_batch_precomputed(
    const double*  F,
    const double*  K,
    const double*  T,
    const double*  sqrt_T,
    const double*  disc,
    const double*  sigma,
    const uint8_t* is_call,
    Black76Result* out,
    int            count
) noexcept;

void compute_batch_quote_precomputed(
    const double*  F,
    const double*  K,
    const double*  sqrt_T,
    const double*  disc,
    const double*  sigma,
    const uint8_t* is_call,
    Black76QuoteResult* out,
    int            count
) noexcept;

#ifdef OMM_BLACK76_AVX2_BACKEND
void compute_batch_avx2(
    const double*  F,
    const double*  K,
    const double*  T,
    const double*  r,
    const double*  sigma,
    const uint8_t* is_call,
    Black76Result* out,
    int            count
) noexcept;

void compute_batch_avx2_precomputed(
    const double*  F,
    const double*  K,
    const double*  T,
    const double*  sqrt_T,
    const double*  disc,
    const double*  sigma,
    const uint8_t* is_call,
    Black76Result* out,
    int            count
) noexcept;
#endif

#ifdef OMM_BLACK76_AVX512_BACKEND
void compute_batch_avx512(
    const double*  F,
    const double*  K,
    const double*  T,
    const double*  r,
    const double*  sigma,
    const uint8_t* is_call,
    Black76Result* out,
    int            count
) noexcept;

void compute_batch_avx512_precomputed(
    const double*  F,
    const double*  K,
    const double*  T,
    const double*  sqrt_T,
    const double*  disc,
    const double*  sigma,
    const uint8_t* is_call,
    Black76Result* out,
    int            count
) noexcept;
#endif

[[nodiscard]] double implied_vol(
    double market_price,
    double F,
    double K,
    double T,
    double r,
    bool   is_call,
    double tol = 1e-7
) noexcept;

} // namespace omm
