#include "risk/post_trade_risk.h"
#include <cstring>
#include <cmath>

namespace omm {

/**
 * @brief Implements Reset.
 * @return None.
 * @note Noexcept API preserves hot-path failure and latency invariants.
 */
void PostTradeRisk::reset() noexcept {
    for (auto& p : positions_) p = Position{};
    portfolio_ = PortfolioGreeks{};
    pos_breach_.store(false, std::memory_order_relaxed);
    delta_breach_.store(false, std::memory_order_relaxed);
    gamma_breach_.store(false, std::memory_order_relaxed);
    vega_breach_.store(false, std::memory_order_relaxed);
}

/**
 * @brief Implements Restore positions.
 * @param positions Parameter supplied by the caller.
 * @param n_positions Parameter supplied by the caller.
 * @return None.
 * @note Noexcept API preserves hot-path failure and latency invariants.
 */
void PostTradeRisk::restore_positions(const Position* positions,
                                      uint16_t n_positions) noexcept {
    reset();
    if (positions == nullptr) return;

    for (uint16_t i = 0; i < n_positions; ++i) {
        const Position& pos = positions[i];
        if (pos.instrument_id >= MAX_INSTRUMENTS) continue;
        positions_[pos.instrument_id] = pos;
    }
}

/**
 * @brief Implements On fill.
 * @param trade Parameter supplied by the caller.
 * @return None.
 * @note Noexcept API preserves hot-path failure and latency invariants.
 */
void PostTradeRisk::on_fill(const Trade& trade) noexcept {
    if (trade.instrument_id >= MAX_INSTRUMENTS) return;

    Position& pos = positions_[trade.instrument_id];
    pos.instrument_id = trade.instrument_id;
    pos.product_index = trade.product_index;

    int32_t qty = trade.fill_volume;
    if (trade.side == Side::Buy) {
        // Update average long price
        double total_cost = pos.avg_long_price * pos.long_position
                          + trade.fill_price * qty;
        pos.long_position += qty;
        pos.long_today    += qty;
        pos.avg_long_price = (pos.long_position > 0)
                             ? total_cost / pos.long_position : 0.0;
    } else {
        // Sell: realize PnL against long position
        if (pos.long_position > 0) {
            int32_t close_qty = (qty <= pos.long_position) ? qty : pos.long_position;
            pos.realized_pnl += (trade.fill_price - pos.avg_long_price) * close_qty;
        }
        double total_cost = pos.avg_short_price * pos.short_position
                          + trade.fill_price * qty;
        pos.short_position += qty;
        pos.short_today    += qty;
        pos.avg_short_price = (pos.short_position > 0)
                              ? total_cost / pos.short_position : 0.0;
    }
    pos.net_position = pos.long_position - pos.short_position;

    // Check per-instrument position limit immediately
    if (std::abs(pos.net_position) > cfg_.max_net_position)
        pos_breach_.store(true, std::memory_order_release);
}

/**
 * @brief Implements Check limits.
 * @param greeks_table Parameter supplied by the caller.
 * @param n_instruments Parameter supplied by the caller.
 * @return Return value produced by the operation.
 * @note Noexcept API preserves hot-path failure and latency invariants.
 */
bool PostTradeRisk::check_limits(const Greeks* greeks_table,
                                  uint16_t n_instruments) noexcept {
    // Aggregate portfolio Greeks from current positions × per-instrument Greeks
    PortfolioGreeks pg{};

    for (uint16_t id = 0; id < n_instruments; ++id) {
        const Position& pos = positions_[id];
        if (pos.net_position == 0) continue;

        const Greeks& g = greeks_table[id];
        double net = static_cast<double>(pos.net_position);
        pg.net_delta += g.delta * net;
        pg.net_gamma += g.gamma * net;
        pg.net_vega  += g.vega  * net;
        pg.net_theta += g.theta * net;
        pg.pnl_realized += pos.realized_pnl;
        // Unrealized: net_position * (current_theo - avg_entry_price)
        double avg_entry = (pos.net_position > 0) ? pos.avg_long_price
                                                   : pos.avg_short_price;
        pg.pnl_unrealized += net * (g.theo_price - avg_entry);
    }
    portfolio_ = pg;

    // Check limits and update breach flags (release store so strategy threads see it)
    bool ok = true;

    bool db = std::fabs(pg.net_delta) > cfg_.max_delta;
    delta_breach_.store(db, std::memory_order_release);
    if (db) ok = false;

    bool gb = std::fabs(pg.net_gamma) > cfg_.max_gamma;
    gamma_breach_.store(gb, std::memory_order_release);
    if (gb) ok = false;

    bool vb = std::fabs(pg.net_vega) > cfg_.max_vega;
    vega_breach_.store(vb, std::memory_order_release);
    if (vb) ok = false;

    return ok;
}

} // namespace omm
