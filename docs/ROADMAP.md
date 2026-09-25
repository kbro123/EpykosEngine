# EpykosEngine — Roadmap

Ordered by risk: each milestone is chosen to kill the plan as cheaply as possible if it is going to die.
Every milestone lists its **exit gate** and its **kill criterion**.

## M0 — Design (this commit)
Design, decisions, prior art, roadmap. No code.

## M1 — Kill test: can a generic program match a hand-fused kernel?
Build the minimum to answer the one question that decides the architecture. Fixture: `WORKLOADS.md` §M1 (D16).
- `Rec` recording scalar (constants as leaves, no implicit conversion, `RecBool`, taint).
- A minimal templated rates kernel: linear-in-zero-rate curve on 12 knots, compounded OIS swaps written
  uncollapsed (the fusion pass telescopes them), fixed/float legs, one seasoned bucket with a realised first fixing.
- Signature pass → domain IR, with the round-trip identity check.
- Tiled interpreter (no catalogue); runtime batch width; tile size swept 128/256/512 (D15).
- A hand-fused reference kernel for the same book (gather → fma → segment_sum, shared reciprocal), written from
  the description in `DESIGN.md` only (D11).
- Benchmarks: 1k-swap book, single state and batch of 64 states, fingerprinted (D13).
- Scaffold: CMake + Ninja, `scripts/bootstrap.sh`, GoogleTest, Google Benchmark, fingerprint script, GitHub
  Actions (tests only, macOS clang + Linux GCC) (D12).

**Exit gate:** round-trip identity on every test book; interpreter values E0 vs templated `double` (`-ffp-contract=off`).
**Go:** interpreter within **1.3×** of the hand-fused kernel single-state, within **1.1×** batched.
**Kill / redesign:** > 2× single-state ⇒ rethink tile size and intermediate layout before anything else (at most
three iterations, each recorded); if still > 2×, the catalogue carries the design and M3 moves ahead of M2 (D18).
**Result (2026-09-23): go.** On fingerprint `d448afd70180` (Xeon W-3223, 8 cores / 16 threads, Apple clang 21, `-O3 -march=x86-64-v3 -fno-math-errno`, load 2.4/16) the interpreter is **1.044×** the hand-fused kernel single-state (56.60 vs 54.23 µs, ≤ 1.3) and **1.079×** batched (1584.0 vs 1467.7 µs per 64 states at tile 256 / lane tile 32, ≤ 1.1), libm `std::exp` on both sides (D27; E1-vs-E1 with the hand kernel's polynomial exp 1.077 / 1.240, informational), after three kill-path iterations (1.291 / 1.905 → 1.207 / 1.340 → 1.101 / 1.126 → 1.047 / 1.075, re-measured 1.044 / 1.079 at b32182a, the first commit with green CI); round-trip identity and the E0 / E1 gates pass under the reference preset; `docs/RESUME.md` §5 "M1 result", `bench/results/d448afd70180/README.md`.

## M2 — Verification harness and adjoints
Fixture: `WORKLOADS.md` §M2.
- Differential tester (randomised state ball), finite-difference and forward-mode (dual-number `Scalar`) adjoint checks.
- Mechanical adjoint generation per group; pull-transposes for scatters.
- Mutation testing on every rewrite rule.
- Perf-gate tooling: fingerprinting, baselines, self-regression + absolute targets.

**Exit gate:** adjoint vs FD at 1e-6 rel, vs forward mode at 1e-12; every mutated rule caught.
**Result (2026-09-23): pass.** On fingerprint `d448afd70180` (Xeon W-3223, 8 cores / 16 threads, Apple clang 21, `-O3 -march=x86-64-v3 -fno-math-errno`; the reference preset's `-ffp-contract=off` for the E0 gates) at `de1ed79` (integrate/m1-m5 tip; engine code as of `359f321`): the mechanical adjoint's `d(book)/dz` vs central finite difference (h = 1e-6) within **2.89e-9** relative at the record point and 8 ball states, the 50 sampled swaps within 1.67e-8 (gate 1e-6), linearity in the seed 1.3e-14; the full 1,001 × 12 Jacobian vs forward mode (`Dual`) within **2.06e-13** (gate 1e-12; transpose consistency 1.29e-13, dot-product identity 2.2e-14; forward outputs bitwise the `Dual` value channel); **13 of 13 registered mutants caught** (8 in the passes, 5 in the adjoint; 21 gates, `scripts/mutation_test.sh`, exit 0 — the two adjoint rules the book cannot exercise are caught by the near-miss shapes fixture); differential tester: interpreter and replay bitwise vs the templated `double` maths at all 256 ball draws at B = 1 and B = 64, E1 within D26 under release (worst swap 1.55e-15 of the leg scale); batched adjoint bitwise across 42 (tile, lane tile) configurations; 39/39 tests under release, reference and debug; perf gate (D29, D34): the M1 numbers are the baseline, the re-measured M1 benches pass at 0.945–1.020× of it (D27 pair 1.048 / 1.076), value + adjoint(book) 418.5 µs at B = 1 = 7.38× the value-only interpreter and 9.31× at B = 64 (informational, D9); Q6 review: 11 findings, 1 actionable (the adjoint baseline keyed by an ad hoc run name, D34), fixed and re-verified. `docs/RESUME.md` §5 "M2 result", `bench/results/d448afd70180/m2.md`.

## M3 — Groundwork and the Stage A tape (`PROBLEM.md` §4 Stage A; D35)
Supersedes the island M3–M5 first written here (rewrites, curves, MC as separate fixtures); their content moves
into M4–M6 below, applied to the desk problem.
- Conventions layer (structure only): calendars from published rules (NY/SIFMA, TARGET), day counts, rolls, spot /
  payment / fixing lags, IMM dates, schedules with stubs and end-of-month, observation shift / lookback / lockout
  windows, fixings history; sources in `docs/G4_BUNDLE.md`.
- Curve schemes as templated maths: Flat, Linear, Hermite, NaturalCubic, MonotoneCubic (Hyman via `select`),
  BSpline; interpolation variables `zero` / `logdf` / `forward`; composite curves by region. Eigen on the `double`
  side only (D14).
- Instruments as templated maths over convention tables: RFR compounded (natural product recurrence → `scan`),
  RFR averaging, term-rate legs with fixing in advance, fixed legs, OIS / IBOR / tenor-basis swaps, deposits,
  futures (convexity 0, stated); par rates and residuals (G2 landed 2026-09-23, D43: row tables from the registry,
  instrument blueprints and curve definitions as JSON, the compounded coupons recorded as scan domains, the Stage A
  curve definitions calibrating through the implicit node).
- `scan` domains: recurrence detection, interpreter and reverse-scan adjoint (G3 landed 2026-09-23, D41: chains of
  identical steps detected without hints, wave-by-wave interpreter, row-by-row reverse scan, three mutants caught).
- `implicit` node with multi-curve dependencies inside the tape; residual sub-program sharing the DF domains with
  the book (`PROBLEM.md` §5).
- The Stage A fixture from the seed (synthetic quotes, ~2,000 trades, 1,000 scenarios) recorded as **one tape**
  producing O1–O4 and O6. (G5 landed 2026-09-23, D44: `blueprints/problems/stage_a.json` + `fixtures/stage_a`; the recording
  round-trips, the IR shows the two DF domains read by both the residuals and the book, the ladder agrees with forward
  mode to 5.8e-15 and with bump-and-recalibrate to 5.1e-8, the scenario lanes are bitwise their single runs; G6 runs
  the remaining §6 gates and states the M4 baseline.)

**Exit gate:** every `PROBLEM.md` §6 gate on the Stage A tape; the IR shows one DF domain read by both the residual
and the book; conventions match published examples. Timings recorded as the M4 baseline (informational).
**Result (2026-09-23): pass.** On fingerprint `d448afd70180` (Xeon W-3223, 8 cores / 16 threads, Apple clang 21,
`-O3 -march=x86-64-v3 -fno-math-errno`) at `589245d` (integrate/m1-m5 tip, M3/G6 gate run 2 — an independent re-run
confirming `f49d72a`'s m3-gate-1 result): the Stage A tape (G5, D44) is one recording of 2,000 trades / 70 quotes
across 4 curves / 1,000 scenario lanes, 148 inputs (70 free), 8,191 outputs, 27,459,283 raw nodes → **517,036** after
the E0 passes; the IR is 67 domains / 423,235 values / 5 scan domains (1,107 chains, 255,959 rows), with two DF
domains (177 + 16,917 rows) read by both the calibration residuals and the book (`duplicates(Exp)` = 0). Every
`PROBLEM.md` §6 gate holds (D45, G6): round-trip identity 0 mismatches; differential ball E0 0 mismatches of 8,191
outputs (64 draws); whole-program adjoint of O2 vs FD **9.65e-10** (gate 1e-6) and vs forward mode (`Dual<70>`)
**2.79e-15** (gate 1e-12); IFT risk ladder vs Dual **5.75e-15** on the full 2,000×70 ladder and vs Richardson
bump-and-recalibrate **5.09e-8** (gate 1e-6); optimality **1.235e-13** (gate 1e-12) at the record point, the run and
on 32 sampled lanes; the full 1,000-lane O4 grid: 0 not converged, 0 of 262,112 sampled outputs differ from an
independent single-lane run; conventions vs published 52/52 and closed forms 27/27 (a case-count discrepancy against
m3-gate-1's 64/64 / 25/25 on the same two suites is reported, not reconciled — every case passes in both runs, no
gate boolean is affected). Suites: `ctest` release and reference 80/80 each; `scripts/mutation_test.sh` **20/20**
mutants caught against the 40-gate set. Timings accepted as the M4 baseline (D9, D45; re-confirmed by G6 run 2 at
0.977–1.002× across 17/17 rows, 0 regressions): record 7,806.8 ms, one calibration 14.93 ms, O2 evaluation 1.530 ms
at B=1 / 52.27 ms at B=64, O3 reverse (IFT) 32.63 ms (book) / 278.4 ms (64 outputs), O3 forward (`Dual<70>`)
1,373.4 ms, O4 **16.11 ms/scenario**; fusion coverage shows the compounding scan at 67–77% of every evaluation
regardless of lane tile — the standing M4 target. Review: 5 findings (`m3-review-market`, `m3-review-one-tape`),
2 actionable and fixed in `m3-fix` (`cc23459`: a lookback variant unwired from any blueprint, and a payment-lag
convention disagreement given a named variant per further research, `docs/G4_BUNDLE.md` EUR.3). Simplifications
labelled throughout (D44; full list in `docs/RESUME.md` §5 "M3 result" / "M3/G5" and `docs/G4_BUNDLE.md` §6–7):
notably no FX in Stage A (placeholder 1.0), the scheme-sweep variants as separate recordings gated on 300 trades,
O3's forward mode as `Dual<70>` rather than a tangent interpreter, and the calibration solve tolerance at 1e-13
(not G4's 1e-14) forced by the 30-year daily products' rounding floor. `docs/RESUME.md` §5 "M3 result",
`bench/results/d448afd70180/m3.md`.

## M4 — Optimise the totality (`PROBLEM.md` §7)
- Rewrites R1–R7 with exactness classes, differential and mutation tests; the interpreter's planner decisions
  re-expressed as rules of the same kind.
- Cost model calibrated from the per-domain profiling timers, per fingerprint; validated by prediction error.
- Equality saturation over the domain IR; extraction by cost at the requested exactness class; AD mode per Jacobian
  block as a rule; cross-stage sharing rules (DF domain, factored Jacobian across lanes).
- Catalogue generated from the Stage A tape's hot groups; coverage report; regeneration no-op in CI.

**Exit gate:** the search rediscovers M1's three kill-path fusions with the planner's hard-coded rules switched off;
finds at least one cross-stage optimisation the greedy pipeline cannot express; every extracted program passes
`PROBLEM.md` §6 at its declared class; self-regression gate (D9) against the M3 baseline. Informational rows: the
M1 sub-book vs the hand kernel under every D27 pairing; adjoint risk vs bump-and-recalibrate.

**Result (2026-09-24): fail.** On fingerprint `d448afd70180` (Xeon W-3223, 8 cores / 16 threads, Apple clang 21, `-O3
-march=x86-64-v3 -fno-math-errno`) at `a49ad34` (M4-gate-2, the independent re-verification run; `d8700e8` on top is a
docs-only CI-verification addendum): the Rule interface, cost model, two-tier e-graph and extraction, rules R1–R7 plus
fma-contraction, the AD-mode-per-Jacobian-block rule and its required consumer, a cross-stage-sharing extraction
guard, and the catalogue all landed (D47–D55), each with its exactness class, differential test and mutation test —
43/43 registered mutants caught, `ctest` 107/107 under both release and reference. Of this section's own four
exit-gate clauses, two hold and two miss. Hold: every e-graph-extracted program verifies at its declared class against
the true unrewritten original (D57's own added check); the self-regression gate against the M3 baseline passes, 17/17
benchmarks within 1.25x, 0 regressions — catalogue coverage gives a real 1.11–1.16x win on the reverse risk ladder
(`adjoint::Adjoint` now dispatches to a catalogued kernel for ~99.9% of its own Stage A wall time). Miss: rediscovery
of M1's three kill-path fusions ties the cost model's own estimate (ratio 1.0, `plan_bridge.hpp`'s documented pricing
gap) but misses its 1.02x measured-wall-clock target at **1.0277x** (B=1) / **1.0427x** (B=64); the one cross-stage
candidate found (`r5.group_formation` applied twice on a bounded Stage A fixture) was originally reported as a 0.977x
win but, re-priced under this fingerprint's real fitted cost model rather than the synthetic default (D57's
correction), is **0.999716x** — noise, not a win, so `cross_stage_wins` = 0. The cost model's own mean relative error
(84.4% overall / 69.3% restricted to material domains) misses its <25% target; `EGraph::saturate` has no
redundancy/subsumption check and grows unboundedly on Stage-A-shaped programs past a small bound (1.87 GB and climbing
by round 4 on a 60-trade fixture, killed); rules R1, R2 (with its safety gate), R3, R4a, R4b and fma-contraction fire
zero times on both reference fixtures (each for a different, independently measured reason, D51/D52; **R2 and R1
unblocked by D65 — R2 now fires on 0 of the M1 book's 10 domains and 3 of the full Stage A tape's 67, and R1 on 16
of the 80 domains Stage A becomes after that R2 pass, which changes nothing about what the search selects**); a live,
reproducible `adjoint::` crash on a real R2 split shape (D52 point 4, `src/adjoint/` — **root-caused and closed by D65: it was a test-harness buffer-sizing defect, not an adjoint defect**) and a silent
`ExpMode::poly`-disabled interaction with the catalogue's own production default (`task_f7b9c87d`) are both flagged,
unowned by any M4 package. CI: `ci_green` = false, citing a pre-existing, unrelated GCC-only catalogue-coverage gap
(`task_919ea449`) confirmed unfixed at both M4-gate runs; this package's own diff touches only `docs/` and
`bench/results/`. Full account: `docs/RESUME.md` §5 "M4 result", `docs/DECISIONS.md` D47–D59.

## M5 — Stages B and C, streaming (`PROBLEM.md` §4 Stages B, C)
- EURUSD MtM-resetting xccy basis swaps, FX spot as input, EUR discounting under USD collateral, FX delta.
- GBP (SONIA) and JPY (TONA) with their calendars and conventions; GBPUSD and USDJPY xccy; the ~5,000-trade book.
- `pin` + `rank_update` + hysteresis: frozen-Newton streaming; a kink 2-cycle fixture on a composite curve that
  fails without `pin` and converges with it.
- The optimiser and catalogue re-run on the Stage C tape.

**Exit gate:** `PROBLEM.md` §6 on the Stage C tape; kink fixture converges within 5 iterations and no forced
refresh; self-regression gate.

## M6 — Stage D: Monte Carlo exposure and MC risk (`PROBLEM.md` §4 Stage D)
- `std::thread` pool and counter-based RNG with inverse-CDF Gaussians (D17); fixed-order reductions.
- Hull–White 1F and LGM per currency sharing one affine kernel (D19), calibrated to O1.
- Exposure grid on the Stage C book: EE / PFE per netting set and trade; CVA delta to quotes through the adjoint of
  the simulation and the IFT.

**Exit gate:** per-path E0 vs a scalar reference; EE to 1e-12; bit-identical across thread counts and tiles; CVA
delta vs bump at 1e-4 relative; absolute timing target stated as an estimate until measured (10k paths × 100 dates
× the Stage C book on 8 threads).

## MX — Stretch: the SwapEngine comparison on Stage C (D21)
The same bundle, instruments, portfolio and scenarios on SwapEngine as a black box, one fingerprint, each engine's
own timing harness; an informational table with like-for-like caveats under `bench/compare/`. No gate.

**Stage 1 done ahead of Stage C, on the risk ladder alone (D71, 2026-09-25).** Run on a single USD SOFR OIS
curve rather than the Stage C desk, because that is what can be made like-for-like without reading the other
engine's source: `fixtures/compare_ois.hpp` writes the problem in a purely time-based exchange form,
`tools/compare/compare_ois` produces it and this engine's answers, and `scripts/compare_swapengine.py` feeds it
to the other engine's public JSON interface and diffs the two. The two engines agree on the O3 ladder
d(book PV)/d(quote) to **2.747e-13** relative, and on discount factors, model par rates and per-trade PVs to
between 4e-14 and 3e-12; `bench/compare/README.md` is the matched-versus-unmatched statement D21 asks for.
**No timing has been taken**: it is deferred to a reserved machine, and the largest caveat on whatever ratio it
produces is already known — the two engines can be made to agree on the answer but not to do the same
arithmetic, because their bundle can only express the telescoped coupon and ours evaluates the daily product
(D71 §6a). Stage C breadth (GBP, JPY, xccy, the G10 desk) is untouched and needs five more currencies'
conventions.

## Later
Payoff scripting language targeting the op set; vol surfaces/cubes; SIMM and marginal (pre-trade) analytics; portable
kernel serialisation; GPU backend for the batch axis.

---

## Risk register

| risk | milestone that tests it | mitigation |
|---|---|---|
| generic path slower than hand-fused on linear books | M1, M3 | tile/layout tuning; catalogue; keep a hand path only if gates force it |
| scan domains mis-detected | M3 (G3) | round-trip identity on every recording; a chain the builder cannot lay out falls back to level splitting (D41); explicit scope markers as hints remain available |
| silent compiler bugs | M2 onward | round-trip identity, differential tests, mutation testing before feature work |
| kink flip storms | M4 | `select` + arm-gap classification + `pin` active set |
| adjoint memory at MC scale | M6 | batch-lane adjoints, per-thread accumulators, binomial checkpointing |
| code size for irregular payoffs | Later | signature reuse; interpreter; JIT only by explicit decision |

