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
(`docs/RESUME.md` §5 "M2 result"); M3 onward in progress on `integrate/m1-m5`, re-planned around the desk problem of
[`docs/PROBLEM.md`](docs/PROBLEM.md). See [`docs/DESIGN.md`](docs/DESIGN.md), [`docs/ROADMAP.md`](docs/ROADMAP.md),
[`docs/DECISIONS.md`](docs/DECISIONS.md), [`docs/WORKLOADS.md`](docs/WORKLOADS.md) and [`docs/PRIOR_ART.md`](docs/PRIOR_ART.md).
