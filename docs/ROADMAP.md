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

## M2 — Verification harness and adjoints
Fixture: `WORKLOADS.md` §M2.
- Differential tester (randomised state ball), finite-difference and forward-mode (dual-number `Scalar`) adjoint checks.
- Mechanical adjoint generation per group; pull-transposes for scatters.
- Mutation testing on every rewrite rule.
- Perf-gate tooling: fingerprinting, baselines, self-regression + absolute targets.

**Exit gate:** adjoint vs FD at 1e-6 rel, vs forward mode at 1e-12; every mutated rule caught.

## M3 — Fusion rewrites and the catalogue
Fixture: `WORKLOADS.md` §M3.
- Rewrites R1–R7 with exactness classes, each with its differential test and mutation test.
- Catalogue generator under `tools/catalogue/` (hot signatures → emitted C++ under `src/catalogue/generated/`,
  committed; CI checks regeneration is a no-op).
- Coverage report: which groups ran on the interpreter.

**Exit gate:** hand-fused parity (≤ 1.05×) on catalogued groups; E0/E1 parity per rule.

## M4 — Curves and calibration
Fixture: `WORKLOADS.md` §M4. Full scope, in this order: schemes → `implicit` → `pin` + `rank_update`.
- Region schemes as templated maths (Flat, Linear, Hermite, NaturalCubic, MonotoneCubic, BSpline); linear ones collapse
  to `linmap`, value-dependent ones through `select`. Eigen on the `double` side only (D14).
- `implicit` node: least-squares calibration with IFT risk (`dx/dq`).
- `pin` + `rank_update`: band edges and interpolation kinks as one active set; frozen-Newton streaming.

**Exit gate:** calibration first-order optimality (`‖Jᵀr‖∞`), IFT risk vs bump-and-recalibrate at 1e-6, a kink
2-cycle repro that converges without a forced refresh.

## M5 — Batch axis, scan domains, Monte Carlo exposure
Fixture: `WORKLOADS.md` §M5.
- Recurrence detection → `scan` domains.
- Affine short-rate models, Hull–White 1F **and** LGM sharing one kernel: `log P(t,T) = A − B·x_t` as a batched
  `linmap`, the time step an affine `scan` (D19).
- `std::thread` pool and counter-based RNG with inverse-CDF Gaussians; bit-identical across tile size and thread
  count (D17).
- Exposure grid (paths × dates × book), tiled; per-date structure representation measured, not assumed.

**Exit gate:** 10k paths × 100 dates × 1k swaps under 1 s on 8 threads; per-path parity E0 with a scalar reference
under `-ffp-contract=off`, expected exposure to 1e-12.

## M6 — XVA and sensitivities
- CVA/FVA with adjoint sensitivities to market quotes through the calibration `implicit` node.
- LSM with `frozen` regression; checkpointed adjoint MC.

**Exit gate:** adjoint CVA sensitivities vs bump at 1e-4 rel; cost ≤ 5× one valuation.

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

## MX — Stretch: G4 multi-currency bundle, EpykosEngine vs SwapEngine
Only after M5 passes. Fixture: `WORKLOADS.md` §MX.
- A realistic G4 (USD, EUR, GBP, JPY) curve bundle built from researched market conventions: OIS curves (SOFR, €STR,
  SONIA, TONA), tenor-basis curves where the market still has them (EURIBOR 3M/6M vs €STR; JPY TIBOR if warranted),
  cross-currency basis (EURUSD, GBPUSD, USDJPY, MtM-resetting) — each from its correct instruments with correct day
  counts, lags, calendars and roll rules. Sources cited in `docs/G4_BUNDLE.md`.
- Calibration, risk ladder and a portfolio scenario run on both engines, as like-for-like as the two designs allow
  (D21), under one fingerprint. Differences in what is priced are stated, not hidden.

**Exit:** a report table (informational, D9) with the like-for-like caveats; no gate.
