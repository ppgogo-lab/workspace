# Plan: Remove CTP Gateway and Order/Quote Recovery System

## Context

The trading system currently implements:
1. **CTP Gateway** - Doesn't support native quotes, sends quotes as separate bid/ask orders
2. **Recovery System** - Persists live orders/quotes to SQLite and restores them on restart

**Why remove CTP gateway:** User confirmed CTP is not used as a gateway choice since it doesn't support native quotes. Removing it simplifies the codebase.

**Why remove recovery system:** User wants to simplify the system. If the system crashes with orders/quotes in the market, traders will manually cancel them using external tools (the "future counter system") rather than relying on automatic recovery.

**Benefits:**
- Simpler codebase, less complexity
- Faster startup (no recovery loading/cancellation)
- Reduced memory overhead (no recovery handles in every order/quote)
- Less persistence overhead (no recovery handle storage)
- No CTP-specific quote-to-order-leg complexity

**Trade-offs:**
- Manual cleanup required after crashes
- No automatic position restoration (positions must be queried from exchange or manually entered)
- Risk of stale orders causing unexpected fills if not cleaned up promptly

**Remaining gateways:** FEMAS (production) and Sim (testing)

## Critical Files to Modify

### CTP Gateway Removal
- `src/gateway/ctp_gateway.cpp` - Remove entire file
- `include/gateway/ctp_gateway.h` - Remove entire file
- `src/gateway/gateway_factory.cpp` - Remove CTP gateway creation logic
- `include/common/config.h` - Remove CTP from GatewayType enum (or mark deprecated)

### Core Structures
- `include/gateway/gateway.h` - Remove recovery handle structures and gateway recovery methods
- `include/engine/trading_engine.h` - Remove recovery fields from LiveOrderState/LiveQuoteState

### Engine
- `src/engine/trading_engine.cpp` - Remove recovery startup sequence and helper methods

### Gateways (FEMAS and Sim only)
- `src/gateway/femas_gateway.cpp` - Remove recovery methods and handle population
- `src/gateway/sim_gateway.cpp` - Remove recovery methods and handle population
- `include/gateway/femas_gateway.h` - Remove recovery method declarations
- `include/gateway/sim_gateway.h` - Remove recovery method declarations

### Persistence
- `src/persistence/data_repository.cpp` - Remove recovery state loading and recovery handle persistence
- `include/persistence/data_repository.h` - Remove RecoveryState structure and related methods

### Risk
- `src/risk/post_trade_risk.cpp` - Remove or modify `restore_positions()` to work without recovery

## Implementation Plan

### Phase 1: Remove CTP Gateway

**Files to delete:**
- `src/gateway/ctp_gateway.cpp`
- `include/gateway/ctp_gateway.h`

**File: `include/common/config.h`**
1. Remove `CTP` from `GatewayType` enum (or mark as deprecated/unused)

**File: `src/gateway/gateway_factory.cpp` (or wherever gateways are instantiated)**
1. Remove CTP gateway creation case from factory
2. Remove CTP-specific includes

**File: Build system (CMakeLists.txt or similar)**
1. Remove ctp_gateway.cpp from build sources
2. Remove CTP SDK linking if present

### Phase 2: Remove Recovery Structures from Gateway Interface

**File: `include/gateway/gateway.h`**

1. Remove structures (lines 39-59):
   - `GatewayOrderRecoveryHandle`
   - `GatewayQuoteRecoveryHandle`

2. Remove structures (lines 77-85):
   - `GatewayRecoveredOrder`
   - `GatewayRecoveredQuote`

3. Remove recovery handle fields from `GatewayEvent` (lines 70-72):
   - `order_recovery`
   - `quote_recovery`
   - Update constructor accordingly

4. Remove recovery handle parameters from `IGateway` interface:
   - `send_order()` - remove `GatewayOrderRecoveryHandle* recovery` parameter
   - `send_quote()` - remove `GatewayQuoteRecoveryHandle* recovery` parameter

5. Remove recovery methods from `IGateway` interface:
   - `restore_order_recovery()`
   - `restore_quote_recovery()`
   - `get_order_recovery_handle()`
   - `get_quote_recovery_handle()`

### Phase 3: Remove Recovery from LiveOrderState and LiveQuoteState

**File: `include/engine/trading_engine.h`**

1. Remove recovery field from `LiveOrderState` (line 227):
   ```cpp
   struct LiveOrderState {
       Order order{};
       // Remove: GatewayOrderRecoveryHandle recovery{};
   };
   ```

2. Remove recovery field from `LiveQuoteState` (line 232):
   ```cpp
   struct LiveQuoteState {
       Quote quote{};
       // Remove: GatewayQuoteRecoveryHandle recovery{};
       Volume remaining_bid{0};
       Volume remaining_ask{0};
   };
   ```

3. Remove recovery-related private methods:
   - `apply_recovery_state()`
   - `seed_gateway_recovery()`
   - `request_recovery_cancels()`
   - `rebuild_book_state_from_history()`
   - `update_live_order_recovery()`
   - `update_live_quote_recovery()`
   - `merge_order_recovery()`
   - `merge_quote_recovery()`

### Phase 4: Remove Recovery from Engine Implementation

**File: `src/engine/trading_engine.cpp`**

1. Remove recovery startup sequence (lines 668-689):
   - Remove `RecoveryState recovery{}` declaration
   - Remove `load_recovery_state()` call
   - Remove `apply_recovery_state()` call
   - Remove `seed_gateway_recovery()` call
   - Remove `request_recovery_cancels()` call
   - Keep `repository_->start()` call

2. Remove method implementations:
   - `apply_recovery_state()` (lines 720-774)
   - `seed_gateway_recovery()` (lines 776-785)
   - `request_recovery_cancels()` (lines 787-805)
   - `rebuild_book_state_from_history()` (if exists)
   - `update_live_order_recovery()` (lines 3101-3127 per agent report)
   - `update_live_quote_recovery()` (lines 3101-3127 per agent report)
   - `merge_order_recovery()` (lines 73-89 per agent report)
   - `merge_quote_recovery()` (lines 91-109 per agent report)

3. Update order/quote tracking code:
   - Remove recovery handle assignments when orders/quotes are sent
   - Remove recovery handle updates when acks arrive
   - Simplify `LiveOrderState` and `LiveQuoteState` initialization

4. **Critical: Fix quote leg tracking**
   - FEMAS gateway generates synthetic bid/ask order IDs for quotes (even though it supports native quotes)
   - Currently `quote_leg_to_quote_` mapping uses `recovery.bid_order_id` and `recovery.ask_order_id`
   - Need alternative approach: Add `parent_quote_id` field to Order structure or use different tracking mechanism
   - This is essential for FEMAS gateway to track which orders belong to which quotes

### Phase 5: Remove Recovery from Gateway Implementations

**Files: `src/gateway/femas_gateway.cpp`, `include/gateway/femas_gateway.h`**

1. Remove method implementations (lines 756-843 per agent report):
   - `restore_order_recovery()`
   - `restore_quote_recovery()`
   - `get_order_recovery_handle()` (lines 697-720)
   - `get_quote_recovery_handle()` (lines 721-754)

2. Update `send_order()` (lines 444-517):
   - Remove recovery handle parameter
   - Remove recovery handle population

3. Update `send_quote()` (lines 519-625):
   - Remove recovery handle parameter
   - Remove recovery handle population
   - Keep bid/ask order ID generation (lines 615-616) but store differently (see Phase 8)

**Files: `src/gateway/sim_gateway.cpp`, `include/gateway/sim_gateway.h`**

1. Remove method implementations (lines 236-265 per agent report):
   - `restore_order_recovery()`
   - `restore_quote_recovery()`
   - `get_order_recovery_handle()`
   - `get_quote_recovery_handle()`

2. Update `send_order()` (lines 45-87):
   - Remove recovery handle parameter
   - Remove recovery handle population

3. Update `send_quote()` (lines 89-150):
   - Remove recovery handle parameter
   - Remove recovery handle population

### Phase 6: Remove Recovery from Persistence Layer

**File: `include/persistence/data_repository.h`**

1. Remove `RecoveryState` structure (lines 153-173 per agent report)

2. Remove recovery-related methods:
   - `load_recovery_state()`
   - Any methods that persist recovery handles

**File: `src/persistence/data_repository.cpp`**

1. Remove `load_recovery_state()` implementation (line 617 per agent report)

2. Remove recovery handle persistence from order/quote storage:
   - Remove recovery handle fields from SQL INSERT/UPDATE statements (lines 1564-1665 for orders, 1687-1769 for quotes per agent report)
   - Remove recovery handle columns from database schema (or mark as deprecated)

3. Keep position persistence (positions can still be persisted, just not via recovery mechanism)

### Phase 7: Update Risk Management

**File: `src/risk/post_trade_risk.cpp`**

1. Modify `restore_positions()` (line 16 per agent report):
   - Change to accept positions directly rather than from RecoveryState
   - Or remove if positions will be queried from exchange on startup
   - Or keep as-is if positions are persisted separately from recovery

### Phase 8: Alternative Quote Leg Tracking (Critical for FEMAS)

**Problem:** FEMAS gateway generates synthetic bid/ask order IDs for quotes (lines 615-616 in femas_gateway.cpp). The engine needs to track which orders belong to which quote for proper state management and fill handling.

**Current approach:** Uses `recovery.bid_order_id` and `recovery.ask_order_id` from recovery handles.

**New approach - Return bid/ask order IDs from send_quote():**

**Option A: Add output parameter to send_quote()**
- Change `IGateway::send_quote()` signature to include `OrderId* bid_order_id_out, OrderId* ask_order_id_out`
- FEMAS populates these with the synthetic order IDs it generates
- Engine uses these to populate `quote_leg_to_quote_` mapping
- Cleaner than recovery handles, explicit return values

**Option B: Add structure for quote leg IDs**
```cpp
struct QuoteLegIds {
    OrderId bid_order_id{0};
    OrderId ask_order_id{0};
};
```
- Change `send_quote()` to return `bool` and take `QuoteLegIds* leg_ids_out` parameter
- Similar to Option A but more structured

**Recommendation: Option A** - Simpler, no new structures needed, explicit output parameters.

### Phase 9: Configuration Cleanup (Optional)

**File: `include/common/config.h`**

1. Consider removing or deprecating recovery-related config:
   - `PersistenceConfig` fields related to recovery (lines 271-278 per agent report)
   - Or keep persistence config for other data (positions, parameters)

## Quote Leg Tracking Solution (Detailed)

FEMAS gateway generates synthetic bid/ask order IDs for quotes even though it supports native quotes. We need to maintain the `quote_leg_to_quote_` mapping without recovery handles.

**Implementation:**

1. Modify `include/gateway/gateway.h`:
   ```cpp
   virtual bool send_quote(
       const Quote& quote,
       OrderId* bid_order_id_out = nullptr,
       OrderId* ask_order_id_out = nullptr) noexcept = 0;
   ```

2. In FEMAS gateway `send_quote()` (lines 615-616):
   ```cpp
   OrderId bid_order_id = quote.client_quote_id;
   OrderId ask_order_id = quote.client_quote_id | (1ULL << 47);
   
   if (bid_order_id_out != nullptr) *bid_order_id_out = bid_order_id;
   if (ask_order_id_out != nullptr) *ask_order_id_out = ask_order_id;
   ```

3. In Sim gateway `send_quote()`:
   - Can leave output parameters null (doesn't need quote leg tracking)
   - Or generate synthetic IDs similar to FEMAS

4. In engine when sending quotes:
   ```cpp
   OrderId bid_order_id = 0, ask_order_id = 0;
   if (gateway_->send_quote(quote, &bid_order_id, &ask_order_id)) {
       if (bid_order_id != 0) {
           quote_leg_to_quote_.insert(bid_order_id, quote.client_quote_id);
       }
       if (ask_order_id != 0) {
           quote_leg_to_quote_.insert(ask_order_id, quote.client_quote_id);
       }
   }
   ```

5. In engine when processing quote acks:
   - Use the stored `quote_leg_to_quote_` mapping
   - No changes needed to lookup logic

## Verification Plan

After implementation, verify:

1. **Build succeeds** - No compilation errors

2. **Unit tests pass** - Run existing test suite

3. **CTP gateway removed**:
   - Verify CTP files are deleted
   - Verify no CTP references in build system
   - Verify GatewayType enum updated

4. **Startup works without recovery**:
   - Start system fresh (no existing orders)
   - Verify no recovery-related errors in logs
   - Verify system starts faster than before

4. **Order/quote sending works**:
   - Send test orders and quotes
   - Verify they reach the gateway
   - Verify no recovery handle errors

5. **Quote leg tracking works (FEMAS)**:
   - Send quotes via FEMAS gateway
   - Verify bid/ask order IDs are returned and tracked correctly
   - Cancel quotes and verify proper cleanup
   - Verify fills on quote legs are properly attributed to parent quote

6. **Crash recovery behavior**:
   - Start system, send orders/quotes
   - Kill system (simulate crash)
   - Restart system
   - Verify: No automatic cancellation
   - Verify: System starts cleanly without recovery errors
   - Manually verify orders remain in market (expected behavior)

7. **Position tracking**:
   - Verify positions can still be tracked during runtime
   - Verify position persistence still works (if kept)

8. **Performance**:
   - Measure startup time (should be faster)
   - Measure order send latency (should be same or slightly faster due to less overhead)

## Risks and Mitigations

**Risk 1: Stale orders after crash**
- **Mitigation:** Document that traders must use external tools to cancel orders after system crashes
- **Mitigation:** Consider adding a "cancel all" button to the GUI for emergency cleanup

**Risk 2: Position tracking breaks**
- **Mitigation:** Ensure positions are still tracked during runtime via trade fills
- **Mitigation:** Consider adding position query from exchange on startup

**Risk 3: Quote leg tracking breaks (FEMAS)**
- **Mitigation:** Implement output parameters in send_quote() as described above
- **Mitigation:** Add tests specifically for FEMAS quote leg tracking and cancellation

**Risk 4: Database schema changes**
- **Mitigation:** Don't drop recovery columns immediately - mark as deprecated
- **Mitigation:** Add migration script if needed

## Rollback Plan

If issues arise:
1. Revert commits in reverse order
2. Recovery system is self-contained, so rollback should be clean
3. Database schema changes may need migration script to restore recovery columns

## Estimated Complexity

- **Phase 1:** Low - CTP gateway removal, straightforward file deletion
- **Phase 2-3:** Low - Structural changes, straightforward
- **Phase 4:** Medium - Engine changes require careful testing
- **Phase 5:** Medium - Two gateways to update consistently (FEMAS and Sim)
- **Phase 6:** Low - Persistence layer cleanup
- **Phase 7:** Low - Risk management update
- **Phase 8:** Medium - Quote leg tracking requires new design
- **Phase 9:** Low - Configuration cleanup

**Total:** Medium complexity, ~5-7 hours of implementation + testing
