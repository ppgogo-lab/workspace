# UI Spec

## Goal
Build a high-visibility, low-latency trader monitor GUI for market making on Windows.

Priorities:
- performance first
- live data streaming
- colorful, attention-oriented trader visuals
- fast manual interaction with minimal clicks

## Runtime Model
- Platform: native Windows desktop
- UI stack: Qt 6 Widgets
- Transport: gRPC client over snapshot + server streaming
- Process model: GUI runs as a separate Windows monitor client and connects to the trading backend remotely

## Main Views

### Top Toolbar
- Product selector
- Connection state
- Quick desk status indicator

### T-Table
- Central primary view
- Product-filtered option ladder
- Rows sorted by strike
- Middle column shows strike and instrument label
- Left side shows bid market data
- Right side shows ask market data
- Extra columns show theo, delta, and current position
- Strong cell coloring to help traders scan quickly

Current columns:
- `BidQty`
- `BidPx`
- `Theo`
- `Strike/Inst`
- `AskPx`
- `AskQty`
- `Delta`
- `Pos`

### Quick Order / Strategy Dock
- Product-filtered instrument picker
- Side selector
- Price editor
- Volume editor
- `Send Buy`
- `Send Sell`
- `Start MM`
- `Stop MM`

### Strategy Params Panel
- `Bid Spread`
- `Ask Spread`
- `Hedge Delta`
- `Quote Volume`
- `Max Position`
- `Apply Params`

### ORC Wing / Vol Curves
- Central lower panel
- Grid of up to 9 curve panels
- Each panel labeled by future code or underlying/product label
- Real-time curve rendering from streamed vol surface data

### Positions / Greeks
- Hierarchical tree view
- Top node shows portfolio-level summary for selected product
- Child nodes grouped by underlying/product
- Leaf nodes show instrument-level position and Greeks

Tree columns:
- `Node`
- `Net`
- `Avg`
- `UPnL`
- `Delta`
- `Gamma`
- `Vega`

### Bottom Blotters
- Orders table
- Quotes table
- Trades table
- Filtered to selected product

## Interaction Design

### T-Table to Order Ticket
- Clicking a T-table row selects that instrument in the order panel
- Clicking bid cells prefills a sell-side order
- Clicking ask cells prefills a buy-side order
- Clicking other cells prefills the theo price

### Manual Order Entry
- Trader selects instrument, side, price, volume
- GUI sends `SendManualOrder`
- Status label shows success or failure

### Strategy Control
- `Start MM` sends `StartStrategy`
- `Stop MM` sends `StopStrategy`
- Strategy params panel sends `SetStrategyParams`

### Position/Risk Navigation
- Position tree groups instruments by underlying/product for faster scanning
- Portfolio-level and grouped Greek summaries are visible without opening separate windows

## Backend Message Model

### Snapshot
Used on startup and periodically refreshed:
- instrument metadata
- greeks
- positions
- portfolio greeks
- MM params

### Streaming
Long-lived server streams:
- ticks
- greeks
- positions
- orders
- trades
- quotes
- vol surfaces

### Unary Actions
- `SendManualOrder`
- `StartStrategy`
- `StopStrategy`
- `SetStrategyParams`

## What Has Been Implemented

### Backend / Monitor API
- Monitoring fan-out topics added in engine for ticks, orders, quotes, and trades
- gRPC monitor streams implemented for:
  - ticks
  - greeks
  - positions
  - orders
  - trades
  - quotes
  - risk alerts
  - vol surfaces
- Snapshot extended with instrument metadata
- Dedicated `StreamTrades` RPC added

### GUI
- Native Qt trader dashboard scaffold implemented
- Product-filtered T-table implemented
- Metadata-driven instrument picker implemented
- T-table click-to-prefill order workflow implemented
- Strategy start/stop controls implemented
- Strategy parameter editing and apply flow implemented
- Vol curve grid widget implemented
- Hierarchical positions and Greeks tree implemented
- Orders, quotes, and trades blotters implemented
- Product filtering across main views implemented

### Windows Build Path
- Native Windows Qt 6.8.3 installed
- Visual Studio 2022 Build Tools installed
- Native Windows `protobuf` and `grpc` installed with `vcpkg`
- Windows-only CMake path added for GUI build
- Windows-compatible protobuf/gRPC generated files regenerated
- Native GUI executable built successfully:
  - [optionmm_trader_gui.exe](D:/workspace/optionMM/build-win-gui/optionmm_trader_gui.exe:1)

### Windows Helper Scripts
- Build script:
  - [build_windows_gui.cmd](D:/workspace/optionMM/scripts/build_windows_gui.cmd:1)
- Run script:
  - [run_windows_gui.cmd](D:/workspace/optionMM/scripts/run_windows_gui.cmd:1)

## How To Build And Run

Build:
```cmd
scripts\build_windows_gui.cmd
```

Run:
```cmd
scripts\run_windows_gui.cmd
scripts\run_windows_gui.cmd 127.0.0.1:50051
```

## Current Gaps
- No full multi-window trader workspace management yet
- No dedicated quote-status visualization inside the T-table beyond current market/position coloring
- No auto-fit start/stop button set in the GUI yet
- No manual ORC Wing parameter editor beyond current MM parameter controls
- Position hierarchy is grouped by underlying/product, but not yet a deeper desk/account/expiry hierarchy
- Risk alerts are streamed server-side but not yet surfaced in a dedicated GUI alert panel
- The Windows build is for the monitor client only, not the Linux trading engine

## Next Recommended UI Work
- Add dedicated quote-state columns and row flashing for stale/reject/fill conditions
- Add auto-fit controls and ORC Wing parameter editors
- Add future/expiry selectors for vol panels
- Add dedicated risk alert dock
- Add saved layouts and detachable windows for trader workflows
- Add tighter keyboard shortcuts for rapid order entry
