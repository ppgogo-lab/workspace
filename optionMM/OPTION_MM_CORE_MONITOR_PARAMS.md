# option_mm_core Monitor Parameter Review

## Scope

This review is based on the backend implementation of `option_mm_core`, not on the older simple MM surface.

Primary sources:

- `src/strategy/option_mm_core.cpp`
- `include/strategy/mm_params.h`
- `include/common/config.h`
- `src/common/config.cpp`
- `src/monitoring/grpc_server.cpp`
- `proto/trading.proto`
- `src/gui/trader_main_window.cpp`

## Key Findings

1. `SetStrategyParams` is implemented as a full overwrite, not a partial update.
   `src/monitoring/grpc_server.cpp` only fills 6 fields in `MMParamsConfig`, then calls `AtomicMMParams::apply()`, which writes every field. Any field not sent by the monitor is reset to its struct default, not preserved from YAML or the previous runtime state.

2. The current monitor exposes `bid_spread` and `ask_spread`, but `option_mm_core` does not read them at runtime.
   In this strategy, quote width comes from `base_half_spread_ticks`, `min_half_spread_ticks`, and `max_half_spread_ticks`. `bid_spread` and `ask_spread` only participate in a YAML parse fallback when `base_half_spread_ticks` is omitted.

3. The current monitor snapshot and RPC surface are incomplete for `option_mm_core`.
   The proto/UI only expose:
   `bid_spread`, `ask_spread`, `hedge_delta_threshold`, `quote_volume`, `max_position`, `enabled`
   but the strategy actually depends on a much larger set of MM parameters.

4. Session schedule is parsed but not currently driven into the strategy.
   `option_mm_core` handles `SessionOpen` and `SessionClose`, but `TradingEngine::timer_loop()` only emits `HedgeCheck` and `QuoteRefresh`. `timer.session_schedule` should not be presented as an active trader control until that wiring exists.

## Actual option_mm_core Parameter Inventory

### 1. Quote Shape

These are the core quoting knobs traders should be able to adjust per product.

- `base_half_spread_ticks`
  Primary quote half-width before dynamic widening.
- `min_half_spread_ticks`
  Lower clamp for quote width.
- `max_half_spread_ticks`
  Upper clamp for quote width.
- `follow_weight`
  Blends theo with live option market.
  `0.0` means pure theo, `1.0` means pure market follow.
- `inventory_skew_per_lot_ticks`
  Shifts quote center away from current inventory.
- `market_width_widen_threshold_ticks`
  Starts widening when the live option market becomes too wide.

Backend usage:

- `src/strategy/option_mm_core.cpp`: lines around quote construction in `build_decision()`

### 2. Size And Inventory Shaping

These control size and how the strategy reduces risk before full suppression.

- `quote_volume`
  Base quote size on both sides and also the hedge order size cap.
- `warning_position`
  Near-limit threshold where inventory pressure and one-sided behavior start.
- `max_position`
  Hard local position stop for an individual option.
- `use_one_sided_at_limits`
  Enables reducing-side-only quoting once `warning_position` is reached.

Backend usage:

- `src/strategy/option_mm_core.cpp`: local position checks, inventory pressure, one-sided logic

### 3. Quote Churn / Requote Control

These should be exposed because they materially change fill rate versus churn.

- `requote_price_epsilon_ticks`
  Minimum price move required before replacing a live quote.
- `min_quote_interval_ms`
  Minimum delay between quote updates.
  Also reused as the minimum hedge cadence and as part of temporary suppression hold time.

Backend usage:

- `src/strategy/option_mm_core.cpp`: `send_quote()`, `is_material_change()`, hedge pacing, underlying shock hold

### 4. Hedge And Product Suppression

These are the main product-level risk controls inside the strategy.

- `hedge_delta_threshold`
  Local aggregate delta threshold.
  Above this, the strategy sends an immediate hedge and suppresses the product if exposure remains breached.
- `product_vega_threshold`
  Local aggregate vega threshold for whole-product suppression.
- `underlying_move_widen_threshold_ticks`
  Triggers a temporary suppression window after a sharp move in the underlying reference.
- `enabled`
  Master strategy on/off gate for the product.

Backend usage:

- `src/strategy/option_mm_core.cpp`: `on_signal()`, `maybe_trigger_hedge()`, `product_exposure_breached()`, `product_temporarily_suppressed()`

## Shared Backend Controls That Also Affect option_mm_core

These are not part of per-product MM quoting, but the strategy behavior still depends on them.

### 5. Supervisory Risk Gates

`option_mm_core` checks `post_risk_->any_breach()` and cancels/suppresses quotes when the shared risk monitor reports a breach.

Shared runtime controls:

- `risk.soft.max_net_position`
- `risk.soft.max_delta`
- `risk.soft.max_gamma`
- `risk.soft.max_vega`

Current backend status:

- These are already exposed through `SetRiskThreshold`.
- They are system-level supervisory limits, not strategy-local MM tuning knobs.

### 6. Hard Execution Limits

Quote and hedge submission are still filtered by pre-trade hard risk.

- `risk.hard.max_volume_per_order`

Effect:

- Rejects quotes whose bid or ask size exceeds the hard limit.
- Rejects hedge orders whose size exceeds the hard limit.

Current backend status:

- Not exposed in the current trader monitor.
- This is a shared hard-risk control, not a per-product MM control.

### 7. Engine Timing Inputs

The strategy receives timer events from the engine, so these global controls still affect behavior.

- `timer.quote_refresh_interval_ms`
  Periodic re-evaluation interval.
- `timer.hedge_check_interval_ms`
  Periodic hedge evaluation interval.

Current backend status:

- Parsed from YAML and used by `TradingEngine::timer_loop()`
- Global engine settings, not per-product strategy params

## Parameters That Should Be In The Trader MM Panel

For `option_mm_core`, the trader-facing per-product parameter set should be:

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

## Parameters That Should Not Be The Main Trader MM Panel

- `bid_spread`
  Legacy/simple-MM style field. Not read by `option_mm_core` hot path.
- `ask_spread`
  Same issue as `bid_spread`.
- `risk.soft.*`
  Should remain in a separate risk control surface.
- `risk.hard.max_volume_per_order`
  Should remain in a separate hard-risk/admin surface.
- `timer.quote_refresh_interval_ms`
  Shared engine timing control, not product-local MM tuning.
- `timer.hedge_check_interval_ms`
  Shared engine timing control, not product-local MM tuning.
- `timer.session_schedule`
  Not active yet for this strategy because the engine does not emit session open/close timer events.

## Current Monitor Gap Summary

Current proto/UI surface:

- `bid_spread`
- `ask_spread`
- `hedge_delta_threshold`
- `quote_volume`
- `max_position`
- `enabled`

Missing but required for real `option_mm_core` control:

- `base_half_spread_ticks`
- `min_half_spread_ticks`
- `max_half_spread_ticks`
- `follow_weight`
- `inventory_skew_per_lot_ticks`
- `market_width_widen_threshold_ticks`
- `warning_position`
- `use_one_sided_at_limits`
- `requote_price_epsilon_ticks`
- `min_quote_interval_ms`
- `product_vega_threshold`
- `underlying_move_widen_threshold_ticks`

## Recommended Backend Change Before Extending The UI

Fix `SetStrategyParams` semantics first.

Recommended rule:

- Read the current `mm_params(idx).snapshot()`
- Patch only the fields present in the request
- Re-apply the merged result

Without that fix, any monitor-side edit will silently reset hidden `option_mm_core` parameters back to defaults.
