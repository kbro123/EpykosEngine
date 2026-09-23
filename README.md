# EpykosEngine

Generic, fast financial computation from a recorded graph.

Pricing maths is written once, as C++ templated on `Scalar`. The engine records it against a fixed structure (schedules,
curve topology, payoff logic), infers a domain-typed array program from the recording, fuses it into pre-compiled
kernels, and derives adjoints mechanically. The same program runs a single live tick, a batch of scenarios, or a Monte
Carlo path grid. No JIT.

**Status:** M0 design complete; **M1 (kill test) passed** 2026-09-23 — the tiled interpreter runs the 1k-swap book at 1.044× the
hand-fused kernel single-state and 1.079× batched (fingerprint d448afd70180, libm `std::exp` on both sides, D27; `docs/RESUME.md`
§5 "M1 result"); M2–M5 in progress on `integrate/m1-m5`. See [`docs/DESIGN.md`](docs/DESIGN.md), [`docs/ROADMAP.md`](docs/ROADMAP.md),
[`docs/DECISIONS.md`](docs/DECISIONS.md), [`docs/WORKLOADS.md`](docs/WORKLOADS.md) and [`docs/PRIOR_ART.md`](docs/PRIOR_ART.md).
