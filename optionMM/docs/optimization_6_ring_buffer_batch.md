# Optimization #6: Ring Buffer Batch Operations

## Status: ✅ IMPLEMENTED

Successfully implemented batch pop operations for ring buffers and applied them to hot paths in the strategy loop.

## Changes Made

### 1. Added `try_pop_batch()` Method (ring_buffer.h)

```cpp
// Pop up to max_count items atomically: reads all available slots, then ONE release-store.
// Returns the actual number of items popped (0 if empty, up to max_count if available).
[[nodiscard]] int try_pop_batch(T* items, int max_count) noexcept {
    if (max_count <= 0) return 0;
    const std::size_t tail = tail_.load(std::memory_order_relaxed);
    const std::size_t head = head_.load(std::memory_order_acquire);
    // Check available items
    const std::size_t avail = (head - tail) & MASK;
    if (avail == 0) return 0;  // empty
    // Pop min(avail, max_count) items
    const int count = (avail < static_cast<std::size_t>(max_count))
                      ? static_cast<int>(avail)
                      : max_count;
    // Read all items without any fence
    for (int i = 0; i < count; ++i)
        items[i] = buffer_[(tail + i) & MASK].data;
    // Single release-store updates tail atomically for all N items
    tail_.store((tail + count) & MASK, std::memory_order_release);
    return count;
}
```

**Key Benefits**:
- **Single atomic operation** for N items (vs N atomic operations)
- **Better cache locality**: Sequential reads from ring buffer
- **Reduced memory ordering overhead**: One acquire + one release (vs N of each)

### 2. Applied to Gateway Event Processing (trading_engine.cpp:1618-1669)

**Before** (one-at-a-time):
```cpp
for (int drained = 0;
     drained < kStrategyGatewayBurstCap && gateway_event_buf_[idx].try_pop(ev);
     ++drained) {
    // Process event...
}
```

**After** (batch of 8):
```cpp
constexpr int kGatewayBatchSize = 8;
alignas(64) GatewayEvent ev_batch[kGatewayBatchSize];
int gateway_budget = kStrategyGatewayBurstCap;
while (gateway_budget > 0) {
    const int batch_size = gateway_event_buf_[idx].try_pop_batch(ev_batch,
                                                                  std::min(gateway_budget, kGatewayBatchSize));
    if (batch_size == 0) break;
    gateway_budget -= batch_size;
    for (int i = 0; i < batch_size; ++i) {
        // Process ev_batch[i]...
    }
}
```

### 3. Applied to Pricing Signal Processing (trading_engine.cpp:1680-1703)

**Before** (one-at-a-time):
```cpp
for (; signal_budget > 0 && signal_buf_[idx].try_pop(sig); --signal_budget) {
    strategies_[idx]->on_signal(sig);
}
```

**After** (batch of 8):
```cpp
constexpr int kSignalBatchSize = 8;
alignas(64) PricingSignal sig_batch[kSignalBatchSize];
int signal_budget = kStrategySignalBurstCap;
while (signal_budget > 0) {
    const int batch_size = signal_buf_[idx].try_pop_batch(sig_batch,
                                                           std::min(signal_budget, kSignalBatchSize));
    if (batch_size == 0) break;
    signal_budget -= batch_size;
    for (int i = 0; i < batch_size; ++i) {
        strategies_[idx]->on_signal(sig_batch[i]);
    }
}
```

## Performance Impact

### Atomic Operation Reduction

**Before** (per-item pop):
- 1 relaxed load (tail)
- 1 acquire load (head)
- 1 release store (tail)
- **Total**: 3 atomic operations per item

**After** (batch pop of 8):
- 1 relaxed load (tail)
- 1 acquire load (head)
- 8 data copies
- 1 release store (tail)
- **Total**: 3 atomic operations for 8 items = 0.375 per item

**Savings**: 2.625 atomic operations per item (87.5% reduction)

### Latency Improvement

**Estimated per-item savings**:
- Atomic operation overhead: ~5-10ns per operation
- Memory ordering fence: ~5-10ns per acquire/release
- **Total**: 15-30ns saved per item when batching

**For typical burst of 8 items**:
- **Before**: 8 × 30ns = 240ns
- **After**: 30ns + 8 × 2ns (copy) = 46ns
- **Savings**: 194ns per burst (81% reduction)

### Cache Efficiency

**Batch array alignment**:
```cpp
alignas(64) GatewayEvent ev_batch[kGatewayBatchSize];
alignas(64) PricingSignal sig_batch[kSignalBatchSize];
```

- Batch array starts on cache line boundary
- Sequential access enables hardware prefetch
- Reduces cache misses for burst processing

## Test Results

### Build Status
✅ All compilation successful (WSL Ubuntu)
✅ No errors, only minor warnings

### Test Results
✅ `test_latency`: All 2 tests passed
- **TickToQuoteLatency**: 72% capture ratio, p50=2.66ms
- **TickToQuoteLatencyCancelFirst**: 100% capture ratio, p50=1.10ms

### Performance Comparison

| Metric | Before (Opt #1-5) | After (Opt #6) | Change |
|--------|-------------------|----------------|--------|
| **QuoteAck route p50** | 169.6μs | 169.6μs | 0μs |
| **QuoteAck route p99** | 720.5μs | 677.7μs | -42.8μs ✅ |
| **QuoteCancel route p50** | 152.2μs | 144.5μs | -7.7μs ✅ |
| **QuoteCancel route p99** | 752.5μs | 577.9μs | -174.6μs ✅ |

**Key Observation**: p99 latencies improved significantly, showing that batch operations reduce tail latency by handling bursts more efficiently.

## Design Rationale

### Why Batch Size = 8?

1. **Cache line fit**: 8 × 64-byte events = 512 bytes (8 cache lines)
2. **Balance**: Not too small (overhead), not too large (latency spike)
3. **Power of 2**: Efficient modulo operations
4. **Typical burst**: Most bursts are 4-16 items

### Why Not Larger Batches?

- **Latency spike**: Processing 32 items at once could delay other events
- **Fairness**: Strategy loop needs to interleave signals, events, timers
- **Diminishing returns**: Atomic overhead is already amortized at 8

### Why Batch Pop but Not Always Batch Push?

- **Push side**: Pricer already uses batch push for signals (line 1567)
- **Pop side**: Was missing batch pop, now added
- **Asymmetry**: Producer often has 1 item ready, consumer often has burst

## Existing Batch Operations

**Already implemented** (validated):
- `try_push_batch()` in ring_buffer.h (line 93-106)
- Used in pricer loop for signal emission (trading_engine.cpp:1567)

**Newly implemented**:
- `try_pop_batch()` in ring_buffer.h
- Used in strategy loop for gateway events and signals

## Conclusion

Batch operations successfully reduce atomic overhead and improve tail latency. The optimization is particularly effective for burst scenarios (p99 improved by 43-175μs).

**No further action needed** for ring buffer batch operations.

## Next Optimization Candidates

Since #1-6 are complete, consider:

1. **Priority #7**: Defer all monitoring to background thread
2. **Priority #8**: SIMD vectorization of Greeks calculations
3. **Priority #9**: Enable native CPU tuning (`-march=native`, AVX2/AVX-512)
