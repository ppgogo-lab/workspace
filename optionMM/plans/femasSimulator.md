# FEMAS Simulator Design - 2026-04-20

## Summary

This note records the plan, design, current implementation, and remaining work for a FEMAS-compatible simulator in `optionMM`.

The target is a standalone gateway simulator that can mock FEMAS market data and trader behavior:

- market data side behaving like `CUstpFtdcMduserSpi`
- trader side behaving like `CUstpFtdcTraderSpi`
- replay input selected by exchange, products, and one intraday time window
- replay source from DolphinDB
- support for futures and options instruments
- support for orders, quotes, cancel order, cancel quote, and quote replace
- matching driven by level2 market data with 5 bid levels and 5 ask levels

The current implementation delivers the core simulator path inside the existing process behind a `sim://` front address. It is integrated into the existing FEMAS feed and gateway code paths and supports 5-depth replay and matching. It is not yet a separate external daemon, and it does not yet talk to a live DolphinDB server directly.

## Plan

- Add an SDK-compatible FEMAS simulation path that existing code can use without changing strategy interfaces.
- Use one shared simulation session for both market data and trader APIs so the trader side always sees the same live replayed book as the market data side.
- Load the instrument universe for the selected day and product set before replay starts.
- Replay level2 market data with 5 bid levels and 5 ask levels.
- Build a matching service on top of the current replayed 5-level book.
- Support:
  - order insert
  - order cancel
  - quote insert
  - quote cancel
  - quote replace
- Keep callback behavior FEMAS-like enough for the existing gateway and feed integration code.
- Validate the simulator with focused tests and a full project build.

## Design

## Public Entry

- `sim://...` is the public front address contract for the simulator.
- Existing FEMAS integration points route to the simulator when the front address starts with `sim://`.
- Non-`sim://` addresses still use the real vendor FEMAS APIs.

Example shape:

```text
sim://session?exchange=CFFEX&products=IF,IO&date=2025-04-20&start=09:30:00&end=11:30:00&speed=10x&ddb=file:///path/to/export
```

Current required parameters:

- `exchange`
- `products`
- `date`
- `start`
- `end`
- `ddb`

Current optional parameters:

- `speed`

## Session Model

- A shared `SimulationSession` is created per normalized `sim://` configuration.
- The session owns:
  - instrument universe
  - replay tick stream
  - current 5-level book per instrument
  - active orders
  - active quotes
  - subscriber sets for market data and trader clients
- Market data and trader API objects attach to the same session, so instrument query, replay, and matching all use one consistent state model.

## Replay Model

- Replay data is time-ordered by exchange timestamp.
- Replay speed supports:
  - `1x`
  - `Nx` style scaled replay such as `10x`
  - `max` for best-effort fast replay
- Each tick carries:
  - instrument id
  - exchange id
  - last price
  - bid price 1..5
  - bid volume 1..5
  - ask price 1..5
  - ask volume 1..5
  - volume / turnover / open interest when present
- The market data side emits FEMAS depth-market-data callbacks using this 5-level snapshot.

## Matching Model

- Matching is driven by the current replayed 5-level visible book.
- Aggressive orders consume the opposite-side ladder from level 1 outward.
- A multi-level sweep can generate multiple fills at different prices.
- Passive limit orders remain live when they do not cross the book immediately.
- Passive orders can fill later when later replay ticks touch or trade through the resting price.
- Fills are capped by the visible displayed volume from the replayed ladder.
- There is no queue-position model in the current version.

## Quote Model

- Quotes track bid and ask legs independently.
- A quote can partially fill on either leg.
- Quote matching also uses the visible 5-level opposite-side ladder.
- Quote replace is native:
  - at most one live quote per `user + instrument`
  - a new quote replaces the previous live quote atomically for exposure
  - the old quote is canceled before the new quote becomes active

## Data Source Model

The design target is DolphinDB-backed replay. The current implementation uses a file-backed export layout as a practical first step:

- `instruments.csv`
- `ticks.csv`

This keeps the simulator testable and deterministic while leaving the actual DolphinDB client integration as a separate follow-up task.

## Implementation

## Added Interfaces

Added FEMAS wrapper interfaces so the feed and gateway no longer depend directly on vendor API classes:

- `include/femas/api_wrapper.h`
  - `IFemasMdApi`
  - `IFemasTraderApi`
  - `create_femas_md_api(...)`
  - `create_femas_trader_api(...)`

This abstraction lets the existing runtime choose between:

- the real FEMAS SDK
- the in-process simulator

## Added Simulator Module

Added:

- `include/sim/femas_simulator.h`
- `src/sim/femas_simulator.cpp`

Implemented behavior:

- `sim://` front parsing
- shared session registry keyed by simulator config
- instrument loading
- 5-depth tick loading
- replay scheduling with configurable speed
- market data subscription fanout
- instrument query response
- order insert and cancel
- quote insert and cancel
- quote replace
- 5-level matching for both orders and quotes
- replay-end handling that stops new submissions after the configured time span

## Existing Integration Changes

Updated the existing FEMAS integration points:

- `src/feed/femas_feed.cpp`
  - now creates market-data API objects through `create_femas_md_api(...)`
- `src/gateway/femas_gateway.cpp`
  - now creates trader API objects through `create_femas_trader_api(...)`
- `include/feed/femas_feed.h`
  - now stores `IFemasMdApi*`
- `include/gateway/femas_gateway.h`
  - now stores `IFemasTraderApi*`

This keeps the higher-level feed and gateway logic unchanged while allowing a `sim://` front to drive the simulator.

## Build Integration

Added build targets in `CMakeLists.txt`:

- `femas_simulator_lib`
- `femas_api_lib`
- `test_femas_simulator`

The existing feed and gateway libraries now link through `femas_api_lib`, which dispatches to either the vendor SDK or the simulator.

## Test Coverage

Added:

- `tests/test_femas_simulator.cpp`

Current coverage includes:

- `sim://` URI parsing
- 5-depth replay delivery
- instrument query
- multi-level order sweep behavior
- quote replace behavior
- quote fill behavior driven by later replay ticks

## Validation

Validated with:

- successful build of `test_femas_simulator`
- successful execution of `test_femas_simulator`
- successful rebuild of `optionmm`

## Current Limitations

- The simulator currently runs in-process. It is not yet a standalone external service.
- `ddb=` currently points to a file-backed export directory, not a live DolphinDB connection.
- Matching uses visible 5-depth only.
- There is no queue-position model, hidden liquidity model, or exchange-grade time-priority simulation.
- The current implementation is effectively single-account scoped by its quote replacement and order state assumptions.
- Session parameters are fixed for the life of the simulator session.

## TODO List

- Replace the file-backed `ddb=` loader with a real DolphinDB adapter.
- Define the exact DolphinDB schema contract for:
  - instrument metadata
  - level2 tick rows
  - trading session timestamps
- Support reconnect and resubscribe semantics closer to real FEMAS behavior.
- Add explicit handling for trading session boundaries such as:
  - pre-open
  - lunch break
  - day close
  - night session where relevant
- Add more complete trader query support if the gateway needs it:
  - order query
  - trade query
  - quote query
  - position and account query
- Add better error-code mapping so simulator rejects look closer to real FEMAS responses.
- Support multiple simulated users and clearer account isolation rules.
- Decide whether quote replacement should remain a simulator-native behavior or be exposed as a higher-level policy flag.
- Add persistence or replay checkpoints if long windows need restart support.
- Add acceptance fixtures using real exported DolphinDB samples for futures and options products.
- Add more tests for:
  - passive order fills over multiple replay ticks
  - cancel races near fill time
  - quote partial fill then replace
  - replay end-state behavior
  - invalid request handling
- Decide whether to keep the simulator in-process or also ship a standalone daemon that exposes FEMAS-compatible connectivity for external clients.

## Recommended Next Step

The next high-value step is to replace the temporary file-backed replay loader with a real DolphinDB adapter while keeping the current `SimulationSession` and matching logic unchanged. That preserves the current tested core and moves the remaining risk into one isolated integration layer.
