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

## M3 — Groundwork and the Stage A tape (`PROBLEM.md` §4 Stage A; D28)
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
  futures (convexity 0, stated); par rates and residuals.
- `scan` domains: recurrence detection, interpreter and reverse-scan adjoint.
- `implicit` node with multi-curve dependencies inside the tape; residual sub-program sharing the DF domains with
  the book (`PROBLEM.md` §5).
- The Stage A fixture from the seed (synthetic quotes, ~2,000 trades, 1,000 scenarios) recorded as **one tape**
  producing O1–O4 and O6.

**Exit gate:** every `PROBLEM.md` §6 gate on the Stage A tape; the IR shows one DF domain read by both the residual
and the book; conventions match published examples. Timings recorded as the M4 baseline (informational).

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

## Later
Payoff scripting language targeting the op set; vol surfaces/cubes; SIMM and marginal (pre-trade) analytics; portable
kernel serialisation; GPU backend for the batch axis.

---

## Risk register

| risk | milestone that tests it | mitigation |
|---|---|---|
| generic path slower than hand-fused on linear books | M1, M3 | tile/layout tuning; catalogue; keep a hand path only if gates force it |
| scan domains mis-detected | M5 | explicit scope markers as hints; round-trip check |
| silent compiler bugs | M2 onward | round-trip identity, differential tests, mutation testing before feature work |
| kink flip storms | M4 | `select` + arm-gap classification + `pin` active set |
| adjoint memory at MC scale | M6 | batch-lane adjoints, per-thread accumulators, binomial checkpointing |
| code size for irregular payoffs | Later | signature reuse; interpreter; JIT only by explicit decision |

