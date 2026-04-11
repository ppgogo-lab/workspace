#include "pricing/orc_wing.h"
#include "pricing/wing.h"

#include <ceres/ceres.h>

#include <array>
#include <limits>

namespace omm {

namespace {

constexpr double kMinVol = 1e-4;
constexpr double kMaxVol = 4.0;

double clamp_vol(double v) noexcept {
    return std::clamp(v, kMinVol, kMaxVol);
}

class OrcWingResidual final : public ceres::CostFunction {
public:
    OrcWingResidual(double strike, double market_vol, double F, double T, double weight)
        : strike_(strike), market_vol_(market_vol), F_(F), T_(T), weight_(std::sqrt(weight)) {
        set_num_residuals(1);
        mutable_parameter_block_sizes()->push_back(8);
    }

    bool Evaluate(double const* const* parameters,
                  double* residuals,
                  double** jacobians) const override {
        const double* p = parameters[0];
        OrcWingParams params;
        params.ref_price      = F_;
        params.atm_forward    = F_;
        params.ssr            = 1.0;
        params.vol_ref        = p[0];
        params.slope_ref      = p[1];
        params.vcr            = 0.0;
        params.scr            = 0.0;
        params.put_curv       = p[2];
        params.call_curv      = p[3];
        params.down_cutoff    = p[4];
        params.up_cutoff      = p[5];
        params.down_smoothing = p[6];
        params.up_smoothing   = p[7];
        params.expiry_T       = T_;
        params.valid          = true;

        double model = OrcWingVolSurface::eval_x(params, std::log(strike_ / F_));
        residuals[0] = weight_ * (model - market_vol_);

        if (!jacobians || !jacobians[0]) return true;

        static constexpr std::array<double, 8> kSteps{
            1e-5, 1e-4, 1e-4, 1e-4, 1e-4, 1e-4, 1e-4, 1e-4
        };

        for (int j = 0; j < 8; ++j) {
            OrcWingParams plus = params;
            OrcWingParams minus = params;

            double* plus_fields[] = {
                &plus.vol_ref, &plus.slope_ref, &plus.put_curv, &plus.call_curv,
                &plus.down_cutoff, &plus.up_cutoff, &plus.down_smoothing, &plus.up_smoothing
            };
            double* minus_fields[] = {
                &minus.vol_ref, &minus.slope_ref, &minus.put_curv, &minus.call_curv,
                &minus.down_cutoff, &minus.up_cutoff, &minus.down_smoothing, &minus.up_smoothing
            };

            *plus_fields[j] += kSteps[j];
            *minus_fields[j] -= kSteps[j];

            if (j == 2 || j == 3 || j == 6 || j == 7) {
                *minus_fields[j] = std::max(*minus_fields[j], 1e-6);
            }
            if (j == 4) *minus_fields[j] = std::min(*minus_fields[j], -1e-6);
            if (j == 5) *minus_fields[j] = std::max(*minus_fields[j], 1e-6);

            double model_plus = OrcWingVolSurface::eval_x(plus, std::log(strike_ / F_));
            double model_minus = OrcWingVolSurface::eval_x(minus, std::log(strike_ / F_));
            jacobians[0][j] = weight_ * (model_plus - model_minus) / (2.0 * kSteps[j]);
        }

        return true;
    }

private:
    double strike_;
    double market_vol_;
    double F_;
    double T_;
    double weight_;
};

class OrcWingPrior final : public ceres::SizedCostFunction<1, 8> {
public:
    OrcWingPrior(int idx, double target, double weight)
        : idx_(idx), target_(target), weight_(std::sqrt(weight)) {}

    bool Evaluate(double const* const* parameters,
                  double* residuals,
                  double** jacobians) const override {
        residuals[0] = weight_ * (parameters[0][idx_] - target_);
        if (jacobians && jacobians[0]) {
            for (int i = 0; i < 8; ++i) jacobians[0][i] = 0.0;
            jacobians[0][idx_] = weight_;
        }
        return true;
    }

private:
    int idx_;
    double target_;
    double weight_;
};

} // namespace

double OrcWingVolSurface::effective_forward(const OrcWingParams& p) noexcept {
    const double ref = std::max(p.ref_price, 1e-8);
    const double atm = std::max(p.atm_forward, 1e-8);
    const double ssr = std::clamp(p.ssr, 0.0, 1.0);
    return std::exp(ssr * std::log(atm) + (1.0 - ssr) * std::log(ref));
}

double OrcWingVolSurface::current_vol(const OrcWingParams& p) noexcept {
    const double ref = std::max(p.ref_price, 1e-8);
    const double ssr = std::clamp(p.ssr, 0.0, 1.0);
    const double vc = p.vol_ref - p.vcr * ssr * (p.atm_forward - ref) / ref;
    return clamp_vol(vc);
}

double OrcWingVolSurface::current_slope(const OrcWingParams& p) noexcept {
    const double ref = std::max(p.ref_price, 1e-8);
    const double ssr = std::clamp(p.ssr, 0.0, 1.0);
    return p.slope_ref - p.scr * ssr * (p.atm_forward - ref) / ref;
}

double OrcWingVolSurface::eval_x(const OrcWingParams& p, double x) noexcept {
    const double vc  = current_vol(p);
    const double sc  = current_slope(p);
    const double pc  = std::max(p.put_curv, 0.0);
    const double cc  = std::max(p.call_curv, 0.0);
    const double dc  = std::min(p.down_cutoff, -1e-6);
    const double uc  = std::max(p.up_cutoff, 1e-6);
    const double dsm = std::max(p.down_smoothing, 1e-6);
    const double usm = std::max(p.up_smoothing, 1e-6);

    if (x <= dc * (1.0 + dsm)) {
        const double tail = vc + dc * (2.0 + dsm) * (sc / 2.0)
                          + (1.0 + dsm) * pc * dc * dc;
        return clamp_vol(tail);
    }
    if (x <= dc) {
        const double a = vc - (1.0 + 1.0 / dsm) * pc * dc * dc
                       - (sc * dc) / (2.0 * dsm);
        const double b = (1.0 + 1.0 / dsm) * (2.0 * pc * dc + sc);
        const double c = -(pc / dsm + sc / (2.0 * dc * dsm));
        return clamp_vol(a + b * x + c * x * x);
    }
    if (x <= 0.0) {
        return clamp_vol(vc + sc * x + pc * x * x);
    }
    if (x <= uc) {
        return clamp_vol(vc + sc * x + cc * x * x);
    }
    if (x <= uc * (1.0 + usm)) {
        const double a = vc - (1.0 + 1.0 / usm) * cc * uc * uc
                       - (sc * uc) / (2.0 * usm);
        const double b = (1.0 + 1.0 / usm) * (2.0 * cc * uc + sc);
        const double c = -(cc / usm + sc / (2.0 * uc * usm));
        return clamp_vol(a + b * x + c * x * x);
    }

    const double tail = vc + uc * (2.0 + usm) * (sc / 2.0)
                      + (1.0 + usm) * cc * uc * uc;
    return clamp_vol(tail);
}

OrcWingParams OrcWingVolSurface::interpolate(const OrcWingParams& a,
                                             const OrcWingParams& b,
                                             double alpha) noexcept {
    OrcWingParams out;
    auto lerp = [alpha](double x, double y) noexcept { return (1.0 - alpha) * x + alpha * y; };
    out.ref_price      = lerp(a.ref_price, b.ref_price);
    out.atm_forward    = lerp(a.atm_forward, b.atm_forward);
    out.ssr            = lerp(a.ssr, b.ssr);
    out.vol_ref        = lerp(a.vol_ref, b.vol_ref);
    out.slope_ref      = lerp(a.slope_ref, b.slope_ref);
    out.vcr            = lerp(a.vcr, b.vcr);
    out.scr            = lerp(a.scr, b.scr);
    out.put_curv       = lerp(a.put_curv, b.put_curv);
    out.call_curv      = lerp(a.call_curv, b.call_curv);
    out.down_cutoff    = lerp(a.down_cutoff, b.down_cutoff);
    out.up_cutoff      = lerp(a.up_cutoff, b.up_cutoff);
    out.down_smoothing = lerp(a.down_smoothing, b.down_smoothing);
    out.up_smoothing   = lerp(a.up_smoothing, b.up_smoothing);
    out.expiry_T       = lerp(a.expiry_T, b.expiry_T);
    out.valid          = a.valid && b.valid;
    return out;
}

double OrcWingVolSurface::get_vol(double k, double T) const noexcept {
    if (n_slices == 0 || T < 1e-10) return 0.20;
    if (T <= slices[0].expiry_T) return eval_x(slices[0], k);
    if (T >= slices[n_slices - 1].expiry_T) return eval_x(slices[n_slices - 1], k);

    for (int i = 0; i < n_slices - 1; ++i) {
        if (T >= slices[i].expiry_T && T <= slices[i + 1].expiry_T) {
            const double t0 = slices[i].expiry_T;
            const double t1 = slices[i + 1].expiry_T;
            const double alpha = (T - t0) / (t1 - t0);
            return eval_x(interpolate(slices[i], slices[i + 1], alpha), k);
        }
    }
    return 0.20;
}

double OrcWingVolSurface::get_vol_by_strike(double F, double K, double T) const noexcept {
    if (n_slices == 0 || F < 1e-10 || K < 1e-10 || T < 1e-10) return 0.20;

    auto eval_strike = [F, K](const OrcWingParams& p) noexcept {
        OrcWingParams local = p;
        local.atm_forward = F;
        if (local.ref_price < 1e-10) local.ref_price = F;
        const double center = effective_forward(local);
        return eval_x(local, std::log(K / std::max(center, 1e-8)));
    };

    if (T <= slices[0].expiry_T) return eval_strike(slices[0]);
    if (T >= slices[n_slices - 1].expiry_T) return eval_strike(slices[n_slices - 1]);

    for (int i = 0; i < n_slices - 1; ++i) {
        if (T >= slices[i].expiry_T && T <= slices[i + 1].expiry_T) {
            const double t0 = slices[i].expiry_T;
            const double t1 = slices[i + 1].expiry_T;
            const double alpha = (T - t0) / (t1 - t0);
            OrcWingParams interp = interpolate(slices[i], slices[i + 1], alpha);
            interp.atm_forward = F;
            interp.ref_price = F;
            return eval_strike(interp);
        }
    }
    return 0.20;
}

bool fit_orc_wing_slice(const double* strikes,
                        const double* market_vols,
                        int           n,
                        double        F,
                        double        T,
                        OrcWingParams& out,
                        int           max_iter) noexcept {
    if (!strikes || !market_vols || n < 8 || F < 1e-10 || T < 1e-10) return false;

    WingParams seed_wing{};
    const bool have_wing_seed = fit_wing_slice(strikes, market_vols, n, F, T, seed_wing);

    std::array<double, 8> params{
        have_wing_seed ? seed_wing.ATM_vol : market_vols[n / 2],
        have_wing_seed ? 0.5 * (seed_wing.slope_call - seed_wing.slope_put) : -0.1,
        have_wing_seed ? std::max(seed_wing.curve_put, 1e-4) : 0.05,
        have_wing_seed ? std::max(seed_wing.curve_call, 1e-4) : 0.05,
        -0.15,
        0.15,
        0.5,
        0.5
    };

    ceres::Problem problem;
    for (int i = 0; i < n; ++i) {
        const double log_m = std::fabs(std::log(strikes[i] / F));
        const double weight = std::exp(-2.0 * log_m);
        problem.AddResidualBlock(
            new OrcWingResidual(strikes[i], market_vols[i], F, T, weight),
            new ceres::HuberLoss(0.02),
            params.data());
    }

    problem.AddResidualBlock(new OrcWingPrior(6, 0.5, 1e-2), nullptr, params.data());
    problem.AddResidualBlock(new OrcWingPrior(7, 0.5, 1e-2), nullptr, params.data());
    problem.AddResidualBlock(new OrcWingPrior(4, -0.15, 1e-3), nullptr, params.data());
    problem.AddResidualBlock(new OrcWingPrior(5, 0.15, 1e-3), nullptr, params.data());

    problem.SetParameterLowerBound(params.data(), 0, 1e-4);
    problem.SetParameterUpperBound(params.data(), 0, 4.0);
    problem.SetParameterLowerBound(params.data(), 1, -10.0);
    problem.SetParameterUpperBound(params.data(), 1, 10.0);
    problem.SetParameterLowerBound(params.data(), 2, 0.0);
    problem.SetParameterUpperBound(params.data(), 2, 50.0);
    problem.SetParameterLowerBound(params.data(), 3, 0.0);
    problem.SetParameterUpperBound(params.data(), 3, 50.0);
    problem.SetParameterLowerBound(params.data(), 4, -3.0);
    problem.SetParameterUpperBound(params.data(), 4, -1e-3);
    problem.SetParameterLowerBound(params.data(), 5, 1e-3);
    problem.SetParameterUpperBound(params.data(), 5, 3.0);
    problem.SetParameterLowerBound(params.data(), 6, 0.05);
    problem.SetParameterUpperBound(params.data(), 6, 5.0);
    problem.SetParameterLowerBound(params.data(), 7, 0.05);
    problem.SetParameterUpperBound(params.data(), 7, 5.0);

    ceres::Solver::Options options;
    options.linear_solver_type = ceres::DENSE_QR;
    options.trust_region_strategy_type = ceres::LEVENBERG_MARQUARDT;
    options.max_num_iterations = max_iter;
    options.num_threads = 1;
    options.minimizer_progress_to_stdout = false;

    ceres::Solver::Summary summary;
    ceres::Solve(options, &problem, &summary);

    if (!summary.IsSolutionUsable()) return false;

    out.ref_price      = F;
    out.atm_forward    = F;
    out.ssr            = 1.0;
    out.vol_ref        = params[0];
    out.slope_ref      = params[1];
    out.vcr            = 0.0;
    out.scr            = 0.0;
    out.put_curv       = params[2];
    out.call_curv      = params[3];
    out.down_cutoff    = params[4];
    out.up_cutoff      = params[5];
    out.down_smoothing = params[6];
    out.up_smoothing   = params[7];
    out.expiry_T       = T;
    out.valid          = std::isfinite(summary.final_cost);
    return out.valid;
}

} // namespace omm
