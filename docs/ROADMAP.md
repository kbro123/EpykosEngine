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

**Exit gate, as changed 2026-09-29 (D86; `PROBLEM.md` §7 is authoritative and carries the reasoning):** the search
rediscovers M1's three kill-path fusions with the planner's hard-coded rules switched off; every extracted program
passes `PROBLEM.md` §6 at its declared class; self-regression gate (D9) against the M3 baseline. The **cross-stage
optimisation clause is retired on Stage A** and so is the cost model's <25% prediction-error target: D68 measured a
cost model with ZERO error at 0.166% of an O4 lane batch, and a clause a correct implementation cannot satisfy is
not a gate. The optimisation gate is re-pointed at the block solves, the reverse ladder and record-plus-build.
M4 is **not** retroactively passed — the result below stands unedited. Informational rows: the M1 sub-book vs the
hand kernel under every D27 pairing; adjoint risk vs bump-and-recalibrate.

**Exit gate as originally written, for the record:** *"…rediscovers M1's three kill-path fusions with the planner's
hard-coded rules switched off; finds at least one cross-stage optimisation the greedy pipeline cannot express; every
extracted program passes `PROBLEM.md` §6 at its declared class; self-regression gate (D9) against the M3 baseline."*

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

## Invariants I1–I3 (`PRINCIPLES.md` §1b; commissioned by the owner 2026-10-06, D95)
`PRINCIPLES.md` §1b is the specification and is authoritative; this section is the schedule and its state. The
staging is D95 §3's — dependency, not preference — and the stage names below are its own. Three concurrent builds
make the box dirty, so **no number produced during this programme is a performance claim** (D95 §4); sizes and
counts are not timings.

- **A — I3's gate** (a ratchet, needs no capability): the pinned registry of host-computed tape inputs with a
  justification each, failing when the list grows. **Built** (`tests/invariants/i3_host_inputs_test.cpp`); I3 still
  does not hold, and its substantive fix — recording ‖JᵀF‖∞ — is downstream of C4 because it needs an IR that can
  express a derivative.
- **B — I2.** B1 re-template `Dual` to `template <int N, class S>` so it nests, existing tests bit-identical; B2
  instantiate the pricing maths at `Dual<1, Dual<1>>`; B3 second derivatives against central differences of the
  firsts at a **measured, then stated** tolerance; B4 report on `adjoint::Adjoint`'s `double*` interface and change
  nothing. **B1–B3 done** (D97: I2 holds at the maths level, 2e-8 measured); the `double*` interface is the
  remaining half and is reported, not fixed.
- **C — I1, staged so each step verifies alone.**
  - C0 design: the op set, and whether bitwise is reachable at all. **Done, and it refuted §1b's own stated root
    cause** (D96): `Op::Affine` over a `Segment` IS a scatter-accumulate read backwards, and `AdjointPlan`'s four
    reverse-adjacency CSR lists are already the transpose the pull needs — so **C1, "the op", was never built and is
    not needed**. `Op::Gather`, `Op::SegmentSum` and `Op::Linmap` stay reserved and unused, the `Op` enum is
    unchanged, and no `Op` dispatch site moved.
  - C2 `adjoint_to_program` over the elementwise subset and gathers; C3a segments; C3b scans (the forward half
    re-emitted as an `ir::Scan`, the reverse as one recurrent non-scan domain walking the rows backwards). **Done**,
    each bitwise-gated as it landed.
  - C4 the full gate. **Done**: `adjoint::Adjoint::to_program()` is the public face (`program()` still returns the
    FORWARD program, deliberately unchanged), and `tests/adjoint/i1_gate_e0_test.cpp` is one named test asserting
    §1b's clause over every fixture `tests/adjoint/` gates the adjoint on, Stage A included — see `DESIGN.md` §11.
    It additionally measures, per fixture, whether a defect in the emitted reverse can reach a state adjoint, and
    labels a fixture that cannot a structural check rather than counting it as coverage.

**Gate:** §1b's, verbatim and unamended — `adjoint(P)` is an `ir::Program` that passes `ir::validate`, round-trips
through `ir::serialize`/`deserialize`, and that `exec::Interpreter` run on it reproduces `adjoint::Adjoint::run`
**bitwise**. Not a tolerance. The one question that could have stopped the programme — whether the emitted program
can accumulate in the order the `acc_*` kernels do, since reordering `Adjoint::run` is a §5.2a case-1 question
**reserved to the owner** — is answered yes, and no agent took that question (D96 §3).

**Not in scope here, and still refused:** a recurrent domain that is not a scan, and a scan reading its own rows
through anything but its carry. Either needs two carried quantities per row and a domain row produces exactly one
value. Second derivatives through the adjoint remain blocked on `Adjoint`'s `double*` interface (B4), not on I1.

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

### The reverse pass's per-row dispatch (owner, 2026-10-09; D105 §7)

**Stated with its measured reason, and not yet authorised.** D105 measured the cost of `pull()`'s
per-row work and found the **dispatch**, not the zero fill, is what it is made of: one four-way
branch plus eight CSR offset loads per row, independent of lane count. Meanwhile **79.3% of
`stage_a`'s non-empty pulls and 93.6% of `compare_ois`(256)'s are a single reader at unit
coefficient** — the entire pull is a copy.

A precomputed per-value classification byte in `AdjointPlan` would turn the four-way branch into one
indexed dispatch. It is a new plan table, so it needs its own measurement before anything is built,
and D105's own result is the reason: the previous lead on this code path looked like 26% of the
arithmetic and measured **negative**, because an op count treats a store and a branch as
commensurable and they are not.

### Lane widths and chunk tiling (owner, 2026-10-09; D104, and measurements in this section)

Three items from the lane-width work, in descending measured value. **The first two apply to EVERY run; the third
applies to nothing we benchmark**, which is the order they should be done in and the opposite of the order they were
found in.

1. **A `bench/` sweep over `lane_tile` x `max_batch` x fixture, gated.** Not a `tools/` probe: it needs a baseline so
   `scripts/perf_gate.py` catches regressions, and per D34 only the default invocation is gated, so the default sweep
   must be the one that matters. It should cover `compare_ois` AND `stage_a`, because every figure below is one
   fixture on one box — the same objection the orchestrator raised against an agent's measurement and then committed
   itself.

2. **`lane_tile = 32` is a reproducible ~22% penalty, and it is a published option.** Measured on `compare_ois` 64
   trades, B=256, adjoint us/lane, three replicates at 1-minute load 2.3-2.7, fingerprint `d448afd70180`:

   | `lane_tile` | 8 (shipped) | 16 | 32 | 64 |
   |---|---|---|---|---|
   | us/lane at B=256 | 7.507-7.537 | **7.152-7.230** | **9.138-9.239** | 7.223-7.234 |

   So **16 beats the shipped 8 by ~4.8%** at B=256 and 2.3-3.0% at B=64/100 — diluted through the ladder (the reverse
   pass is 62-79% of it, D103) that is ~3-4% on the workloads this project quotes, from a one-line default. And 32,
   which anyone may set, costs ~22% against both its neighbours with nothing in the tree to warn them.

   **The mechanism is NOT established and should not be guessed.** `acc_rows_in_flight_for(L)` drops rows in flight
   from 2 to 1 exactly at L=32 — but L=64 also gets 1 and is fine, so that does not explain it alone. Investigate
   before fixing. Changing the default needs its own decision entry and a second fixture.

3. **A known regression shipped on `integrate/invariants`, and it must be fixed or reverted before that branch
   merges.** D104's chunk tiling regresses **B = 3, 7, 11, 15 by 4-13%** (adjoint and interpreter independently,
   three replicates) — D104 reported these as "+0.7% / +2.4%, no gain, inside spread" because it measured through the
   whole ladder, which dilutes the component. One condition fixes all of them: **3 is the only tail that ever needs
   three width-1 chunks**, and 7, 11 and 15 all end in a tail of 3, so emitting a tail of exactly 3 as one generic
   chunk removes every regression and keeps every win (B=2 -18.9%, B=5 -24.2%, B=6 -6.9%, B=10 -7.1%, B=13 -11.1%).

   **But it is worth ~0% on anything we measure**, and that is the point of listing it third. `compare_ois_ladder`
   chunks rows by `max_batch` = 64 and each chunk is split by `lane_tile` = 8, so D90's h2h ladder (R=1, B=1), the
   256-row ladder (4x64), the 1000-trade book (15x64 + 40, and 40 mod 8 = 0) and the warm calibration (B=1) are all
   **unaffected**. It is a correctness-of-performance fix for a pathology — `B=2` costing more than `B=1` — not a
   speedup. Do NOT build a cost model or a DP optimiser over decomposition choices: that is D86's retired cost model
   again, fitted to a lever measured at zero.

### Interpolation schemes (owner, 2026-10-06)

`curve::SchemeKind` has six: `flat, linear, hermite, natural_cubic, monotone_cubic, bspline`. Two items, and they are
NOT the same job — the first is coverage, the second is the one that answers an open question.

1. **`tension` — and it is worth more than parity.** The only scheme the other engine has that we do not (its
   `Scheme` enum is `Flat, Linear, NaturalCubic, Hermite, MonotoneCubic, BSpline, Tension`, seven to our six; read
   under the D11 grant of D92 §5). A spline under tension puts `f ∈ span{1, t, sinh(σt), cosh(σt)}` on each interval
   instead of a cubic: σ→0 recovers the natural cubic, σ→∞ approaches piecewise linear. With **σ fixed** the knot
   curvatures solve a tridiagonal system whose matrices depend only on the knot SPACINGS and σ, never on the values,
   so the interpolant is a constant matrix times the knot vector — **linear in the knot forwards**. (That is an
   elementary property of tension splines, not borrowed design; their `scheme_is_linear` excludes only
   `MonotoneCubic`, consistent with it.)

   The reason to want it is what our own measurement says `monotone_cubic` costs: its Hyman limiter is recorded as
   seven `select`s, and the Stage A residual slice goes **284 values → 708** against log-DF with **+33% per solve**
   (D92 §7a). Tension controls cubic overshoot **without** value-dependent branches, so it would buy similar shape
   control at linear cost and stay eligible for every structure-keyed decision `is_linmap_domain` gates. It does not
   stress the solver — it is a cheaper way to avoid needing to.
2. **Parametric forms: `nelson_siegel`, `svensson`.** We have no parametric curve at all; they have both (plus
   `ModularCurve` for splines). A few economically-interpretable, time-stable parameters instead of a forward per
   pillar — the fair-value-curve workhorse for bond RV. Note for item 3: **as implemented there they are also linear
   in the free parameters**, because τ is held as a fixed hyperparameter and only the betas are fitted. Freeing τ is
   what makes them non-linear, and their own header lists that as a later option too.

3. **`monotone_convex` (Hagan–West) — the open item, and NOBODY has it.** Absent here and absent there: that
   engine's only Hagan–West reference is a comment citing the overshoot problem its `Tension` scheme solves a
   different way, so **this cannot be obtained by porting.**

   **Corrected 2026-10-06 (owner, who wrote that engine).** An earlier version of this entry said "every curve form
   that engine ships is linear — or, for `MonotoneCubic`, piecewise-linear — in its free parameters". That
   mischaracterises it. **`MonotoneCubic` IS a non-linear-in-the-knots interpolator by that engine's own
   classification** — `scheme_is_linear` returns false for it and nothing else, its own comment saying "whose Hyman
   filter is value-dependent" — and its W-cache, `is_linear_map` and hybrid-residual machinery exist precisely to
   route around that one case. Calling it "piecewise-linear" softened a distinction that engine treats as binary and
   load-bearing. The narrower thing actually measured here (D92 §7a) is that over ±100bp bumps monotone cubic's
   recorded `select`s do not flip, so OUR chord does not stall: that is branch stability over a bump range, and it
   licenses nothing about global linearity.

   What stands: `Tension` is built to control overshoot without value-dependent branches, and the parametric forms
   fix τ to keep the zero rate linear in the betas — so most of that scheme set is deliberately kept on the
   fast path, with `MonotoneCubic` the acknowledged exception that falls off it.

   **Epykos has no such dependency.** The chord policy needs CONTRACTION, not linearity: it freezes a factorisation
   and refreshes when a step stalls, which is a property of the iteration rather than of the curve's algebra (and
   D92 §7a measured monotone cubic's seven recorded `select`s never flipping a branch over ±100bp). So monotone
   convex is the case where their fast path has to fall back and ours does not have one to fall off. Whether that
   converts into a measured advantage is unknown and would be the point of building it — stated as the hypothesis it
   is, not a claim. Until then D92 §1's warm-path result holds only for affine and piecewise-affine schemes. Every scheme either
   engine ships is linear or piecewise-linear in the knot values, which is exactly why D92 §7a could not answer the
   question it set out to: *does genuine level-dependence stall the chord, so the warm path's zero Jacobian builds
   becomes positive?* Monotone cubic only looked like that case — its Hyman limiter is recorded as a `select`, so it
   is affine within a branch and a 1–100bp bump almost never flips one. Monotone convex is genuinely non-linear
   within a region (a shape parameter derived from the discrete forwards, with case analysis on the data), so it is
   the scheme that would test it. **A maths-layer feature, not an optimisation**, and until it exists D92 §1's
   warm-path result is established only for affine and piecewise-affine schemes.

Also worth taking when a scheme is added: a **compile-time tripwire** on the scheme count, so appending one fails to
build until every place that must learn about it has. The idea is an ordinary `static_assert` pattern and not
borrowed code; `is_linmap_domain` is the equivalent here of the "does this ride the closed-form path" question, and
it answers by structure rather than by exclusion, so a new value-dependent scheme would correctly find no linmap
rather than silently claiming one.

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

