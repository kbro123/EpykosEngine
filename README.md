# EpykosEngine

Generic, fast financial computation from a recorded graph.

Pricing maths is written once, as C++ templated on `Scalar`. The engine records it against a fixed structure (schedules,
curve topology, payoff logic), infers a domain-typed array program from the recording, fuses it into pre-compiled
kernels, and derives adjoints mechanically. The same program runs a single live tick, a batch of scenarios, or a Monte
Carlo path grid. No JIT.

**Status:** design (M0). See [`docs/DESIGN.md`](docs/DESIGN.md), [`docs/ROADMAP.md`](docs/ROADMAP.md),
[`docs/DECISIONS.md`](docs/DECISIONS.md) and [`docs/PRIOR_ART.md`](docs/PRIOR_ART.md).
