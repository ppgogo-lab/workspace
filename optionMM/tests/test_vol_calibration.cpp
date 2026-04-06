#include <gtest/gtest.h>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <algorithm>
#include <cmath>
#include "pricing/black76.h"
#include "pricing/svi.h"
#include "pricing/sabr.h"
#include "pricing/wing.h"
#include "pricing/cubic_spline.h"

using namespace omm;

// ── Data structures ──────────────────────────────────────────────────────────

struct FutureRow {
    std::string expiry;
    double price;
};

struct OptionRow {
    std::string instrument;
    double price;
    std::string option_type;   // "Call" or "Put"
    std::string underlying;
    double strike;
};

struct IVPoint {
    double strike;
    double log_moneyness;
    double market_iv;
    bool is_call;
    std::string instrument;
    double market_price;
};

struct ExpirySlice {
    std::string expiry_code;
    double F;
    double T;
    std::vector<IVPoint> iv_points;
};

struct CalibrationResult {
    SplineSlice spline;
    SVIParams svi;
    SABRParams sabr;
    WingParams wing;
    bool spline_ok;
    bool svi_ok;
    bool sabr_ok;
    bool wing_ok;
};

// ── Helper functions ─────────────────────────────────────────────────────────

// Julian day number calculation
int julian_day_number(int year, int month, int day) {
    int a = (14 - month) / 12;
    int y = year + 4800 - a;
    int m = month + 12 * a - 3;
    return day + (153 * m + 2) / 5 + 365 * y + y / 4 - y / 100 + y / 400 - 32045;
}

// Convert expiry code like "au2605" to time in years from 2026-04-03
// Assumes last trading day is 25th of the expiry month (SHFE gold convention estimate)
double expiry_code_to_T(const std::string& code) {
    if (code.size() < 6) return 0.0;

    int year  = 2000 + std::stoi(code.substr(2, 2));
    int month = std::stoi(code.substr(4, 2));
    int day   = 25;

    int ref_jdn    = julian_day_number(2026, 4, 3);
    int expiry_jdn = julian_day_number(year, month, day);

    double days = static_cast<double>(expiry_jdn - ref_jdn);
    return days / 365.25;
}

// Parse MarketTick.csv file
// CSV format: Instrument,Price,InstrumentKind,OptionType,Underlying,Strike
// InstrumentKind values: "Future", "Option"
// OptionType values: "Call", "Put" (empty for futures)
bool parse_market_csv(const std::string& path,
                      std::vector<FutureRow>& futures,
                      std::vector<OptionRow>& options) {
    std::ifstream file(path);
    if (!file.is_open()) return false;

    std::string line;
    std::getline(file, line);  // Skip header

    while (std::getline(file, line)) {
        // Trim trailing \r (Windows line endings)
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;

        std::stringstream ss(line);
        std::string instrument, price_str, kind, opt_type, underlying, strike_str;

        std::getline(ss, instrument, ',');
        std::getline(ss, price_str,  ',');
        std::getline(ss, kind,       ',');
        std::getline(ss, opt_type,   ',');
        std::getline(ss, underlying, ',');
        std::getline(ss, strike_str, ',');

        if (price_str.empty()) continue;
        double price = std::stod(price_str);

        if (kind == "Future") {
            futures.push_back({instrument, price});
        } else if (kind == "Option") {
            if (strike_str.empty()) continue;
            double strike = std::stod(strike_str);
            options.push_back({instrument, price, opt_type, underlying, strike});
        }
    }

    return true;
}

// Build expiry slice: apply OTM filter, invert IV, sort by strike
// OTM filter: calls with K >= 0.98*F, puts with K <= 1.02*F, price >= 0.05
ExpirySlice build_expiry_slice(const std::string& expiry_code,
                               double F,
                               double T,
                               const std::vector<OptionRow>& all_options,
                               double r) {
    ExpirySlice slice;
    slice.expiry_code = expiry_code;
    slice.F = F;
    slice.T = T;

    for (const auto& opt : all_options) {
        if (opt.underlying != expiry_code) continue;
        if (opt.price < 0.05) continue;

        bool is_call = (opt.option_type == "Call");

        // OTM filter
        if (is_call  && opt.strike < 0.98 * F) continue;
        if (!is_call && opt.strike > 1.02 * F) continue;

        // Implied vol inversion (returns 0.0 on failure)
        double iv = implied_vol(opt.price, F, opt.strike, T, r, is_call);
        if (iv <= 0.0 || !std::isfinite(iv)) continue;

        IVPoint pt;
        pt.strike       = opt.strike;
        pt.log_moneyness = std::log(opt.strike / F);
        pt.market_iv    = iv;
        pt.is_call      = is_call;
        pt.instrument   = opt.instrument;
        pt.market_price = opt.price;

        slice.iv_points.push_back(pt);
    }

    // Sort by strike (ascending)
    std::sort(slice.iv_points.begin(), slice.iv_points.end(),
              [](const IVPoint& a, const IVPoint& b) { return a.strike < b.strike; });

    return slice;
}

// Calibrate all four models for one expiry slice
CalibrationResult calibrate_slice(const ExpirySlice& slice, double sabr_beta = 0.5) {
    CalibrationResult result{};
    result.spline_ok = false;
    result.svi_ok    = false;
    result.sabr_ok   = false;
    result.wing_ok   = false;

    int n = static_cast<int>(slice.iv_points.size());
    if (n < 5) return result;

    // Prepare parallel arrays
    std::vector<double> strikes(n), log_moneyness(n), vols(n);
    for (int i = 0; i < n; ++i) {
        strikes[i]      = slice.iv_points[i].strike;
        log_moneyness[i] = slice.iv_points[i].log_moneyness;
        vols[i]         = slice.iv_points[i].market_iv;
    }

    // Cubic Spline (takes log-moneyness[], vols[], n, T, out)
    fit_cubic_spline_slice(log_moneyness.data(), vols.data(), n, slice.T, result.spline);
    result.spline_ok = result.spline.valid;

    // SVI (takes strikes[], vols[], n, F, T, out)
    result.svi_ok = fit_svi_slice(strikes.data(), vols.data(), n, slice.F, slice.T, result.svi);

    // SABR (takes strikes[], vols[], n, F, T, beta, out)
    result.sabr_ok = fit_sabr_slice(strikes.data(), vols.data(), n,
                                    slice.F, slice.T, sabr_beta, result.sabr);

    // Wing (takes strikes[], vols[], n, F, T, out)
    result.wing_ok = fit_wing_slice(strikes.data(), vols.data(), n, slice.F, slice.T, result.wing);

    return result;
}

// Write model parameters CSV (one row per expiry)
void write_params_csv(const std::string& path,
                      const std::vector<ExpirySlice>& slices,
                      const std::vector<CalibrationResult>& results) {
    std::ofstream file(path);
    file << "expiry,T,F,n_points,"
         << "svi_ok,svi_a,svi_b,svi_rho,svi_m,svi_sigma,"
         << "sabr_ok,sabr_alpha,sabr_beta,sabr_rho,sabr_nu,"
         << "wing_ok,wing_ATM_vol,wing_slope_call,wing_slope_put,wing_curve_call,wing_curve_put,"
         << "spline_ok,spline_n_knots\n";

    for (size_t i = 0; i < slices.size(); ++i) {
        const auto& s = slices[i];
        const auto& r = results[i];

        file << s.expiry_code << "," << s.T << "," << s.F << ","
             << s.iv_points.size() << ",";

        // SVI
        file << r.svi_ok << ",";
        if (r.svi_ok) {
            file << r.svi.a     << "," << r.svi.b   << "," << r.svi.rho << ","
                 << r.svi.m     << "," << r.svi.sigma << ",";
        } else {
            file << ",,,,, ";
        }

        // SABR
        file << r.sabr_ok << ",";
        if (r.sabr_ok) {
            file << r.sabr.alpha << "," << r.sabr.beta << ","
                 << r.sabr.rho   << "," << r.sabr.nu   << ",";
        } else {
            file << ",,,,";
        }

        // Wing
        file << r.wing_ok << ",";
        if (r.wing_ok) {
            file << r.wing.ATM_vol    << "," << r.wing.slope_call << ","
                 << r.wing.slope_put  << "," << r.wing.curve_call << ","
                 << r.wing.curve_put  << ",";
        } else {
            file << ",,,,,";
        }

        // Spline
        file << r.spline_ok << ",";
        if (r.spline_ok) {
            file << r.spline.n;
        }
        file << "\n";
    }
}

// Evaluate Wing vol for output
inline double safe_wing_vol(const CalibrationResult& r, double k) {
    if (!r.wing_ok) return std::numeric_limits<double>::quiet_NaN();
    double v = WingVolSurface::wing_iv(r.wing, k);
    return std::isfinite(v) ? v : std::numeric_limits<double>::quiet_NaN();
}

// Write vol results CSV (one row per valid IV point)
void write_vol_results_csv(const std::string& path,
                           const std::vector<ExpirySlice>& slices,
                           const std::vector<CalibrationResult>& results) {
    std::ofstream file(path);
    file << "expiry,T,F,instrument,strike,log_moneyness,option_type,market_price,market_iv,"
         << "spline_vol,svi_vol,sabr_vol,wing_vol\n";

    for (size_t i = 0; i < slices.size(); ++i) {
        const auto& s = slices[i];
        const auto& r = results[i];

        // Build surface objects once per slice for SVI and SABR
        SVIVolSurface svi_surf;
        SABRVolSurface sabr_surf;
        if (r.svi_ok) {
            svi_surf.n_slices = 1;
            svi_surf.slices[0] = r.svi;
        }
        if (r.sabr_ok) {
            sabr_surf.n_slices = 1;
            sabr_surf.slices[0] = r.sabr;
        }

        for (const auto& pt : s.iv_points) {
            file << s.expiry_code << "," << s.T << "," << s.F << ","
                 << pt.instrument << "," << pt.strike << "," << pt.log_moneyness << ","
                 << (pt.is_call ? "Call" : "Put") << ","
                 << pt.market_price << "," << pt.market_iv << ",";

            // Spline vol
            if (r.spline_ok) {
                double v = CubicSplineVolSurface::eval_slice(r.spline, pt.log_moneyness);
                if (std::isfinite(v)) file << v;
            }
            file << ",";

            // SVI vol
            if (r.svi_ok) {
                double v = svi_surf.get_vol(pt.log_moneyness, s.T);
                if (std::isfinite(v)) file << v;
            }
            file << ",";

            // SABR vol
            if (r.sabr_ok) {
                double v = sabr_surf.get_vol(pt.log_moneyness, s.T);
                if (std::isfinite(v)) file << v;
            }
            file << ",";

            // Wing vol
            if (r.wing_ok) {
                double v = WingVolSurface::wing_iv(r.wing, pt.log_moneyness);
                if (std::isfinite(v)) file << v;
            }
            file << "\n";
        }
    }
}

// ── Main calibration test ─────────────────────────────────────────────────────

TEST(VolCalibration, RunAll) {
    const std::string csv_path   = "../tests/MarketTick.csv";
    const std::string params_out = "../tests/params_results.csv";
    const std::string vols_out   = "../tests/vol_results.csv";
    const double r = 0.03;  // Risk-free rate (annualised)

    // ── Step 1: Parse CSV ────────────────────────────────────────────────────
    std::vector<FutureRow> futures;
    std::vector<OptionRow> options;

    if (!parse_market_csv(csv_path, futures, options)) {
        GTEST_SKIP() << "Could not open " << csv_path;
    }

    ASSERT_GT(futures.size(), 0u) << "No futures found in CSV";
    ASSERT_GT(options.size(), 0u) << "No options found in CSV";

    // Build futures map: expiry_code -> forward price
    std::unordered_map<std::string, double> fwd_map;
    for (const auto& fut : futures) {
        fwd_map[fut.expiry] = fut.price;
    }

    // ── Step 2: Build expiry slices ──────────────────────────────────────────
    // Collect unique expiry codes from options
    std::vector<std::string> expiry_codes;
    for (const auto& opt : options) {
        if (fwd_map.count(opt.underlying) == 0) continue;
        if (std::find(expiry_codes.begin(), expiry_codes.end(), opt.underlying)
            == expiry_codes.end()) {
            expiry_codes.push_back(opt.underlying);
        }
    }
    // Sort by T (ascending)
    std::sort(expiry_codes.begin(), expiry_codes.end());

    std::vector<ExpirySlice> slices;
    std::vector<CalibrationResult> results;

    for (const auto& code : expiry_codes) {
        double F = fwd_map.at(code);
        double T = expiry_code_to_T(code);
        if (T <= 0.0) continue;

        ExpirySlice slice = build_expiry_slice(code, F, T, options, r);

        if (slice.iv_points.size() < 5) continue;

        CalibrationResult result = calibrate_slice(slice, /*sabr_beta=*/0.5);
        slices.push_back(slice);
        results.push_back(result);
    }

    ASSERT_GT(slices.size(), 0u) << "No valid expiry slices produced";

    // ── Step 3: Write output files ────────────────────────────────────────────
    write_params_csv(params_out, slices, results);
    write_vol_results_csv(vols_out, slices, results);

    // Verify files were written
    {
        std::ifstream check(params_out);
        ASSERT_TRUE(check.good()) << "params_results.csv was not written";
    }
    {
        std::ifstream check(vols_out);
        ASSERT_TRUE(check.good()) << "vol_results.csv was not written";
    }

    // ── Summary to stdout ─────────────────────────────────────────────────────
    std::cout << "\n=== Calibration Summary ===\n";
    for (size_t i = 0; i < slices.size(); ++i) {
        const auto& s = slices[i];
        const auto& r = results[i];
        std::cout << s.expiry_code << "  T=" << s.T << "  F=" << s.F
                  << "  n=" << s.iv_points.size()
                  << "  SVI=" << r.svi_ok
                  << "  SABR=" << r.sabr_ok
                  << "  Wing=" << r.wing_ok
                  << "  Spline=" << r.spline_ok << "\n";
    }
    std::cout << "\nOutput files:\n  " << params_out << "\n  " << vols_out << "\n";
}
