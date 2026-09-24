# EpykosEngine

Generic, fast financial computation from a recorded graph.

Pricing maths is written once, as C++ templated on `Scalar`. The engine records it against a fixed structure (schedules,
curve topology, payoff logic), infers a domain-typed array program from the recording, fuses it into pre-compiled
kernels, and derives adjoints mechanically. The same program runs a single live tick, a batch of scenarios, or a Monte
Carlo path grid. No JIT.

**Status:** M0 design complete; **M1 (kill test) passed** 2026-09-23 — the tiled interpreter runs the 1k-swap book at 1.044× the
hand-fused kernel single-state and 1.079× batched (fingerprint d448afd70180, libm `std::exp` on both sides, D27; `docs/RESUME.md`
§5 "M1 result"); **M2 (verification harness and adjoints) passed** 2026-09-23 — the mechanical adjoint within 2.9e-9 of central
finite differences (gate 1e-6) and within 2.1e-13 of forward mode (gate 1e-12) on the same book, 13/13 mutants caught, the
differential tester bitwise against the templated `double` maths, perf-gate tooling with the M1 numbers as the baseline
(`docs/RESUME.md` §5 "M2 result"); **M3 (groundwork and the Stage A tape) passed** 2026-09-23 — the desk problem of
[`docs/PROBLEM.md`](docs/PROBLEM.md) §4 Stage A recorded as one tape (2,000 trades, 70 quotes across 4 curves, 1,000
scenario lanes; 517,036 nodes, 67 IR domains, 5 scan domains) with every `PROBLEM.md` §6 gate passing on fingerprint
d448afd70180 (adjoint vs FD 9.65e-10, vs forward mode 2.79e-15, IFT risk vs bump-and-recalibrate 5.09e-8, optimality
1.235e-13, 20/20 mutants caught; `docs/RESUME.md` §5 "M3 result"); **M4 (optimise the totality) failed** 2026-09-24 —
of `PROBLEM.md` §7's four exit-gate clauses, two hold (every e-graph-extracted program verifies at its declared
exactness class; self-regression against the M3 baseline passes, 17/17 benchmarks) and two miss (rediscovery of M1's
three kill-path fusions misses its 1.02× wall-clock target at 1.0277×/1.0427×; the one cross-stage optimisation found
is 0.999716× under this fingerprint's real fitted cost model — noise, not a win); the Rule/e-graph/cost-model/catalogue
framework and rules R1–R7 landed regardless, with 43/43 mutants caught (D59; `docs/RESUME.md` §5 "M4 result"). See
[`docs/DESIGN.md`](docs/DESIGN.md), [`docs/ROADMAP.md`](docs/ROADMAP.md),
[`docs/DECISIONS.md`](docs/DECISIONS.md), [`docs/WORKLOADS.md`](docs/WORKLOADS.md) and [`docs/PRIOR_ART.md`](docs/PRIOR_ART.md).
