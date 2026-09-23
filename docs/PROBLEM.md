# EpykosEngine — The desk problem (the total tape)

Status: **draft for owner review (2026-09-23)**. Once agreed, this replaces the island fixtures of M3–M5 as the
thing every later milestone builds, verifies and optimises. `WORKLOADS.md` §M1/§M2 stay as the kill-test and
verification fixtures; everything from M3 onward is a stage of this problem.

## 1. Why

Some optimisations only exist when calibration, pricing, risk and scenarios are one recorded graph: shared discount
factors between the solver and the book, the factored Jacobian reused across scenarios, forward-versus-reverse
choice per Jacobian block, incremental re-evaluation when a few quotes move. Optimising stages in isolation bakes
in boundaries that hide those wins. So the problem is defined by its **outputs**, recorded as **one tape per
stage**, and every optimisation from here is judged on that tape.

## 2. Outputs (the contract every stage must meet)

From quotes, FX spots, model parameters (the *state*) and trade / curve / convention tables (the *structure*),
one recorded program produces:

| id | output | form |
|---|---|---|
| O1 | calibrated curves | knot values of every curve in its scheme's variable; solver diagnostics `‖Jᵀr‖∞`, iterations, active-set masks |
| O2 | priced book | PV per trade in trade currency and in USD; per-leg PVs and accrued; aggregates per currency and per netting set |
| O3 | risk ladder | par-delta of every trade and of the book to every calibrating quote of every curve (adjoint through the IFT), FX delta; units: PV change per 1 bp / per 1 % FX |
| O4 | scenario grid | book and per-trade PV under `S` quote scenarios, each = shocked quotes → recalibration → repricing (batch lanes); parallel, twist, butterfly and per-curve shocks |
| O5 | MC exposure (Stage E) | EE / PFE profiles per netting set and per trade from a short-rate model calibrated to O1; MC-based risk (CVA delta to quotes) via the adjoint through the simulation |
| O6 | diagnostics | select masks, margins and arm gaps; per-output-group timings; structure-churn statistics |

A stage passes only when every output it claims is produced by the **same recording** and verified (§6).

## 3. Realism checklist (what "real instruments" means here)

Every item below is *structure*: computed at table-build time from conventions and calendars, never a branch on a
`Scalar`. Each is exercised by at least one instrument in the stage that introduces it.

- compounded-in-arrears RFR coupons (SOFR, €STR, SONIA, TONA) with **observation shift / lookback** and **lockout**;
- **arithmetic-average** RFR coupons (SOFR averaging swaps) — do not telescope, so they exercise per-day domains;
- **payment delay** (e.g. 2 business days), **spot lag**, **fixing lag** for term-rate legs;
- fixed/float **frequency mismatch**, stubs (front/back, short/long), **end-of-month** rule, business-day rolls;
- **seasoned trades**: realised fixings history as data; partially accrued current periods;
- term-rate (EURIBOR-style) legs with fixing in advance and **tenor-basis** swaps (3s6s);
- **cross-currency basis swaps**: MtM-resetting notionals from FX forwards, notional exchange, spread on the
  non-USD leg, USD-collateral discounting of the foreign leg;
- **FX spots** as inputs; **calendars** (NY/SIFMA, TARGET, London, Tokyo) generated from published rules;
  day counts ACT/360, ACT/365F, 30/360, 30E/360, ACT/ACT ISDA; IMM dates;
- **interpolation**: linear zero, log-linear DF, piecewise-constant / linear forward, natural cubic, monotone cubic
  (Hyman), B-spline; **composite curves by region** with a scheme and variable per region;
- **calibration instruments** per curve at researched standard tenors (deposits / fixings, futures or FRAs where
  market practice, OIS swaps, basis swaps, xccy basis swaps); futures convexity stated as a simplification until
  a model provides it.

## 4. Stages (the fixture grows; the tape is always the whole problem)

| stage | adds | curves | book | outputs |
|---|---|---|---|---|
| **A** | USD, one currency, every single-curve mechanism | SOFR OIS (linear zero *and* log-DF *and* monotone cubic *and* composite variants of the same curve) | OIS compounded (shift, lockout, delay), SOFR averaging swaps, seasoned trades; ~2,000 trades | O1–O4 |
| **B** | EUR: multi-curve | €STR OIS; EURIBOR 3M and 6M projection curves calibrated to tenor-basis swaps | EURIBOR swaps (fixing in advance), 3s6s basis swaps | O1–O4 |
| **C** | cross-currency | EURUSD xccy basis curve; EUR discounting under USD collateral | MtM-resetting EURUSD basis swaps; book valued in USD; FX delta | O1–O4 |
| **D** | G4 breadth | GBP (SONIA, ACT/365F, T+0), JPY (TONA, ACT/365F, Tokyo), GBPUSD and USDJPY xccy | ~5,000 trades across four currencies (the `WORKLOADS.md` §MX portfolio) | O1–O4 |
| **E** | Monte Carlo | Hull–White / LGM per currency calibrated to O1 | exposure grid on the Stage D book | O5 |

Quotes are synthetic from the seed (stated as synthetic); conventions and tenor sets are researched with sources
(`docs/G4_BUNDLE.md`).

## 5. What "one tape" means

- One recording per stage. Inputs: every quote of every curve, FX spots, model parameters. Structure: tables.
  Outputs: O1–O4 (O5 in Stage E).
- The implicit node lives **inside** the tape. Its residual sub-program shares the Times / DF domains with the
  pricing of the book; the calibration's final discount factors are the ones the book prices off.
- Curves depend on each other through the implicit node (projection on OIS; xccy on both OIS curves); the
  solver order or joint system is an implementation choice that must be measured, not assumed.
- Scenarios are batch lanes over the same tape. Risk is the adjoint over the whole graph with the IFT through the
  implicit node; the forward-versus-reverse choice per Jacobian block is the optimiser's decision (§7).
- A gate of every stage: the IR shows the cross-stage sharing (one DF domain read by both the residual and the
  book; one factored Jacobian per scenario batch), and the diagnostics report it.

## 6. Verification (per stage, before any optimisation)

- oracle: the same templated maths on `double`; round-trip identity of the IR; differential ball (E0 under the
  reference preset);
- O1: `‖Jᵀr‖∞ < 1e-12`; recovers the generating curve when quotes are its par rates;
- O2: closed forms where they exist (telescoping OIS legs, par swaps at zero PV); conventions against published
  examples (holidays, IMM dates, day-count fractions, a compounded coupon with shift and lockout by hand);
- O3: adjoint vs finite difference (1e-6) and vs forward mode (1e-12); IFT risk vs bump-and-recalibrate (1e-6);
- O4: each scenario lane equals an independent single-scenario run bitwise;
- O5: per-path E0 vs a scalar reference; EE to 1e-12; thread-count independence;
- external oracle (QuantLib, test-only, D2) per instrument type once the stage is stable.

## 7. Optimisation of the totality (after Stage A exists)

Rewrites R1–R7 and the interpreter's planner decisions are expressed as rules with exactness classes; a cost model
is calibrated from the per-domain profiling timers per fingerprint; **equality saturation** over the domain IR
enumerates the compositions of the rules and extracts the cheapest program at the requested exactness class;
AD mode per Jacobian block is a rule; the catalogue is generated from the pipeline's hot groups. Gate: it
rediscovers M1's three kill-path fusions unaided, finds at least one cross-stage optimisation the greedy pipeline
cannot express, and every extracted program passes §6 at its declared class. Performance is gated against
ourselves (D9); the M1 hand kernel and bump-and-recalibrate risk are informational rows.

## 8. Milestones (proposed re-sequencing; supersedes ROADMAP M3–M5 on acceptance)

- **M3 — groundwork and the Stage A tape**: conventions layer, real instruments, all schemes and composites,
  the implicit node, scan domains (compounding written naturally records as a recurrence), Stage A recorded as
  one tape with O1–O4 and every §6 gate.
- **M4 — optimise the totality**: §7 on the Stage A tape.
- **M5 — Stages B, C, D** on the same machinery: multi-curve, xccy, G4; streaming active sets (`pin`,
  `rank_update`, kink 2-cycle); the optimiser re-run on the larger tape.
- **M6 — Stage E**: Monte Carlo exposure and MC risk.
- **MX — the SwapEngine comparison** on Stage D (D21).

## 9. Open choices for the owner

1. Stage A scope: USD only (as above) or USD + EUR so tenor basis and multi-curve dependence exist before the
   optimiser first runs?
2. Calibration instruments in Stage A: OIS swaps only, or also SOFR futures (with stated no-convexity)?
3. Scenario semantics for O4: full recalibration per scenario (as above) — confirm; and `S` for the gate (1,000?).
4. Book sizes: Stage A ~2,000 trades, Stage D ~5,000 — confirm or change.
5. O5 scope: exposure only, or also CVA delta to quotes (MC risk)?
