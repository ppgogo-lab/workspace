# Add AVX-512 Black76 Backend With Runtime Dispatch

## Summary

Add a new AVX-512 implementation for Black76 batch pricing, keep the existing AVX2 path as the dev/test baseline, and select the best backend at runtime on x86 hosts. The hot path to optimize is `compute_batch_precomputed()` because `TradingEngine` uses it for the three per-batch repricing passes.

## Implementation Changes

- Refactor Black76 batch pricing into three backend levels:
  - scalar fallback
  - existing AVX2 backend processing 4 doubles per batch
  - new AVX-512 backend processing 8 doubles per batch
- Keep the public batch entrypoints unchanged:
  - `compute_batch(...)`
  - `compute_batch_precomputed(...)`
  - The engine call sites stay as-is; dispatch happens inside the pricing layer.
- Split ISA-specific code so AVX-512 is compiled separately from the baseline pricing translation unit.
  - Keep the baseline pricing code buildable on the current AVX2 dev box.
  - Compile the AVX-512 translation unit with `-mavx512f -mfma`.
  - Do not require `-mavx512dq` unless the final intrinsic set actually needs it.
- Add runtime backend selection using CPU feature detection on x86.
  - Prefer AVX-512 when `avx512f` is available.
  - Otherwise use AVX2 when available.
  - Otherwise fall back to scalar.
  - Store the selected backend in a static function pointer or equivalent one-time dispatch table so the hot path does not re-check CPU flags each call.
- Add a lightweight diagnostics API for tests and logging:
  - `black76_backend_name()` or equivalent readonly accessor returning `scalar`, `avx2`, or `avx512`.
- Update the build so portable Linux binaries are not tied to the configure host’s microarchitecture.
  - Remove `-march=native` from the x86 release and profiling defaults.
  - Keep explicit baseline SIMD flags where already required.
  - Add a CMake option such as `OMM_ENABLE_BLACK76_AVX512` default `ON` on Linux x86_64.
- Update the RHEL deployment docs to state:
  - baseline build remains runnable on AVX2 hosts
  - Gold 6544Y will automatically take the AVX-512 backend at runtime
  - the AVX-512 path only depends on `AVX-512F` and `FMA`

## Test Plan

- Extend `test_black76` with direct AVX-512 coverage gated by compile support:
  - AVX-512 batch matches scalar within the same tolerance policy as AVX2
  - AVX-512 precomputed batch matches scalar
  - non-multiple-of-8 tails are correct
  - no `NaN` or `inf` on edge-case inputs
- Add backend-dispatch tests:
  - selected backend reports `avx512` on Gold 6544Y-class hosts
  - selected backend reports `avx2` or `scalar` on lower-capability hosts
- Run at least:
  - `test_black76`
  - `test_vol_calibration` compile check only if needed by pricing linkages
  - `test_latency` or a focused pricing microbenchmark on the target Xeon to confirm the AVX-512 path is actually faster than AVX2 for the precomputed batch workload

## Assumptions

- The production CPU is Intel Xeon Gold 6544Y and supports `AVX-512F` plus `FMA`.
- The target RHEL 8.4 build toolchain provides AVX-512 intrinsics support; no vendor math library is required.
- No config-file knob is needed; backend choice is automatic.
- Numerical behavior should remain aligned with current Black76 tolerances; this is a throughput optimization, not a model change.
