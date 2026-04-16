# agent.md

## 1. Project Overview
**Low Latency design is high priority**
OptionMM is an ultra-low latency (<2μs tick-to-trade), high-frequency trading system for market making in Chinese commodity and equity index options markets (SHFE, DCE, CZCE, CFFEX). The system uses Black-76 pricing, lock-free ring buffers, SIMD-optimized math (AVX2/AVX-512), and core pinning to achieve sub-microsecond latency on the critical path.

**Key constraints:**
- Zero dynamic memory allocation on critical path
- No locks on critical path (SPSC ring buffers only)
- No exceptions or RTTI on critical path
- All hot threads pinned to dedicated CPU cores
- Target hardware: Intel Xeon Gold 6544Y with Solarflare NIC + OpenOnload

## 2. Code and build

- Code: add detail comments for every task, especially special design for low latency
- Build: when in dev stage, build backend in WSL + Ubuntu, while building ui in windows.

## 3. Think Before Coding

**Don't assume. Don't hide confusion. Surface tradeoffs.**

Before implementing:
- State your assumptions explicitly. If uncertain, ask.
- If multiple interpretations exist, present them - don't pick silently.
- If something is unclear, stop. Name what's confusing. Ask.

## 4. Simplicity First

**Minimum code that solves the problem. Nothing speculative.**

- No features beyond what was asked.
- No "flexibility" or "configurability" that wasn't requested.
- No error handling for impossible scenarios.

## 5. Goal-Driven Execution

**Define success criteria. Loop until verified.**

Transform tasks into verifiable goals:
- "Add validation" → "Write tests for invalid inputs, then make them pass"
- "Fix the bug" → "Write a test that reproduces it, then make it pass"
- "Refactor X" → "Ensure tests pass before and after"

---

