# User And Book Plan

## Summary

This document records the plan, implemented design, current behavior, main use cases, and remaining TODOs for the user/session/book subsystem added to this project.

Primary goals:

- add user login and logout for the monitor/control plane
- keep all trading strategies and live state shared across users
- make strategy status and parameter changes visible to every logged-in user immediately
- add books for grouping trades, positions, PnL, PMS, and strategy performance
- ensure strategy-generated trades are tagged to the correct book
- ensure manual orders use either an explicit book or the user's default book
- stop strategies and cancel all live orders and quotes immediately when the active session count reaches zero

Chosen approach:

- local persisted users and books in SQLite
- password hashing with PBKDF2-SHA256
- in-memory bearer-token sessions
- shared global engine state, not per-user strategy state
- book tagging on orders, quotes, and trades
- per-book aggregation inside the engine for snapshot and monitoring views

## Requirements

The subsystem must support:

- user login and logout
- shared trades and shared strategies across all users
- immediate visibility of parameter changes and strategy start/stop changes
- one book per strategy instance
- one default book per user
- explicit or default-book selection for manual orders
- PMS grouping by book and by book plus product
- stop-and-cancel behavior when all users log out

Non-goals in v1:

- per-user private strategies
- persistent sessions across server restart
- runtime user or book administration UI
- per-book pre-trade or soft-risk limits

## Design

### Identity Model

Users are operator identities for the gRPC monitor/control plane only.

Persisted user data includes:

- `user_id`
- `username`
- `display_name`
- password hash
- `active`
- `default_book_id`

Sessions are in-memory only:

- `Login` returns a bearer token
- all non-login RPCs and all streams require `authorization: Bearer <token>`
- `Logout` revokes the token
- after process restart, all users must log in again

### Shared Strategy Model

Strategy state remains global:

- MM and arbitrage params are shared
- strategy enabled and disabled state is shared
- one user's control action updates the same runtime state seen by every other user

There is no user-specific strategy copy.

### Book Model

Books are grouping labels for:

- trades
- positions
- realized and unrealized PnL
- PMS and portfolio-style greeks aggregation
- strategy performance analysis

Book assignment rules:

- each MM product has one configured MM book
- each arbitrage strategy instance has one configured strategy book
- manual orders use request `book_id` if provided
- otherwise manual orders use the logged-in user's `default_book_id`
- manual orders are rejected if no valid active book can be resolved

### Zero-Session Safety Rule

When the active session count reaches zero:

- strategy dispatch is suspended
- all MM strategies are disabled
- all arbitrage strategies are disabled
- live orders are canceled
- live quotes are canceled

This is treated as an operational safety condition.

Logging back in clears dispatch suspension, but strategies remain disabled until explicitly restarted.

## Implementation

### Config And Types

Added config/bootstrap support for:

- `books`
- `users`
- per-product MM `book_id`
- per-arbitrage-strategy `book_id`

Added core identifiers and tagging fields:

- `UserId`
- `BookId`
- `book_id` on `Order`
- `book_id` on `Quote`
- `book_id` on `Trade`
- `BookPosition`
- `BookPortfolioGreeks`

### Authentication

Added:

- `include/common/auth.h`
- `src/common/auth.cpp`

Current behavior:

- passwords are stored as PBKDF2-SHA256 hashes in SQLite
- bootstrap config passwords are hashed when first seeded
- login verifies the stored hash
- session tokens are random and stored in memory in the gRPC server

### Persistence

The SQLite repository now persists:

- `users`
- `books`
- `strategy_book_bindings`
- book-tagged `trades`
- book-tagged `live_orders`
- book-tagged `live_quotes`

Bootstrap rules:

- if identity tables are empty, seed from config
- otherwise SQLite is the source of truth

### Engine Integration

`TradingEngine` now:

- loads persisted identity state during startup
- keeps MM and arbitrage strategy to book bindings in memory
- tags strategy-generated orders and quotes with the configured book
- carries `book_id` through gateway callback handling and fill processing
- tracks live orders and live quotes for recovery and zero-session cancel-all
- maintains per-book positions from fills
- builds per-book and per-book-plus-product portfolio aggregates for snapshot consumers

### gRPC API

Added RPCs:

- `Login`
- `Logout`
- `WhoAmI`

Updated behavior:

- every non-login RPC requires authentication
- every stream requires authentication and stops when the session is revoked
- manual order submission now supports optional `book_id`
- snapshot now includes:
  - current user
  - book list
  - per-book positions
  - per-book portfolios

### Monitoring Visibility

Because the engine state is shared, all authenticated users see:

- strategy start and stop changes
- MM parameter changes
- arbitrage parameter changes
- book-tagged orders, trades, and quotes

No extra cross-user sync layer was needed beyond the shared engine and existing monitor streams.

## Current Status

Implemented and verified:

- local user and book bootstrap in SQLite
- password hashing and login verification
- session-based gRPC authentication
- logout revocation
- zero-session stop-and-cancel behavior
- strategy book tagging
- manual order book resolution through explicit book or user default book
- per-book position and PMS-style aggregation in snapshot output
- persistence and recovery of book-tagged live state and trades

Validation completed:

- `cmake --build build-wsl --target optionmm -j4`
- `cmake --build build-wsl --target test_data_repository test_simple_mm -j4`
- `./build-wsl/test_data_repository --gtest_color=no`
- `./build-wsl/test_simple_mm --gtest_color=no`

## Main Use Cases

### User Login And Shared Monitor

1. user A logs in and opens monitoring
2. user B logs in and opens monitoring
3. user A changes MM parameters or starts or stops a strategy
4. user B sees the updated shared state immediately through snapshot or streaming data

### Strategy Book Tagging

1. MM strategy for product `P` emits an order or quote
2. engine stamps the configured MM book for that product
3. fills inherit the same book
4. positions, PnL, and PMS are grouped under that book

The same pattern applies to each arbitrage strategy instance using its configured book.

### Manual Trading

1. user logs in
2. user submits a manual order with an explicit `book_id`
3. if omitted, engine uses the user's default book
4. if neither resolves to a valid active book, the request is rejected

### Zero-Session Shutdown

1. one or more users are logged in and strategies are active
2. the last active session logs out
3. engine disables strategies immediately
4. engine sends cancel requests for all live orders and live quotes
5. the system stays in a stopped state until a user logs in and explicitly restarts strategies

### Reporting And PMS

Book-tagged fills update:

- per-book positions
- per-book realized PnL
- per-book unrealized PnL
- per-book portfolio greeks
- per-book-plus-product portfolio greeks

This gives a backend basis for PMS, reporting, and strategy performance evaluation by book.

## TODO List

### High Priority

- add gRPC or GUI support to let operators choose books from the live book catalog instead of config-driven knowledge only
- add integration tests for full login/logout plus zero-session shutdown behavior through the actual gRPC service
- add explicit monitoring state for "zero-session safety stop active"
- add stream and snapshot coverage in tests for `current_user`, `books`, `book_positions`, and `book_portfolios`

### Medium Priority

- add runtime administration RPCs for creating, updating, and disabling users
- add runtime administration RPCs for creating, updating, and disabling books
- add audit persistence for which user changed params or strategy state
- add richer book performance reporting on top of the current aggregates
- add persistence for per-book snapshots if startup cost grows with trade history size

### Lower Priority

- add per-book risk limits if the operating model needs that later
- add session timeout and idle-expiry policies
- add role or permission separation between read-only and control users
- add GUI login flow and book-aware controls in the trader UI

## Main Files

- [include/common/auth.h](</D:/workspace/optionMM/include/common/auth.h>)
- [src/common/auth.cpp](</D:/workspace/optionMM/src/common/auth.cpp>)
- [include/common/config.h](</D:/workspace/optionMM/include/common/config.h>)
- [src/common/config.cpp](</D:/workspace/optionMM/src/common/config.cpp>)
- [include/common/types.h](</D:/workspace/optionMM/include/common/types.h>)
- [include/engine/trading_engine.h](</D:/workspace/optionMM/include/engine/trading_engine.h>)
- [src/engine/trading_engine.cpp](</D:/workspace/optionMM/src/engine/trading_engine.cpp>)
- [include/persistence/data_repository.h](</D:/workspace/optionMM/include/persistence/data_repository.h>)
- [src/persistence/data_repository.cpp](</D:/workspace/optionMM/src/persistence/data_repository.cpp>)
- [src/monitoring/grpc_server.cpp](</D:/workspace/optionMM/src/monitoring/grpc_server.cpp>)
- [proto/trading.proto](</D:/workspace/optionMM/proto/trading.proto>)
- [tests/test_data_repository.cpp](</D:/workspace/optionMM/tests/test_data_repository.cpp>)
