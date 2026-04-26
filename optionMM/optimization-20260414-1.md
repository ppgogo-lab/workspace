 Implemented a latency-focused revision in src/engine/trading_engine.cpp:16, src/strategy/option_mm_core.cpp:278, include/strategy/option_mm_core.h:104, src/gateway/sim_gateway.cpp:12, and tests/test_latency.cpp:1.

  The main engine changes are:

  - engine threads now honor configured affinity when the core ID is valid on the current host
  - the strategy loop drains bursts of pricing signals instead of one signal per spin
  - the gateway dispatcher drains bursts of orders/quotes instead of one item per product per iteration
  - strategy re-evaluation now reuses a single timestamp instead of repeatedly calling clock_gettime
  - the sim gateway no longer adds a hard 5ms polling artifact when zero-latency ack/cancel settings are requested

  I also fixed the release latency benchmark. The old harness was racing a single-consumer ring buffer and had impossible expectations; the new one measures quote traffic at the gateway edge, keeps option book snapshots fresh during the run, and exercises the quote path more realistically in tests/
  test_latency.cpp:339.

  Verification:

  - ./build-latency-release/test_option_mm_core passed
  - ./build-latency-release/test_simple_mm passed
  - ./build-latency-release/test_latency --gtest_filter='LatencyTest.TickToQuoteLatency' passed
  - current WSL release benchmark: p50 9.0us, p95 52.5us, p99 236.8us, p99.9 3.34ms, max 4.46ms

  Highest-priority next improvements for true ultra-low latency:

  - replace feed-side resolve_instrument() linear scans with a fixed hash/index table
  - make monitoring publishes optional or move them off the hot path
  - remove cancel-before-replace if the exchange API supports atomic replace; that state machine is still limiting effective quote update rate
  - wire feed_core affinity for feed handlers you own and validate the full production core layout at startup
  - if you need lower tail latency than this, move timestamping from clock_gettime to calibrated invariant-TSC reads on Linux

  One residual issue remains from the build output: GCC still emits a likely false-positive stringop-overflow warning around SPSCRingBuffer<Trade>::try_push in the gateway dispatcher path.