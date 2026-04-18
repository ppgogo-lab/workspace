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
    explicit PostTradeRisk(const SoftRiskConfig& cfg) noexcept : cfg_(cfg) {
        reset();
    }

    // Called by the risk monitor thread when a fill arrives.
    void on_fill(const Trade& trade) noexcept;

    // Called by the risk monitor thread periodically.
    // Updates portfolio Greeks from current positions and checks limits.
    // Returns true if all limits are within bounds.
    bool check_limits(const Greeks* greeks_table,   // indexed by instrument_id
                      uint16_t      n_instruments) noexcept;

    // Read by strategy threads (relaxed — eventual consistency acceptable)
    [[nodiscard]] bool position_breach() const noexcept {
        return pos_breach_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] bool delta_breach() const noexcept {
        return delta_breach_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] bool gamma_breach() const noexcept {
        return gamma_breach_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] bool vega_breach() const noexcept {
        return vega_breach_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] bool any_breach() const noexcept {
        return position_breach() || delta_breach() || gamma_breach() || vega_breach();
    }

    // Read position for a specific instrument (risk monitor thread only)
    [[nodiscard]] const Position& get_position(uint16_t instrument_id) const noexcept {
        if (instrument_id >= MAX_INSTRUMENTS) return positions_[0];
        return positions_[instrument_id];
    }

    // Read all positions (gRPC snapshot — risk monitor thread or read-only)
    [[nodiscard]] const Position* positions() const noexcept { return positions_; }

    [[nodiscard]] const PortfolioGreeks& portfolio_greeks() const noexcept {
        return portfolio_;
    }

    [[nodiscard]] const SoftRiskConfig& limits() const noexcept {
        return cfg_;
    }

    // Update soft risk thresholds at runtime (called from gRPC server thread)
    void set_limits(const SoftRiskConfig& cfg) noexcept { cfg_ = cfg; }

    // Convenience overload for gRPC server
    void set_limits(double max_pos, double max_delta,
                    double max_gamma, double max_vega) noexcept {
        cfg_.max_net_position = static_cast<int32_t>(max_pos);
        cfg_.max_delta        = max_delta;
        cfg_.max_gamma        = max_gamma;
        cfg_.max_vega         = max_vega;
    }

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
