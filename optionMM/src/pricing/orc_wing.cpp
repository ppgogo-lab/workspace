#include "pricing/orc_wing.h"
#include "pricing/wing.h"

#include <algorithm>
#include <array>
#include <limits>

namespace omm {

namespace {

constexpr double kMinVol = 1e-4;
constexpr double kMaxVol = 4.0;

double clamp_vol(double v) noexcept {
    return std::clamp(v, kMinVol, kMaxVol);
}

constexpr double kInvSqrt2Pi = 0.39894228040143267794;
constexpr double kBadFit = 1.0e100;

struct OrcWingFitPoint {
    double x;
    double vol;
    double weight;
};

struct OrcWingShape {
    double dc;
    double uc;
    double dsm;
    double usm;
};

struct OrcWingLinearFit {
    double vc;
    double sc;
    double pc;
    double cc;
    double cost;
    bool   ok;
};

double normal_pdf(double x) noexcept {
    return kInvSqrt2Pi * std::exp(-0.5 * x * x);
}

double vega_weight(double F, double K, double T, double vol) noexcept {
    if (F <= 0.0 || K <= 0.0 || T <= 0.0 || vol <= 0.0) return 1.0;
    const double sqrt_T = std::sqrt(T);
    const double denom = std::max(vol * sqrt_T, 1e-8);
    const double d1 = (std::log(F / K) + 0.5 * vol * vol * T) / denom;
    return std::max(F * sqrt_T * normal_pdf(d1), 1e-8);
}

OrcWingShape sanitize_shape(const OrcWingShape& s) noexcept {
    OrcWingShape out{};
    out.dc = std::clamp(s.dc, -3.0, -1e-3);
    out.uc = std::clamp(s.uc, 1e-3, 3.0);
    out.dsm = std::clamp(s.dsm, 0.05, 5.0);
    out.usm = std::clamp(s.usm, 0.05, 5.0);
    return out;
}

void wing_basis(const OrcWingShape& raw_shape, double x, double b[4]) noexcept {
    const OrcWingShape s = sanitize_shape(raw_shape);
    b[0] = 1.0;
    b[1] = 0.0;
    b[2] = 0.0;
    b[3] = 0.0;

    if (x <= s.dc * (1.0 + s.dsm)) {
        b[1] = s.dc * (2.0 + s.dsm) / 2.0;
        b[2] = (1.0 + s.dsm) * s.dc * s.dc;
        return;
    }
    if (x <= s.dc) {
        const double inv_dsm = 1.0 / s.dsm;
        b[1] = -s.dc / (2.0 * s.dsm)
             + (1.0 + inv_dsm) * x
             - x * x / (2.0 * s.dc * s.dsm);
        b[2] = -(1.0 + inv_dsm) * s.dc * s.dc
             + (1.0 + inv_dsm) * 2.0 * s.dc * x
             - x * x * inv_dsm;
        return;
    }
    if (x <= 0.0) {
        b[1] = x;
        b[2] = x * x;
        return;
    }
    if (x <= s.uc) {
        b[1] = x;
        b[3] = x * x;
        return;
    }
    if (x <= s.uc * (1.0 + s.usm)) {
        const double inv_usm = 1.0 / s.usm;
        b[1] = -s.uc / (2.0 * s.usm)
             + (1.0 + inv_usm) * x
             - x * x / (2.0 * s.uc * s.usm);
        b[3] = -(1.0 + inv_usm) * s.uc * s.uc
             + (1.0 + inv_usm) * 2.0 * s.uc * x
             - x * x * inv_usm;
        return;
    }

    b[1] = s.uc * (2.0 + s.usm) / 2.0;
    b[3] = (1.0 + s.usm) * s.uc * s.uc;
}

bool solve_4x4(double a[4][5], double out[4]) noexcept {
    for (int col = 0; col < 4; ++col) {
        int pivot = col;
        double best = std::fabs(a[col][col]);
        for (int row = col + 1; row < 4; ++row) {
            const double v = std::fabs(a[row][col]);
            if (v > best) {
                best = v;
                pivot = row;
            }
        }
        if (best < 1e-14) return false;
        if (pivot != col) {
            for (int k = col; k < 5; ++k) std::swap(a[col][k], a[pivot][k]);
        }

        const double inv_pivot = 1.0 / a[col][col];
        for (int k = col; k < 5; ++k) a[col][k] *= inv_pivot;
        for (int row = 0; row < 4; ++row) {
            if (row == col) continue;
            const double f = a[row][col];
            for (int k = col; k < 5; ++k) a[row][k] -= f * a[col][k];
        }
    }

    for (int i = 0; i < 4; ++i) out[i] = a[i][4];
    return true;
}

OrcWingLinearFit solve_linear_fit(const OrcWingFitPoint* pts, int n,
                                  const OrcWingShape& shape) noexcept {
    double normal[4][5]{};
    double weight_sum = 0.0;

    for (int i = 0; i < n; ++i) {
        double b[4];
        wing_basis(shape, pts[i].x, b);
        const double w = pts[i].weight;
        weight_sum += w;
        for (int r = 0; r < 4; ++r) {
            for (int c = 0; c < 4; ++c) normal[r][c] += w * b[r] * b[c];
            normal[r][4] += w * b[r] * pts[i].vol;
        }
    }

    constexpr double ridge = 1e-10;
    for (int i = 0; i < 4; ++i) normal[i][i] += ridge * std::max(weight_sum, 1.0);

    double coef[4]{};
    if (!solve_4x4(normal, coef)) return {0.0, 0.0, 0.0, 0.0, kBadFit, false};

    double cost = 0.0;
    for (int i = 0; i < n; ++i) {
        double b[4];
        wing_basis(shape, pts[i].x, b);
        const double model = coef[0] + coef[1] * b[1] + coef[2] * b[2] + coef[3] * b[3];
        const double r = model - pts[i].vol;
        cost += pts[i].weight * r * r;
    }

    double penalty = 0.0;
    if (coef[0] < kMinVol) penalty += (kMinVol - coef[0]) * (kMinVol - coef[0]) * 1e5;
    if (coef[0] > kMaxVol) penalty += (coef[0] - kMaxVol) * (coef[0] - kMaxVol) * 1e5;
    if (coef[2] < 0.0) penalty += coef[2] * coef[2] * 1e2;
    if (coef[3] < 0.0) penalty += coef[3] * coef[3] * 1e2;

    return {coef[0], coef[1], coef[2], coef[3], cost + penalty, std::isfinite(cost + penalty)};
}

double score_shape(const OrcWingFitPoint* pts, int n, const OrcWingShape& shape,
                   OrcWingLinearFit* fit = nullptr) noexcept {
    const OrcWingShape s = sanitize_shape(shape);
    OrcWingLinearFit local = solve_linear_fit(pts, n, s);
    if (!local.ok) return kBadFit;

    const double center_prior =
        1e-3 * ((s.dc + 0.18) * (s.dc + 0.18) + (s.uc - 0.18) * (s.uc - 0.18));
    const double smooth_prior =
        1e-4 * ((s.dsm - 0.5) * (s.dsm - 0.5) + (s.usm - 0.5) * (s.usm - 0.5));
    local.cost += center_prior + smooth_prior;

    if (fit) *fit = local;
    return local.cost;
}

struct SimplexVertex {
    OrcWingShape shape;
    double       score;
};

OrcWingShape add_scaled(const OrcWingShape& a, const OrcWingShape& b, double scale) noexcept {
    return sanitize_shape({a.dc + scale * b.dc,
                           a.uc + scale * b.uc,
                           a.dsm + scale * b.dsm,
                           a.usm + scale * b.usm});
}

OrcWingShape sub_shape(const OrcWingShape& a, const OrcWingShape& b) noexcept {
    return {a.dc - b.dc, a.uc - b.uc, a.dsm - b.dsm, a.usm - b.usm};
}

OrcWingShape centroid_without_worst(const SimplexVertex simplex[5]) noexcept {
    OrcWingShape c{};
    for (int i = 0; i < 4; ++i) {
        c.dc += simplex[i].shape.dc;
        c.uc += simplex[i].shape.uc;
        c.dsm += simplex[i].shape.dsm;
        c.usm += simplex[i].shape.usm;
    }
    c.dc /= 4.0;
    c.uc /= 4.0;
    c.dsm /= 4.0;
    c.usm /= 4.0;
    return c;
}

void sort_simplex(SimplexVertex simplex[5]) noexcept {
    std::sort(simplex, simplex + 5, [](const SimplexVertex& a, const SimplexVertex& b) {
        return a.score < b.score;
    });
}

OrcWingShape fit_shape_nelder_mead(const OrcWingFitPoint* pts, int n,
                                   const OrcWingShape& seed,
                                   int max_iter) noexcept {
    SimplexVertex simplex[5]{};
    simplex[0].shape = sanitize_shape(seed);
    simplex[1].shape = sanitize_shape({seed.dc * 1.25, seed.uc, seed.dsm, seed.usm});
    simplex[2].shape = sanitize_shape({seed.dc, seed.uc * 1.25, seed.dsm, seed.usm});
    simplex[3].shape = sanitize_shape({seed.dc, seed.uc, seed.dsm + 0.25, seed.usm});
    simplex[4].shape = sanitize_shape({seed.dc, seed.uc, seed.dsm, seed.usm + 0.25});

    for (auto& v : simplex) v.score = score_shape(pts, n, v.shape);

    const int iterations = std::max(20, max_iter);
    for (int iter = 0; iter < iterations; ++iter) {
        sort_simplex(simplex);

        const double spread = simplex[4].score - simplex[0].score;
        if (spread >= 0.0 && spread < 1e-12) break;

        const OrcWingShape c = centroid_without_worst(simplex);
        const OrcWingShape worst_delta = sub_shape(c, simplex[4].shape);

        SimplexVertex reflected{add_scaled(c, worst_delta, 1.0), 0.0};
        reflected.score = score_shape(pts, n, reflected.shape);

        if (reflected.score < simplex[0].score) {
            SimplexVertex expanded{add_scaled(c, worst_delta, 2.0), 0.0};
            expanded.score = score_shape(pts, n, expanded.shape);
            simplex[4] = expanded.score < reflected.score ? expanded : reflected;
            continue;
        }

        if (reflected.score < simplex[3].score) {
            simplex[4] = reflected;
            continue;
        }

        SimplexVertex contracted{add_scaled(c, sub_shape(simplex[4].shape, c), 0.5), 0.0};
        contracted.score = score_shape(pts, n, contracted.shape);
        if (contracted.score < simplex[4].score) {
            simplex[4] = contracted;
            continue;
        }

        for (int i = 1; i < 5; ++i) {
            simplex[i].shape = add_scaled(simplex[0].shape,
                                          sub_shape(simplex[i].shape, simplex[0].shape),
                                          0.5);
            simplex[i].score = score_shape(pts, n, simplex[i].shape);
        }
    }

    sort_simplex(simplex);
    return simplex[0].shape;
}

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

    std::array<OrcWingFitPoint, MAX_STRIKES * MAX_EXPIRIES> pts{};
    if (n > static_cast<int>(pts.size())) return false;

    double max_weight = 0.0;
    double min_x = std::numeric_limits<double>::max();
    double max_x = std::numeric_limits<double>::lowest();
    double nearest_atm_abs_x = std::numeric_limits<double>::max();
    double atm_vol = market_vols[0];

    for (int i = 0; i < n; ++i) {
        if (strikes[i] <= 0.0 || !std::isfinite(strikes[i])
            || market_vols[i] <= 0.0 || !std::isfinite(market_vols[i])) {
            return false;
        }
        pts[i].x = std::log(strikes[i] / F);
        pts[i].vol = market_vols[i];
        pts[i].weight = vega_weight(F, strikes[i], T, market_vols[i]);
        max_weight = std::max(max_weight, pts[i].weight);
        min_x = std::min(min_x, pts[i].x);
        max_x = std::max(max_x, pts[i].x);
        const double abs_x = std::fabs(pts[i].x);
        if (abs_x < nearest_atm_abs_x) {
            nearest_atm_abs_x = abs_x;
            atm_vol = market_vols[i];
        }
    }

    if (max_weight <= 0.0 || !std::isfinite(max_weight)) return false;
    for (int i = 0; i < n; ++i) pts[i].weight /= max_weight;

    const double left_span = std::max(std::fabs(std::min(min_x, -0.05)), 0.05);
    const double right_span = std::max(std::fabs(std::max(max_x, 0.05)), 0.05);
    OrcWingShape seed{
        -std::clamp(0.75 * left_span, 0.05, 0.35),
        std::clamp(0.75 * right_span, 0.05, 0.35),
        0.5,
        0.5
    };

    OrcWingShape best_shape = fit_shape_nelder_mead(pts.data(), n, seed, max_iter);
    OrcWingLinearFit best_fit{};
    double best_score = score_shape(pts.data(), n, best_shape, &best_fit);

    constexpr std::array<OrcWingShape, 4> fallback_seeds{{
        {-0.10, 0.10, 0.5, 0.5},
        {-0.15, 0.15, 0.5, 0.5},
        {-0.22, 0.18, 0.8, 0.35},
        {-0.30, 0.30, 0.5, 0.5},
    }};

    for (const OrcWingShape& fallback_seed : fallback_seeds) {
        const OrcWingShape candidate_shape =
            fit_shape_nelder_mead(pts.data(), n, fallback_seed, max_iter / 2);
        OrcWingLinearFit candidate_fit{};
        const double candidate_score =
            score_shape(pts.data(), n, candidate_shape, &candidate_fit);
        if (candidate_fit.ok && candidate_score < best_score) {
            best_shape = candidate_shape;
            best_fit = candidate_fit;
            best_score = candidate_score;
        }
    }

    if (!best_fit.ok || !std::isfinite(best_score)) return false;

    if (have_wing_seed) {
        best_fit.vc = std::isfinite(best_fit.vc) ? best_fit.vc : seed_wing.ATM_vol;
    } else if (!std::isfinite(best_fit.vc)) {
        best_fit.vc = atm_vol;
    }

    out.ref_price      = F;
    out.atm_forward    = F;
    out.ssr            = 1.0;
    out.vol_ref        = clamp_vol(best_fit.vc);
    out.slope_ref      = std::clamp(best_fit.sc, -10.0, 10.0);
    out.vcr            = 0.0;
    out.scr            = 0.0;
    out.put_curv       = std::clamp(best_fit.pc, 0.0, 50.0);
    out.call_curv      = std::clamp(best_fit.cc, 0.0, 50.0);
    out.down_cutoff    = best_shape.dc;
    out.up_cutoff      = best_shape.uc;
    out.down_smoothing = best_shape.dsm;
    out.up_smoothing   = best_shape.usm;
    out.expiry_T       = T;
    out.valid          = true;
    return out.valid;
}

} // namespace omm
