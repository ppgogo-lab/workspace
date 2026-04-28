   #─Abstraction─Patterns─Analysis─-─optionMM─Codebase───────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
                                                                                                                                                                                                                                                                                                                                ## Executive Summary                                 t

   The optionMM codebase demonstrates a sophisticated, performance-oriented architecture with clear separation of concerns. The abstraction patterns prioritize:
   1. **Zero-allocation hot paths** (noexcept, pre-allocated buffers)
   2. **Template-based specialization** for performance-critical paths
   3. **Interface-based composition** for pluggable components
   4. **Atomic snapshots** for lock-free thread communication
   5. **Ring buffers** for inter-thread communication

   ---

   ## 1. Interface Definition Patterns

   ### 1.1 Core Strategy Interfaces

   **IMarketMaker** (`include/strategy/mm_framework.h`)
   - **Purpose**: Abstract per-product market-making strategy
   - **Key characteristics**:
     - All methods are `noexcept` (SCHED_FIFO thread safety)
     - Event-driven: `on_signal()`, `on_fill()`, `on_order_ack()`, `on_quote_ack()`, `on_timer()`
     - Protected member variables for dependency injection: `quote_buf_`, `order_buf_`, `pre_risk_`, `params_`, `instruments_`
     - Monitoring methods: `read_product_monitor_state()`, `read_instrument_monitor_states()`, `read_runtime_stats()`
     - No virtual destructor needed (protected)

   **IArbitrageStrategy** (`include/strategy/arbitrage_strategy.h`)
   - **Purpose**: Abstract arbitrage strategy (e.g., PCP)
   - **Key characteristics**:
     - Similar event-driven pattern: `evaluate()`, `on_market_update()`, `on_timer()`, `on_order_ack()`, `on_fill()`
     - Ownership query: `owns_order_id()` (to route callbacks correctly)
     - Type identification: `strategy_type()` (returns `ArbitrageStrategyType`)
     - Monitoring: `read_monitor_state()`, `read_pcp_monitor_states()`
     - Protected members: `product_idx_`, `intent_buf_`, `params_`, `instruments_`, `tick_snapshot_`, `greeks_snapshot_`

   ### 1.2 Infrastructure Interfaces

   **IFeedHandler** (`include/feed/feed_handler.h`)
   - **Purpose**: Abstract market data ingestion (multicast, FPGA, simulator, etc.)
   - **Key characteristics**:
     - Constructor takes `SPSCRingBuffer<TopOfBookTick, 1024>*` (dependency injection)
     - Lifecycle: `start()`, `stop()`
     - Query methods: `is_connected()`, `message_count()`, `error_count()`, `dropped_count()`
     - Protected members: `tick_buf_`, `stop_flag_`, `connected_`, counters, `instruments_`, `instrument_lookup_`
     - Helper: `resolve_instrument()` for code → ID mapping

   **IGateway** (`include/gateway/gateway.h`)
   - **Purpose**: Abstract order routing (CTP, FEMAS, simulator)
   - **Key characteristics**:
     - Lifecycle: `connect()`, `disconnect()`, `is_connected()`
     - Order routing (noexcept): `send_order()`, `send_quote()`, `cancel_order()`, `cancel_quote()`
     - Startup query: `query_instruments()` (blocking, called once)
     - Public ring buffer: `callback_buf` (GatewayEvent)
     - Protected helpers: `instrument_by_id()`, `find_instrument_id()`

   **IVolSurface** (`include/pricing/vol_surface.h`)
   - **Purpose**: Abstract volatility surface (SVI, Wing, OrcWing, CubicSpline)
   - **Key characteristics**:
     - Minimal interface: `get_vol()`, `get_vol_by_strike()`, `is_valid()`
     - All methods `noexcept` (pricer hot path)
     - Managed by `VolSurfaceManager<Surface>` template for lock-free updates

   **INetworkTransport** (`include/common/network_transport.h`)
   - **Purpose**: Abstract network I/O backend (sockets, DPDK, OpenOnload)
   - **Key characteristics**:
     - Batch operations: `receive_batch()`, `release_packets()`
     - Zero-copy: `NetworkPacket` with metadata
     - Query: `backend()`, `is_ready()`, statistics

   **IFemasMdApi / IFemasTraderApi** (`include/femas/api_wrapper.h`)
   - **Purpose**: Adapter pattern for FEMAS C API
   - **Key characteristics**:
     - Thin wrapper around C structs (CUstpFtdcMduserSpi, CUstpFtdcTraderSpi)
     - Factory functions: `create_femas_md_api()`, `create_femas_trader_api()`
     - Enables testing with mock implementations

   ---

   ## 2. Template-Based Abstraction (CRTP-like)

   ### 2.1 TypedPricer Pattern

   **File**: `include/pricing/typed_pricer.h`

   ```cpp
   template <typename VolSurface>
   class TypedPricer {
       const VolSurface* surf_;
       void compute_batch_vols(...) const noexcept;
   };

   // Template specialization for SVIVolSurface
   template <>
   inline void TypedPricer<SVIVolSurface>::compute_batch_vols(...) { ... }

   // Template specialization for OrcWingVolSurface
   template <>
   inline void TypedPricer<OrcWingVolSurface>::compute_batch_vols_by_strike(...) { ... }
   ```

   **Benefits**:
   - Eliminates virtual dispatch (8-12 cycles/option speedup)
   - Compile-time specialization for different vol surface types
   - Zero runtime overhead for type-specific optimizations
   - Maintains interface contract via `IVolSurface` base class

   ### 2.2 VolSurfaceManager Pattern

   **File**: `include/pricing/vol_surface.h`

   ```cpp
   template<typename Surface>
   class VolSurfaceManager {
       alignas(64) Surface bufs_[2];  // Ping-pong buffers
       std::atomic<int> active_idx_;

       const IVolSurface* get() const noexcept;      // Pricer thread
       Surface* get_inactive() noexcept;              // Fitter thread
       void publish() noexcept;                       // Fitter thread
   };
   ```

   **Pattern**: Lock-free atomic snapshot with double-buffering
   - Pricer thread reads from active buffer (acquire semantics)
   - Fitter thread writes to inactive buffer, then swaps (release semantics)
   - Zero allocation, zero locks, zero contention

   ---

   ## 3. Code Reuse Patterns

   ### 3.1 Dependency Injection via Protected Members

   All strategy interfaces use protected member variables set by the engine:

   ```cpp
   class IMarketMaker {
   protected:
       SPSCRingBuffer<Quote, 512>* quote_buf_{nullptr};
       SPSCRingBuffer<Order, 512>* order_buf_{nullptr};
       PreTradeRisk* pre_risk_{nullptr};
       AtomicMMParams* params_{nullptr};
       const Instrument* instruments_{nullptr};
       uint8_t product_idx_{0};
       uint64_t order_seq_{0};
   };
   ```

   **Reuse mechanism**:
   - Engine calls `init()` on each strategy to wire dependencies
   - Concrete implementations (SimpleMMStrategy, OptionMMCoreStrategy) inherit these members
   - No virtual calls needed for dependency access
   - Enables testing by injecting mock buffers/params

   ### 3.2 Monitoring State Pattern

   All strategies implement monitoring accessors:

   ```cpp
   [[nodiscard]] bool read_product_monitor_state(ProductMonitorState* out) const noexcept;
   [[nodiscard]] int read_instrument_monitor_states(InstrumentMonitorState* out, int max_count) const noexcept;
   [[nodiscard]] bool read_runtime_stats(StrategyRuntimeStats* out) const noexcept;
   ```

   **Reuse**:
   - gRPC server calls these methods to populate monitoring dashboards
   - Default implementations return false/0 (opt-in monitoring)
   - No allocation, no locks (read-only snapshots)

   ### 3.3 Order ID Generation Pattern

   Strategies generate unique order IDs using product index + local sequence:

   ```cpp
   [[nodiscard]] OrderId next_order_id() noexcept {
       return (static_cast<uint64_t>(product_idx_) << 32) | (++order_seq_);
   }
   ```

   **Reuse**:
   - Arbitrage strategies use tagged order IDs:
     ```cpp
     OrderId make_arb_order_id(uint8_t product_idx, ArbitrageStrategyType type, uint64_t seq);
     bool is_arb_order_id(OrderId id);
     uint8_t arb_order_product(OrderId id);
     ArbitrageStrategyType arb_order_type(OrderId id);
     ```
   - Enables callback routing without hash tables

   ---

   ## 4. Separation of "What" from "How"

   ### 4.1 Strategy Intent vs. Execution

   **What**: Strategy decides to send an order
   ```cpp
   // Strategy calls (what to do)
   quote_buf_->push(quote);
   order_buf_->push(order);
   ```

   **How**: Gateway executes the order
   ```cpp
   // Gateway implements (how to do it)
   bool IGateway::send_order(const Order& order) noexcept;
   bool IGateway::send_quote(const Quote& quote) noexcept;
   ```

   ### 4.2 Risk Checking Abstraction

   **What**: Strategy wants to send an order
   **How**: PreTradeRisk checks it

   ```cpp
   // Strategy (what)
   PreTradeRisk::RejectReason reason = pre_risk_->check_order(order);
   if (reason != PreTradeRisk::RejectReason::OK) return;

   // PreTradeRisk (how)
   class PreTradeRisk {
       [[nodiscard]] RejectReason check_order(const Order& order) noexcept;
       [[nodiscard]] RejectReason check_quote(const Quote& quote) noexcept;
   };
   ```

   ### 4.3 Pricing Abstraction

   **What**: Strategy needs theoretical prices
   **How**: Pricer computes them

   ```cpp
   // Strategy receives (what)
   void on_signal(const PricingSignal& signal) noexcept;

   // Pricer computes (how)
   // - Reads market data from tick_snapshot_
   // - Reads Greeks from greeks_snapshot_
   // - Reads vol surface from VolSurfaceManager
   // - Computes Black-76 prices
   // - Pushes PricingSignal to strategy
   ```

   ### 4.4 Volatility Surface Abstraction

   **What**: Pricer needs implied volatility
   **How**: Vol surface provides it

   ```cpp
   // Pricer (what)
   double iv = vol_surface->get_vol(log_k, T);

   // Vol surface implementations (how)
   class SVIVolSurface : public IVolSurface { ... };
   class WingVolSurface : public IVolSurface { ... };
   class OrcWingVolSurface : public IVolSurface { ... };
   ```

   ---

   ## 5. Subsystem Abstraction Patterns

   ### 5.1 Feed Handler Pattern

   **Abstraction**: Market data source
   **Implementations**:
   - `SimFeedHandler` (simulator)
   - `FemasFeedHandler` (FEMAS API)
   - `FpgaFeedHandler` (FPGA hardware)
   - `MulticastFeedHandler` (UDP multicast)

   **Interface contract**:
   - Constructor takes `SPSCRingBuffer<TopOfBookTick, 1024>*`
   - `start()` spawns feed thread
   - `stop()` signals and joins thread
   - Writes `TopOfBookTick` to ring buffer
   - Provides statistics: `message_count()`, `error_count()`, `dropped_count()`

   ### 5.2 Gateway Pattern

   **Abstraction**: Order routing
   **Implementations**:
   - `SimGateway` (simulator)
   - `FemasGateway` (FEMAS API)

   **Interface contract**:
   - `connect()` / `disconnect()` lifecycle
   - `send_order()`, `send_quote()`, `cancel_order()` (noexcept)
   - `query_instruments()` (blocking, startup only)
   - Pushes `GatewayEvent` to `callback_buf` ring buffer
   - Dispatcher thread drains events and routes to strategies

   ### 5.3 Pricing Pipeline

   **Abstraction**: Compute Greeks and theoretical prices
   **Components**:
   - `IVolSurface` (volatility surface)
   - `TypedPricer<Surface>` (Black-76 pricer)
   - `VolSurfaceManager<Surface>` (lock-free updates)

   **Data flow**:
   1. Feed thread writes `TopOfBookTick` to `tick_snapshot_`
   2. Pricer thread reads tick, vol surface, computes Greeks
   3. Pricer writes `Greeks` to `greeks_snapshot_`
   4. Pricer writes `PricingSignal` to strategy ring buffer
   5. Strategy thread reads signal, makes quoting decision

   ### 5.4 Risk Management

   **Abstraction**: Hard limits enforcement
   **Components**:
   - `PreTradeRisk` (per-strategy, no locks)
   - `PostTradeRisk` (global, shared)

   **Separation**:
   - **PreTradeRisk**: Synchronous checks on critical path
     - Self-trade detection
     - Max volume limits
     - Max open orders
   - **PostTradeRisk**: Asynchronous monitoring
     - Portfolio delta/vega exposure
     - Breach alerts

   ---

   ## 6. Key Design Principles

   ### 6.1 Performance-First Design

   1. **Zero allocation on hot path**
      - All buffers pre-allocated
      - Ring buffers for inter-thread communication
      - No dynamic memory in `on_signal()`, `on_fill()`, etc.

   2. **Lock-free communication**
      - SPSC ring buffers (single producer, single consumer)
      - Atomic snapshots (VolSurfaceManager)
      - No mutexes on critical path

   3. **Cache-line alignment**
      - `alignas(64)` on hot-path structs
      - Prevents false sharing
      - Improves cache locality

   4. **Template specialization**
      - TypedPricer eliminates virtual dispatch
      - Compile-time optimization
      - Zero runtime overhead

   ### 6.2 Thread Safety Model

   1. **Per-thread ownership**
      - Each strategy thread owns its PreTradeRisk instance
      - No shared state, no locks needed
      - Feed thread owns tick_buf, pricer thread reads it

   2. **Atomic snapshots**
      - VolSurfaceManager uses atomic<int> for active buffer index
      - Acquire/release semantics for visibility
      - No locks, no contention

   3. **Ring buffers**
      - SPSC: single producer, single consumer
      - No locks needed
      - Bounded capacity prevents unbounded growth

   ### 6.3 Extensibility

   1. **Interface-based composition**
      - IMarketMaker, IArbitrageStrategy, IFeedHandler, IGateway, IVolSurface
      - New implementations can be added without modifying existing code
      - Factory functions for creation

   2. **Configuration-driven selection**
      - Strategy type selected from config: `"option_mm_core"` vs `"simple_mm"`
      - Feed handler selected from config: `"sim"`, `"femas"`, `"fpga"`, `"multicast"`
      - Gateway selected from config: `"sim"`, `"femas"`

   3. **Monitoring hooks**
      - All strategies implement monitoring accessors
      - gRPC server queries these without modifying strategy code
      - Opt-in: default implementations return false/0

   ---

   ## 7. Concrete Implementation Examples

   ### 7.1 SimpleMMStrategy

   **File**: `include/strategy/simple_mm.h`

   ```cpp
   class SimpleMMStrategy : public IMarketMaker {
   public:
       void init(...) noexcept { /* wire dependencies */ }
       void on_signal(const PricingSignal& signal) noexcept override;
       void on_fill(const Trade& trade) noexcept override;
       // ... other event handlers

   private:
       int32_t net_position_[MAX_INSTRUMENTS]{};
       double portfolio_delta_{0.0};
       struct LastQuote { double bid, ask; QuoteId id; bool live; };
       LastQuote last_quote_[MAX_INSTRUMENTS]{};
   };
   ```

   **Pattern**:
   - Inherits from IMarketMaker
   - Implements all event handlers
   - Maintains per-instrument state
   - No virtual calls in hot path (all methods override)

   ### 7.2 OptionMMCoreStrategy

   **File**: `include/strategy/option_mm_core.h`

   ```cpp
   class OptionMMCoreStrategy : public IMarketMaker {
   public:
       void init(uint8_t product_idx,
                 SPSCRingBuffer<Quote, 512>* quote_buf,
                 SPSCRingBuffer<Order, 512>* order_buf,
                 PreTradeRisk* pre_risk,
                 AtomicMMParams* params,
                 const Instrument* instruments,
                 const SnapshotArray<TopOfBookTick, MAX_INSTRUMENTS>* tick_snapshot,
                 const PostTradeRisk* post_risk,
                 MonitoringTopic<SystemAlert, 256>* alert_topic) noexcept;

       // ... event handlers

       [[nodiscard]] bool read_product_monitor_state(ProductMonitorState* out) const noexcept override;
       [[nodiscard]] int read_instrument_monitor_states(InstrumentMonitorState* out, int max_count) const noexcept override;
   };
   ```

   **Pattern**:
   - More complex than SimpleMMStrategy
   - Receives additional dependencies: tick_snapshot, post_risk, alert_topic
   - Implements monitoring accessors
   - Maintains quote lifecycle state machine per instrument

   ### 7.3 PCPArbitrageStrategy

   **File**: `include/strategy/pcp_arbitrage.h` (inferred from engine code)

   ```cpp
   class PCPArbitrageStrategy : public IArbitrageStrategy {
   public:
       void init(uint8_t product_idx,
                 SPSCRingBuffer<ArbIntent, 256>* intent_buf,
                 AtomicArbParams* params,
                 const Instrument* instruments,
                 const SnapshotArray<TopOfBookTick, MAX_INSTRUMENTS>* tick_snapshot,
                 const SnapshotArray<Greeks, MAX_INSTRUMENTS>* greeks_snapshot,
                 double risk_free_rate,
                 const HardRiskConfig& risk_cfg,
                 AccountId account_id) noexcept;

       void evaluate(Timestamp now_ns) noexcept override;
       void on_order_ack(const Order& order) noexcept override;
       void on_fill(const Trade& trade) noexcept override;
       // ... other handlers
   };
   ```

   **Pattern**:
   - Implements IArbitrageStrategy
   - Receives intent_buf (not quote_buf/order_buf)
   - Pushes ArbIntent (higher-level abstraction)
   - Dispatcher converts ArbIntent → Order

   ---

   ## 8. Recommended Abstraction for New Strategies

   Based on the patterns observed, here's the recommended design:

   ### 8.1 Interface Definition

   ```cpp
   class IMarketMaker {
   public:
       // Event handlers (all noexcept)
       virtual void on_signal(const PricingSignal& signal) noexcept = 0;
       virtual void on_fill(const Trade& trade) noexcept = 0;
       virtual void on_order_ack(const Order& order) noexcept = 0;
       virtual void on_quote_ack(const Quote& quote) noexcept = 0;
       virtual void on_quote_cancel(const Quote& quote) noexcept = 0;
       virtual void on_quote_reject(const Quote& quote) noexcept = 0;
       virtual void on_order_cancel(OrderId id) noexcept = 0;
       virtual void on_order_reject(const Order& order) noexcept = 0;
       virtual void on_timer(const TimerEvent& event) noexcept = 0;

       // Query methods
       [[nodiscard]] virtual bool is_enabled() const noexcept = 0;
       [[nodiscard]] virtual uint8_t product_index() const noexcept = 0;

       // Monitoring (optional)
       [[nodiscard]] virtual bool read_product_monitor_state(ProductMonitorState* out) const noexcept {
           if (out) *out = ProductMonitorState{};
           return false;
       }

       virtual ~IMarketMaker() = default;

   protected:
       // Dependency injection (set by engine)
       SPSCRingBuffer<Quote, 512>* quote_buf_{nullptr};
       SPSCRingBuffer<Order, 512>* order_buf_{nullptr};
       PreTradeRisk* pre_risk_{nullptr};
       AtomicMMParams* params_{nullptr};
       const Instrument* instruments_{nullptr};
       uint8_t product_idx_{0};
       uint64_t order_seq_{0};

       [[nodiscard]] OrderId next_order_id() noexcept {
           return (static_cast<uint64_t>(product_idx_) << 32) | (++order_seq_);
       }
   };
   ```

   ### 8.2 Concrete Implementation Template

   ```cpp
   class MyStrategy : public IMarketMaker {
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
       }

       void on_signal(const PricingSignal& signal) noexcept override {
           // 1. Check if enabled
           if (!is_enabled()) return;

           // 2. Check signal staleness
           // 3. Compute bid/ask
           // 4. Pre-trade risk check
           // 5. Push quote to quote_buf_
       }

       void on_fill(const Trade& trade) noexcept override {
           // Update position tracking
       }

       // ... other handlers

       [[nodiscard]] bool is_enabled() const noexcept override {
           return params_ && params_->enabled.load(std::memory_order_relaxed);
       }

       [[nodiscard]] uint8_t product_index() const noexcept override {
           return product_idx_;
       }

   private:
       // Per-instrument state (no allocation)
       int32_t net_position_[MAX_INSTRUMENTS]{};
       // ... other state
   };
   ```

   ### 8.3 Engine Integration

   ```cpp
   // In TradingEngine::init_strategies()
   if (std::strncmp(cfg_.products[i].strategy_type, "my_strategy",
                    sizeof(cfg_.products[i].strategy_type)) == 0) {
       auto* s = new MyStrategy();
       s->init(static_cast<uint8_t>(i),
               &quote_buf_[i],
               &order_buf_[i],
               &pre_risk_[i],
               &mm_params_[i],
               instruments_);
       strategies_[i].reset(s);
   }
   ```

   ---

   ## 9. Summary Table

   | Aspect | Pattern | Example |
   |--------|---------|---------|
   | **Interface** | Virtual base class | IMarketMaker, IArbitrageStrategy |
   | **Dependency Injection** | Protected members + init() | quote_buf_, order_buf_, params_ |
   | **Performance** | Template specialization | TypedPricer<VolSurface> |
   | **Thread Safety** | Lock-free + atomic snapshots | VolSurfaceManager, SPSC ring buffers |
   | **Extensibility** | Configuration-driven selection | strategy_type from config |
   | **Monitoring** | Optional virtual methods | read_product_monitor_state() |
   | **Code Reuse** | Inheritance + composition | SimpleMMStrategy extends IMarketMaker |
   | **Separation of Concerns** | Ring buffers + callbacks | Strategy pushes intent, engine routes |

   ---

   ## 10. Files to Reference

   - **Strategy interfaces**: `include/strategy/mm_framework.h`, `include/strategy/arbitrage_strategy.h`
   - **Concrete strategies**: `include/strategy/simple_mm.h`, `include/strategy/option_mm_core.h`
   - **Infrastructure**: `include/feed/feed_handler.h`, `include/gateway/gateway.h`
   - **Pricing**: `include/pricing/vol_surface.h`, `include/pricing/typed_pricer.h`
   - **Risk**: `include/risk/pre_trade_risk.h`
   - **Engine**: `include/engine/trading_engine.h`, `src/engine/trading_engine.cpp`
   - **Types**: `include/common/types.h`
