# CLAUDE.md — EpykosEngine

**Read `docs/PRINCIPLES.md` first** — it is the contract and it outranks everything else here. Then `docs/DESIGN.md`,
`docs/ROADMAP.md` and `docs/WORKLOADS.md`. `docs/DECISIONS.md` is the append-only ledger: cite it, do not read it end to
end. `docs/PRIOR_ART.md` holds the measured evidence the design rests on; `docs/RESUME.md` is the handoff brief.

## What this is
A financial computation engine that records pricing maths written once (templated on `Scalar`) and compiles it into a
domain-typed array program executed by pre-compiled fused kernels, with mechanically derived adjoints. **No JIT.**

## Status
**The front half of the compiler exists and the engine derives telescoping.** `docs/PRINCIPLES.md`
is the contract and is authoritative; this paragraph summarises it and is rewritten, never extended
(§9).

M1–M3 passed 2026-09-23. **M4 failed its exit gate** 2026-09-24 (D59) and the post-mortem found the
cause was structural: the engine had a good back half and no front half, and the tape passes were
contracted to be bit-identical, which forbids algebra outright. **D79 replaced that contract with
the pin** — above it the engine chooses WHICH expression to evaluate and is judged against the
recorded expression in exact real arithmetic; below it, HOW, bit-identical to the pinned tape under
a declared contraction and transcendental policy.

**D81, 2026-09-27: `epykos::compile` now collapses the compounded OIS coupon by itself.** Five
local peepholes in one forward sweep — the tau cancellation, the +1/-1 cancellation, the
multiplicative identity, the telescope `mul(div(p,q),div(q,s)) -> div(p,s)` and one reassociation
for seasoned coupons. No recurrence solver, no e-graph, no new types. compare_ois goes from
1,020,583 recorded nodes to **1,183**, where the four data-movement passes alone left 81,321 and
D74's hand-written spike produced 1,313; Stage A goes 517,036 → **174,388**. It costs 1.03–1.08x
the old pass time. Verified against an independent engine and TIGHTER than before the collapse
(discount factors 7.355e-16, was 5.329e-15); the mechanical adjoint, the IFT ladder and every
error-against-truth gate pass unedited.

**Against a specialist, both engines in one process on one clock** (D81 §7, `tools/h2h`, 25 knots
and 1,000 trades, idle box): 161x faster cold calibration than the engine managed a day earlier,
150x warm, 36.5x on the risk ladder, each better than the hand-written spike. The **risk ladder is
5.70x in our favour**. Calibration is still **1.7x slower cold and 7.1x warm** and the collapse did
not move that ratio at all, which says the remaining gap is solver algorithm, not arithmetic —
`PRINCIPLES.md` §4's named exception.

Two defects found on the way and fixed: `exec::Interpreter` silently left duplicated outputs
unwritten (pre-existing, exposed by the collapse — silent zeros for 19 of 300 trade PVs on a Stage
A variant), and the generated catalogue registry was stale so coverage had dropped a quarter.

**D85, 2026-09-29: the solve cache is built, measured at 0.99x, and reverted.** D82 put ~65 us of a
74.9 us warm solve in one Jacobian build paid every call, so a seed cache was aimed at the small
half — measured, it lands exactly on a fixed record-point seed. What replaces it is a **lazy final
Jacobian**: build `J_z` at the solution when the adjoint or the IFT asks, not at the end of every
`run`. A re-quote falls **69.0 us → 11.4 us** with the ladder untouched, and because it changes no
iterate path it costs the contract nothing — D83's §5.2 amendment is withdrawn, and §4a's gate
restates as a *bitwise* equality. **Built 2026-10-03 (D89)**: paired before/after at load 1.95–1.97,
**69.0–70.7 us → 15.1–15.5 us (4.6x)** on the h2h hot configuration and 241 → 188 on the cold
default. It also exposed a latent hazard — `Factors` had no `valid()` guard, so an unbuilt Eigen
decomposition returned a silently all-zero risk ladder; it throws now. **D90, the head-to-head that
settles it:** both engines one process one clock, 25 knots / 1,000 trades, the warm calibration gap
goes **7.36x → 2.24x slower** (ours 31.1 us against 13.9) and cold 1.73x → 1.33x; the ladder stays
~6x in our favour and pricing ~48x. D89's "~1.6x" was optimistic — it divided a minimum by a
self-reported figure across two harnesses. Agreement with the independent engine survives at
5.533e-15 on book NPV. **D91 decomposes what is left**: the warm solve fits
`2.22·evals + 0.34·iters + 8.39 us` (worst error 8.45%), so it is **53% residual evaluation, 43%
fixed, 6% linear algebra** — and the ‖JᵀF‖∞ diagnostic is now the largest single fixed item at
~5.5 us. Bigger finding: the probe measures 19.4 us for the call h2h reports at 31.1 on an
identical workload, and forced eviction reproduces the difference, so **~40% of the head-to-head
number is cache refill, not arithmetic**. `max_batch` was tested as the cause and largely refuted.
The levers, measured: residual evaluation (§10 step 9's vector transcendentals now have a
denominator — d3 is `exp` over 101 of 402 values), the diagnostic, and residency, which nothing on
the roadmap attacks. **D94 takes the second of those**: `SolveOptions::optimality_diagnostic`,
default true and bit-for-bit today's behaviour, and when false the `jt_product` lane is skipped and
both `SolveReport::jtr_inf` and the block's diagnostic output are **NaN**, never zero — zero is the
signature of a converged solve, so it would announce success, and this repository has twice shipped
a plausible number where there should have been an unmistakable one (D81 §6(a), D89 §3). Paired
interleaved arms, **load 4.22, not a reserved box, so read the delta and not the levels**: the
record point **9.94 → 4.51 us** and a +1bp re-quote **20.33 → 15.19 us**, i.e. ~5.4/5.1 us saved
against the 5.46 us D91 attributed to it by an independent method; a clean re-run is pending. The engine
could not find this itself — a derivative is not a tape node (D87 §1), so the one optimisation every
compiler does to an unused value, deleting it, is unavailable here. That is **invariant I3 failing
in its smallest instance**, and the switch is a mitigation, not a discharge of its gate.

**D92, 2026-10-06: the owner widened D11 to read the other engine's `OPTIMIZATION.md`; its "single
biggest win" turns out to be our smallest.** Their W-cache (`DF = exp(−W·x)`, precomputed W,
analytic Jacobian) removes "no curve rebuild, no autodiff sweep" — and Epykos never rebuilds a
curve, because the curve is inlined into the tape as d2/d3 at recording time, with the adjoint
batched over a collapsed 402-value slice. **We already hold that benefit by construction.** Measured
ceiling for a closed-form residual Jacobian: 87% of cold calibration, **0% of the warm path** (chord
builds none), and **0.5% of a 256-row risk ladder** — the earlier 63.7% was a one-row artefact,
since `Factors` is built once per solve and reused across every seed. A *free* Jacobian would save
229 us once per session — 2.0% of one 256-row ladder, 1.4% at the achievable 2x-4x, and decaying as
1/N in the ladders a session runs. **Not built; `AdMode::ClosedFormAffine` stays
unconsumed.** D68's method in a new subsystem: measure the share before fitting. The guard that
matters now: having read their list, no search that "rediscovers" an item on it is evidence of
anything (D88).
Dropping the final Jacobian outright is NOT the same thing and is wrong: the O3 ladder goes 6.5%
wrong at +25bp and 24% at +100bp.

**D86, 2026-09-29: M4's exit gate is changed, not passed.** The owner took all three of D68 §5's
recommendations. The cross-stage-optimisation clause is retired on Stage A and the cost model's
<25% error target with it — a model with ZERO error is worth 0.166% of an O4 lane batch — and the
optimisation gate is re-pointed at the block solves (~46% of the problem), the reverse ladder
(24.66%, uncosted) and record-plus-build (26.97%). M4 is not retroactively passed: its gate is
replaced because the gate was wrong, and D59's account stands unedited. The framework itself never
failed — saturation fixpoints a 517,036-node tape in 3.9 s, every extracted program verifies — it
was aimed at 2.513% of the problem.

**D87, 2026-09-29: the thesis has only been applied to half the engine.** Asked why D85's win was
found by a person rather than by the engine, the answer is structural: ‖JᵀF‖∞ is written in Eigen in
`src/solver/residual.cpp` because **a derivative cannot be expressed as a tape node** — `Op::Linmap`
is reserved with no producer, and `adjoint::Adjoint` is a compiled artifact, so differentiation is a
pipeline STAGE, not an OPERATION. Every quantity defined by a derivative — the diagnostic, the IFT's
own products, any Hessian — is forced out of the recorded world by construction. Pricing maths is
recorded and collapses 2,565x; the engine's own numerics are hand-written and get none of it, and
D68 measures that half at ~95% of an O4 lane. Third independent route to the same place. §1 states
the limit; D87 §7 gives three options and **decides none** — the owner's call. **D88** follows it
with the vocabulary for the next question — what earns the name *operator* (the catalogue already
discovers kernels and measured 1.11–1.16x; an operator is a kernel that also carries an algebra, a
derivative rule and a structural property), which operators are discoverable at all, and the finding
that a rewrite which is inexact-but-within-budget is licensed **nowhere** in this contract: it fails
the pin's exact-real test above and §5.3 forbids the search from choosing it below. Also undecided.

The M4 search — the e-graph, the cost model and layout rules R1–R7 — stays **quarantined behind
`EPYKOS_LEGACY_SEARCH`, default OFF, and deliberately not deleted** (§7.1). §10 step 11's
term-level e-graph is a *different* object: that one's e-nodes are whole `ir::Program`s and it sits
below the pin. R1, R2 and R5 now find **zero sites** on both fixtures, because the algebra removed
the near-duplicate structure they matched.

`ctest`: **133/133** on the release preset at `d448afd70180`. **CI green on all four jobs** (ubuntu
release / reference / mutation, macos release) at `b294efc`, which is what `main` points at: the
rebuild was merged with the owner on 2026-09-29 (`e85734f`) and **`main` is now the line of
development**. `origin/integrate/rebuild` is retained at `fb5f0ed`, 20 commits behind `main` and
ahead by none — history, not a branch to push to. `v1.0-m4` tags the pre-rebuild engine.

**D93, 2026-10-06: the architecture is now specified as INVARIANTS WITH GATES (§1b), because goals
do not bind.** Asked whether these capabilities should have driven the design from day one: partly —
two representation decisions were load-bearing and unexamined (`SlotKind::Gather` as an addressing
mode, whose reverse is a scatter-accumulate the IR cannot express; `Dual<N>` over hardcoded `double`
rather than `Dual<Scalar,N>`, which forecloses every second derivative — **the engine cannot compute
a gamma by any route**). But the expensive foundations are right, which is why D81's collapse was
possible at all, and fixing either is an extension rather than a demolition. The cause was stating
goals where closure properties were needed, and `DESIGN.md` §6's named-but-ungated W-cache is the
proof that a goal does not survive a milestone. **§1b's three invariants (I1 closed under
differentiation, I2 second order reachable, I3 the engine's own maths recorded) all currently FAIL,
each with a stated gate**, and §10 now derives from them. Not a rewrite; and argued as capability,
not performance, since D92 priced the optimisation case at roughly nothing.

## Rules
- **`docs/PRINCIPLES.md` is the contract** and outranks every other document, this one included. It is **rewritten in
  place**, never appended to; so is the Status block above (PRINCIPLES.md §9).
- **`docs/DECISIONS.md` is an append-only ledger**: the evidence and the reasoning as they stood on the day. Entries are
  cited, not read end to end, and are never edited afterwards except to add a one-line pointer to what superseded them.
  A change to a principle lands as a ledger entry in the same commit as the rewrite of `PRINCIPLES.md`.
- **No code shared with SwapEngine (D11).** Never copy, port or vendor anything from that checkout; the engine is
  written from these docs. **One exception, owner-granted (D78): `tools/h2h/` alone** may compile against its public
  facade so both engines can be timed in one process. It is behind `-DEPYKOS_H2H=ON`, OFF by default, never in CI, and
  `bench/compare/README.md` §0 lists every file read. Nothing under `include/epykos/` or `src/` may follow it.
- **Maths is written once**, templated on `Scalar`. No hand-written derivatives, no per-product hot-path kernels.
- **Recording discipline:** constants are leaves; no implicit `Rec → double`; value branches use `select`;
  `structural_if` only for branches that provably do not depend on inputs; no `std::max/min/abs` on a `Scalar`.
- **A rewrite is licensed by which side of THE PIN it is on** (PRINCIPLES.md §1), not by a declared exactness class.
  Above the pin — canonicalise, simplify, solve recurrences — the engine chooses WHICH expression to evaluate, is judged
  against the recorded expression in exact real arithmetic, and may re-round. Below the pin — inference, planning,
  execution, the adjoint — it chooses HOW, and is bit-identical to the pinned tape under the declared contraction and
  transcendental policy (§5.3). Every rewrite still ships with its differential test and a mutation test.
- **Verification before features:** round-trip identity and differential tests land with the pass that needs them.
- **Performance claims are measured**, per machine+toolchain fingerprint, never compared across fingerprints; state load
  and flags alongside any number. Estimates are labelled as estimates.
- **Fixtures are generated from the seed in `docs/WORKLOADS.md`.** No data files in the repo, except definitions:
  conventions, curve definitions and instrument blueprints are JSON under `blueprints/` (D36).
- **Dependencies are fixed by D12.** Adding one needs a decision entry.
- **Keep the docs current:** a change to the op set, the pipeline or a milestone updates `docs/DESIGN.md` /
  `docs/ROADMAP.md` in the same commit.

## Build
```
scripts/bootstrap.sh            # fetch pinned third_party (Eigen 3.4.0, GoogleTest 1.18.0, Google Benchmark 1.9.5)
cmake --preset release && cmake --build --preset release
ctest --preset release
cmake --preset reference        # -ffp-contract=off build for E0 gates (then build/ctest --preset reference)
cmake --preset debug            # -O0 -g
scripts/mutation_test.sh        # mutation gate: builds the `mutation` preset (reference + EPYKOS_MUTATIONS=ON) and
                                # runs the gate tests once per registered mutant; every mutant must be caught (D32)
cmake --preset profile           # release + EPYKOS_EXEC_PROFILE=ON (D48): tools/costmodel/'s calibration only,
                                # never a gated or perf-gated build (adds instrumentation to every Interpreter::run())
```
Presets build into `build/<preset>/`. Flags per D13; the `-march=x86-64-v3` flag is dropped on non-x86-64 hosts (the
arm64 CI runner) and `build/<preset>/epykos_flags.txt` / `epykos::build_flags()` state the flags actually used.
Fingerprint: `scripts/fingerprint.sh`.
Definitions are data (D36, D42, D43, D44): `blueprints/conventions/*.json` is the conventions registry, `blueprints/instruments/*.json`
the instrument blueprints, `blueprints/curves/*.json` the curve definitions and `blueprints/problems/*.json` the problem
definitions (the Stage A desk problem: `fixtures/stage_a.hpp` fills one from the seed), located at build time (`EPYKOS_BLUEPRINTS_DIR`
= the source tree's `blueprints/`) or by `$EPYKOS_BLUEPRINTS`; the citations are in `docs/G4_BUNDLE.md`.
Benchmarks and the perf gate (D29): `bench/run.sh build/release/bench/<name>_bench [gbench args]` runs one binary (refuses
at 1-minute load > cores/2) and writes `bench/results/<fingerprint-id>/<name>.json` (fingerprint, loads before/after, commit,
preset, parameters, min/median/p90); `scripts/perf_gate.py <that file>` compares it with `baseline.json` of the same fingerprint
only (> 1.25× a median fails, exit 1; load, cross-fingerprint or no baseline: refused, exit 2) and with `bench/targets.json`;
`--accept` moves the baseline — perf commits only, before/after in the message. The baseline is keyed by that `<name>`, the one
`run.sh` derives from the binary, so the default invocation is what it gates; `--name` runs are exploratory, not gated (D34).

Adding code needs no CMake edits (all globs are `CONFIGURE_DEPENDS`): headers under `include/epykos/**` and sources
under `src/**/*.cpp` compile into the library `epykos` (`include/epykos/fixtures/` and `src/fixtures/` are test-only
fixtures, not engine API; engine headers never include them; D28); `tests/**/<name>_test.cpp` becomes the gtest
executable and ctest entry `<subdir>_<name>_test`; `bench/**/<name>_bench.cpp` becomes a Google Benchmark executable
(built, never run by ctest or CI); `bench/hand/` is the hand-fused reference kernel, the static library `epykos_hand`
that `tests/hand/` and the hand benches link (D9, D28). **Tests named `*_e0_test.cpp` are compiled with
`-ffp-contract=off` in every preset** (label `e0`, `ctest -L e0`): that is how E0 gates get the reference flags.
**Library sources named `src/**/*_e0.cpp` are pinned the same way in every preset** (fixture generators, the
interpreter kernels; D25), and so is `bench/hand/*_e0.cpp` (the hand-fused reference; D28) — a clang pragma is not
enough, GCC contracts in C++ even in ISO mode. An E0 gate that crosses into any other code compiled in `src/` runs
under the reference preset.


## Commits
`type(scope): summary`, type ∈ `feat|perf|fix|test|bench|refactor|build|docs|chore`. `perf` commits include measured
before/after. Do not put model identifiers in commits or code.
