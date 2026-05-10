# Architecture Latency Implementation Pass

## Goal
Implement the highest-value recommendations from `docs/architecture_latency_review_20260510.md` while preserving the low-latency constraints.

## Assumptions
- The review document is the implementation guide.
- Changes should start with correctness and measurable latency hygiene before riskier gateway refactors.
- No dynamic allocation, locks, exceptions, or RTTI should be added to strategy/gateway hot paths.
- Existing review/planning artifacts are user-visible work and must be preserved.

## Success Criteria
- Fix pre-trade risk fill bookkeeping and reduce the strategy-side pre-trade risk check cost.
- Add useful latency regression thresholds or reporting around the existing latency benchmark.
- Document the implementation and test result under `docs/`.
- Run focused tests/builds where available and record blockers.

## Phases
| Phase | Status | Notes |
|---|---|---|
| Inspect implementation targets | complete | Read risk, strategy lifecycle, latency tests, and build presets. |
| Implement pre-trade risk improvements | complete | Fixed fill bookkeeping and replaced hot check scans with fixed indexes/summaries. |
| Add latency threshold support | complete | Added configurable p99/capture thresholds to `tests/test_latency.cpp`. |
| Document implementation | complete | Added `docs/architecture_latency_implementation_20260510.md`. |
| Verify | complete | Focused WSL tests and release latency preset passed; warnings recorded. |

## Errors Encountered
| Error | Attempt | Resolution |
|---|---|---|
| Best-side recompute still included removed order | Focused `test_pre_trade_risk` run | Clear the slot's `used` flag before recomputing the instrument side summary. |
| Hedge fill did not clear pre-trade risk state | Focused `test_option_mm_core` run | Add hedge-specific `pre_risk_->on_order_fill` in `OptionMMCoreStrategy::on_fill_impl`. |
