#pragma once

#include "common/types.h"
#include "common/config.h"
#include <atomic>

namespace omm {

// ─── Post-trade risk (soft limits) ───────────────────────────────────────────
// Monitored asynchronously on the risk monitor side thread.
// Tracks positions and portfolio Greeks; sets atomic breach flags that
// strategy threads check at the top of each tick loop.
//
// Thread model:
//   Writer: risk monitor thread (calls update() and check_limits())
//   Readers: strategy threads (read breach flags via relaxed atomic loads)
//
// No lock needed: each breach flag is a single atomic<bool>.
// Eventual consistency is acceptable — a one-tick delay in detecting a breach
// is safe given the soft-limit nature of these checks.

class PostTradeRisk {
public:
    /**
     * @brief PostTradeRisk.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    explicit PostTradeRisk(const SoftRiskConfig& cfg) noexcept : cfg_(cfg) {
        reset();
    }

    // Called by the risk monitor thread when a fill arrives.
    /**
     * @brief On fill.
     * @param trade Parameter supplied by the caller.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    void on_fill(const Trade& trade) noexcept;

    // Called by the risk monitor thread periodically.
    // Updates portfolio Greeks from current positions and checks limits.
    // Returns true if all limits are within bounds.
    /**
     * @brief Check limits.
     * @param greeks_table Parameter supplied by the caller.
     * @param n_instruments Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    bool check_limits(const Greeks* greeks_table,   // indexed by instrument_id
                      uint16_t      n_instruments) noexcept;

    // Read by strategy threads (relaxed — eventual consistency acceptable)
    /**
     * @brief Position breach.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] bool position_breach() const noexcept {
        return pos_breach_.load(std::memory_order_relaxed);
    }
    /**
     * @brief Delta breach.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] bool delta_breach() const noexcept {
        return delta_breach_.load(std::memory_order_relaxed);
    }
    /**
     * @brief Gamma breach.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] bool gamma_breach() const noexcept {
        return gamma_breach_.load(std::memory_order_relaxed);
    }
    /**
     * @brief Vega breach.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] bool vega_breach() const noexcept {
        return vega_breach_.load(std::memory_order_relaxed);
    }
    /**
     * @brief Any breach.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] bool any_breach() const noexcept {
        return position_breach() || delta_breach() || gamma_breach() || vega_breach();
    }

    // Read position for a specific instrument (risk monitor thread only)
    /**
     * @brief Get position.
     * @param instrument_id Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] const Position& get_position(uint16_t instrument_id) const noexcept {
        if (instrument_id >= MAX_INSTRUMENTS) return positions_[0];
        return positions_[instrument_id];
    }

    // Read all positions (gRPC snapshot — risk monitor thread or read-only)
    /**
     * @brief Positions.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] const Position* positions() const noexcept { return positions_; }

    /**
     * @brief Portfolio greeks.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] const PortfolioGreeks& portfolio_greeks() const noexcept {
        return portfolio_;
    }

    /**
     * @brief Limits.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] const SoftRiskConfig& limits() const noexcept {
        return cfg_;
    }

    // Update soft risk thresholds at runtime (called from gRPC server thread)
    /**
     * @brief Set limits.
     * @param cfg Parameter supplied by the caller.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    void set_limits(const SoftRiskConfig& cfg) noexcept { cfg_ = cfg; }

    // Convenience overload for gRPC server
    /**
     * @brief Set limits.
     * @param max_pos Parameter supplied by the caller.
     * @param max_delta Parameter supplied by the caller.
     * @param max_gamma Parameter supplied by the caller.
     * @param max_vega Parameter supplied by the caller.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    void set_limits(double max_pos, double max_delta,
                    double max_gamma, double max_vega) noexcept {
        cfg_.max_net_position = static_cast<int32_t>(max_pos);
        cfg_.max_delta        = max_delta;
        cfg_.max_gamma        = max_gamma;
        cfg_.max_vega         = max_vega;
    }

    /**
     * @brief Restore positions.
     * @param positions Parameter supplied by the caller.
     * @param n_positions Parameter supplied by the caller.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    void restore_positions(const Position* positions,
                           uint16_t n_positions) noexcept;

    /**
     * @brief Reset.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    void reset() noexcept;

private:
    SoftRiskConfig cfg_;

    Position       positions_[MAX_INSTRUMENTS]{};
    PortfolioGreeks portfolio_{};

    // Breach flags — written by risk monitor, read by strategy threads
    alignas(64) std::atomic<bool> pos_breach_{false};
    alignas(64) std::atomic<bool> delta_breach_{false};
    alignas(64) std::atomic<bool> gamma_breach_{false};
    alignas(64) std::atomic<bool> vega_breach_{false};
};

} // namespace omm
