#include <gtest/gtest.h>
#include "pricing/vol_surface.h"
#include "pricing/svi.h"
#include "pricing/sabr.h"
#include "pricing/cubic_spline.h"
#include "pricing/wing.h"
#include "pricing/orc_wing.h"

#include <cmath>
#include <thread>
#include <atomic>
#include <random>

using namespace omm;

// ─── SVI: single-slice sanity ─────────────────────────────────────────────────

TEST(SVI, WFormula) {
    // Verify w(k) formula is always >= 0 for typical parameters
    SVIParams p;
    p.a = 0.04; p.b = 0.1; p.rho = -0.3; p.m = 0.0; p.sigma = 0.1; p.expiry_T = 1.0;
    for (double k = -1.0; k <= 1.0; k += 0.1) {
        double wk = SVIVolSurface::w(p, k);
        EXPECT_GE(wk, 0.0) << "w < 0 at k=" << k;
    }
}

TEST(SVI, FlatSurface) {
    // If b=0 the surface is flat (w=a everywhere)
    SVIVolSurface surf;
    surf.n_slices = 1;
    surf.slices[0] = {0.04, 0.0, 0.0, 0.0, 0.1, 1.0, true};
    double iv_atm = surf.get_vol(0.0, 1.0);
    double iv_otm = surf.get_vol(0.5, 1.0);
    EXPECT_NEAR(iv_atm, iv_otm, 1e-10);
    EXPECT_NEAR(iv_atm, std::sqrt(0.04), 1e-10);
}

TEST(SVI, FallbackWhenEmpty) {
    SVIVolSurface surf;
    surf.n_slices = 0;
    double iv = surf.get_vol(0.0, 1.0);
    EXPECT_GT(iv, 0.0);   // returns fallback, not 0 or NaN
    EXPECT_FALSE(std::isnan(iv));
    EXPECT_FALSE(surf.is_valid());
}

TEST(SVI, GetVolByStrike) {
    SVIVolSurface surf;
    surf.n_slices = 1;
    surf.slices[0] = {0.04, 0.1, -0.3, 0.0, 0.1, 1.0, true};
    // get_vol_by_strike(F,K,T) should equal get_vol(log(K/F), T)
    double F = 100.0, K = 105.0, T = 0.5;
    double v1 = surf.get_vol(std::log(K / F), T);
    double v2 = surf.get_vol_by_strike(F, K, T);
    EXPECT_NEAR(v1, v2, 1e-14);
}

TEST(SVI, NoNaNOverRange) {
    SVIVolSurface surf;
    surf.n_slices = 1;
    surf.slices[0] = {0.04, 0.15, -0.4, 0.0, 0.1, 1.0, true};
    for (double k = -2.0; k <= 2.0; k += 0.05) {
        for (double T = 0.1; T <= 2.0; T += 0.1) {
            double v = surf.get_vol(k, T);
            EXPECT_FALSE(std::isnan(v)) << "NaN at k=" << k << " T=" << T;
            EXPECT_FALSE(std::isinf(v)) << "Inf at k=" << k << " T=" << T;
            EXPECT_GE(v, 0.0) << "Negative vol at k=" << k << " T=" << T;
        }
    }
}

TEST(SVI, MultiSliceInterpolation) {
    // Two slices with the same flat vol (20%), at T=0.5 and T=1.0.
    // Interpolated vol at any T between them should also be ~20%.
    SVIVolSurface surf;
    surf.n_slices = 2;
    // a = vol^2 * T for each slice to give flat 20% vol
    surf.slices[0] = {0.04 * 0.5, 0.0, 0.0, 0.0, 0.1, 0.5, true};  // flat 20% at T=0.5
    surf.slices[1] = {0.04 * 1.0, 0.0, 0.0, 0.0, 0.1, 1.0, true};  // flat 20% at T=1.0
    // At any T in [0.5, 1.0], ATM vol should be close to 20%
    for (double T = 0.5; T <= 1.0; T += 0.1) {
        double v = surf.get_vol(0.0, T);
        EXPECT_NEAR(v, 0.20, 0.01) << "Vol should be ~20% at T=" << T;
    }
}

// ─── SVI fitter ───────────────────────────────────────────────────────────────

TEST(SVIFitter, FlatSmile) {
    // Fit to a flat implied vol surface (should converge to b≈0)
    constexpr int N = 11;
    double F = 100.0, T = 0.5;
    double strikes[N], vols[N];
    for (int i = 0; i < N; ++i) {
        strikes[i] = 80.0 + i * 4.0;
        vols[i]    = 0.25;  // flat 25%
    }
    SVIParams out{};
    bool ok = fit_svi_slice(strikes, vols, N, F, T, out);
    ASSERT_TRUE(ok);
    EXPECT_TRUE(out.valid);
    // Fitted surface should reproduce the flat vol within 2%
    SVIVolSurface surf;
    surf.n_slices = 1;
    surf.slices[0] = out;
    for (int i = 0; i < N; ++i) {
        double k  = std::log(strikes[i] / F);
        double iv = surf.get_vol(k, T);
        EXPECT_NEAR(iv, 0.25, 0.02) << "slice vol error at strike " << strikes[i];
    }
}

TEST(SVIFitter, GaussianSmile) {
    // Fit to a Gaussian smile: higher vol at OTM strikes
    constexpr int N = 9;
    double F = 100.0, T = 1.0;
    double strikes[N] = {75, 80, 85, 90, 95, 100, 105, 110, 115};
    double vols[N];
    for (int i = 0; i < N; ++i) {
        double k  = std::log(strikes[i] / F);
        vols[i] = 0.20 + 0.05 * k * k;  // parabolic smile
    }
    SVIParams out{};
    bool ok = fit_svi_slice(strikes, vols, N, F, T, out);
    ASSERT_TRUE(ok);
    // Fitted surface should reproduce vols within 2% error
    SVIVolSurface surf;
    surf.n_slices = 1;
    surf.slices[0] = out;
    double max_err = 0.0;
    for (int i = 0; i < N; ++i) {
        double k  = std::log(strikes[i] / F);
        double iv = surf.get_vol(k, T);
        max_err = std::fmax(max_err, std::fabs(iv - vols[i]));
    }
    EXPECT_LT(max_err, 0.02) << "Max SVI fit error: " << max_err;
}

// ─── SABR: sanity ────────────────────────────────────────────────────────────

TEST(SABR, ATMSpecialCase) {
    SABRParams p{0.3, 1.0, -0.2, 0.4, 0.5, true};
    // ATM: F=K, should not produce NaN
    double v = sabr_vol(p, 100.0, 100.0, 0.5);
    EXPECT_FALSE(std::isnan(v));
    EXPECT_FALSE(std::isinf(v));
    EXPECT_GT(v, 0.0);
}

TEST(SABR, NoNaNOverRange) {
    SABRParams p{0.3, 1.0, -0.2, 0.4, 1.0, true};
    SABRVolSurface surf;
    surf.n_slices = 1;
    surf.slices[0] = p;
    for (double k = -0.5; k <= 0.5; k += 0.05) {
        for (double T = 0.1; T <= 2.0; T += 0.25) {
            double v = surf.get_vol(k, T);
            EXPECT_FALSE(std::isnan(v)) << "NaN k=" << k << " T=" << T;
            EXPECT_GT(v, 0.0);
        }
    }
}

TEST(SABRFitter, FlatSmile) {
    constexpr int N = 9;
    double F = 100.0, T = 0.5;
    double strikes[N], vols[N];
    for (int i = 0; i < N; ++i) {
        strikes[i] = 80.0 + i * 5.0;
        vols[i]    = 0.20;
    }
    SABRParams out{};
    bool ok = fit_sabr_slice(strikes, vols, N, F, T, 1.0, out);
    ASSERT_TRUE(ok);
    EXPECT_TRUE(out.valid);
    SABRVolSurface surf;
    surf.n_slices = 1;
    surf.slices[0] = out;
    for (int i = 0; i < N; ++i) {
        double v = surf.get_vol_by_strike(F, strikes[i], T);
        EXPECT_NEAR(v, 0.20, 0.03) << "SABR fit error at K=" << strikes[i];
    }
}

// ─── Cubic spline: sanity ────────────────────────────────────────────────────

TEST(CubicSpline, InterpolatesExactlyAtKnots) {
    constexpr int N = 5;
    double k[N]   = {-0.4, -0.2, 0.0, 0.2, 0.4};
    double vol[N] = {0.25, 0.22, 0.20, 0.22, 0.25};
    SplineSlice s{};
    fit_cubic_spline_slice(k, vol, N, 1.0, s);
    ASSERT_TRUE(s.valid);
    for (int i = 0; i < N; ++i) {
        double v = CubicSplineVolSurface::eval_slice(s, k[i]);
        EXPECT_NEAR(v, vol[i], 1e-10) << "Spline doesn't interpolate at knot " << i;
    }
}

TEST(CubicSpline, SmoothnessAtKnots) {
    // Values between knots should be between adjacent knot values for a convex smile
    constexpr int N = 5;
    double k[N]   = {-0.4, -0.2, 0.0, 0.2, 0.4};
    double vol[N] = {0.26, 0.22, 0.20, 0.22, 0.26};
    SplineSlice s{};
    fit_cubic_spline_slice(k, vol, N, 1.0, s);
    // Between -0.2 and 0.0, vol should be between 0.20 and 0.22
    for (double ki = -0.2; ki <= 0.0; ki += 0.02) {
        double v = CubicSplineVolSurface::eval_slice(s, ki);
        EXPECT_GE(v, 0.198);
        EXPECT_LE(v, 0.222);
    }
}

TEST(CubicSpline, NoNaNOverRange) {
    constexpr int N = 7;
    double k[N]   = {-0.6, -0.4, -0.2, 0.0, 0.2, 0.4, 0.6};
    double vol[N] = {0.30, 0.26, 0.22, 0.20, 0.22, 0.26, 0.30};
    SplineSlice s{};
    fit_cubic_spline_slice(k, vol, N, 1.0, s);

    CubicSplineVolSurface surf;
    surf.n_slices = 1;
    surf.slices[0] = s;
    for (double ki = -0.7; ki <= 0.7; ki += 0.05) {
        double v = surf.get_vol(ki, 1.0);
        EXPECT_FALSE(std::isnan(v)) << "NaN at k=" << ki;
        EXPECT_GE(v, 0.0);
    }
}

// ─── VolSurfaceManager: atomic swap thread safety ────────────────────────────

TEST(VolSurfaceManager, AtomicSwapThreadSafety) {
    // Writer thread repeatedly publishes new SVI surfaces.
    // Reader thread continuously calls get() and reads vol.
    // No crash or torn read after 2 seconds.
    VolSurfaceManager<SVIVolSurface> mgr;

    // Initialise both buffers to valid surfaces
    for (int buf = 0; buf < 2; ++buf) {
        SVIVolSurface* s = mgr.get_inactive();
        s->n_slices = 1;
        s->slices[0] = {0.04, 0.1, -0.3, 0.0, 0.1, 1.0, true};
        mgr.publish();
    }

    std::atomic<bool> stop{false};
    std::atomic<int>  reader_iters{0}, writer_iters{0};

    std::thread writer([&] {
        double a = 0.04;
        while (!stop.load(std::memory_order_relaxed)) {
            SVIVolSurface* s = mgr.get_inactive();
            s->n_slices = 1;
            s->slices[0] = {a, 0.1, -0.3, 0.0, 0.1, 1.0, true};
            mgr.publish();
            a = 0.04 + 0.01 * ((writer_iters.load() % 3));
            writer_iters.fetch_add(1, std::memory_order_relaxed);
        }
    });

    std::thread reader([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            const IVolSurface* s = mgr.get();
            double v = s->get_vol(0.0, 1.0);
            EXPECT_FALSE(std::isnan(v));
            EXPECT_GT(v, 0.0);
            reader_iters.fetch_add(1, std::memory_order_relaxed);
        }
    });

    std::this_thread::sleep_for(std::chrono::seconds(2));
    stop.store(true, std::memory_order_relaxed);
    writer.join();
    reader.join();

    EXPECT_GT(reader_iters.load(), 1000) << "Reader barely ran";
    EXPECT_GT(writer_iters.load(), 100)  << "Writer barely ran";
}

// ─── Wing: single-slice sanity ────────────────────────────────────────────────

TEST(Wing, FlatSurface) {
    // slope_call=slope_put=curve_call=curve_put=0 → flat vol everywhere
    WingVolSurface surf;
    surf.n_slices = 1;
    surf.slices[0] = {0.20, 0.0, 0.0, 0.0, 0.0, 1.0, true};
    double iv_atm = surf.get_vol(0.0, 1.0);
    double iv_otm = surf.get_vol(0.5, 1.0);
    EXPECT_NEAR(iv_atm, 0.20, 1e-10);
    EXPECT_NEAR(iv_otm, 0.20, 1e-10);
}

TEST(Wing, FallbackWhenEmpty) {
    WingVolSurface surf;
    surf.n_slices = 0;
    double iv = surf.get_vol(0.0, 1.0);
    EXPECT_GT(iv, 0.0);
    EXPECT_FALSE(std::isnan(iv));
    EXPECT_FALSE(surf.is_valid());
}

TEST(Wing, GetVolByStrike) {
    WingVolSurface surf;
    surf.n_slices = 1;
    surf.slices[0] = {0.20, -0.1, 0.1, 0.05, 0.05, 1.0, true};
    double F = 100.0, K = 105.0, T = 0.5;
    double v1 = surf.get_vol(std::log(K / F), T);
    double v2 = surf.get_vol_by_strike(F, K, T);
    EXPECT_NEAR(v1, v2, 1e-14);
}

TEST(Wing, NoNaNOverRange) {
    WingVolSurface surf;
    surf.n_slices = 1;
    surf.slices[0] = {0.20, -0.15, 0.20, 0.05, 0.08, 1.0, true};
    for (double k = -2.0; k <= 2.0; k += 0.05) {
        for (double T = 0.1; T <= 2.0; T += 0.1) {
            double v = surf.get_vol(k, T);
            EXPECT_FALSE(std::isnan(v)) << "NaN at k=" << k << " T=" << T;
            EXPECT_FALSE(std::isinf(v)) << "Inf at k=" << k << " T=" << T;
            EXPECT_GE(v, 0.0) << "Negative vol at k=" << k << " T=" << T;
        }
    }
}

TEST(Wing, CallPutAsymmetry) {
    // slope_call != slope_put → call wing and put wing differ
    WingVolSurface surf;
    surf.n_slices = 1;
    surf.slices[0] = {0.20, -0.2, 0.3, 0.05, 0.05, 1.0, true};
    double iv_call = surf.get_vol( 0.2, 1.0);  // k > 0: call wing
    double iv_put  = surf.get_vol(-0.2, 1.0);  // k < 0: put wing
    // put wing has larger slope so iv_put > iv_call
    EXPECT_GT(iv_put, iv_call);
}

TEST(Wing, MultiSliceInterpolation) {
    // Two flat slices at T=0.5 and T=1.0, both at 20%.
    // Interpolated vol at any T in between should be ~20%.
    WingVolSurface surf;
    surf.n_slices = 2;
    surf.slices[0] = {0.20, 0.0, 0.0, 0.0, 0.0, 0.5, true};
    surf.slices[1] = {0.20, 0.0, 0.0, 0.0, 0.0, 1.0, true};
    for (double T = 0.5; T <= 1.0; T += 0.1) {
        double v = surf.get_vol(0.0, T);
        EXPECT_NEAR(v, 0.20, 1e-6) << "Vol should be 20% at T=" << T;
    }
}

// ─── Wing fitter ──────────────────────────────────────────────────────────────

TEST(WingFitter, FlatSmile) {
    constexpr int N = 11;
    double F = 100.0, T = 0.5;
    double strikes[N], vols[N];
    for (int i = 0; i < N; ++i) {
        strikes[i] = 80.0 + i * 4.0;
        vols[i]    = 0.25;
    }
    WingParams out{};
    bool ok = fit_wing_slice(strikes, vols, N, F, T, out);
    ASSERT_TRUE(ok);
    EXPECT_TRUE(out.valid);
    WingVolSurface surf;
    surf.n_slices = 1;
    surf.slices[0] = out;
    for (int i = 0; i < N; ++i) {
        double k  = std::log(strikes[i] / F);
        double iv = surf.get_vol(k, T);
        EXPECT_NEAR(iv, 0.25, 0.02) << "Flat fit error at K=" << strikes[i];
    }
}

TEST(WingFitter, SymmetricParabolicSmile) {
    // iv(k) = 0.20 + 0.05*k^2 — symmetric, no skew
    // Wing model with slope_call=slope_put=0, curve_call=curve_put=0.05 should fit exactly.
    constexpr int N = 9;
    double F = 100.0, T = 1.0;
    double strikes[N] = {75, 80, 85, 90, 95, 100, 105, 110, 115};
    double vols[N];
    for (int i = 0; i < N; ++i) {
        double k = std::log(strikes[i] / F);
        vols[i] = 0.20 + 0.05 * k * k;
    }
    WingParams out{};
    bool ok = fit_wing_slice(strikes, vols, N, F, T, out);
    ASSERT_TRUE(ok);
    double max_err = 0.0;
    for (int i = 0; i < N; ++i) {
        double k  = std::log(strikes[i] / F);
        double iv = WingVolSurface::wing_iv(out, k);
        max_err = std::fmax(max_err, std::fabs(iv - vols[i]));
    }
    EXPECT_LT(max_err, 0.005) << "Max symmetric parabolic fit error: " << max_err;
}

TEST(WingFitter, AsymmetricSkew) {
    // Typical equity skew: put wing steeper than call wing
    constexpr int N = 9;
    double F = 100.0, T = 0.5;
    double strikes[N] = {75, 80, 85, 90, 95, 100, 105, 110, 115};
    double vols[N];
    for (int i = 0; i < N; ++i) {
        double k = std::log(strikes[i] / F);
        // Asymmetric: steeper on put side
        double kp = std::fmax(k, 0.0);
        double km = std::fmax(-k, 0.0);
        vols[i] = 0.20 - 0.10 * kp + 0.20 * km + 0.04 * kp * kp + 0.06 * km * km;
    }
    WingParams out{};
    bool ok = fit_wing_slice(strikes, vols, N, F, T, out);
    ASSERT_TRUE(ok);
    EXPECT_TRUE(out.valid);
    // Put slope should be larger in magnitude than call slope
    EXPECT_GT(out.slope_put, -out.slope_call);
    // Fit quality
    double max_err = 0.0;
    for (int i = 0; i < N; ++i) {
        double k  = std::log(strikes[i] / F);
        double iv = WingVolSurface::wing_iv(out, k);
        max_err = std::fmax(max_err, std::fabs(iv - vols[i]));
    }
    EXPECT_LT(max_err, 0.005) << "Max asymmetric skew fit error: " << max_err;
}

TEST(WingFitter, InsufficientPoints) {
    // Fewer than 5 points — fitter should return false
    double strikes[3] = {95.0, 100.0, 105.0};
    double vols[3]    = {0.22, 0.20, 0.22};
    WingParams out{};
    bool ok = fit_wing_slice(strikes, vols, 3, 100.0, 0.5, out);
    EXPECT_FALSE(ok);
    EXPECT_FALSE(out.valid);
}

// ─── VolSurfaceManager: Wing atomic swap ─────────────────────────────────────

TEST(VolSurfaceManager, WingAtomicSwap) {
    VolSurfaceManager<WingVolSurface> mgr;

    for (int buf = 0; buf < 2; ++buf) {
        WingVolSurface* w = mgr.get_inactive();
        w->n_slices = 1;
        w->slices[0] = {0.20, -0.1, 0.1, 0.05, 0.05, 1.0, true};
        mgr.publish();
    }

    std::atomic<bool> stop{false};
    std::atomic<int>  reader_iters{0}, writer_iters{0};

    std::thread writer([&] {
        double atm = 0.20;
        while (!stop.load(std::memory_order_relaxed)) {
            WingVolSurface* w = mgr.get_inactive();
            w->n_slices = 1;
            w->slices[0] = {atm, -0.1, 0.1, 0.05, 0.05, 1.0, true};
            mgr.publish();
            atm = 0.18 + 0.04 * ((writer_iters.load() % 3) * 0.5);
            writer_iters.fetch_add(1, std::memory_order_relaxed);
        }
    });

    std::thread reader([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            const IVolSurface* s = mgr.get();
            double v = s->get_vol(0.0, 1.0);
            EXPECT_FALSE(std::isnan(v));
            EXPECT_GT(v, 0.0);
            reader_iters.fetch_add(1, std::memory_order_relaxed);
        }
    });

    std::this_thread::sleep_for(std::chrono::seconds(2));
    stop.store(true, std::memory_order_relaxed);
    writer.join();
    reader.join();

    EXPECT_GT(reader_iters.load(), 1000) << "Reader barely ran";
    EXPECT_GT(writer_iters.load(), 100)  << "Writer barely ran";
}

TEST(OrcWing, FlatSurface) {
    OrcWingVolSurface surf;
    surf.n_slices = 1;
    surf.slices[0].ref_price = 100.0;
    surf.slices[0].atm_forward = 100.0;
    surf.slices[0].ssr = 1.0;
    surf.slices[0].vol_ref = 0.20;
    surf.slices[0].slope_ref = 0.0;
    surf.slices[0].put_curv = 0.0;
    surf.slices[0].call_curv = 0.0;
    surf.slices[0].down_cutoff = -0.2;
    surf.slices[0].up_cutoff = 0.2;
    surf.slices[0].down_smoothing = 0.5;
    surf.slices[0].up_smoothing = 0.5;
    surf.slices[0].expiry_T = 1.0;
    surf.slices[0].valid = true;
    EXPECT_NEAR(surf.get_vol_by_strike(100.0, 80.0, 1.0), 0.20, 1e-10);
    EXPECT_NEAR(surf.get_vol_by_strike(100.0, 100.0, 1.0), 0.20, 1e-10);
    EXPECT_NEAR(surf.get_vol_by_strike(100.0, 120.0, 1.0), 0.20, 1e-10);
}

TEST(OrcWing, GetVolByStrikeMatchesXEval) {
    OrcWingParams p;
    p.ref_price = 100.0;
    p.atm_forward = 100.0;
    p.ssr = 1.0;
    p.vol_ref = 0.22;
    p.slope_ref = -0.15;
    p.put_curv = 0.08;
    p.call_curv = 0.03;
    p.down_cutoff = -0.2;
    p.up_cutoff = 0.25;
    p.down_smoothing = 0.7;
    p.up_smoothing = 0.4;
    p.expiry_T = 1.0;
    p.valid = true;

    OrcWingVolSurface surf;
    surf.n_slices = 1;
    surf.slices[0] = p;

    const double F = 100.0;
    const double K = 110.0;
    const double x = std::log(K / F);
    EXPECT_NEAR(surf.get_vol_by_strike(F, K, 1.0), OrcWingVolSurface::eval_x(p, x), 1e-12);
}

TEST(OrcWing, ContinuousAtBoundaries) {
    OrcWingParams p;
    p.ref_price = 100.0;
    p.atm_forward = 100.0;
    p.ssr = 1.0;
    p.vol_ref = 0.22;
    p.slope_ref = -0.10;
    p.put_curv = 0.12;
    p.call_curv = 0.06;
    p.down_cutoff = -0.18;
    p.up_cutoff = 0.16;
    p.down_smoothing = 0.6;
    p.up_smoothing = 0.8;
    p.valid = true;

    const double eps = 1e-7;
    EXPECT_NEAR(OrcWingVolSurface::eval_x(p, p.down_cutoff - eps),
                OrcWingVolSurface::eval_x(p, p.down_cutoff + eps), 1e-5);
    EXPECT_NEAR(OrcWingVolSurface::eval_x(p, p.up_cutoff - eps),
                OrcWingVolSurface::eval_x(p, p.up_cutoff + eps), 1e-5);
    EXPECT_NEAR(OrcWingVolSurface::eval_x(p, p.down_cutoff * (1.0 + p.down_smoothing) - eps),
                OrcWingVolSurface::eval_x(p, p.down_cutoff * (1.0 + p.down_smoothing) + eps), 1e-5);
    EXPECT_NEAR(OrcWingVolSurface::eval_x(p, p.up_cutoff * (1.0 + p.up_smoothing) - eps),
                OrcWingVolSurface::eval_x(p, p.up_cutoff * (1.0 + p.up_smoothing) + eps), 1e-5);
}

TEST(OrcWing, MultiSliceInterpolation) {
    OrcWingVolSurface surf;
    surf.n_slices = 2;
    surf.slices[0] = {100.0, 100.0, 1.0, 0.18, -0.1, 0.0, 0.0, 0.05, 0.05, -0.15, 0.15, 0.5, 0.5, 0.5, true};
    surf.slices[1] = {100.0, 100.0, 1.0, 0.22, -0.1, 0.0, 0.0, 0.05, 0.05, -0.15, 0.15, 0.5, 0.5, 1.0, true};
    EXPECT_NEAR(surf.get_vol_by_strike(100.0, 100.0, 0.75), 0.20, 1e-6);
}

TEST(OrcWing, NoNaNOverRange) {
    OrcWingVolSurface surf;
    surf.n_slices = 1;
    surf.slices[0] = {100.0, 100.0, 1.0, 0.22, -0.15, 0.0, 0.0, 0.08, 0.04, -0.2, 0.25, 0.7, 0.4, 1.0, true};
    for (double K = 50.0; K <= 150.0; K += 2.5) {
        for (double T = 0.1; T <= 2.0; T += 0.1) {
            const double v = surf.get_vol_by_strike(100.0, K, T);
            EXPECT_FALSE(std::isnan(v));
            EXPECT_FALSE(std::isinf(v));
            EXPECT_GT(v, 0.0);
        }
    }
}

TEST(OrcWing, BatchStrikeEvaluationMatchesScalar) {
    OrcWingVolSurface surf;
    surf.n_slices = 2;
    surf.slices[0] = {100.0, 100.0, 1.0, 0.22, -0.15, 0.0, 0.0, 0.08, 0.04, -0.2, 0.25, 0.7, 0.4, 0.5, true};
    surf.slices[1] = {100.0, 100.0, 1.0, 0.26, -0.05, 0.0, 0.0, 0.04, 0.09, -0.18, 0.2, 0.5, 0.8, 1.0, true};

    const double F = 101.5;
    const double K[] = {75.0, 90.0, 100.0, 110.0, 135.0};
    const double T[] = {0.25, 0.5, 0.75, 1.0, 1.25};
    double out[5]{};
    surf.get_vols_by_strike(F, K, T, out, 5);

    for (int i = 0; i < 5; ++i) {
        EXPECT_NEAR(out[i], surf.get_vol_by_strike(F, K[i], T[i]), 1e-12);
    }
}

TEST(OrcWing, EdgeParametersRemainFinite) {
    OrcWingParams p;
    p.ref_price = 100.0;
    p.atm_forward = 100.0;
    p.ssr = 1.0;
    p.vol_ref = 0.00001;
    p.slope_ref = 20.0;
    p.put_curv = -1.0;
    p.call_curv = 100.0;
    p.down_cutoff = -1e-12;
    p.up_cutoff = 1e-12;
    p.down_smoothing = 1e-12;
    p.up_smoothing = 1e-12;
    p.valid = true;

    for (double x = -1.0; x <= 1.0; x += 0.05) {
        const double v = OrcWingVolSurface::eval_x(p, x);
        EXPECT_TRUE(std::isfinite(v));
        EXPECT_GE(v, 1e-4);
        EXPECT_LE(v, 4.0);
    }
}

TEST(OrcWingFitter, RecoversSyntheticSmile) {
    constexpr int N = 17;
    const double F = 100.0;
    const double T = 0.75;
    OrcWingParams truth{100.0, 100.0, 1.0, 0.21, -0.12, 0.0, 0.0, 0.09, 0.04, -0.22, 0.18, 0.8, 0.35, T, true};
    double strikes[N];
    double vols[N];
    for (int i = 0; i < N; ++i) {
        strikes[i] = 70.0 + 4.0 * i;
        vols[i] = OrcWingVolSurface::eval_x(truth, std::log(strikes[i] / F));
    }

    OrcWingParams out{};
    ASSERT_TRUE(fit_orc_wing_slice(strikes, vols, N, F, T, out));
    EXPECT_TRUE(out.valid);

    double max_err = 0.0;
    for (int i = 0; i < N; ++i) {
        const double model = OrcWingVolSurface::eval_x(out, std::log(strikes[i] / F));
        max_err = std::fmax(max_err, std::fabs(model - vols[i]));
    }
    EXPECT_LT(max_err, 0.02);
    EXPECT_NEAR(out.down_smoothing, truth.down_smoothing, 0.35);
    EXPECT_NEAR(out.up_smoothing, truth.up_smoothing, 0.35);
}

TEST(OrcWingFitter, SparseOneSidedDataStaysStable) {
    const double F = 100.0;
    const double T = 0.5;
    const double strikes[] = {72.0, 78.0, 84.0, 90.0, 96.0, 100.0, 104.0, 108.0};
    const double vols[] = {0.38, 0.34, 0.30, 0.27, 0.24, 0.225, 0.23, 0.245};

    OrcWingParams seed{100.0, 100.0, 1.0, 0.23, -0.10, 0.0, 0.0, 0.08, 0.03, -0.20, 0.12, 0.5, 0.5, T, true};
    OrcWingParams out{};
    ASSERT_TRUE(fit_orc_wing_slice_seeded(strikes, vols, 8, F, T, &seed, out));
    EXPECT_TRUE(out.valid);
    EXPECT_GE(out.vol_ref, 1e-4);
    EXPECT_GE(out.put_curv, 0.0);
    EXPECT_GE(out.call_curv, 0.0);

    for (int i = 0; i < 8; ++i) {
        const double v = OrcWingVolSurface::eval_x(out, std::log(strikes[i] / F));
        EXPECT_TRUE(std::isfinite(v));
        EXPECT_GE(v, 1e-4);
        EXPECT_LE(v, 4.0);
    }
}

TEST(VolSurfaceManager, OrcWingAtomicSwap) {
    VolSurfaceManager<OrcWingVolSurface> mgr;
    for (int buf = 0; buf < 2; ++buf) {
        OrcWingVolSurface* s = mgr.get_inactive();
        s->n_slices = 1;
        s->slices[0] = {100.0, 100.0, 1.0, 0.20, -0.1, 0.0, 0.0, 0.05, 0.05, -0.15, 0.15, 0.5, 0.5, 1.0, true};
        mgr.publish();
    }

    std::atomic<bool> stop{false};
    std::atomic<int> reader_iters{0}, writer_iters{0};

    std::thread writer([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            OrcWingVolSurface* s = mgr.get_inactive();
            s->n_slices = 1;
            s->slices[0] = {100.0, 100.0, 1.0, 0.18 + 0.01 * (writer_iters.load() % 5), -0.1,
                            0.0, 0.0, 0.05, 0.05, -0.15, 0.15, 0.5, 0.5, 1.0, true};
            mgr.publish();
            writer_iters.fetch_add(1, std::memory_order_relaxed);
        }
    });

    std::thread reader([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            const IVolSurface* s = mgr.get();
            const double v = s->get_vol_by_strike(100.0, 100.0, 1.0);
            EXPECT_FALSE(std::isnan(v));
            EXPECT_GT(v, 0.0);
            reader_iters.fetch_add(1, std::memory_order_relaxed);
        }
    });

    std::this_thread::sleep_for(std::chrono::seconds(2));
    stop.store(true, std::memory_order_relaxed);
    writer.join();
    reader.join();
    EXPECT_GT(reader_iters.load(), 1000);
    EXPECT_GT(writer_iters.load(), 100);
}
