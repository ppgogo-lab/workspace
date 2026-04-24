# Optimization Plan and Implementation 202604-1

## Goal
Reduce latency variance and CPU overhead in the option market-making hot paths identified in `findings-20260423.md`, prioritizing the strategy-to-gateway submit path, production gateway callback routing, and option repricing.

## Original Plan
- Remove synchronous recovery-handle lookups from the gateway dispatcher after successful `send_order` and `send_quote`.
- Flatten production gateway state lookup paths so callbacks do not scan fixed arrays while holding gateway state mutexes.
- Reduce pricer hot-path work by separating quote-path pricing from colder full-Greeks calculations.
- Reduce simulator-side benchmark noise with an explicit benchmark mode instead of changing normal simulation behavior.
- Add opt-in deployment-tuned release build flags while keeping the default build portable.

## Implemented Changes
- Extended `IGateway::send_order` and `IGateway::send_quote` to optionally return recovery metadata during submit.
- Updated Sim, CTP, and FEMAS gateways to populate recovery metadata directly from submit-side state.
- Updated `TradingEngine::gateway_dispatcher_loop` to consume returned recovery metadata and stop calling `get_order_recovery_handle` / `get_quote_recovery_handle` immediately after sends.
- Added indexed FEMAS gateway lookup maps for order client ID, order local ID, order sys ID, quote client ID, quote local ID, and quote sys ID.
- Updated FEMAS state lifecycle paths to maintain indexes on submit, restore, sys-ID enrichment, reject, cancel, and clear.
- Moved CTP and FEMAS per-fill callback logging from `INFO` to `DEBUG`.
- Added `Black76QuoteResult` and `compute_batch_quote_precomputed` for quote-path price, delta, gamma, and vega only.
- Updated pricer loop to use quote-only Black-76 batches and stop computing theta/rho on each quote-path repricing batch.
- Added `sim.gateway_benchmark_mode` so benchmark runs can avoid forced simulator worker sleeps while normal simulation keeps existing timing behavior.
- Added `OMM_ENABLE_NATIVE_RELEASE` and `OMM_ENABLE_IPO_RELEASE` CMake options for opt-in host-native and IPO/LTO release tuning.

## Verification
- Built WSL target: `cmake --build build-wsl --target optionmm -j4`.
- Ran targeted regression tests: `ctest --test-dir build-wsl -R 'test_black76|test_simple_mm|test_option_mm_core|test_pcp_arbitrage' --output-on-failure`.
- All targeted tests passed.

## Deferred Work
- Split `MarketTick` into compact top-of-book hot-path data and colder full-depth/monitoring data.
- Convert PCP arbitrage evaluation from polling to an event/coalesced-mailbox trigger model.
- Add a cold-path theta/rho refresh if live monitoring or risk requires current theta/rho snapshots.
- Add typed per-product vol-surface pricers with cached expiry buckets to remove remaining virtual dispatch and expiry-slice scans.
