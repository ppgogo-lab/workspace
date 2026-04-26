Based on my analysis of the codebase, here are the ultra-low latency optimization opportunities ordered by priority:
                                         s)
  Priority 1: Critical Path Optimizations (Highest Impact)

  1. Remove Gateway Recovery Lookups from Send Path ⚡ HIGHEST PRIORITY                                                                                                                                                                      
  Impact: 500-2000ns reduction per order/quote                                                                                                                                                                                                 Location: trading_engine.cpp:1783-1848, gateway implementations

  Problem: After every send_order/send_quote, the dispatcher synchronously calls gateway recovery handle lookups that lock and scan state (femas_gateway.cpp:470,492, ctp_gateway.cpp:293). This is O(n) work directly in the critical path.

  Solution:
  - Return recovery metadata directly from send_order/send_quote (populate during submit)
  - OR defer recovery handle population to the ack callback path (off critical path)
  - Replace linear scans with flat hash tables keyed by local_id/sys_id

  2. Flatten Gateway State Tables ⚡

  Impact: 200-800ns reduction per callback
  Location: femas_gateway.cpp:121,149,705,733,815

  Problem: Production gateway callbacks use mutex + linear scan for every order/quote lookup while holding state_mutex_.

  Solution:
  - Replace std::vector linear scans with FixedHashTable<local_id, state*> (already available in codebase)
  - Use separate hash tables for: local_id → state, sys_id → state, client_id → state
  - Remove or sample fill logging (currently logs every fill on callback thread)

  3. Split Hot Tick to Top-of-Book Only ⚡

  Impact: 100-300ns reduction per tick
  Location: types.h:107,124, trading_engine.cpp:1234

  Problem: MarketTick is 256 bytes but strategy only needs top-of-book (bid/ask/last). Copying 256 bytes through ring buffer + tick_snapshot wastes cache bandwidth.

  Solution:
  - Already partially done! TopOfBookTick (64 bytes) exists but not used everywhere
  - Convert pricer loop to write TopOfBookTick instead of full MarketTick
  - Keep full depth in separate cold storage for monitoring/vol fitting

  ---
  Priority 2: Pricer Thread Optimizations

  4. Eliminate Virtual Dispatch in Pricer Hot Path ⚡

  Impact: 50-150ns per option repricing
  Location: trading_engine.cpp:1282,1324

  Problem: Every option repricing pays virtual dispatch through IVolSurface*, then linear scans expiry slices in svi.h:45, wing.h:51, orc_wing.cpp:204.

  Solution:
  - Template pricer loop on vol surface type (compile-time dispatch)
  - Pre-bucket options by expiry at startup
  - Cache expiry slice pointers per product to eliminate linear scan

  5. Minimize Black-76 Computation ⚡

  Impact: 100-400ns per option batch
  Location: black76.cpp:58, black76_avx2.cpp:253, black76_avx512.cpp:251

  Problem:
  - "Precomputed" path still reconstructs r from disc and T to compute theta/rho
  - Strategy only needs {price, delta, vega} but full Greeks computed
  - Three separate Black-76 sweeps per future tick

  Solution:
  - Create Black76QuoteResult with only {bid_price, ask_price, delta, vega} (already exists!)
  - Use compute_batch_quote_precomputed for hot path (line 230-267 in black76.cpp)
  - Move theta/rho to cold path (computed every 1-5 seconds, not per tick)
  - Eliminate r reconstruction if theta/rho not needed

  6. Batch Pricer Signal Emission ⚡

  Impact: 50-200ns per option
  Location: trading_engine.cpp:1061-1142 (signal suppression logic)

  Problem: Per-option signal suppression checks run for every option, even when surface unchanged.

  Solution:
  - Batch-check surface version first before per-option epsilon checks
  - Use SIMD for epsilon comparisons across option batch
  - Pre-filter options by moneyness (skip deep OTM options)

  ---
  Priority 3: Architecture Improvements

  7. Make Arbitrage Event-Driven 🔄

  Impact: Reduces CPU burn, improves cache efficiency
  Location: trading_engine.cpp:1531,1571, pcp_arbitrage.cpp:204,290

  Problem: Arbitrage thread polls every 100μs, scans all pairs, rebuilds monitor state even when no opportunity exists.

  Solution:
  - Add ArbMarketTrigger mailbox (already exists! arb_market_trigger_buf_)
  - Pricer emits trigger only when underlying moves significantly
  - Arbitrage thread sleeps until triggered
  - Decimate monitor publication (only on state change)

  8. Precompute Option Time-to-Expiry Terms ⏱️

  Impact: 20-80ns per option
  Location: trading_engine.h:333-339

  Problem: option_T_, option_sqrt_T_, option_disc_ refreshed every second, but could cache more terms.

  Solution:
  - Precompute sigma * sqrt(T) per option (changes only when vol surface updates)
  - Precompute log(K) per option (already done! option_log_K_)
  - Store 1 / (F * sigma * sqrt(T)) for gamma calculation

  ---
  Priority 4: Build & Hardware Optimizations

  9. Enable Native CPU Tuning 🏗️

  Impact: 5-15% overall latency reduction
  Location: CMakeLists.txt:77,82

  Problem: Release build uses portable -O3 without host-specific optimizations.

  Solution:
  # For production deployment on Intel Xeon Gold 6544Y
  set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=sapphirerapids -mtune=sapphirerapids")
  set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)  # LTO
  # Optional: PGO (Profile-Guided Optimization)
  # 1. Build with -fprofile-generate
  # 2. Run test_latency to collect profile
  # 3. Rebuild with -fprofile-use

  10. Optimize Ring Buffer Batch Operations 📦

  Impact: 30-100ns per batch
  Location: ring_buffer.h:93-106

  Problem: try_push_batch writes items one-by-one in a loop.

  Solution:
  - Use memcpy for trivially copyable types (already enforced by static_assert)
  - Unroll loop for common batch sizes (4, 8, 16)
  - Prefetch next cache line during batch write

  ---
  Priority 5: Monitoring & Persistence (Off Critical Path)

  11. Defer All Monitoring to Background Thread 📊

  Location: trading_engine.cpp:1144-1194

  Problem: Deferred monitoring still pushes to ring buffers on hot path.

  Solution:
  - Already implemented! Just ensure MonitoringPublishMode::Deferred is used
  - Consider sampling (publish 1 in N ticks for high-frequency instruments)

  12. Batch Persistence Writes 💾

  Location: Persistence ring buffers

  Solution:
  - Batch SQLite writes (already using deferred buffers)
  - Use WAL mode for SQLite
  - Consider memory-mapped files for hot data

  ---
  Quick Wins (Low Effort, Measurable Impact)

  13. Eliminate Unnecessary Atomics ⚛️

  - Replace relaxed atomics with plain writes where single-writer guaranteed
  - Use std::atomic_ref for batch updates

  14. Align Hot Structures to Cache Lines 📏

  - Verify all hot structures are 64-byte aligned (mostly done)
  - Pad structures to avoid false sharing

  15. Reduce Spin-Pause Overhead 🔄

  - Replace spin_pause() with adaptive spinning (spin N times, then yield)
  - Use _mm_prefetch to prefetch next ring buffer slot

  ---
  Measurement & Validation

  After each optimization:
  1. Run test_latency benchmark
  2. Measure p50, p99, p99.9 latencies
  3. Profile with perf to identify new bottlenecks
  4. Verify correctness with unit tests

  Expected cumulative impact: 1-3μs reduction in tick-to-trade latency, bringing you closer to the <2μs target.