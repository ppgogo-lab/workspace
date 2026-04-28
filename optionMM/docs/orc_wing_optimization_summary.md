# ORC Wing Optimization Summary

## Summary

Implemented ORC Wing fitting and runtime evaluation improvements after reviewing the proposed optimization list. The numerical-Jacobian optimization was not applicable because ORC Wing calibration no longer uses Ceres; the implemented work instead focuses on fitter stability, better seeding, robust objectives, and faster batch evaluation.

Implementation commit:

- `c867b4e Improve ORC Wing fitting stability and batch evaluation`

## Plan Evaluation

- Analytical Jacobians: not implemented because the current fitter is derivative-free Nelder-Mead over shape parameters plus a linear solve for `vc/sc/pc/cc`.
- SIMD batch strike evaluation: deferred. The scalar batch API is now in place, which is the prerequisite for future SIMD work.
- Numerical stability: implemented with bounded coefficient scoring, active-set curvature constraints, robust residual scoring, finite checks, and edge-case tests.
- Piecewise evaluation optimization: implemented with precomputed per-evaluation ORC Wing constants.
- Better initial seeding: implemented with log-moneyness quantile seeds, fallback seeds, and optional previous-slice seeding in live engine calibration.
- Variance reduction in objective: implemented with capped vega weights and Huber-style robust residual costs.

## Key Changes

- Added `OrcWingVolSurface::get_vols_by_strike()` for batch strike-vol calculation.
- Added `fit_orc_wing_slice_seeded()` so live calibration can reuse the currently published expiry slice as a seed.
- Updated ORC Wing runtime pricing loops to use the batch helper instead of per-option scalar calls.
- Removed the stale Ceres dependency from `pricing_lib` because no source file still uses Ceres.
- Refreshed `tests/params_results_20260411.csv` and `tests/vol_results_20260411.csv` with the new ORC Wing calibration behavior.

## Verification

- `test_vol_surface`: 36 tests passed.
- `test_vol_calibration`: passed from `build-wsl` using `tests/MarketTick.csv`; all five ORC Wing expiries fit successfully.
- Direct WSL compile of `trading_engine.cpp` object passed.
- Windows GUI CMake build path reconfigured and reported no build work needed.

Known limitation:

- Full WSL `optionmm` target is still blocked by unrelated existing `OptionState::quote_lifecycle` errors in `src/strategy/option_mm_core.cpp`.

