#include <gtest/gtest.h>
#include "pricing/vol_surface.h"
#include "pricing/svi.h"
#include "pricing/sabr.h"
#include "pricing/cubic_spline.h"

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
