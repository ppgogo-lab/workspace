  The timeout/cancel behavior lives inside the existing option MM quote state machine, not in a new thread. The core state and hardcoded thresholds are in /D:/workspace/optionMM/include/strategy/option_mm_core.h:49: QUOTE_MAX_LIVE_NS =
  3s, CANCEL_RETRY_NS = 1s, MAX_CANCEL_ATTEMPTS = 3, plus per-instrument fields like live_since_ts, cancel_last_send_ts, cancel_attempts, and cancel_target_quote_id.

  The strategy evaluates this in the normal strategy loop. maybe_quote() now calls manage_quote_lifecycle() first in /D:/workspace/optionMM/src/strategy/option_mm_core.cpp:325. That means:

  - If a quote is Live for more than 3 seconds, it sends a cancel.
  - If a quote is already CancelPending, it waits 1 second, then resends cancel.
  - After 3 failed cancel attempts, it moves to CancelFailed, suppresses further quoting for that instrument, and emits one warning alert.
  - Because this runs on signals and QuoteRefresh timer events, the actual trigger time is threshold + timer/scheduling granularity, not a separate exact wall-clock callback.

  Cancel / Fill Interaction
  Cancels now target the original quote id, not a fresh id, in /D:/workspace/optionMM/src/strategy/option_mm_core.cpp:547. That is important because the gateway needs to map the cancel to the live quote. On fills, the strategy decrements
  remaining bid/ask working volume in /D:/workspace/optionMM/src/strategy/option_mm_core.cpp:169, and quote_fully_filled() in /D:/workspace/optionMM/src/strategy/option_mm_core.cpp:662 stops the retry path immediately once both sides are
  fully filled. So a fully-filled quote does not keep retrying cancel and does not raise the warning alert.

  Alert Path
  The alert is modeled as a strategy-originated system alert, then exposed through the existing risk alert stream:

  - Per-product alert topics were added in /D:/workspace/optionMM/include/engine/trading_engine.h:93 and wired into strategy init in /D:/workspace/optionMM/src/engine/trading_engine.cpp:139.
  - The strategy publishes QuoteCancelGiveUp once when it gives up in /D:/workspace/optionMM/src/strategy/option_mm_core.cpp:645.
  - RiskAlert got a new enum QUOTE_CANCEL_GIVE_UP in /D:/workspace/optionMM/proto/trading.proto:35.
  - The gRPC StreamRiskAlerts endpoint now merges normal post-trade breaches with these custom alerts in /D:/workspace/optionMM/src/monitoring/grpc_server.cpp:421.

  One gateway-specific fix was needed for FEMAS: zero-volume quote cancels now call the real quote cancel API instead of going through quote insert, in /D:/workspace/optionMM/src/gateway/femas_gateway.cpp:280.

  The behavior is covered by focused tests in /D:/workspace/optionMM/tests/test_option_mm_core.cpp:231.
  
  The quote lifecycle is managed as a per-instrument finite state machine inside OptionMMCoreStrategy, and all state transitions happen on the product’s single strategy thread. The engine guarantees that by serializing pricing signals,
  timer events, and gateway callbacks through /D:/workspace/optionMM/src/engine/trading_engine.cpp:430. The gateway dispatcher sends quotes out and routes QuoteAck / QuoteCancel / QuoteReject / fills back to that same strategy thread
  in /D:/workspace/optionMM/src/engine/trading_engine.cpp:492.

  The per-instrument state is in /D:/workspace/optionMM/include/strategy/option_mm_core.h:49. The important states are:

  - Idle: nothing working.
  - ReplacePending: a new quote was sent, waiting for ack.
  - Live: the quote is acknowledged and working.
  - CancelPending: a cancel was sent, waiting for cancel confirmation.
  - CancelFailed: cancel was retried 3 times and the strategy gave up.
  - Suppressed: quoting is intentionally off because of stale theo, invalid market, risk, session state, etc.

  The normal lifecycle is:

  1. on_signal() updates theo/greeks/underlying context and calls maybe_quote() in /D:/workspace/optionMM/src/strategy/option_mm_core.cpp:83.
  2. maybe_quote() first runs manage_quote_lifecycle() in /D:/workspace/optionMM/src/strategy/option_mm_core.cpp:325 and /D:/workspace/optionMM/src/strategy/option_mm_core.cpp:591. This handles timeout cancels, cancel retries, and cancel
     give-up before any new quoting decision.
  3. If lifecycle handling does not block, build_decision() decides whether to quote, cancel-only, or stay suppressed in /D:/workspace/optionMM/src/strategy/option_mm_core.cpp:342.
  4. send_quote() pushes a quote into the quote ring buffer in /D:/workspace/optionMM/src/strategy/option_mm_core.cpp:493. It records the pending quote id and caches price/size immediately so later fills/cancel logic has a working view
     even before ack.
  5. on_quote_ack() promotes the quote to Live and records live_since_ts in /D:/workspace/optionMM/src/strategy/option_mm_core.cpp:208.

  Replacement is conservative: the strategy keeps at most one live quote per instrument. If a new price is needed while a quote is already Live, send_quote() does not send a replacement immediately; it sends cancel first and waits. Only
  after on_quote_cancel() resets the old quote state does it call maybe_quote() again and allow the new quote in /D:/workspace/optionMM/src/strategy/option_mm_core.cpp:245.

  Timeout cancel behavior is:

  - If a quote stays Live for more than 3 seconds, manage_quote_lifecycle() calls send_cancel() in /D:/workspace/optionMM/src/strategy/option_mm_core.cpp:591.
  - send_cancel() targets the existing quote id, not a fresh id, in /D:/workspace/optionMM/src/strategy/option_mm_core.cpp:547. That matters because the gateway cancel path is keyed by the live quote id.
  - If the quote is still not canceled after 1 second, the cancel is resent.
  - After 3 tries, the state becomes CancelFailed; build_decision() then refuses to generate new quotes for that instrument in /D:/workspace/optionMM/src/strategy/option_mm_core.cpp:345. The design assumption is: if cancel is stuck, the
    market may still have a live quote, so the strategy must stop layering more exposure on top.

  Fills are integrated directly into the lifecycle. on_fill() decrements the remaining bid/ask working size for the quote in /D:/workspace/optionMM/src/strategy/option_mm_core.cpp:138. If both sides are fully filled, quote_fully_filled()
  The alert path is separate from the state machine itself:

  - When cancel give-up happens, the strategy publishes a SystemAlert in /D:/workspace/optionMM/src/strategy/option_mm_core.cpp:645.
  - The engine owns one alert topic per product in /D:/workspace/optionMM/include/engine/trading_engine.h:93 and wires it into the strategy in /D:/workspace/optionMM/src/engine/trading_engine.cpp:139.
  - gRPC exposes that as RiskAlert.QUOTE_CANCEL_GIVE_UP in /D:/workspace/optionMM/proto/trading.proto:35 and /D:/workspace/optionMM/src/monitoring/grpc_server.cpp:421.

  One gateway detail matters here: FEMAS now treats a zero-volume quote sent through send_quote() as a real quote cancel request by routing it to cancel_order(quote_id, instrument_id) in /D:/workspace/optionMM/src/gateway/
  femas_gateway.cpp:280. Without that, retries would not actually hit the exchange quote-cancel path.
  
  