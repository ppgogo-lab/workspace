 Strategy Abstraction and Lifecycle Management Refactoring                                                                                                                                                                                                                                                                                                                           t
 Context

 The current strategy architecture tightly couples quote/order lifecycle management with strategy-specific logic. OptionMMCoreStrategy (915 lines) contains both "what to quote" (pricing decisions) and "how to manage quotes" (lifecycle state machines). Every new strategy must reimplement lifecycle management,
 leading to code duplication and inconsistency.

 Problems:
 1. Quote lifecycle management is embedded in OptionMMCoreStrategy
 2. No order lifecycle controller exists (manual tracking only)
 3. SimpleMMStrategy doesn't use QuoteLifecycleController (inconsistent behavior)
 4. New strategies must reimplement lifecycle management from scratch
 5. No clear separation between decision-making and execution

 Goal:
 Create a base strategy class that handles quote/order lifecycle management, allowing quants to focus on implementing pricing logic through a simple interface.

 Design Overview

 Class Hierarchy

 IMarketMaker (existing interface)
     ↓
 BaseQuotingStrategy (new abstract base class)
     ↓                           ↓
 OptionMMCoreStrategy    SimpleMMStrategy (refactored)

 Key Components

 1. OrderLifecycleController - New static utility for order state management
 2. BaseQuotingStrategy - New base class handling lifecycle management
 3. Refactored OptionMMCoreStrategy - Extends BaseQuotingStrategy
 4. Refactored SimpleMMStrategy - Extends BaseQuotingStrategy

 Implementation Plan

 Phase 1: Create OrderLifecycleController

 File: include/strategy/order_lifecycle.h

 Create a static utility class similar to QuoteLifecycleController for managing order lifecycle:

 enum class OrderLifecycleState : uint8_t {
     Idle = 0,
     Pending,      // Submitted, awaiting ack
     Live,         // Acknowledged by exchange
     CancelPending,
     Filled,
     Cancelled,
     Rejected,
 };

 struct OrderLifecycleTracker {
     OrderLifecycleState status{OrderLifecycleState::Idle};
     OrderId order_id{0};
     uint16_t instrument_id{INVALID_INSTRUMENT_ID};
     Side side{Side::Buy};
     Volume original_volume{0};
     Volume filled_volume{0};
     Volume remaining_volume{0};
     double price{0.0};
     Timestamp submit_ts{0};
     Timestamp ack_ts{0};
     bool is_hedge{false};
 };

 class OrderLifecycleController {
 public:
     static void note_order_submitted(OrderLifecycleTracker& tracker,
                                      const Order& order,
                                      int64_t now_ns) noexcept;

     static void on_order_ack(OrderLifecycleTracker& tracker,
                             const Order& order,
                             int64_t now_ns) noexcept;

     static void on_fill(OrderLifecycleTracker& tracker,
                        Volume fill_volume) noexcept;

     static void on_cancel(OrderLifecycleTracker& tracker) noexcept;

     static void on_reject(OrderLifecycleTracker& tracker) noexcept;

     [[nodiscard]] static bool is_live(const OrderLifecycleTracker& tracker) noexcept;
     [[nodiscard]] static bool is_terminal(const OrderLifecycleTracker& tracker) noexcept;
 };

 Implementation details:
 - Mirror QuoteLifecycleController patterns for consistency
 - All methods are static and noexcept
 - State transitions: Idle → Pending → Live → Filled/Cancelled/Rejected
 - Track filled/remaining volume for partial fills

 Phase 2: Create BaseQuotingStrategy

 Files:
 - include/strategy/base_quoting_strategy.h
 - src/strategy/base_quoting_strategy.cpp

 Create an abstract base class that handles lifecycle management:

 class BaseQuotingStrategy : public IMarketMaker {
 protected:
     // Lifecycle management helpers for subclasses
     void request_quote(uint16_t instrument_id,
                       double bid, double ask,
                       Volume bid_vol, Volume ask_vol,
                       int64_t now_ns) noexcept;

     void request_cancel(uint16_t instrument_id, int64_t now_ns) noexcept;

     OrderId submit_order(const Order& order) noexcept;

     void cancel_order(OrderId id) noexcept;

     // Access to lifecycle state
     [[nodiscard]] const QuoteLifecycleState* get_quote_state(uint16_t instrument_id) const noexcept;
     [[nodiscard]] const OrderLifecycleTracker* get_order_tracker(OrderId id) const noexcept;

     // Subclass hooks (pure virtual)
     virtual void on_signal_impl(const PricingSignal& signal) noexcept = 0;
     virtual void on_fill_impl(const Trade& trade) noexcept = 0;
     virtual void on_timer_impl(const TimerEvent& event) noexcept = 0;

     // IMarketMaker implementation (final - subclasses cannot override)
     void on_signal(const PricingSignal& signal) noexcept final;
     void on_fill(const Trade& trade) noexcept final;
     void on_quote_ack(const Quote& quote) noexcept final;
     void on_quote_cancel(const Quote& quote) noexcept final;
     void on_quote_reject(const Quote& quote) noexcept final;
     void on_order_ack(const Order& order) noexcept final;
     void on_order_cancel(OrderId id) noexcept final;
     void on_order_reject(const Order& order) noexcept final;
     void on_timer(const TimerEvent& event) noexcept final;

     // Per-instrument state
     struct InstrumentState {
         bool active{false};
         uint16_t instrument_id{INVALID_INSTRUMENT_ID};
         QuoteLifecycleState quote_lifecycle{};
         int32_t net_position{0};
     };

     InstrumentState instrument_state_[MAX_INSTRUMENTS]{};

     // Order tracking
     static constexpr size_t MAX_TRACKED_ORDERS = 256;
     OrderLifecycleTracker order_trackers_[MAX_TRACKED_ORDERS]{};
     uint16_t order_tracker_count_{0};

     // Configuration
     QuoteLifecycleConfig quote_config_{
         3'000'000'000LL,  // quote_max_live_ns (3s)
         1'000'000'000LL,  // cancel_retry_ns (1s)
         3                 // max_cancel_attempts
     };

 private:
     // Internal lifecycle management
     bool manage_quote_lifecycle(uint16_t instrument_id, int64_t now_ns) noexcept;
     void send_quote_internal(uint16_t instrument_id,
                             double bid, double ask,
                             Volume bid_vol, Volume ask_vol,
                             int64_t now_ns) noexcept;
     void send_cancel_internal(uint16_t instrument_id, int64_t now_ns) noexcept;
     OrderLifecycleTracker* find_order_tracker(OrderId id) noexcept;
     OrderLifecycleTracker* allocate_order_tracker() noexcept;
 };

 What moves from OptionMMCoreStrategy:
 - send_quote() → send_quote_internal() (private)
 - send_cancel() → send_cancel_internal() (private)
 - manage_quote_lifecycle() → stays but becomes private
 - All quote lifecycle event handlers (on_quote_ack, on_quote_cancel, on_quote_reject)
 - Basic order event handlers (on_order_ack, on_order_cancel, on_order_reject)
 - Per-instrument QuoteLifecycleState storage

 What stays in OptionMMCoreStrategy:
 - build_decision() - strategy-specific pricing logic
 - Product-level exposure tracking (product_net_delta_, product_net_vega_)
 - Hedging logic (maybe_trigger_hedge)
 - Product regime management
 - Underlying shock detection
 - Spread widening logic
 - Inventory skew calculations

 Phase 3: Refactor OptionMMCoreStrategy

 Files:
 - include/strategy/option_mm_core.h
 - src/strategy/option_mm_core.cpp

 Changes:

 1. Change inheritance:
 class OptionMMCoreStrategy : public BaseQuotingStrategy {

 2. Update method signatures:
 protected:
     void on_signal_impl(const PricingSignal& signal) noexcept override;
     void on_fill_impl(const Trade& trade) noexcept override;
     void on_timer_impl(const TimerEvent& event) noexcept override;

 3. Replace direct quote/order submission:
 // Old:
 send_quote(state, decision, now_ns);

 // New:
 request_quote(state.instrument_id,
              decision.bid, decision.ask,
              decision.bid_vol, decision.ask_vol,
              now_ns);

 4. Update OptionState struct:
 struct OptionState {
     bool active{false};
     uint16_t instrument_id{INVALID_INSTRUMENT_ID};
     uint16_t underlying_id{INVALID_INSTRUMENT_ID};
     int32_t net_position{0};
     double last_theo_bid{0.0};
     double last_theo_ask{0.0};
     double last_delta{0.0};
     double last_vega{0.0};
     double last_underlying_px{0.0};
     int64_t last_signal_ts{0};
     // REMOVED: QuoteLifecycleState quote_lifecycle{};  // Now in base class
     uint32_t suppress_flags{SuppressNone};
 };

 5. Remove quote lifecycle event handlers (base class handles them)
 6. Keep all product-level logic intact:
   - build_decision()
   - maybe_trigger_hedge()
   - update_product_exposure()
   - capture_product_regime()
   - handle_product_regime_transition()

 Phase 4: Refactor SimpleMMStrategy

 Files:
 - include/strategy/simple_mm.h
 - src/strategy/simple_mm.cpp

 Changes:

 1. Change inheritance:
 class SimpleMMStrategy : public BaseQuotingStrategy {

 2. Update method signatures:
 protected:
     void on_signal_impl(const PricingSignal& signal) noexcept override;
     void on_fill_impl(const Trade& trade) noexcept override;
     void on_timer_impl(const TimerEvent& event) noexcept override;

 3. Remove manual quote tracking:
 // REMOVE:
 struct LastQuote {
     double   bid{0.0}, ask{0.0};
     QuoteId  id{0};
     bool     live{false};
 };
 LastQuote last_quote_[MAX_INSTRUMENTS]{};

 4. Replace send_quote() with request_quote():
 // Old:
 send_quote(signal, bid, ask, bid_vol, ask_vol);

 // New:
 request_quote(id, bid, ask, bid_vol, ask_vol, get_monotonic_ns());

 5. Remove quote event handlers (base class handles them)
 6. Simplify on_timer(SessionClose):
 // Old: Manual cancel loop
 for (uint16_t i = 0; i < MAX_INSTRUMENTS; ++i) {
     if (!last_quote_[i].live) continue;
     Quote cancel{};
     // ... manual cancel construction
 }

 // New: Use base class helper
 for (uint16_t i = 0; i < MAX_INSTRUMENTS; ++i) {
     request_cancel(i, event.trigger_ts_ns);
 }

 Benefits:
 - Reduces from 177 lines to ~120 lines
 - Automatic quote lifecycle management
 - Consistent behavior with OptionMMCoreStrategy
 - No manual quote state tracking needed

 Phase 5: Create Example Template

 File: include/strategy/template_mm_strategy.h

 Create a minimal template showing how quants can extend BaseQuotingStrategy:

 // Example template for creating new market making strategies
 class TemplateMMStrategy : public BaseQuotingStrategy {
 public:
     void init(uint8_t product_idx,
               SPSCRingBuffer<Quote, 512>* quote_buf,
               SPSCRingBuffer<Order, 512>* order_buf,
               PreTradeRisk* pre_risk,
               AtomicMMParams* params,
               const Instrument* instruments) noexcept {
         product_idx_ = product_idx;
         quote_buf_ = quote_buf;
         order_buf_ = order_buf;
         pre_risk_ = pre_risk;
         params_ = params;
         instruments_ = instruments;

         // Initialize strategy-specific state here
     }

 protected:
     void on_signal_impl(const PricingSignal& signal) noexcept override {
         // 1. Check if strategy is enabled
         if (!params_ || !params_->enabled.load(std::memory_order_relaxed)) return;

         // 2. Update internal state from signal (theo, greeks, etc.)

         // 3. Build quote decision (bid/ask/size)
         double bid = /* your pricing logic */;
         double ask = /* your pricing logic */;
         Volume bid_vol = /* your sizing logic */;
         Volume ask_vol = /* your sizing logic */;

         // 4. Submit quote (base class handles lifecycle)
         request_quote(signal.instrument_id, bid, ask, bid_vol, ask_vol, get_monotonic_ns());
     }

     void on_fill_impl(const Trade& trade) noexcept override {
         // Update positions and risk metrics
         // Base class already updated quote lifecycle state
     }

     void on_timer_impl(const TimerEvent& event) noexcept override {
         // Handle periodic tasks (hedging, refresh, session close, etc.)
         switch (event.type) {
         case TimerEventType::HedgeCheck:
             // Trigger hedge orders if needed
             break;
         case TimerEventType::SessionClose:
             // Cancel all live quotes
             for (uint16_t i = 0; i < MAX_INSTRUMENTS; ++i) {
                 request_cancel(i, event.trigger_ts_ns);
             }
             break;
         default:
             break;
         }
     }

 private:
     // Strategy-specific state
     int32_t net_position_[MAX_INSTRUMENTS]{};
     double portfolio_delta_{0.0};
 };

 Critical Files

 New files:
 - include/strategy/order_lifecycle.h - OrderLifecycleController
 - include/strategy/base_quoting_strategy.h - BaseQuotingStrategy header
 - src/strategy/base_quoting_strategy.cpp - BaseQuotingStrategy implementation
 - include/strategy/template_mm_strategy.h - Example template

 Modified files:
 - include/strategy/option_mm_core.h - Change inheritance, update methods
 - src/strategy/option_mm_core.cpp - Refactor to use base class
 - include/strategy/simple_mm.h - Change inheritance, remove manual tracking
 - src/strategy/simple_mm.cpp - Refactor to use base class

 Unchanged files:
 - include/strategy/mm_framework.h - IMarketMaker interface
 - include/strategy/quote_lifecycle.h - QuoteLifecycleController

 Performance Considerations

 Zero-overhead design:
 - All lifecycle management uses static methods (no virtual calls in hot path)
 - Quote/order state stored in fixed-size arrays (no dynamic allocation)
 - Virtual calls only at event boundaries (already present in IMarketMaker)
 - Inline small helpers in headers
 - Same number of virtual calls as current design

 Memory layout:
 - Keep cache-line alignment for hot structures
 - Separate hot (per-tick) from cold (per-event) data
 - Use same atomic patterns for monitor state

 Latency impact:
 - No additional function call overhead (inline helpers)
 - Lifecycle state machines unchanged (proven fast)
 - Target: maintain <2μs tick-to-trade latency

 Verification

 Unit tests:
 - OrderLifecycleController state transitions
 - BaseQuotingStrategy quote request/cancel flow
 - Quote lifecycle integration with base class

 Integration tests:
 - Refactored OptionMMCoreStrategy produces identical quotes
 - Refactored SimpleMMStrategy behavior unchanged
 - Performance benchmarks show no regression

 Validation checklist:
 - Run existing latency benchmarks
 - Compare quote sequences before/after refactor
 - Verify monitor state consistency
 - Test with SimGateway
 - Test with CTP gateway
 - Verify pre-trade risk integration
 - Check memory usage (no increase)
 - Profile hot path (no additional overhead)

 Migration Strategy

 Backward compatibility:
 - IMarketMaker interface unchanged
 - QuoteLifecycleController unchanged
 - OptionMMCoreStrategy::init() signature unchanged
 - Existing strategies continue to work

 Incremental rollout:
 1. Phase 1-2: Add new base class (no breaking changes)
 2. Phase 3-4: Refactor OptionMMCoreStrategy (internal only)
 3. Phase 5: Refactor SimpleMMStrategy (internal only)
 4. Phase 6: Document new pattern for future strategies

 Rollback plan:
 - Keep original implementations in git history
 - Can revert individual strategies independently
 - No changes to external interfaces

 Benefits

 For quants:
 - Focus on pricing logic, not lifecycle management
 - Consistent quote/order behavior across strategies
 - Reduced code duplication (50-60% less boilerplate)
 - Clear template to follow for new strategies

 For system:
 - Centralized lifecycle management (easier to debug)
 - Consistent error handling and retry logic
 - Better testability (lifecycle logic tested once)
 - Easier to add new features (e.g., order timeouts)

 For maintenance:
 - Single source of truth for lifecycle logic
 - Easier to understand strategy implementations
 - Reduced surface area for bugs
 - Better code organization
 
 ✅ Completed Components

  1. OrderLifecycleController (include/strategy/order_lifecycle.h)
    - Static utility class for managing order lifecycle state transitions
    - States: Idle → Pending → Live → Filled/Cancelled/Rejected
    - Tracks filled/remaining volume for partial fills
  2. BaseQuotingStrategy (include/strategy/base_quoting_strategy.h + src/strategy/base_quoting_strategy.cpp)
    - Abstract base class extending IMarketMaker
    - Handles quote lifecycle using QuoteLifecycleController
    - Handles order lifecycle using OrderLifecycleController
    - Provides protected helpers: request_quote(), request_cancel(), submit_order()
    - Implements final IMarketMaker callbacks that route through lifecycle controllers
    - Defines pure virtual hooks: on_signal_impl(), on_fill_impl(), on_timer_impl()
  3. TemplateMMStrategy (include/strategy/template_mm_strategy.h)
    - Example template showing how to extend BaseQuotingStrategy
    - ~100 lines of clean, focused code
    - Demonstrates pricing logic, sizing, hedging, and session management
  4. Refactored SimpleMMStrategy
    - Now extends BaseQuotingStrategy
    - Reduced from 177 lines to ~100 lines
    - Removed manual quote tracking (LastQuote struct)
    - Removed all quote lifecycle event handlers
    - Uses request_quote() and request_cancel() from base class
  5. Refactored OptionMMCoreStrategy
    - Now extends BaseQuotingStrategy
    - Removed ~150 lines of lifecycle management code
    - Removed send_quote(), send_cancel(), manage_quote_lifecycle() methods
    - Removed quote lifecycle event handlers
    - Removed QuoteLifecycleState from OptionState struct
    - Uses request_quote() and request_cancel() from base class
    - Kept all product-level logic intact (exposure tracking, hedging, regime management)

  Key Benefits

  For Quants:
  - Focus on pricing logic, not lifecycle management
  - Consistent quote/order behavior across strategies
  - Reduced code duplication (50-60% less boilerplate)
  - Clear template to follow for new strategies

  For System:
  - Centralized lifecycle management (easier to debug)
  - Consistent error handling and retry logic
  - Better testability (lifecycle logic tested once)
  - Easier to add new features (e.g., order timeouts)

  For Maintenance:
  - Single source of truth for lifecycle logic
  - Easier to understand strategy implementations
  - Reduced surface area for bugs
  - Better code organization

  The refactoring maintains backward compatibility and zero performance overhead while significantly improving code organization and reusability.
 