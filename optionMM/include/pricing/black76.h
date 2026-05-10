#pragma once

#include <cmath>
#include <cstdint>

namespace omm {

struct Black76Result {
    double price;
    double std_delta;
    double delta;
    double delta_cash;
    double std_gamma;
    double gamma;
    double gamma_cash;
    double vega;
    double vega_cash;
    double theta;
    double theta_cash;
    double rho;
    double rho_cash;
    double vanna;
    double volga;
    double charm;
};

struct Black76QuoteResult {
    double price;
    double std_delta;
    double delta;
    double gamma;
    double vega;
    double vega_cash;
};

enum class Black76Backend : uint8_t {
    Scalar = 0,
    Avx2,
    Avx512,
};

/**
 * @brief Black76 backend name.
 * @return Return value produced by the operation.
 * @note Noexcept API preserves hot-path failure and latency invariants.
 */
[[nodiscard]] const char* black76_backend_name() noexcept;
/**
 * @brief Black76 backend.
 * @return Return value produced by the operation.
 * @note Noexcept API preserves hot-path failure and latency invariants.
 */
[[nodiscard]] Black76Backend black76_backend() noexcept;
/**
 * @brief Black76 backend available.
 * @param backend Parameter supplied by the caller.
 * @return Return value produced by the operation.
 * @note Noexcept API preserves hot-path failure and latency invariants.
 */
[[nodiscard]] bool black76_backend_available(Black76Backend backend) noexcept;

/**
 * @brief Compute scalar.
 * @param F Parameter supplied by the caller.
 * @param K Parameter supplied by the caller.
 * @param T Parameter supplied by the caller.
 * @param r Parameter supplied by the caller.
 * @param sigma Parameter supplied by the caller.
 * @param is_call Parameter supplied by the caller.
 * @param option_multiplier Parameter supplied by the caller.
 * @param future_multiplier Parameter supplied by the caller.
 * @param trading_days_per_year Parameter supplied by the caller.
 * @return Return value produced by the operation.
 * @note Noexcept API preserves hot-path failure and latency invariants.
 */
[[nodiscard]] Black76Result compute_scalar(
    double F,
    double K,
    double T,
    double r,
    double sigma,
    bool   is_call,
    double option_multiplier = 1.0,
    double future_multiplier = 1.0,
    double trading_days_per_year = 252.0
) noexcept;

/**
 * @brief Compute batch.
 * @param F Parameter supplied by the caller.
 * @param K Parameter supplied by the caller.
 * @param T Parameter supplied by the caller.
 * @param r Parameter supplied by the caller.
 * @param sigma Parameter supplied by the caller.
 * @param is_call Parameter supplied by the caller.
 * @param out Parameter supplied by the caller.
 * @param count Parameter supplied by the caller.
 * @return None.
 * @note Noexcept API preserves hot-path failure and latency invariants.
 */
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

/**
 * @brief Compute batch precomputed.
 * @param F Parameter supplied by the caller.
 * @param K Parameter supplied by the caller.
 * @param T Parameter supplied by the caller.
 * @param sqrt_T Parameter supplied by the caller.
 * @param disc Parameter supplied by the caller.
 * @param sigma Parameter supplied by the caller.
 * @param is_call Parameter supplied by the caller.
 * @param out Parameter supplied by the caller.
 * @param count Parameter supplied by the caller.
 * @return None.
 * @note Noexcept API preserves hot-path failure and latency invariants.
 */
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

/**
 * @brief Compute batch precomputed scaled.
 * @param F Parameter supplied by the caller.
 * @param K Parameter supplied by the caller.
 * @param T Parameter supplied by the caller.
 * @param sqrt_T Parameter supplied by the caller.
 * @param disc Parameter supplied by the caller.
 * @param sigma Parameter supplied by the caller.
 * @param option_multiplier Parameter supplied by the caller.
 * @param future_multiplier Parameter supplied by the caller.
 * @param is_call Parameter supplied by the caller.
 * @param trading_days_per_year Parameter supplied by the caller.
 * @param out Parameter supplied by the caller.
 * @param count Parameter supplied by the caller.
 * @return None.
 * @note Noexcept API preserves hot-path failure and latency invariants.
 */
void compute_batch_precomputed_scaled(
    const double*  F,
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
    int            count
) noexcept;

/**
 * @brief Compute batch quote precomputed.
 * @param F Parameter supplied by the caller.
 * @param K Parameter supplied by the caller.
 * @param sqrt_T Parameter supplied by the caller.
 * @param disc Parameter supplied by the caller.
 * @param sigma Parameter supplied by the caller.
 * @param is_call Parameter supplied by the caller.
 * @param out Parameter supplied by the caller.
 * @param count Parameter supplied by the caller.
 * @return None.
 * @note Noexcept API preserves hot-path failure and latency invariants.
 */
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

// Fused batch pricing: computes bid, mid, ask in single pass
// Reuses sigma lookup and d1/d2 computation (3× → 1×)
/**
 * @brief Compute batch quote fused.
 * @param F_bid Parameter supplied by the caller.
 * @param F_mid Parameter supplied by the caller.
 * @param F_ask Parameter supplied by the caller.
 * @param K Parameter supplied by the caller.
 * @param sqrt_T Parameter supplied by the caller.
 * @param disc Parameter supplied by the caller.
 * @param sigma Parameter supplied by the caller.
 * @param is_call Parameter supplied by the caller.
 * @param bid_out Parameter supplied by the caller.
 * @param mid_out Parameter supplied by the caller.
 * @param ask_out Parameter supplied by the caller.
 * @param count Parameter supplied by the caller.
 * @return None.
 * @note Noexcept API preserves hot-path failure and latency invariants.
 */
void compute_batch_quote_fused(
    const double*  F_bid,
    const double*  F_mid,
    const double*  F_ask,
    const double*  K,
    const double*  sqrt_T,
    const double*  disc,
    const double*  sigma,
    const uint8_t* is_call,
    Black76QuoteResult* bid_out,
    Black76QuoteResult* mid_out,
    Black76QuoteResult* ask_out,
    int            count
) noexcept;

/**
 * @brief Compute batch quote fused scaled.
 * @param F_bid Parameter supplied by the caller.
 * @param F_mid Parameter supplied by the caller.
 * @param F_ask Parameter supplied by the caller.
 * @param K Parameter supplied by the caller.
 * @param sqrt_T Parameter supplied by the caller.
 * @param disc Parameter supplied by the caller.
 * @param sigma Parameter supplied by the caller.
 * @param option_multiplier Parameter supplied by the caller.
 * @param future_multiplier Parameter supplied by the caller.
 * @param is_call Parameter supplied by the caller.
 * @param bid_out Parameter supplied by the caller.
 * @param mid_out Parameter supplied by the caller.
 * @param ask_out Parameter supplied by the caller.
 * @param count Parameter supplied by the caller.
 * @return None.
 * @note Noexcept API preserves hot-path failure and latency invariants.
 */
void compute_batch_quote_fused_scaled(
    const double*  F_bid,
    const double*  F_mid,
    const double*  F_ask,
    const double*  K,
    const double*  sqrt_T,
    const double*  disc,
    const double*  sigma,
    const double*  option_multiplier,
    const double*  future_multiplier,
    const uint8_t* is_call,
    Black76QuoteResult* bid_out,
    Black76QuoteResult* mid_out,
    Black76QuoteResult* ask_out,
    int            count
) noexcept;

#ifdef OMM_BLACK76_AVX2_BACKEND
/**
 * @brief Compute batch avx2.
 * @param F Parameter supplied by the caller.
 * @param K Parameter supplied by the caller.
 * @param T Parameter supplied by the caller.
 * @param r Parameter supplied by the caller.
 * @param sigma Parameter supplied by the caller.
 * @param is_call Parameter supplied by the caller.
 * @param out Parameter supplied by the caller.
 * @param count Parameter supplied by the caller.
 * @return None.
 * @note Noexcept API preserves hot-path failure and latency invariants.
 */
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

/**
 * @brief Compute batch avx2 precomputed.
 * @param F Parameter supplied by the caller.
 * @param K Parameter supplied by the caller.
 * @param T Parameter supplied by the caller.
 * @param sqrt_T Parameter supplied by the caller.
 * @param disc Parameter supplied by the caller.
 * @param sigma Parameter supplied by the caller.
 * @param is_call Parameter supplied by the caller.
 * @param out Parameter supplied by the caller.
 * @param count Parameter supplied by the caller.
 * @return None.
 * @note Noexcept API preserves hot-path failure and latency invariants.
 */
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
/**
 * @brief Compute batch avx512.
 * @param F Parameter supplied by the caller.
 * @param K Parameter supplied by the caller.
 * @param T Parameter supplied by the caller.
 * @param r Parameter supplied by the caller.
 * @param sigma Parameter supplied by the caller.
 * @param is_call Parameter supplied by the caller.
 * @param out Parameter supplied by the caller.
 * @param count Parameter supplied by the caller.
 * @return None.
 * @note Noexcept API preserves hot-path failure and latency invariants.
 */
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

/**
 * @brief Compute batch avx512 precomputed.
 * @param F Parameter supplied by the caller.
 * @param K Parameter supplied by the caller.
 * @param T Parameter supplied by the caller.
 * @param sqrt_T Parameter supplied by the caller.
 * @param disc Parameter supplied by the caller.
 * @param sigma Parameter supplied by the caller.
 * @param is_call Parameter supplied by the caller.
 * @param out Parameter supplied by the caller.
 * @param count Parameter supplied by the caller.
 * @return None.
 * @note Noexcept API preserves hot-path failure and latency invariants.
 */
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

/**
 * @brief Implied vol.
 * @param market_price Parameter supplied by the caller.
 * @param F Parameter supplied by the caller.
 * @param K Parameter supplied by the caller.
 * @param T Parameter supplied by the caller.
 * @param r Parameter supplied by the caller.
 * @param is_call Parameter supplied by the caller.
 * @param tol Parameter supplied by the caller.
 * @return Return value produced by the operation.
 * @note Noexcept API preserves hot-path failure and latency invariants.
 */
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
