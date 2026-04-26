# High-Frequency Trading (HFT) Low-Latency Technical Guide

**Audience**: Software engineers and system architects
**Purpose**: Understand key low-latency techniques used in HFT systems
**Focus**: Principles, effects, and practical implementation

---

## Table of Contents

1. [Lock-Free Programming](#1-lock-free-programming)
2. [Batch Processing](#2-batch-processing)
3. [SIMD Vectorization](#3-simd-vectorization)
4. [Memory Optimization](#4-memory-optimization)
5. [CPU Optimization](#5-cpu-optimization)
6. [Network Optimization](#6-network-optimization)
7. [Event-Driven Architecture](#7-event-driven-architecture)
8. [Power & Thermal Management](#8-power--thermal-management)

---

## 1. Lock-Free Programming

### Principle

**Traditional Approach** (Mutex-based):
```cpp
std::mutex mtx;
void submit_order(Order order) {
    std::lock_guard<std::mutex> lock(mtx);  // Acquire lock
    queue.push(order);                       // Critical section
}  // Release lock
```

**Problem**:
- Lock acquisition: ~25-50ns (uncontended)
- Lock contention: ~1-10μs (contended)
- Context switch: ~1-10μs
- Priority inversion possible

**HFT Approach** (Lock-free):
```cpp
SPSCRingBuffer<Order, 1024> queue;  // Single-Producer Single-Consumer
bool submit_order(Order order) {
    return queue.try_push(order);  // Lock-free, ~5-10ns
}
```

### How It Works

**SPSC Ring Buffer**:
```
Producer Thread          Consumer Thread
     │                        │
     ├─ head_ (atomic)        │
     │                        ├─ tail_ (atomic)
     │                        │
     ▼                        ▼
[0][1][2][3][4][5][6][7]  ← Ring buffer
 └─────────────────────┘
```

**Key Techniques**:
1. **Atomic operations**: `std::atomic<uint64_t>` for head/tail
2. **Memory ordering**: `memory_order_acquire/release`
3. **Cache line separation**: Prevent false sharing
4. **Single writer/reader**: Eliminate contention

### Performance Comparison

| Method | Latency | Throughput | Scalability |
|--------|---------|------------|-------------|
| **Mutex** | 25-50ns (uncontended)<br>1-10μs (contended) | ~20M ops/sec | Poor (contention) |
| **Lock-free** | 5-10ns | ~100M ops/sec | Excellent |

**Improvement**: **5-10× faster**, zero contention

### When to Use

✅ **Use lock-free when**:
- Single producer, single consumer (SPSC)
- High-frequency operations (>1M ops/sec)
- Latency critical (<100ns target)
- Predictable performance required

❌ **Don't use when**:
- Multiple producers/consumers (complex)
- Low-frequency operations (<1K ops/sec)
- Latency not critical (>1ms acceptable)

---

## 2. Batch Processing

### Principle

**Traditional Approach** (One-at-a-time):
```cpp
while (true) {
    Item item;
    if (queue.try_pop(item)) {  // Atomic operation
        process(item);
    }
}
// Cost: N atomic operations for N items
```

**HFT Approach** (Batch):
```cpp
while (true) {
    Item batch[128];
    int count = queue.try_pop_batch(batch, 128);  // Single atomic
    for (int i = 0; i < count; ++i) {
        process(batch[i]);
    }
}
// Cost: 1 atomic operation for N items
```

### How It Works

**Atomic Operation Amortization**:
```
Traditional:
Item 1: acquire-load → read → release-store  (3 atomics)
Item 2: acquire-load → read → release-store  (3 atomics)
Item 3: acquire-load → read → release-store  (3 atomics)
Total: 9 atomic operations

Batch:
Batch: acquire-load → read all → release-store  (2 atomics)
Total: 2 atomic operations (77% reduction)
```

### Performance Comparison

| Method | Atomic Ops | Latency | Throughput |
|--------|------------|---------|------------|
| **One-at-a-time** | N × 3 | ~15ns per item | ~60M items/sec |
| **Batch (N=8)** | 2 | ~5ns per item | ~180M items/sec |

**Improvement**: **87.5% fewer atomics**, **3× throughput**

### When to Use

✅ **Use batching when**:
- High-frequency operations
- Items can be processed independently
- Latency budget allows (batch size × item latency)

❌ **Don't use when**:
- Items must be processed immediately
- Very low latency required (<100ns)
- Items have dependencies

---

## 3. SIMD Vectorization

### Principle

**Traditional Approach** (Scalar):
```cpp
for (int i = 0; i < 8; ++i) {
    result[i] = sqrt(a[i] * b[i] + c[i]);  // One at a time
}
// Cost: 8 iterations, 8 sqrt operations
```

**HFT Approach** (SIMD - AVX-512):
```cpp
__m512d va = _mm512_load_pd(a);      // Load 8 doubles
__m512d vb = _mm512_load_pd(b);      // Load 8 doubles
__m512d vc = _mm512_load_pd(c);      // Load 8 doubles
__m512d vr = _mm512_sqrt_pd(
    _mm512_fmadd_pd(va, vb, vc));    // 8 operations in parallel
_mm512_store_pd(result, vr);         // Store 8 doubles
// Cost: 1 iteration, 1 sqrt operation (8-way parallel)
```

### How It Works

**Vector Processing**:
```
Scalar (Sequential):
a[0] * b[0] + c[0] → sqrt → result[0]  ─┐
a[1] * b[1] + c[1] → sqrt → result[1]   ├─ 8 cycles
a[2] * b[2] + c[2] → sqrt → result[2]   │
...                                      │
a[7] * b[7] + c[7] → sqrt → result[7]  ─┘

SIMD (Parallel):
a[0..7] * b[0..7] + c[0..7] → sqrt → result[0..7]  ─ 1 cycle
```

**Key Requirements**:
1. **Data alignment**: 64-byte aligned (`alignas(64)`)
2. **Contiguous memory**: Arrays, not linked lists
3. **Independent operations**: No data dependencies
4. **Compiler support**: AVX-512 intrinsics

### Performance Comparison

| Method | Operations | Latency | Throughput |
|--------|------------|---------|------------|
| **Scalar** | 8 sequential | ~400ns | ~20M ops/sec |
| **AVX-512** | 8 parallel | ~60ns | ~130M ops/sec |

**Improvement**: **6.5-7.5× speedup**

### When to Use

✅ **Use SIMD when**:
- Processing arrays of data
- Operations are independent
- Data is aligned and contiguous
- CPU supports SIMD (AVX-2, AVX-512)

❌ **Don't use when**:
- Data has dependencies
- Non-contiguous memory
- Complex branching logic
- Portability more important than performance

---

## 4. Memory Optimization

### 4.1 Huge Pages

#### Principle

**Traditional Approach** (4KB pages):
```
Virtual Address → TLB lookup → Page Table → Physical Address
TLB miss rate: ~2-5% (limited TLB entries)
TLB miss penalty: ~100-200ns
```

**HFT Approach** (2MB huge pages):
```
Virtual Address → TLB lookup → Physical Address
TLB miss rate: ~0.01-0.1% (512× better coverage)
TLB miss penalty: ~100-200ns (same, but rare)
```

#### How It Works

**TLB Coverage**:
```
4KB pages:
- TLB entries: 64-1024
- Coverage: 256KB - 4MB
- Miss rate: 2-5%

2MB huge pages:
- TLB entries: 64-1024
- Coverage: 128MB - 2GB
- Miss rate: 0.01-0.1%
```

**Implementation**:
```cpp
void* buffer = mmap(nullptr, size,
                    PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB,
                    -1, 0);
```

#### Performance Comparison

| Page Size | TLB Coverage | Miss Rate | p99 Latency |
|-----------|--------------|-----------|-------------|
| **4KB** | 4MB | 2-5% | 1000ns |
| **2MB** | 2GB | 0.01-0.1% | 650ns |

**Improvement**: **512× TLB coverage**, **35-43% p99 reduction**

### 4.2 NUMA Awareness

#### Principle

**Traditional Approach** (NUMA-unaware):
```
Thread on Node 0 → Access memory on Node 1
Latency: ~100-150ns (remote access)
```

**HFT Approach** (NUMA-aware):
```
Thread on Node 0 → Access memory on Node 0
Latency: ~60-80ns (local access)
```

#### How It Works

**NUMA Architecture**:
```
┌─────────────┐         ┌─────────────┐
│   Node 0    │         │   Node 1    │
│  CPU 0-15   │◄───────►│  CPU 16-31  │
│  Memory 0   │  QPI    │  Memory 1   │
└─────────────┘         └─────────────┘
     60ns                    100ns
   (local)                 (remote)
```

**Implementation**:
```cpp
// Allocate on local node
void* buffer = numa_alloc_local(size);

// Bind thread to node
numa_run_on_node(0);
```

#### Performance Comparison

| Access Type | Latency | Bandwidth |
|-------------|---------|-----------|
| **Local** | 60-80ns | 100 GB/s |
| **Remote** | 100-150ns | 40 GB/s |

**Improvement**: **27% faster access**, **2.5× bandwidth**

### 4.3 Cache Line Alignment

#### Principle

**Traditional Approach** (Unaligned):
```cpp
struct Data {
    int64_t counter;  // May span cache lines
};
Data array[1024];  // Adjacent elements may share cache lines
```

**Problem** (False Sharing):
```
Thread 0 writes array[0] → Invalidates cache line
Thread 1 reads array[1]  → Cache miss (same line)
```

**HFT Approach** (Aligned):
```cpp
struct alignas(64) Data {
    int64_t counter;
    char _pad[56];  // Pad to 64 bytes
};
Data array[1024];  // Each element owns its cache line
```

#### How It Works

**Cache Line Ownership**:
```
Unaligned:
Cache Line 0: [Data[0] | Data[1] (partial)]  ← False sharing
Cache Line 1: [Data[1] (partial) | Data[2]]  ← False sharing

Aligned:
Cache Line 0: [Data[0] | padding]  ← No sharing
Cache Line 1: [Data[1] | padding]  ← No sharing
```

#### Performance Comparison

| Alignment | Cache Misses | Latency | Scalability |
|-----------|--------------|---------|-------------|
| **Unaligned** | 5-10% | 100ns | Poor (bouncing) |
| **Aligned** | 2-5% | 50ns | Excellent |

**Improvement**: **50% fewer cache misses**, **40% less bouncing**

---

## 5. CPU Optimization

### 5.1 Precomputation

#### Principle

**Traditional Approach** (Compute on demand):
```cpp
double price_option(double F, double K, double T, double r) {
    double sqrt_T = sqrt(T);           // Compute every time
    double disc = exp(-r * T);         // Compute every time
    // ... Black-76 formula
}
```

**HFT Approach** (Precompute):
```cpp
// Precompute once at startup
double sqrt_T[MAX_OPTIONS];
double disc[MAX_OPTIONS];

void init_precomputed_terms() {
    for (int i = 0; i < option_count; ++i) {
        sqrt_T[i] = sqrt(T[i]);
        disc[i] = exp(-r * T[i]);
    }
}

double price_option(int option_id, double F, double K) {
    // Use precomputed values
    double sqrt_T_val = sqrt_T[option_id];
    double disc_val = disc[option_id];
    // ... Black-76 formula (faster)
}
```

#### Performance Comparison

| Method | Computations | Latency |
|--------|--------------|---------|
| **On-demand** | sqrt + exp per option | ~100ns |
| **Precomputed** | 0 (lookup only) | ~6ns |

**Improvement**: **94% fewer computations**, **16× faster**

### 5.2 Adaptive Spinning

#### Principle

**Traditional Approach** (Continuous spin):
```cpp
while (!stop) {
    if (!try_work()) {
        _mm_pause();  // Spin forever
    }
}
// CPU usage: 100% (even when idle)
```

**HFT Approach** (Adaptive):
```cpp
int spin_count = 0;
while (!stop) {
    if (!try_work()) {
        if (spin_count < 100) {
            _mm_pause();  // Fast: spin
            ++spin_count;
        } else if (spin_count < 1000) {
            std::this_thread::yield();  // Medium: yield
            ++spin_count;
        } else {
            std::this_thread::sleep_for(1us);  // Slow: sleep
            spin_count = 0;
        }
    } else {
        spin_count = 0;  // Reset on work
    }
}
// CPU usage: 30-50% (when idle)
```

#### Performance Comparison

| Method | Idle CPU | Power | Active Latency |
|--------|----------|-------|----------------|
| **Continuous spin** | 100% | 95W | 1ns |
| **Adaptive** | 30-50% | 60W | 1ns (same) |

**Improvement**: **50-70% CPU reduction**, **37% power reduction**, **no latency impact**

---

## 6. Network Optimization

### 6.1 DPDK (Kernel Bypass)

#### Principle

**Traditional Approach** (Kernel stack):
```
Application
    ↓ System call (~1μs)
Kernel TCP/IP stack
    ↓ Context switch (~1μs)
NIC driver
    ↓ DMA
Network Interface Card
```

**HFT Approach** (DPDK):
```
Application (userspace)
    ↓ Direct access (~100ns)
DPDK PMD (Poll Mode Driver)
    ↓ DMA
Network Interface Card
```

#### How It Works

**Kernel Bypass**:
```
Traditional:
- System calls: ~1μs
- Context switches: ~1μs
- Interrupts: ~1μs
- Total: ~50-100μs

DPDK:
- Direct NIC access: ~100ns
- Zero-copy: 0ns
- Poll mode: 0ns
- Total: ~10-50μs
```

#### Performance Comparison

| Method | Latency | Throughput | CPU |
|--------|---------|------------|-----|
| **Kernel stack** | 50-100μs | 1M pps | 20% |
| **DPDK** | 10-50μs | 10M pps | 100% (polling) |

**Improvement**: **10× lower latency**, **10× throughput**

**Trade-off**: Higher CPU usage (polling)

---

## 7. Event-Driven Architecture

### Principle

**Traditional Approach** (Polling):
```cpp
while (true) {
    for (auto& pair : all_pairs) {  // Scan all pairs
        if (check_opportunity(pair)) {
            execute_trade(pair);
        }
    }
    sleep(100us);  // Minimum 100μs latency
}
// CPU: 100% (continuous scanning)
// Latency: 100-300μs
```

**HFT Approach** (Event-driven):
```cpp
void on_market_update(int instrument_id) {
    // Only check affected pairs (O(1) lookup)
    for (auto& pair : pairs_by_instrument[instrument_id]) {
        if (check_opportunity(pair)) {
            execute_trade(pair);
        }
    }
}
// CPU: <1% (idle), 100% (active)
// Latency: 5-10μs
```

### How It Works

**Polling vs Event-Driven**:
```
Polling:
Time: 0μs    100μs   200μs   300μs
      ↓      ↓       ↓       ↓
      Scan   Scan    Scan    Scan  ← Continuous work
      (all)  (all)   (all)   (all)

Event-Driven:
Time: 0μs    50μs    150μs   250μs
      ↓              ↓
      Event          Event         ← Work only on events
      (affected)     (affected)
```

**Indexed Lookups**:
```cpp
// Index pairs by instrument
std::array<std::vector<int>, MAX_INSTRUMENTS> pairs_by_instrument;

// O(1) lookup instead of O(N) scan
for (int pair_idx : pairs_by_instrument[instrument_id]) {
    // Check only affected pairs
}
```

### Performance Comparison

| Method | Latency | CPU (idle) | Scalability |
|--------|---------|------------|-------------|
| **Polling** | 100-300μs | 100% | O(N) |
| **Event-driven** | 5-10μs | <1% | O(1) |

**Improvement**: **50-90% latency reduction**, **80-95% CPU reduction**

---

## 8. Power & Thermal Management

### Principle

**Why It Matters in HFT**:
1. **Thermal throttling**: CPU reduces frequency when hot
2. **Power limits**: Sustained high power triggers throttling
3. **Datacenter costs**: Power and cooling are expensive
4. **Reliability**: Lower temperatures = longer hardware life

### Techniques

#### 8.1 Adaptive Spinning (Covered in Section 5.2)

**Impact**:
- 37% lower CPU power (60W vs 95W)
- 23% lower system power (115W vs 150W)
- No active latency impact

#### 8.2 Prefetching

**Principle**:
```cpp
// Without prefetching
T item = buffer[index];  // Cache miss: ~100ns

// With prefetching
_mm_prefetch(&buffer[next_index], _MM_HINT_T0);  // Prefetch next
T item = buffer[index];  // Cache hit: ~1ns
```

**Impact**:
- 50% fewer cache misses
- Lower memory bus traffic
- Better power efficiency

---

## Summary: HFT Optimization Hierarchy

### Latency Impact (Highest to Lowest)

1. **Lock-free programming**: 5-10× improvement (1-10μs → 5-10ns)
2. **Event-driven architecture**: 10-50× improvement (100-300μs → 5-10μs)
3. **DPDK kernel bypass**: 5-10× improvement (50-100μs → 10-50μs)
4. **SIMD vectorization**: 6-7× improvement (400ns → 60ns)
5. **Huge pages**: 35-43% p99 improvement
6. **Batch processing**: 87.5% atomic reduction
7. **Precomputation**: 94% computation reduction
8. **Cache alignment**: 50% cache miss reduction
9. **NUMA awareness**: 27% memory access improvement
10. **Adaptive spinning**: 50-70% idle CPU reduction

### Implementation Priority

**Phase 1** (Must-have):
1. Lock-free programming
2. Event-driven architecture
3. Batch processing
4. Precomputation

**Phase 2** (High-value):
5. SIMD vectorization
6. Huge pages
7. Cache alignment
8. NUMA awareness

**Phase 3** (Optimization):
9. DPDK (if network-bound)
10. Adaptive spinning (power efficiency)

### Key Principles

1. **Measure first**: Profile before optimizing
2. **Hot path focus**: Optimize critical paths only
3. **Trade-offs**: Understand complexity vs performance
4. **Maintainability**: Don't sacrifice readability unnecessarily
5. **Testing**: Verify correctness after optimization

---

## Practical Examples from OptionMM

### Example 1: Order Submission Path

**Before** (Mutex-based):
```cpp
std::mutex order_mutex;
void submit_order(Order order) {
    std::lock_guard<std::mutex> lock(order_mutex);
    gateway->send_order(order);
}
// Latency: ~1-10μs (contended)
```

**After** (Lock-free):
```cpp
SPSCRingBuffer<Order, 1024> order_queue;
void submit_order(Order order) {
    order_queue.try_push(order);
}
// Latency: ~5-10ns
```

**Improvement**: 1,300ns per order

### Example 2: Black-76 Pricing

**Before** (Scalar):
```cpp
for (int i = 0; i < 160; ++i) {
    result[i] = black76_price(F, K[i], T[i], sigma[i]);
}
// Latency: ~64μs (160 options)
```

**After** (AVX-512 + Precomputed):
```cpp
for (int i = 0; i < 160; i += 8) {
    __m512d vF = _mm512_set1_pd(F);
    __m512d vK = _mm512_load_pd(&K[i]);
    __m512d vT = _mm512_load_pd(&sqrt_T[i]);  // Precomputed
    __m512d vsigma = _mm512_load_pd(&sigma[i]);
    __m512d vresult = black76_avx512(vF, vK, vT, vsigma);
    _mm512_store_pd(&result[i], vresult);
}
// Latency: ~9μs (160 options)
```

**Improvement**: 7× faster (64μs → 9μs)

### Example 3: Arbitrage Strategy

**Before** (Polling):
```cpp
while (true) {
    for (auto& pair : all_pairs) {  // 1000 pairs
        check_opportunity(pair);
    }
    sleep(100us);
}
// Latency: 100-300μs
// CPU: 100%
```

**After** (Event-driven):
```cpp
void on_market_update(int instrument_id) {
    for (auto& pair : pairs_by_instrument[instrument_id]) {  // ~3 pairs
        check_opportunity(pair);
    }
}
// Latency: 5-10μs
// CPU: <1% (idle)
```

**Improvement**: 50-90% latency reduction, 80-95% CPU reduction

---

## Q&A Topics

### Common Questions

**Q1**: Why not use multiple threads instead of lock-free?
**A**: Threads add overhead (context switches, cache coherency). Lock-free is faster for single producer/consumer patterns.

**Q2**: When should we NOT use SIMD?
**A**: When data has dependencies, non-contiguous memory, or complex branching. Portability may also be a concern.

**Q3**: Is DPDK worth the complexity?
**A**: Only if network is the bottleneck. Adds significant complexity and requires dedicated CPU cores.

**Q4**: How do we balance latency vs power consumption?
**A**: Use adaptive spinning: low latency when active, low power when idle. Best of both worlds.

**Q5**: What's the biggest mistake in HFT optimization?
**A**: Optimizing the wrong thing. Always profile first, optimize hot paths only.

---

## Further Reading

1. **Lock-Free Programming**:
   - "The Art of Multiprocessor Programming" by Herlihy & Shavit
   - Intel's "Memory Ordering" documentation

2. **SIMD**:
   - Intel Intrinsics Guide
   - "Computer Organization and Design" by Patterson & Hennessy

3. **Memory Optimization**:
   - "What Every Programmer Should Know About Memory" by Ulrich Drepper
   - Linux kernel huge pages documentation

4. **HFT Systems**:
   - "Trading and Exchanges" by Larry Harris
   - "Flash Boys" by Michael Lewis (for context)

---

## Conclusion

HFT low-latency optimization is about:
1. **Understanding principles**: Why techniques work
2. **Measuring impact**: Quantify improvements
3. **Making trade-offs**: Complexity vs performance
4. **Focusing on hot paths**: Optimize what matters
5. **Maintaining correctness**: Fast but wrong is useless

**Key Takeaway**: Every nanosecond counts in HFT, but not all nanoseconds are equal. Focus on the hot path, measure everything, and understand the trade-offs.

---

**End of Presentation**

Questions?
