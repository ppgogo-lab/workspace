# Optimization #2: Replace Mutex with Read-Write Lock

## Problem

Current bottleneck: `state_mutex_` serializes all operations:
- **Send path**: locks for indexing (insert into hash tables)
- **Callback path**: locks for lookups (find in hash tables) + state updates

Even though hash table operations are O(1), the mutex creates contention between:
- Multiple callback threads (if gateway uses multiple threads)
- Send thread vs callback thread
- Multiple send threads (from different strategy threads)

## Solution

Replace `std::mutex state_mutex_` with `std::shared_mutex state_rw_lock_`:
- **Readers** (lookups): use `std::shared_lock` - multiple readers can proceed concurrently
- **Writers** (indexing, state updates): use `std::unique_lock` - exclusive access

### Benefits

1. **Callback lookups don't block each other** - multiple `OnRtnOrder`/`OnRtnTrade` can run concurrently
2. **Lookups don't block sends** - as long as no indexing is happening
3. **Zero algorithmic changes** - just swap mutex type

### Expected Impact

- **Latency**: 50-200ns reduction in callback path (eliminates reader-reader contention)
- **Throughput**: 2-4x improvement in callback processing (parallel reads)
- **Scalability**: Scales with number of callback threads

## Implementation

### Changes Required

1. **Header file**: Change `std::mutex` to `std::shared_mutex`
2. **Lookup functions**: Use `std::shared_lock` (read-only)
3. **Indexing functions**: Use `std::unique_lock` (write)
4. **State update functions**: Use `std::unique_lock` (write)

### Code Changes

```cpp
// Before:
mutable std::mutex state_mutex_;
std::lock_guard<std::mutex> lock(state_mutex_);

// After:
mutable std::shared_mutex state_rw_lock_;
std::shared_lock<std::shared_mutex> lock(state_rw_lock_);  // for reads
std::unique_lock<std::shared_mutex> lock(state_rw_lock_);  // for writes
```

## Testing

- Verify no deadlocks under high load
- Measure callback latency improvement
- Stress test with concurrent sends + callbacks

## Rollback

If issues arise, simply revert to `std::mutex` - API is compatible.
