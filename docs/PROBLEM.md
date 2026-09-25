# EpykosEngine — The desk problem (the total tape)

Status: **agreed by the owner (2026-09-23; D35)**. This replaces the island fixtures of M3–M5 as the
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
| O5 | MC exposure (Stage D) | EE / PFE profiles per netting set and per trade from a short-rate model calibrated to O1; MC-based risk (CVA delta to quotes) via the adjoint through the simulation |
| O6 | diagnostics | select masks, margins and arm gaps; per-output-group timings; structure-churn statistics |

A stage passes only when every output it claims is produced by the **same recording** and verified (§6).

## 3. Realism checklist (what "real instruments" means here)

Every item below is *structure*: computed at table-build time from conventions and calendars, never a branch on a
`Scalar`. Definitions are data (D36): conventions, curve definitions and instrument blueprints live in JSON under
`blueprints/`; the rule kinds and coupon mechanics are code. Each is exercised by at least one instrument in the stage that introduces it.

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
| **A** | USD + EUR: every single-curve mechanism and multi-curve dependence | SOFR OIS; €STR OIS; EURIBOR 3M and 6M projection curves calibrated to tenor-basis swaps and EURIBOR swaps; deposits / fixings and futures (convexity 0, stated) on the front year; each curve on a stated scheme, and the SOFR curve also recorded on log-DF, monotone cubic and a composite variant | ~2,000 trades: SOFR OIS compounded (shift, lockout, payment delay), SOFR averaging swaps, €STR OIS, EURIBOR swaps (fixing in advance), 3s6s basis swaps; seasoned trades with fixings history | O1–O4 |
| **B** | cross-currency | EURUSD xccy basis curve; EUR discounting under USD collateral; FX spot input | MtM-resetting EURUSD basis swaps; book valued in USD; FX delta | O1–O4 |
| **C** | G4 breadth | GBP (SONIA, ACT/365F, T+0), JPY (TONA, ACT/365F, Tokyo), GBPUSD and USDJPY xccy | ~5,000 trades across four currencies (the `WORKLOADS.md` §MX portfolio) | O1–O4 |
| **D** | Monte Carlo | Hull–White / LGM per currency calibrated to O1 | exposure grid on the Stage C book; CVA delta to quotes | O5 |

Scenario grid (O4): `S = 1,000` scenarios, each a full recalibration of every curve followed by repricing, as batch
lanes; first-order (IFT) scenario PVs are an informational row, never the definition.

**Stage A, as built (M3, 2026-09-23; D44, D45):** one recording — 148 inputs (70 free quotes), 8,191 outputs,
27,459,283 raw nodes → 517,036 after the E0 passes; IR of 67 domains / 423,235 values / 5 scan domains (1,107
chains, 255,959 rows); two DF domains (177 knot-time + 16,917 interpolated rows) read by both the calibration
residuals and the book, `duplicates(Exp)` = 0. Every gate of §6 below holds on this tape (`docs/RESUME.md` §5 "M3
result"); the SOFR curve is additionally recorded on log-DF, monotone-cubic and composite variants (gated on 300 of
the 2,000 trades, D44 simplification 2). Labelled simplifications carried into M4: no FX (§3 line 43 is Stage B); the
solve tolerance is 1e-13, not 1e-14, on the 30-year daily products' own rounding floor; O3's forward-mode comparison
is `Dual<70>` on the templated maths, not a tangent interpreter over the IR.

Quotes are synthetic from the seed (stated as synthetic); conventions and tenor sets are researched with sources
(`docs/G4_BUNDLE.md`).

## 5. What "one tape" means

- One recording per stage. Inputs: every quote of every curve, FX spots, model parameters. Structure: tables.
  Outputs: O1–O4 (O5 in Stage D).
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

**As built (2026-09-24): fail.** All of the above shipped as rules with exactness classes, differential tests
and mutation tests (D47–D55; 43/43 mutants caught). Two of the three gate clauses in this section's own text
miss: rediscovery ties the cost model's own estimate but misses its 1.02x wall-clock target (1.0277x/1.0427x),
and the one cross-stage candidate found is 0.999716x under this fingerprint's real fitted cost model — noise,
not a win, once D57 corrected the synthetic-cost-model pricing bug that had made it look like one. Every
extracted program does pass §6 at its declared class, and the self-regression gate against the M3 baseline (D9)
passes (17/17 benchmarks). Full account: `docs/RESUME.md` §5 "M4 result", `docs/DECISIONS.md` D59.

**Post-M4 correction to the first of those two misses (D63, 2026-09-25).** The rediscovery clause now PASSES on
measured wall clock — 1.0164x at B=1 and 0.9985x at B=64 against the M1 greedy default, against the 1.02x target
D59 recorded as missed at 1.0277x/1.0427x — because the cost model had been pricing `exec::Interpreter`'s step
pairing at exactly zero, so every fusion candidate tied with its unfused twin and extraction's tie-break kept the
unfused one. Read D63 before quoting that pass: it is met BY IDENTITY (the extracted candidate is the default
plan's own execution, not something better than it), and the OTHER miss is unchanged — the search's best Stage A
candidate improved from 0.999055x to 0.991029x on the full tape, which is still noise against the cost model's
own error, and `cross_stage_wins` is still 0. The model's prediction error improved from 84.4%/69.3% to
58.6%/49.7% and still misses this section's <25% target by about a factor of two.

## 8. Milestones (D35; supersedes ROADMAP M3–M5 as first written)

- **M3 — groundwork and the Stage A tape**: conventions layer with sources, real instruments, all schemes and
  composites, scan domains (compounding written naturally records as a recurrence), the implicit node with
  multi-curve dependencies, Stage A recorded as one tape producing O1–O4, every §6 gate, and the cross-stage
  sharing visible in the IR.
- **M4 — optimise the totality**: §7 on the Stage A tape.
- **M5 — Stages B and C, streaming**: xccy and G4 on the same machinery; `pin` / `rank_update` active sets and the
  kink 2-cycle fixture on a composite curve; the optimiser and catalogue re-run on the Stage C tape.
- **M6 — Stage D**: Monte Carlo exposure and CVA delta through the simulation and the IFT.
- **MX — the SwapEngine comparison** on Stage C (D21).

## 9. Choices made by the owner (2026-09-23)

1. Stage A is USD + EUR, so multi-curve dependence and tenor basis exist before the optimiser first runs.
2. Calibration instruments include deposits / fixings and futures on the front year with the convexity adjustment
   set to zero and labelled as a simplification; OIS, EURIBOR and basis swaps beyond.
3. O4 is 1,000 scenarios with full recalibration each; IFT first-order PVs are informational only.
4. O5 includes CVA delta to quotes, not just exposure profiles.
5. Book sizes: ~2,000 trades at Stage A, ~5,000 at Stage C.
