# Data Repository Plan

## Summary

This document records the plan, design, implementation status, TODO list, and main use cases for the SQLite-based data repository added to this project.

Primary goals:

- keep the trading hot path fast
- recover quickly after process crash or restart
- preserve enough live state to cancel recovered orders and quotes
- keep the storage model easy to extend when more fields are needed later

Chosen approach:

- SQLite backend
- single dedicated repository writer thread
- fully asynchronous persistence from trading threads
- WAL mode for durability and write performance
- state-table model for fast recovery

## Requirements

The repository must save:

- trades
- live orders
- live quotes
- positions
- strategy parameters
- instruments
- end-of-day greeks
- end-of-day vol model parameters

Additional requirements:

- performance is top priority
- recovery must be fast and simple
- schema should be easy to extend with more fields
- restart must be able to send cancel requests for recovered live orders and quotes

## Design

### Architecture

The repository is implemented as a dedicated `DataRepository` component.

Design rules:

- strategy, pricer, feed, risk, and gateway callback paths never touch SQLite directly
- those paths only enqueue small persistence DTOs into lock-free SPSC queues
- one repository thread drains queues and writes to SQLite in batches
- SQLite is opened in `WAL` mode
- writes are grouped into short transactions

This keeps the hot path cost limited to queue push plus DTO copy.

### Storage Model

The repository uses current-state tables instead of a custom file journal.

Benefits:

- simpler recovery logic
- simpler maintenance
- easier schema evolution
- easier ad hoc inspection with SQLite tools

Trade-off:

- SQLite has more overhead than a raw append-only binary journal
- this is acceptable because all DB work is isolated to the async repository thread

### Recovery Model

Recovery authority in v1 is the local SQLite database.

Startup flow:

1. load config
2. connect gateway
3. query instruments
4. build runtime instrument registry
5. open SQLite repository
6. load persisted recovery state
7. remap persisted instrument codes to current runtime `instrument_id`
8. restore runtime params and positions
9. restore gateway recovery handles
10. send cancel requests for recovered live orders and quotes
11. start normal trading flow

Persisted live state is keyed by stable instrument code, not runtime `instrument_id`.

### Gateway Recovery

Recovery requires gateway-specific state, not just generic order IDs.

Implemented support:

- `SimGateway`: restored active order and quote state so cancel works after restart
- `CTPGateway`: persisted quote-leg recovery information for synthetic bid and ask legs
- `FEMASGateway`: persisted and restored local/system IDs needed for order and quote cancel requests

## Persisted Data

### Runtime State

- `trades`
  - fill history
- `live_orders`
  - current recoverable live order state
- `live_quotes`
  - current recoverable live quote state
- `positions_snapshot`
  - latest position snapshot for fast restore
- `strategy_params`
  - latest market-making parameters per product
- `arb_strategy_params`
  - latest arbitrage parameters per product and strategy type
- `risk_params`
  - latest soft risk thresholds
- `instruments`
  - latest current instrument catalog

### End Of Day State

- `eod_greeks`
  - end-of-day greeks snapshot by trading day and instrument
- `eod_vol_model_params`
  - end-of-day vol model parameters by trading day, product, model, and expiry
- `eod_instruments`
  - instrument catalog saved for that trading day

Note:

- latest intraday greeks are not persisted
- only end-of-day greeks are persisted
- "wind model parameters" are stored as end-of-day vol model params; current schema supports `Wing`, `OrcWing`, and `SVI`

## Implementation

### Config

Added `PersistenceConfig` under `SystemConfig` with:

- `enabled`
- `data_path`
- `batch_max_rows`
- `flush_interval_ms`
- `snapshot_interval_ms`
- `busy_timeout_ms`

Config parsing and example YAML were updated accordingly.

### Repository Component

Added:

- `include/persistence/data_repository.h`
- `src/persistence/data_repository.cpp`

Main responsibilities:

- create and migrate SQLite schema
- manage prepared statements
- own writer thread
- batch queue drains into transactions
- persist runtime state
- persist end-of-day snapshot data
- load recovery state on startup

### Engine Integration

`TradingEngine` now:

- constructs the repository when persistence is enabled
- opens repository after instrument query
- loads recovery state before normal startup
- restores params and positions
- seeds gateway recovery state
- requests cancel of recovered live orders and quotes
- persists order events, quote events, trades, params, risk limits, and position snapshots
- persists end-of-day snapshot on shutdown

### Runtime Persistence Hooks

Persistence hooks were added for:

- order submit
- order ack
- order cancel
- order reject
- quote submit
- quote ack
- quote cancel
- quote reject
- trade fills
- periodic position snapshots from the risk thread
- MM parameter updates from gRPC
- arbitrage parameter updates from gRPC
- soft risk threshold updates from gRPC

### Recovery Support In Gateways

Gateway interface additions:

- get order recovery handle
- get quote recovery handle
- restore recovered order state
- restore recovered quote state

Implemented in:

- `SimGateway`
- `CTPGateway`
- `FEMASGateway`

### Risk Restore

`PostTradeRisk` now supports restoring positions from persisted snapshots.

## Performance Notes

Hot-path protection rules in the current implementation:

- no SQLite calls on strategy, feed, pricer, or gateway callback code paths
- no synchronous DB reads or writes on the trading path
- queue-based async persistence only
- batched writes inside one repository thread

Current performance-sensitive settings:

- `WAL` journal mode
- `synchronous=NORMAL`
- configurable batch size
- configurable flush interval

## Current Status

Implemented and verified:

- async SQLite repository
- runtime persistence for trades, live orders, live quotes, positions, params, risk limits, instruments
- end-of-day persistence for greeks, vol model params, instruments
- restart recovery of positions, params, and live order/quote state
- gateway recovery handle restore for `Sim`, `CTP`, and `FEMAS`
- recovery cancel dispatch before normal trading flow

Validation completed:

- repository-specific persistence and recovery tests
- main binary rebuild
- existing engine integration regression test

Important bugs found and fixed during implementation:

- live order recovery query used wrong column name
- end-of-day instrument statement used wrong bind order

## TODO List

### High Priority

- add an explicit end-of-day trigger instead of relying only on shutdown-time persistence
- add a manual or RPC-triggered EOD snapshot command
- add schema migration versioning beyond initial `user_version = 1`
- add retention and cleanup policy for old trade and EOD data

### Medium Priority

- add a startup "recovery mode" gate visible to monitoring so operators know cancels are still in progress
- add stronger recovery tests for FEMAS and CTP-specific cancel behavior
- add corruption and partial-commit recovery tests
- add metrics for repository queue depth, flush latency, and dropped persistence events

### Lower Priority

- add optional compression or archival for historical EOD data
- add richer query/reporting helpers on top of the SQLite schema
- add reconciliation against exchange-side state if needed later

## Use Cases

### Crash Recovery

Use case:

- system crashes with live orders or quotes still working at the exchange
- system restarts
- repository reloads live state
- gateway-specific recovery handles are restored
- engine sends cancel requests before new quoting begins

### Trade History

Use case:

- fills must survive restart
- post-trade analysis and reporting need trade history
- trade table keeps durable fill records

### Position Continuity

Use case:

- system restarts mid-session
- positions and realized PnL baseline must be restored quickly
- latest position snapshot is loaded into post-trade risk state

### Runtime Parameter Persistence

Use case:

- operator changes MM or arbitrage parameters via gRPC
- process restarts later
- restored runtime parameters should not fall back to older YAML defaults

### Daily Report Inputs

Use case:

- daily report needs end-of-day greeks, instruments, and vol model params
- repository stores these by trading day for later reporting and audit

### Schema Extension

Use case:

- later we need extra fields on orders, quotes, positions, or EOD data
- SQLite schema can be extended with new nullable columns or migration scripts
- application code can start writing the new fields without redesigning the storage layer

## Assumptions

- one process owns the SQLite file
- local persisted state is the recovery authority in v1
- instrument catalog is freshly queried at startup and persisted state is remapped by instrument code
- end-of-day greeks and vol model params are point-in-time snapshots, not continuous intraday history

## File Map

Main files involved:

- `include/persistence/data_repository.h`
- `src/persistence/data_repository.cpp`
- `include/engine/trading_engine.h`
- `src/engine/trading_engine.cpp`
- `include/gateway/gateway.h`
- `src/gateway/sim_gateway.cpp`
- `src/gateway/ctp_gateway.cpp`
- `src/gateway/femas_gateway.cpp`
- `include/common/config.h`
- `src/common/config.cpp`
- `src/monitoring/grpc_server.cpp`
- `tests/test_data_repository.cpp`

