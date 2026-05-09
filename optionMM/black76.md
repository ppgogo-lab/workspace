# Black-76 Pricing and Greeks

This document records the Black-76 price and Greek formulas used by optionMM.
The conventions match the legacy chi implementation for stored Greeks.

## Inputs

```text
F     = forward/futures price
K     = strike
T     = time to expiry in years
r     = risk-free rate
sigma = volatility
D     = exp(-r * T)
Om    = option contract multiplier
Fm    = underlying/future contract multiplier
DY    = trading days per year, default 252
s     = +1 for call, -1 for put

d1 = [ln(F / K) + 0.5 * sigma^2 * T] / [sigma * sqrt(T)]
d2 = d1 - sigma * sqrt(T)

N(x) = standard normal CDF
n(x) = standard normal PDF
```

## Price

```text
V = D * s * [F * N(s * d1) - K * N(s * d2)]
```

For calls:

```text
V_call = D * [F * N(d1) - K * N(d2)]
```

For puts:

```text
V_put = D * [K * N(-d2) - F * N(-d1)]
```

## Greeks

```text
Std delta = D * s * N(s * d1)
Delta     = Std delta * Om / Fm
Delta cash = Std delta * Om * F
```

```text
Std Gamma = D * n(d1) / [F * sigma * sqrt(T)]
Gamma     = Std Gamma * Om * F * 0.01 / Fm
Gamma cash = Std Gamma * Om * F * F * 0.01
```

```text
Vega      = D * F * n(d1) * sqrt(T) * 0.01
Vega cash = Vega * Om
```

```text
Theta      = [-D * F * n(d1) * sigma / (2 * sqrt(T)) + r * V] / DY
Theta cash = Theta * Om
```

```text
Rho      = -T * V * 0.01
Rho cash = Rho * Om
```

```text
Vanna = D * n(d1) * (-d2 / sigma)
```

```text
Volga = D * F * n(d1) * sqrt(T) * d1 * d2 / sigma * 0.01
```

```text
Charm = -D * n(d1) * [r / (sigma * sqrt(T)) - d2 / (2 * T)]
```

## Units and Scaling

- `Vega`, `Rho`, and `Volga` are stored with `0.01` scaling to match chi.
- `Theta` is stored per trading day using `DY`.
- Cash Greeks use the option multiplier `Om`.
- `Delta` and `Gamma` are scaled by the ratio between option and future multipliers.

## Degenerate Inputs

When `F`, `K`, `sigma`, `T`, or `sqrt(T)` is effectively zero, optionMM returns
discounted intrinsic value, intrinsic boundary delta, scaled delta, and delta
cash. Other Greeks remain zero.
