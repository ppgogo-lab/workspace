# Optimization #17: Eliminate Virtual Dispatch in Pricer Hot Path

## Status: 📋 DOCUMENTED (Future Optimization)

Virtual dispatch in the pricer hot path has been identified and documented as a future optimization opportunity. The current implementation uses `IVolSurface*` with virtual calls, costing ~5-20ns per option.

## Analysis

### Current Implementation (Virtual Dispatch)

**Problem Code** (trading_engine.cpp:1568-1611):
```cpp
// Select surface based on vol method (runtime check)
IVolSurface* surf = nullptr;
if (cfg_.pricing.vol_method == VolMethod::Wing) {
    surf = wing_surfaces_[prod].get();
} else if (cfg_.pricing.vol_method == VolMethod::OrcWing) {
    surf = orc_wing_surfaces_[prod].get();
} else {
    surf = vol_surfaces_[prod].get();
}

// Hot loop - virtual dispatch for every option
for (uint16_t bi = 0; bi < batch_n; ++bi) {
    sigma_arr[bi] = (cfg_.pricing.vol_method == VolMethod::OrcWing)
        ? surf->get_vol_by_strike(F_mid, opt.strike, T_arr[bi])  // Virtual call
        : surf->get_vol(option_log_K_[prod][oi] - log_F_mid, T_arr[bi]);  // Virtual call
}
```

**Cost Per Virtual Call**:
- Indirect branch: ~5-10ns
- I-cache miss potential: ~50-100ns (if not cached)
- Branch misprediction: ~10-20ns (if predicted wrong)
- **Total**: ~5-20ns per call

**Total Cost**:
- 160 options per batch × 5-20ns = **800-3200ns per batch**
- With 320 iterations: **256-1024μs total overhead**

### Root Cause

1. **Runtime Polymorphism**: `IVolSurface*` uses virtual dispatch
2. **Indirect Call**: CPU cannot inline or optimize virtual calls
3. **I-Cache Pollution**: Virtual table lookups pollute instruction cache
4. **Branch Prediction**: Indirect branches are harder to predict

## Recommended Solution: Template-Based Dispatch

### Implementation Strategy

**Step 1**: Extract pricing loop into template function:
```cpp
template<typename VolSurface>
void price_option_batch_impl(int prod, VolSurface* surf,
                             const TopOfBookTick& future_tick,
                             uint16_t start, uint16_t batch_n) {
    // Pricing loop with direct calls (no virtual dispatch)
    for (uint16_t bi = 0; bi < batch_n; ++bi) {
        const uint16_t oi = start + bi;
        const uint16_t opt_id = option_ids_[prod][oi];
        const Instrument& opt = instruments_[opt_id];
        
        // Direct call - compiler can inline
        sigma_arr[bi] = surf->get_vol(option_log_K_[prod][oi] - log_F_mid, T_arr[bi]);
        
        // ... rest of pricing logic
    }
}
```

**Step 2**: Dispatch based on vol method (once per product):
```cpp
void TradingEngine::pricer_loop() noexcept {
    // ... setup code ...
    
    // Dispatch to template specialization
    switch (cfg_.pricing.vol_method) {
    case VolMethod::SVI:
        price_option_batch_impl(prod, vol_surfaces_[prod].get(), future_tick, start, batch_n);
        break;
    case VolMethod::Wing:
        price_option_batch_impl(prod, wing_surfaces_[prod].get(), future_tick, start, batch_n);
        break;
    case VolMethod::OrcWing:
        price_option_batch_impl(prod, orc_wing_surfaces_[prod].get(), future_tick, start, batch_n);
        break;
    }
}
```

### Benefits

**Performance**:
- **Virtual dispatch eliminated**: 0ns overhead (inlined)
- **Better I-cache**: Direct calls, no vtable lookups
- **Better branch prediction**: Direct branches
- **Expected improvement**: 5-10% pricer throughput

**Code Quality**:
- Type-safe (templates)
- Compiler can optimize and inline
- Zero runtime overhead
- Maintainable (clear separation)

### Trade-offs

**Pros**:
- Zero runtime overhead
- Compiler can inline get_vol()
- Better instruction cache utilization
- Type-safe

**Cons**:
- Code duplication (template instantiation)
- Longer compile times (~10-20% increase)
- Larger binary size (~5-10KB per template)
- More complex code structure

## Alternative Approaches

### Approach 2: Function Pointers

**Implementation**:
```cpp
using GetVolFunc = double(*)(const void* surf, double log_moneyness, double T);

double get_vol_svi(const void* surf, double log_moneyness, double T) {
    return static_cast<const SVIVolSurface*>(surf)->get_vol(log_moneyness, T);
}

// Select function pointer once
GetVolFunc get_vol_fn = get_vol_svi;
const void* surf_ptr = vol_surfaces_[prod].get();

// Hot loop - direct function call
for (uint16_t bi = 0; bi < batch_n; ++bi) {
    sigma_arr[bi] = get_vol_fn(surf_ptr, log_moneyness, T_arr[bi]);
}
```

**Trade-offs**:
- Simpler than templates
- Small overhead (~2-5ns per call)
- Not type-safe (void* cast)

### Approach 3: Manual Inlining

**Implementation**:
```cpp
// Manually inline get_vol() for each surface type
switch (cfg_.pricing.vol_method) {
case VolMethod::SVI: {
    SVIVolSurface* svi_surf = vol_surfaces_[prod].get();
    for (uint16_t bi = 0; bi < batch_n; ++bi) {
        // Inline SVI get_vol() logic here
        sigma_arr[bi] = /* SVI calculation */;
    }
    break;
}
case VolMethod::Wing: {
    WingVolSurface* wing_surf = wing_surfaces_[prod].get();
    for (uint16_t bi = 0; bi < batch_n; ++bi) {
        // Inline Wing get_vol() logic here
        sigma_arr[bi] = /* Wing calculation */;
    }
    break;
}
}
```

**Trade-offs**:
- Maximum performance (fully inlined)
- High code duplication
- Hard to maintain
- Error-prone

## Performance Impact

### Expected Improvements

**Virtual Dispatch Elimination**:
- **Before**: 5-20ns per call × 160 options = 800-3200ns per batch
- **After**: 0ns (inlined)
- **Improvement**: 800-3200ns per batch

**Instruction Cache**:
- **Before**: Virtual table lookups pollute I-cache
- **After**: Direct calls, better I-cache utilization
- **Improvement**: 10-20% fewer I-cache misses

**Branch Prediction**:
- **Before**: Indirect branches harder to predict
- **After**: Direct branches, better prediction
- **Improvement**: 5-10% fewer branch mispredictions

**Total Expected**:
- **Per batch**: 800-3200ns reduction
- **Per iteration**: 2.5-10μs reduction
- **Overall**: 5-10% pricer throughput improvement

## Implementation Complexity

### Effort Estimate

**Template Approach**:
- Refactoring: 4-6 hours
- Testing: 2-3 hours
- Debugging: 1-2 hours
- **Total**: 7-11 hours

**Risk Level**: Medium
- Requires careful refactoring
- Must maintain correctness
- Extensive testing needed

### Testing Requirements

1. **Unit Tests**: Verify each vol surface type
2. **Integration Tests**: Verify pricer loop correctness
3. **Performance Tests**: Measure latency improvement
4. **Regression Tests**: Ensure no behavioral changes

## Decision

**Status**: Documented for future implementation

**Rationale**:
- Current system is already highly optimized
- 5-10% improvement is valuable but not critical
- Requires significant refactoring and testing
- Better to implement when:
  - More time available for thorough testing
  - Performance profiling shows this as bottleneck
  - Other higher-priority optimizations complete

**Priority**: Medium (implement after higher-priority items)

## Conclusion

Virtual dispatch in the pricer hot path costs ~800-3200ns per batch. Template-based dispatch can eliminate this overhead entirely, improving pricer throughput by 5-10%. This optimization is documented and ready for future implementation when time permits.

**Recommended approach**: Template-based dispatch for zero-overhead abstraction.

## References

- Findings document: Line 11-15
- Current code: trading_engine.cpp:1568-1611
- Virtual dispatch cost: ~5-20ns per call
- Expected improvement: 5-10% pricer throughput

