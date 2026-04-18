# UI Optimization / Development Plan

## Goal

Turn the current Qt monitor into a trader-facing market making workstation that:

- makes quote state, trade flow, PMS, and risk visible at a glance
- exposes the real `option_mm_core` runtime controls from the monitor
- makes risk impossible to miss
- keeps the fastest workflows to one click or one hotkey sequence

This plan is based on:

- `UI-SPEC.md`
- `src/gui/trader_main_window.cpp`
- `src/monitoring/grpc_server.cpp`
- `proto/trading.proto`
- `src/strategy/option_mm_core.cpp`
- `OPTION_MM_CORE_MONITOR_PARAMS.md`

## Confirmed Direction

Product decisions confirmed:

- traders may edit soft-risk thresholds from the monitor
- quote suppression must be visible at both product level and instrument level
- the default workstation target is a two-screen layout

## Current State

The monitor already has a usable first version:

- product selector and connection state
- central option ladder / T-table
- manual order entry
- per-product MM start/stop
- expanded `option_mm_core` parameter editors
- positions tree
- orders / quotes / trades blotters
- vol curve dock
- saved window geometry and dock state through `QSettings`

Important backend note:

- `SetStrategyParams` merge semantics are already fixed in `src/monitoring/grpc_server.cpp`
- the current tree already supports the expanded `option_mm_core` parameter set in proto and snapshot

## Main Gaps

### 1. The monitor is functionally broad but visually flat

The screen is still arranged like a generic dashboard, not a trader workstation. The most important states are not visually ranked:

- product live or suppressed
- quote live / one-way / rejected / stale / cancel-stuck
- risk warning versus hard breach
- recent fills and hedge activity

### 2. Strategy controls are present but not operator-friendly

All MM controls sit in one dense grid. Traders need grouped controls with clear meaning:

- quote shape
- inventory and size
- churn / requote
- hedge and suppression

### 3. Risk is under-surfaced

The backend streams `RiskAlert`, but the GUI does not consume it. The trader cannot immediately see:

- global soft-risk breach
- quote cancel give-up
- product suppression due to exposure
- underlying shock suppression

### 4. Quote status visibility is incomplete

The T-table shows basic status coloring, but it does not show the reason a product or instrument is not quoting. For `option_mm_core`, the important reasons are:

- stale theo
- invalid market
- position limit
- soft-risk breach
- session off
- underlying shock suppression
- product exposure breach
- cancel-stuck

### 5. PMS / risk / execution are not integrated into one workflow

The current layout splits data into docks, but it does not yet behave like a coherent trading station:

- no dedicated risk board
- no dedicated product summary
- no cancel action from order or quote blotters
- no risk-threshold control panel even though `SetRiskThreshold` exists
- no explicit recent hedge activity view

### 6. Vol panel is passive

Vol curves render, but the trader cannot focus by:

- product
- expiry
- underlying
- selected row from the T-table

## Design Principles

### 1. Risk First

Risk must be more visually dominant than quote cosmetics.

- always-visible top risk strip
- amber for warning, red for breach, blinking red only for true urgent states
- sticky alert queue until acknowledged
- product suppression must be obvious from both toolbar and T-table

### 2. Centralize the Ladder

The T-table remains the center of the workflow. Every major action should pivot from it:

- select instrument
- inspect quote condition
- send manual order
- inspect position
- open relevant curve

### 3. Group Controls by Trading Intent

Do not show a flat parameter wall. Group fields by what the trader is trying to do.

### 4. Keep One-Screen Scanning

The best default workspace should work on a single monitor without forcing dock hunting.

### 5. Keep Remote-Monitor Constraints in Mind

The GUI is a monitor client, not the trading engine. UI additions should prefer:

- snapshot and streaming data already available
- lightweight derived state in the client
- small, explicit proto additions only where the UI cannot infer the needed status

### 6. Default To Two Screens

The default operator workspace should assume:

- screen 1 for ladder, ticket, PMS, orders, trades, and risk strip
- screen 2 for vol, product risk board, and alert-heavy panels

Single-screen fallback should still work, but it is not the primary layout target.

## Target Workspace

### Top: Desk / Risk Strip

Replace the current simple toolbar with a denser, trader-style strip:

- product selector
- connection state
- strategy state for selected product: `RUNNING`, `SUPPRESSED`, `OFF`
- portfolio delta / gamma / vega pills
- global soft-risk state
- latest alert banner
- quick actions: `Start`, `Stop`, `Flatten Product` if supported later

Visual behavior:

- normal: sand / muted green
- warning: amber background on the relevant metric
- breach: red solid pill plus flashing alert banner

This strip should remain visible on the primary screen at all times.

### Center: Enhanced T-Table

Keep the T-table central, but upgrade it into a true quote monitor.

Recommended columns:

- `C.MM`
- `C.QState`
- `C.BQty`
- `C.Bid`
- `C.Theo`
- `C.Ask`
- `C.AQty`
- `Expiry`
- `Strike`
- `Net`
- `P.BQty`
- `P.Bid`
- `P.Theo`
- `P.Ask`
- `P.AQty`
- `P.QState`
- `P.MM`
- `Risk`
- `Why`

New meaning:

- `QState`: live lifecycle state like `LIVE`, `NEW`, `ACK`, `FILL`, `REJ`, `OFF`
- `Risk`: compact badge such as `OK`, `WARN`, `LIMIT`, `SOFT`
- `Why`: suppress reason such as `STALE`, `MKT`, `POS`, `VEGA`, `SHOCK`, `CXL`

Behavior:

- stale or suppressed rows fade but keep a strong reason badge
- reject and cancel-stuck rows flash briefly
- filled rows pulse once, then decay
- clicking bid or ask still prefills the ticket
- selecting a row also focuses positions and vol for the same expiry / strike

### Left: PMS / Risk Board

Replace the current plain positions tree dock with a stronger PMS panel:

- top summary card for selected product
- grouped tree by underlying and expiry
- columns for `Net`, `Avg`, `UPnL`, `RPnL`, `Delta`, `Gamma`, `Vega`
- heat coloring by risk magnitude
- one-click expand/collapse by group

Above the tree, add a compact product risk board:

- net option delta
- net futures hedge position
- net vega
- net gamma
- quote-enabled instrument count
- suppressed instrument count

### Right: Trader Control Stack

Split the current mixed dock into separate sections:

1. Manual Order Ticket
- instrument
- side
- price
- volume
- `Send Buy`
- `Send Sell`
- `Cancel Selected Order`

2. Strategy Control
- `Start MM`
- `Stop MM`
- product status text
- last param apply timestamp

3. MM Parameter Editor

Grouped tabs or collapsible sections:

- `Quote Shape`
  - `base_half_spread_ticks`
  - `min_half_spread_ticks`
  - `max_half_spread_ticks`
  - `follow_weight`
  - `market_width_widen_threshold_ticks`
- `Inventory`
  - `quote_volume`
  - `warning_position`
  - `max_position`
  - `inventory_skew_per_lot_ticks`
  - `use_one_sided_at_limits`
- `Requote`
  - `requote_price_epsilon_ticks`
  - `min_quote_interval_ms`
- `Hedge / Product Gate`
  - `hedge_delta_threshold`
  - `product_vega_threshold`
  - `underlying_move_widen_threshold_ticks`
  - `enabled`

Do not keep `bid_spread` and `ask_spread` as primary trader controls. If they remain for backward compatibility, hide them in an advanced section labeled legacy.

### Bottom: Execution Blotters

Keep orders, quotes, and trades at the bottom, but add execution-focused columns.

Orders:

- add filter chips for `All`, `Working`, `Rejected`, `Filled`, `Hedge`
- add right-click or button action for `Cancel`

Quotes:

- add `QuoteId`, `Ts`, `AgeMs`, `State`
- color rows by working / cancelled / rejected

Trades:

- highlight hedge trades separately from option fills
- keep the newest fills visually hot for a few seconds

### Separate Floating Window: Vol Surface

Keep the detachable vol window, but add trader controls:

- selected product label
- expiry selector
- pin-to-selected-instrument mode
- ATM marker and selected strike marker

For the default two-screen layout, the vol window should open on the secondary screen and remain persistent there.

## Parameter Surface To Expose

The trader MM panel should focus on the real `option_mm_core` knobs:

- `base_half_spread_ticks`
- `min_half_spread_ticks`
- `max_half_spread_ticks`
- `follow_weight`
- `inventory_skew_per_lot_ticks`
- `market_width_widen_threshold_ticks`
- `quote_volume`
- `warning_position`
- `max_position`
- `use_one_sided_at_limits`
- `requote_price_epsilon_ticks`
- `min_quote_interval_ms`
- `hedge_delta_threshold`
- `product_vega_threshold`
- `underlying_move_widen_threshold_ticks`
- `enabled`

Separate risk/admin surface:

- `risk.soft.max_net_position`
- `risk.soft.max_delta`
- `risk.soft.max_gamma`
- `risk.soft.max_vega`
- `risk.hard.max_volume_per_order`

Decision:

- `risk.soft.*` should be editable by traders from the monitor
- `risk.hard.max_volume_per_order` should remain an admin-style control unless explicitly opened later

Not for the main trader MM panel:

- `bid_spread`
- `ask_spread`
- engine timer settings
- inactive session schedule controls

## Backend / API Work Items

### Phase 0: Use Existing RPCs Better

Already present but not surfaced in the GUI:

- `StreamRiskAlerts`
- `SetRiskThreshold`
- `CancelOrder`

These should be wired into the monitor before adding more protocol complexity.

Because `SetRiskThreshold` is trader-editable by product direction, this is a priority UI item rather than a later optional control.

### Phase 1: Add Missing UI-State Data If Needed

The current `QuoteUpdate.status` is not enough to explain why a quote is absent or suppressed. For a trader-grade monitor, consider adding one or both of these:

1. Product-level monitor state stream
- product enabled
- product suppressed
- suppression reason
- net product delta / vega
- hedge state

2. Instrument quote-state snapshot / stream
- quote lifecycle state
- suppress flags
- live quote age
- working bid / ask size

This data already exists inside `OptionMMCoreStrategy`; the monitor just cannot see it directly today.

Direction:

- implement both product-level and instrument-level suppression visibility

### Phase 2: Add Risk Summary Snapshot

If the GUI needs system-level supervisory display without inference, extend snapshot with:

- current soft-risk thresholds
- current breach flags
- optionally hard max volume per order

## Development Plan

### Phase 1: Risk-First Monitor Foundation

Scope:

- add `StreamRiskAlerts` client loop
- add top risk strip
- add dedicated risk alert dock
- add alert severity coloring and sticky recent alerts list
- add editable soft-risk threshold controls
- add clear confirmation and audit-style status text after risk threshold changes

Acceptance:

- trader can see breaches without looking at logs
- risk alerts are visible within one screen refresh cycle
- connection loss and risk breach states are visually distinct
- trader can update soft-risk thresholds directly from the monitor

### Phase 2: Rebuild the Right-Side Control Surface

Scope:

- split manual order, strategy control, and MM params into separate sections
- regroup params by trading intent
- hide legacy fields from the default view
- show dirty-state versus live-state for params
- add `Apply`, `Reset`, and `Revert to Live`

Acceptance:

- trader can change a product parameter set without hunting across a flat grid
- unsaved edits are obvious
- applying params does not visually overwrite local edits mid-edit

### Phase 3: Upgrade T-Table Into a Quote-State Board

Scope:

- add quote-state and suppress-reason columns
- add stronger row and cell coloring
- add transient flash for reject / fill / cancel-stuck
- link row selection to order ticket, PMS, and vol panel

Acceptance:

- trader can tell in under one second whether a strike is quoting, one-way, suppressed, or broken
- missing quotes always show a reason, not just an empty cell

### Phase 4: PMS / Execution Integration

Scope:

- upgrade positions dock into PMS/risk board
- add grouped summaries by underlying and expiry
- extend blotters with filters and cancel actions
- mark hedge trades separately

Acceptance:

- trader can trace quote -> order -> trade -> position impact from one workspace
- selected product exposure is visible without opening another window

### Phase 5: Vol Focus and Workspace Polish

Scope:

- add expiry and focus controls to vol view
- highlight selected strike and related expiry
- add keyboard shortcuts for common actions
- refine saved workspace presets
- make the default saved layout a two-screen workspace

Acceptance:

- trader can switch products and keep the workspace coherent
- frequent actions are accessible by keyboard
- the secondary screen stays useful even when the primary screen is execution-heavy

## Recommended Implementation Order

1. Wire `StreamRiskAlerts` into the GUI and build the risk strip
2. Add soft-risk threshold editing and confirmation flow
3. Restructure the right-side control dock and parameter grouping
4. Upgrade the T-table with instrument-level quote-state and reason badges
5. Add product-level suppression state and richer PMS summary
6. Add order cancel, execution filters, and two-screen default workspace behavior
7. Add optional backend state streams if the UI still cannot explain quote absence cleanly

## Concrete Code Areas

Primary files likely to change:

- `include/gui/trader_main_window.h`
- `src/gui/trader_main_window.cpp`
- `proto/trading.proto`
- `src/monitoring/grpc_server.cpp`
- possibly `include/strategy/option_mm_core.h`
- possibly `src/strategy/option_mm_core.cpp`

## Acceptance Criteria

The redesign is successful when:

- a trader can identify product risk, quote health, and recent trade activity in under one glance
- the selected product's real `option_mm_core` parameters are editable from the monitor
- risk breach and cancel-stuck conditions are impossible to miss
- common actions require minimal clicks
- the default workspace works on one screen and still supports floating vol views

## Open Questions For Product Direction

Resolved:

1. soft-risk thresholds should be trader-editable
2. quote suppression should be visible at both product and instrument levels
3. the default layout target is a two-screen workstation
