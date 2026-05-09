# Chi Design Review / Refactor Plan

Goal: Compare the chi production option market making design and optionMMStrategy implementation with the current optionMM design, then apply low-latency lifecycle improvements that keep the strategy framework simple.

## Phases

1. Complete - Map chi strategy interface, engine callbacks, market maker service, and trade service lifecycle.
2. Complete - Map optionMM strategy framework, engine workers, gateway/order lifecycle, and tests/docs.
3. Complete - Compare responsibilities and extension boundaries.
4. Complete - Produce actionable design recommendations for optionMM.
5. Complete - Refactor quote lifecycle follow-up hooks, cancel-give-up alerts, and base order cancel intent.
6. Complete - Verify with focused strategy lifecycle tests, simple MM integration tests, and main binary build.

## Decisions

- Keep the hot path simple: no dynamic service layer, no heap-heavy lifecycle manager, no extra cancel queue.

## Errors Encountered

| Error | Attempt | Resolution |
|---|---|---|
| `D:\workspace\chi\chi\tradeservice\src\TradeServce.cpp` not found | Read prompt path directly | Located actual file at `D:\workspace\chi\chi\tradeservice\src\TradeService.cpp` |
