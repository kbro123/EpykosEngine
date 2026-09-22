# EpykosEngine — Roadmap

Ordered by risk: each milestone is chosen to kill the plan as cheaply as possible if it is going to die.
Every milestone lists its **exit gate** and its **kill criterion**.

## M0 — Design (this commit)
Design, decisions, prior art, roadmap. No code.

## M1 — Kill test: can a generic program match a hand-fused kernel?
Build the minimum to answer the one question that decides the architecture.
- `Rec` recording scalar (constants as leaves, no implicit conversion, `RecBool`, taint).
- A minimal templated rates kernel: flat/linear curve, compounded OIS swaps (par rate), fixed/float legs.
- Signature pass → domain IR, with the round-trip identity check.
- Tiled interpreter (no catalogue).
- A hand-fused reference kernel for the same book (gather → fma → segment_sum, shared reciprocal).
- Benchmarks: 1k-swap book, single state and batch of 64 states.

**Exit gate:** round-trip identity on every test book; interpreter values E0 vs templated `double` (`-ffp-contract=off`).
**Go:** interpreter within **1.3×** of the hand-fused kernel single-state, within **1.1×** batched.
**Kill / redesign:** > 2× single-state ⇒ rethink tile size and intermediate layout before anything else; if still > 2×,
the catalogue carries the design and M3 moves ahead of M2.

## M2 — Verification harness and adjoints
- Differential tester (randomised state ball), finite-difference and forward-mode adjoint checks.
- Mechanical adjoint generation per group; pull-transposes for scatters.
- Mutation testing on every rewrite rule.
- Perf-gate tooling: fingerprinting, baselines, self-regression + absolute targets.

**Exit gate:** adjoint vs FD at 1e-6 rel, vs forward mode at 1e-12; every mutated rule caught.

## M3 — Fusion rewrites and the catalogue
- Rewrites R1–R7 with exactness classes.
- Catalogue generator (hot signatures → emitted C++ → compiled in).
- Coverage report: which groups ran on the interpreter.

**Exit gate:** hand-fused parity (≤ 1.05×) on catalogued groups; E0/E1 parity per rule.

## M4 — Curves and calibration
- Region schemes as templated maths (Flat, Linear, Hermite, NaturalCubic, MonotoneCubic, BSpline); linear ones collapse
  to `linmap`, value-dependent ones through `select`.
- `implicit` node: least-squares calibration with IFT risk (`dx/dq`).
- `pin` + `rank_update`: band edges and interpolation kinks as one active set; frozen-Newton streaming.

**Exit gate:** calibration first-order optimality (`‖Jᵀr‖∞`), IFT risk vs bump-and-recalibrate at 1e-6, a kink
2-cycle repro that converges without a forced refresh.

## M5 — Batch axis, scan domains, Monte Carlo exposure
- Recurrence detection → `scan` domains.
- Affine short-rate models (Hull–White / LGM): `log P(t,T) = A − B·x_t` as a batched `linmap`.
- Exposure grid (paths × dates × book), tiled.

**Exit gate:** 10k paths × 100 dates × 1k swaps under 1 s on 8 cores; parity with a per-path scalar reference.

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
