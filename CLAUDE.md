# CLAUDE.md — EpykosEngine

Read `docs/DESIGN.md` first, then `docs/ROADMAP.md`, `docs/DECISIONS.md` and `docs/WORKLOADS.md`. `docs/PRIOR_ART.md`
holds the measured evidence the design rests on. `docs/RESUME.md` is the current handoff / launch brief.

## What this is
A financial computation engine that records pricing maths written once (templated on `Scalar`) and compiles it into a
domain-typed array program executed by pre-compiled fused kernels, with mechanically derived adjoints. **No JIT.**

## Status
M0 design complete. **M1 (kill test) passed 2026-09-23**: verdict go — the tiled interpreter at 1.044× the hand-fused
kernel single-state and 1.079× batched on fingerprint d448afd70180, libm `std::exp` on both sides (D27; `docs/RESUME.md`
§5 "M1 result"). **M2 (verification harness and adjoints) passed 2026-09-23**: the mechanical adjoint agrees with central
finite differences within 2.9e-9 relative (gate 1e-6) and with forward mode (`Dual`) within 2.1e-13 (gate 1e-12) on the
M1 book, all 13 registered mutants are caught by the gates, the differential tester is bitwise against the templated
`double` maths at 256 ball states, and the perf gate holds the M1 numbers as the d448afd70180 baseline (D29–D34;
`docs/RESUME.md` §5 "M2 result"). **M3 (groundwork and the Stage A tape) passed 2026-09-23**: the desk problem of
`docs/PROBLEM.md` §4 Stage A recorded as one tape (2,000 trades, 70 quotes, 4 curves, 1,000 scenario lanes; 517,036
nodes, 67 IR domains, 5 scan domains) with every `PROBLEM.md` §6 gate ok on fingerprint d448afd70180 — adjoint vs FD
9.65e-10, vs Dual 2.79e-15, IFT vs bump-and-recalibrate 5.09e-8, optimality 1.235e-13, 0/262,112 O4 mismatches on the
sampled lanes and 0 lanes not converged on the full 1,000-lane grid, 20/20 mutants caught, timings recorded as the M4
baseline (D35, D44, D45; `docs/RESUME.md` §5 "M3 result"). **M4 (optimise the totality) failed 2026-09-24** (D59;
`docs/RESUME.md` §5 "M4 result"): of `PROBLEM.md` §7's four exit-gate clauses, two hold (every e-graph-extracted
program verifies at its declared exactness class; the self-regression gate against the M3 baseline passes, 17/17
benchmarks, 0 regressions, catalogue coverage giving a real 1.11–1.16× win on the reverse risk ladder) and two miss
(rediscovery of M1's three kill-path fusions ties the cost model's own estimate but misses its 1.02× wall-clock
target at 1.0277×/1.0427×; the one cross-stage optimisation found, re-priced under this fingerprint's real fitted
cost model, is 0.999716× — noise, not a win, so `cross_stage_wins` = 0). The Rule/e-graph/cost-model/catalogue
framework (D47–D49, D54–D55) and rules R1–R7 plus fma-contraction all landed with their exactness class,
differential test and mutation test (46/46 mutants caught); the cost model's mean relative error (84.4%) misses its
<25% target. The GCC-only
catalogue-coverage gap that held CI red on ubuntu-latest throughout M4 (`task_919ea449`) is **fixed 2026-09-24**
(D61): `catalogue::Signature` is now canonical under a commutative step's operand order, which unsequenced operand
evaluation had made compiler-dependent; CI green on all four jobs. **M4's own single largest open finding is also
closed (D62, 2026-09-24)**: `EGraph::saturate` now dedups before it constructs (a (program node, rule, site)
application memo, and only a program class's representative is matched) and carries a re-firing policy, so the full
2,000-trade / 517,036-node Stage A tape saturates to a real fixpoint in 7 rounds and 3.9 s at 3.1 GB (it had never
been saturated at all); the search still finds nothing better than noise under the real fitted cost model
(0.9991x on the full tape), which points at the cost model, not the search. **The cost model's own largest blind
spot is closed (D63, 2026-09-24)**: it priced step pairing at exactly zero and reconstructed the interpreter's
materialisation decision instead of reading it, so every fusion candidate tied with its unfused twin and, on the
M1 book, the two largest domains were priced as materialised when the planner folds them away. `infer_plan` is now
`rewrite::planner::default_plan`, pairing is priced, the calibration grid varies it, and the prediction error
falls 84.4%/69.3% to 74.5%/64.6% — a real improvement, still 3x the <25% target. The Stage A search score
improves sixfold (0.9991x to 0.9948x on the full tape) and is still noise. D63 also measured, for the first time,
what the interpreter's three planning decisions are worth: reduction fusion 1.618x (M1) / 1.066x (Stage A),
inlining 1.124x / 1.000x, step pairing 1.021x / 1.014x — so on Stage A the whole plan-level search has at most
~6.6% available to it. M5
onward awaits the owner's decision on this failed exit gate. Nothing merges to `main` without the owner.

## Rules
- **Decisions are in `docs/DECISIONS.md`.** Changing one means appending a new entry that supersedes it, in the same
  commit as the change.
- **No code shared with SwapEngine (D11).** Never open, copy, port or reference the SwapEngine checkout. Everything
  here is written from these docs.
- **Maths is written once**, templated on `Scalar`. No hand-written derivatives, no per-product hot-path kernels.
- **Recording discipline:** constants are leaves; no implicit `Rec → double`; value branches use `select`;
  `structural_if` only for branches that provably do not depend on inputs; no `std::max/min/abs` on a `Scalar`.
- **Every rewrite declares an exactness class** (E0/E1) and ships with its differential test and a mutation test.
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
