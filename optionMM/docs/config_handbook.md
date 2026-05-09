# Configuration Handbook

This handbook documents the YAML configuration consumed by `load_config()`.
Required top-level sections are `instance`, `feed`, `gateway`, and `products`.
Most other sections are optional and fall back to the defaults listed here.

For low-latency FEMAS trading, start from `config/low_latency_femas.yaml` and
adjust credentials, product codes, books, calendars, and CPU cores.

## Instance

| Key | Default | Description |
| --- | --- | --- |
| `instance.exchange_id` | required | Primary exchange id for this running process, such as `SHFE`, `CFFEX`, or `GFEX`. |
| `instance.account_id` | required | Account identifier stored in process identity and persistence snapshots. |

## Feed

| Key | Default | Description |
| --- | --- | --- |
| `feed.type` | `multicast` | Active market-data source. Supported values: `multicast`, `fpga`, `femas`, `sim`. |
| `feed.multicast.interface` | `eth0` | Local network interface for multicast receive. |
| `feed.multicast.port` | `9001` | UDP port used by multicast groups. |
| `feed.multicast.rcvbuf_mb` | `8` | Socket receive buffer size in MB. Increase for burst tolerance. |
| `feed.multicast.groups[]` | empty | Multicast group addresses. Max 8. |
| `feed.fpga.device` | `/dev/fpga0` | Device node used by the FPGA feed adapter. |
| `feed.femas.front_addr` | empty | FEMAS market-data front address, for example `tcp://host:port`. |
| `feed.femas.broker_id` | empty | Broker id for FEMAS market-data login. |
| `feed.femas.user_id` | empty | User id for FEMAS market-data login. |
| `feed.femas.password` | empty | Password for FEMAS market-data login. |
| `feed.femas.exchange_id` | `CFFEX` | Exchange context for FEMAS subscription. |
| `feed.femas.topic_id` | `100` | FEMAS market-data topic id. |
| `feed.femas.heartbeat_timeout_sec` | `30` | FEMAS market-data heartbeat timeout. |

## Gateway

| Key | Default | Description |
| --- | --- | --- |
| `gateway.type` | `femas` | Active order gateway. Supported values: `femas`, `sim`. |
| `gateway.femas.front_addr` | empty | FEMAS trader front address, for example `tcp://host:port`. |
| `gateway.femas.broker_id` | empty | Broker id for FEMAS order entry. |
| `gateway.femas.user_id` | empty | User id for FEMAS order entry. |
| `gateway.femas.password` | empty | Password for FEMAS order entry. |

`gateway.ctp` may appear in old example files, but the current parser only
accepts `femas` or `sim` for `gateway.type`.

## Simulator

The `sim` section is used when either feed or gateway runs in simulator mode.

| Key | Default | Description |
| --- | --- | --- |
| `sim.profile` | `desk` | Built-in behavior preset. `calm` and `fast` override several simulator defaults before explicit keys are applied. |
| `sim.scenario` | `normal` | Scenario label for simulator behavior. |
| `sim.random_seed` | `42` | Seed for deterministic random behavior. |
| `sim.tick_interval_ms` | `100` | Market-data generation interval. |
| `sim.strikes_per_side` | `4` | Number of strikes above and below ATM per expiry. |
| `sim.expiry_count` | `2` | Number of simulated option expiries. |
| `sim.future_wave_bps` | `12.0` | Deterministic futures price wave amplitude in basis points. |
| `sim.future_noise_bps` | `4.0` | Random futures price noise amplitude in basis points. |
| `sim.option_spread_bps` | `30.0` | Simulated option spread width in basis points. |
| `sim.top_level_volume` | `20` | Simulated best-level volume. |
| `sim.au_reference_price` | `580.0` | Reference price for AU products. |
| `sim.ag_reference_price` | `7800.0` | Reference price for AG products. |
| `sim.gateway_ack_latency_ms` | `0` | Artificial submit acknowledgement latency. |
| `sim.gateway_cancel_latency_ms` | `0` | Artificial cancel acknowledgement latency. |
| `sim.gateway_fill_interval_ms` | `25` | Interval between simulated fill attempts. |
| `sim.gateway_order_fill_probability` | `1.0` | Probability that a normal order fills. |
| `sim.gateway_quote_cross_fill_probability` | `1.0` | Probability that a crossed quote fills. |
| `sim.gateway_quote_passive_fill_probability` | `0.0` | Probability that a passive quote fills. |
| `sim.gateway_partial_fill_probability` | `0.0` | Probability that a fill is partial. |
| `sim.gateway_reject_probability` | `0.0` | Probability that a submit is rejected. |
| `sim.gateway_max_fill_size` | `0` | Max simulated fill size. `0` means uncapped. |
| `sim.gateway_slippage_ticks` | `0` | Simulated fill slippage in ticks. |
| `sim.gateway_quote_near_touch_ticks` | `0.5` | Distance from touch where quote passive fill logic can trigger. |
| `sim.gateway_benchmark_mode` | `false` | Reduces simulator overhead for latency benchmark fidelity. |

## Pricing

| Key | Default | Description |
| --- | --- | --- |
| `pricing.risk_free_rate` | `0.025` | Annualized risk-free rate used by Black-76. |
| `pricing.vol_surface.method` | `svi` | Volatility surface model. Supported values: `svi`, `sabr`, `cubic_spline`, `wing`, `orcWing`. |
| `pricing.vol_surface.fit_interval_seconds` | `60` | Vol surface refit interval. |
| `pricing.sabr_beta` | `1.0` | SABR beta parameter when SABR is active. |
| `pricing.signal_emit_price_epsilon_ticks` | `0.0` | Suppress pricing signal unless theo bid/ask moves by this many ticks. |
| `pricing.signal_emit_underlying_epsilon_ticks` | `0.0` | Suppress signal unless underlying reference moves by this many ticks. |
| `pricing.signal_emit_delta_epsilon` | `0.0` | Suppress signal unless delta changes by this amount. |
| `pricing.signal_emit_vega_epsilon` | `0.0` | Suppress signal unless vega changes by this amount. |
| `pricing.hot_path_greeks_mode` | `full` | `full` writes full Greeks during future-tick repricing; `compact` and `off` avoid full hot-path Greeks writes and rely on `PricingSignal` plus cold refresh. |
| `pricing.cold_greeks_interval_ms` | `1000` | Interval for deferred full-Greeks refresh. |
| `pricing.cold_greeks_batch_size` | `64` | Max option count refreshed in one cold-Greeks batch. |

Latency guidance: production FEMAS should normally use
`pricing.hot_path_greeks_mode: compact` unless a strategy or monitor requires
full Greeks on every future tick.

## Risk

| Key | Default | Description |
| --- | --- | --- |
| `risk.hard.max_volume_per_order` | `100` | Blocking pre-trade max volume for one order or quote leg. |
| `risk.soft.max_net_position` | `500` | Soft net-position limit used by risk monitor and strategies. |
| `risk.soft.max_delta` | `1000.0` | Soft portfolio delta limit. |
| `risk.soft.max_gamma` | `500.0` | Soft portfolio gamma limit. |
| `risk.soft.max_vega` | `10000.0` | Soft portfolio vega limit. |

## Books

Books are optional bootstrap data used by strategies, manual trading, and
persistence. Each entry is independent.

| Key | Default | Description |
| --- | --- | --- |
| `books[].book_id` | `INVALID_BOOK_ID` | Stable internal book id. |
| `books[].book_code` | empty | Unique operator-facing book code. |
| `books[].display_name` | empty | Human-readable book name. |
| `books[].active` | `true` | Whether the book starts active. |
| `books[].description` | empty | Optional description. |

## Users

Users are optional bootstrap data for manual trading and GUI flows.

| Key | Default | Description |
| --- | --- | --- |
| `users[].user_id` | `INVALID_USER_ID` | Stable internal user id. |
| `users[].username` | empty | Login/user key. |
| `users[].display_name` | empty | Human-readable user name. |
| `users[].password` | empty | Configured password or password hash, depending on deployment. |
| `users[].active` | `true` | Whether the user starts active. |
| `users[].default_book_id` | `INVALID_BOOK_ID` | Default book for manual orders. |

## Products

`products` is required and must be a non-empty sequence. Each product maps one
underlying future to one strategy thread.

| Key | Default | Description |
| --- | --- | --- |
| `products[].underlying_id` | required | Underlying futures contract code, such as `cu2501`. |
| `products[].exchange_id` | required | Exchange code for this product. |
| `products[].strategy_core` | required | CPU core for this product strategy thread. |
| `products[].arbitrage_core` | `-1` | Optional CPU core for the product arbitrage sidecar. `-1` disables pinning. |
| `products[].strategy_type` | `simple_mm` | Strategy implementation name registered in `TradingEngine::init_strategies()`. |
| `products[].book_id` | `INVALID_BOOK_ID` | Market-making book id for this product. |
| `products[].pricing.base_offset_type` | `price` | Unit for `base_offset_value`: `tick`, `price`, or `percentage`. |
| `products[].pricing.base_offset_value` | `0.0` | Adjustment applied to future price before option pricing. |

### Product Market-Making Params

| Key | Default | Description |
| --- | --- | --- |
| `products[].params.bid_spread` | `0.5` | Legacy bid spread parameter in ticks. |
| `products[].params.ask_spread` | `0.5` | Legacy ask spread parameter in ticks. |
| `products[].params.quote_volume` | `10` | Default quote size per side. |
| `products[].params.product_delta_threshold` | `50.0` | Product delta threshold for hedging/gating. |
| `products[].params.product_vega_threshold` | `1000.0` | Product vega threshold for hedging/gating. |
| `products[].params.min_quote_interval_ms` | `100.0` | Minimum time between quote updates. Lower improves responsiveness and increases order traffic. |
| `products[].params.max_position` | `500` | Strategy position limit. |
| `products[].params.warning_position` | `max(1, max_position / 2)` | Position where defensive quoting starts. |
| `products[].params.base_half_spread_ticks` | derived from spreads | Normal half-spread in ticks. |
| `products[].params.min_half_spread_ticks` | `base_half_spread_ticks` | Tightest allowed half-spread. |
| `products[].params.max_half_spread_ticks` | `max(min_half_spread_ticks, base_half_spread_ticks * 4)` | Widest allowed half-spread. |
| `products[].params.inventory_skew_per_lot_ticks` | `0.01` | Quote skew per lot of inventory. |
| `products[].params.follow_weight` | `0.35` | Weight used when following theoretical price movement. |
| `products[].params.requote_price_epsilon_ticks` | `1.0` | Minimum quote price change before requote. |
| `products[].params.market_width_widen_threshold_ticks` | `6.0` | Widen quotes when market width reaches this threshold. |
| `products[].params.underlying_move_widen_threshold_ticks` | `2.0` | Widen quotes after this underlying move. |
| `products[].params.use_one_sided_at_limits` | `true` | At position limits, quote only the side that reduces exposure. |
| `products[].params.enabled` | `true` | Enables market making for this product. |

### Product Arbitrage Strategies

| Key | Default | Description |
| --- | --- | --- |
| `products[].arbitrage_strategies[].type` | required when entry exists | Arbitrage strategy type. Currently `pcp` or `PCP`. |
| `products[].arbitrage_strategies[].book_id` | `INVALID_BOOK_ID` | Book id used for arbitrage orders. |
| `products[].arbitrage_strategies[].params.min_edge_ticks` | `2.0` | Minimum edge needed to trade. |
| `products[].arbitrage_strategies[].params.cooldown_ms` | `25.0` | Minimum time between arbitrage attempts. |
| `products[].arbitrage_strategies[].params.scan_interval_ms` | `1.0` | Timer scan cadence for non-event scans. |
| `products[].arbitrage_strategies[].params.cleanup_timeout_ms` | `25.0` | Time before stale arbitrage orders are cancelled. |
| `products[].arbitrage_strategies[].params.max_order_volume` | `1` | Max volume for one arbitrage order. |
| `products[].arbitrage_strategies[].params.max_live_orders` | `8` | Max live arbitrage orders for the strategy. |
| `products[].arbitrage_strategies[].params.cleanup_on_partial` | `true` | Cancel residual orders after a partial fill. |
| `products[].arbitrage_strategies[].params.enabled` | `false` | Enables the arbitrage strategy. |

## Timer

| Key | Default | Description |
| --- | --- | --- |
| `timer.hedge_check_interval_ms` | `1000` | Strategy hedge timer interval. |
| `timer.quote_refresh_interval_ms` | `500` | Strategy full-book quote refresh interval. |
| `timer.session_schedule[].open` | empty | Simple local session open time, `HH:MM:SS`. Max 4 windows. |
| `timer.session_schedule[].close` | empty | Simple local session close time, `HH:MM:SS`. Max 4 windows. |

## Exchange Calendars

Exchange calendars are used by trading-day and time-to-expiry logic.

| Key | Default | Description |
| --- | --- | --- |
| `exchange_calendars[].exchange_id` | required | Exchange code for this calendar. |
| `exchange_calendars[].ranges[].start` | required for range | First date in `YYYYMMDD`. |
| `exchange_calendars[].ranges[].end` | required for range | Last date in `YYYYMMDD`. |
| `exchange_calendars[].ranges[].trading` | `true` | Whether dates in the range are trading days. |
| `exchange_calendars[].days[].date` | required for day | Explicit date in `YYYYMMDD`. |
| `exchange_calendars[].days[].trading` | `false` | Whether the explicit date is a trading day. |

Each calendar entry must define either `ranges` or `days`.

## Exchange Trading Times

| Key | Default | Description |
| --- | --- | --- |
| `exchange_trading_times[].exchange_id` | required | Exchange code for this trading-time table. |
| `exchange_trading_times[].sessions[].start_day_offset` | `0` | Start date offset relative to trading day. Use `-1` for night sessions. |
| `exchange_trading_times[].sessions[].start` | required | Session start time, `HH:MM:SS`. |
| `exchange_trading_times[].sessions[].end_day_offset` | `0` | End date offset relative to trading day. |
| `exchange_trading_times[].sessions[].end` | required | Session end time, `HH:MM:SS`. |

## Monitoring

| Key | Default | Description |
| --- | --- | --- |
| `monitoring.grpc_listen_addr` | `0.0.0.0:50051` | gRPC server bind address. |
| `monitoring.hot_path_publish_mode` | `full` | Monitoring event policy: `full`, `deferred`, or `off`. |

Latency guidance: use `off` in the tightest production FEMAS path, or
`deferred` when operators need live UI streams with bounded hot-path overhead.

## Persistence

| Key | Default | Description |
| --- | --- | --- |
| `persistence.enabled` | `false` | Enables SQLite persistence writer. Disable for lowest hot-path latency. |
| `persistence.data_path` | `data/optionmm.sqlite` | SQLite database path. |
| `persistence.batch_max_rows` | `256` | Max rows per persistence batch. Must be positive. |
| `persistence.flush_interval_ms` | `10` | Max delay before flushing a persistence batch. Must be non-negative. |
| `persistence.snapshot_interval_ms` | `1000` | Periodic state snapshot interval. Must be positive. |
| `persistence.busy_timeout_ms` | `1000` | SQLite busy timeout. Must be non-negative. |

## Execution

| Key | Default | Description |
| --- | --- | --- |
| `execution.low_latency_mode` | `false` | Skips optional monitor/persistence side effects in the send/callback path. Use `true` for live latency-sensitive FEMAS trading. |

## Thread Affinity

Core values below zero mean "do not pin" in the thread utility layer.

| Key | Default | Description |
| --- | --- | --- |
| `thread_affinity.feed_core` | `2` | CPU core for feed thread. |
| `thread_affinity.pricer_core` | `3` | CPU core for pricer thread. |
| `thread_affinity.gateway_dispatcher_core` | `12` | CPU core for gateway dispatcher thread. |
| `thread_affinity.vol_fitter_core` | `13` | CPU core for vol fitter thread. |
| `thread_affinity.risk_monitor_core` | `14` | CPU core for risk monitor thread. |
| `thread_affinity.timer_core` | `11` | CPU core for timer thread. |
| `thread_affinity.grpc_server_core` | `15` | CPU core for gRPC monitoring server. |
| `products[].strategy_core` | required | CPU core for the product strategy thread. |
| `products[].arbitrage_core` | `-1` | CPU core for the product arbitrage thread. |

Low-latency deployments should use isolated physical cores and avoid placing
feed, pricer, strategy, and gateway dispatcher on SMT siblings.

## Thread Scheduling

| Key | Default | Description |
| --- | --- | --- |
| `thread_scheduling.enable_realtime` | `false` | Enables realtime scheduler priority application where supported. |
| `thread_scheduling.low_latency_spin` | `false` | Keeps hot workers spinning while idle. Improves wake-up latency and burns CPU. |
| `thread_scheduling.pricer_priority` | `80` | Realtime priority for pricer thread. |
| `thread_scheduling.strategy_priority` | `70` | Realtime priority for strategy threads. |
| `thread_scheduling.arbitrage_priority` | `60` | Realtime priority for arbitrage threads. |
| `thread_scheduling.gateway_dispatcher_priority` | `75` | Realtime priority for gateway dispatcher. |
| `thread_scheduling.vol_fitter_priority` | `20` | Realtime priority for vol fitter. |
| `thread_scheduling.risk_monitor_priority` | `30` | Realtime priority for risk monitor. |
| `thread_scheduling.timer_priority` | `40` | Realtime priority for timer thread. |

Realtime scheduling usually requires elevated OS permissions. If priority
application fails, the engine logs the failure and continues.
