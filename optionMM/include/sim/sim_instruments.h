#pragma once

#include "common/config.h"

namespace omm {

// Builds a static instrument universe for simulation mode.
// The output order is futures first, then each product's option chain.
[[nodiscard]] uint16_t build_sim_instruments(const SystemConfig& cfg,
                                             Instrument* out,
                                             uint16_t max_count) noexcept;

} // namespace omm
