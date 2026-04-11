#pragma once

#include "pricing/vol_surface.h"

#include <algorithm>
#include <cmath>

namespace omm {

struct OrcWingParams {
    double ref_price{100.0};
    double atm_forward{100.0};
    double ssr{1.0};              // [0, 1], 1 = fully swimming skew
    double vol_ref{0.20};
    double slope_ref{-0.1};
    double vcr{0.0};
    double scr{0.0};
    double put_curv{0.05};
    double call_curv{0.05};
    double down_cutoff{-0.15};
    double up_cutoff{0.15};
    double down_smoothing{0.5};
    double up_smoothing{0.5};
    double expiry_T{1.0};
    bool   valid{false};
};

class OrcWingVolSurface : public IVolSurface {
public:
    OrcWingParams slices[MAX_EXPIRIES]{};
    int           n_slices{0};

    [[nodiscard]] static double effective_forward(const OrcWingParams& p) noexcept;
    [[nodiscard]] static double current_vol(const OrcWingParams& p) noexcept;
    [[nodiscard]] static double current_slope(const OrcWingParams& p) noexcept;
    [[nodiscard]] static double eval_x(const OrcWingParams& p, double x) noexcept;
    [[nodiscard]] static OrcWingParams interpolate(const OrcWingParams& a,
                                                   const OrcWingParams& b,
                                                   double alpha) noexcept;

    [[nodiscard]] double get_vol(double k, double T) const noexcept override;
    [[nodiscard]] double get_vol_by_strike(double F, double K, double T) const noexcept override;
    [[nodiscard]] bool is_valid() const noexcept override {
        return n_slices > 0 && slices[0].valid;
    }
};

bool fit_orc_wing_slice(const double* strikes,
                        const double* market_vols,
                        int           n,
                        double        F,
                        double        T,
                        OrcWingParams& out,
                        int           max_iter = 100) noexcept;

} // namespace omm
