# Optimization 1: Remove Gateway Recovery Lookups from Send Path

## Problem Analysis

**Current State:**
- Gateway dispatcher calls `send_order()`/`send_quote()` with recovery pointer
- Most gateways (FEMAS, CTP, Sim) already populate recovery handles during send
- However, FEMAS still locks `state_mutex_` during send (line 349 in femas_gateway.cpp)
- Callback path calls `get_*_recovery_handle()` to refresh with exchange sys_id (correct behavior)

**Bottleneck:**
The main issue is **mutex contention** in FEMAS gateway:
- `send_order`/`send_quote`: locks `state_mutex_` to allocate/index state
- Callback thread: locks same mutex to lookup state by exchange IDs
- This creates contention between hot send path and callback path

## Solution

### Phase 1: Lock-Free State Allocation (Immediate Win)
Replace mutex-protected state arrays with lock-free allocation:

1. **Pre-allocate state slots** at startup (already done - fixed arrays)
2. **Use atomic slot allocation** instead of linear scan under lock
3. **Defer indexing** to callback path (when we have exchange IDs)

### Phase 2: Flat Hash Tables (Already Partially Done)
FEMAS already uses `FixedHashTable` for indexing:
- `order_client_index_`: client_order_id → state index
- `order_local_index_`: exchange_local_id → state index  
- `order_sys_index_`: order_sys_id → state index

These are O(1) lookups, which is good! But they're still protected by `state_mutex_`.

### Phase 3: Split Read/Write Locks
- Send path: only needs to **write** new state
- Callback path: needs to **read** existing state
- Use separate locks or lock-free structures

## Implementation Plan

### Step 1: Add Atomic Slot Allocation to FEMAS
```cpp
// In FEMASGateway class:
std::atomic<uint32_t> next_order_slot_{0};
std::atomic<uint32_t> next_quote_slot_{0};

OrderState* alloc_order_state_lockfree() noexcept {
    for (uint32_t attempt = 0; attempt < MAX_OPEN_ORDERS; ++attempt) {
        uint32_t slot = next_order_slot_.fetch_add(1, std::memory_order_relaxed) % MAX_OPEN_ORDERS;
        OrderState* state = &order_states_[slot];
        bool expected = false;
        if (state->used.compare_exchange_strong(expected, true, std::memory_order_acquire)) {
            return state;
        }
    }
    return nullptr;  // all slots full
}
```

### Step 2: Defer Indexing to Callback Path
- Send path: allocate state, populate fields, return recovery handle
- Callback path: index state when exchange confirms (has sys_id)

### Step 3: Measure Impact
Run `test_latency` before/after to quantify improvement.

## Expected Impact

- **Latency reduction**: 200-800ns per order/quote (eliminates mutex contention)
- **Throughput increase**: Send path no longer blocks on callback processing
- **Scalability**: Multiple strategy threads can send concurrently

## Files to Modify

1. `include/gateway/femas_gateway.h` - add atomic slot counters
2. `src/gateway/femas_gateway.cpp` - implement lock-free allocation
3. `src/gateway/ctp_gateway.cpp` - similar changes (if using state tracking)
4. `src/engine/trading_engine.cpp` - verify recovery handles used correctly

## Testing

1. Unit tests: verify state allocation under concurrent load
2. Latency tests: measure tick-to-trade improvement
3. Stress tests: verify no state corruption under high throughput
