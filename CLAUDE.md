# CLAUDE.md — EpykosEngine

**Read `docs/PRINCIPLES.md` first** — it is the contract and it outranks everything else here. Then `docs/DESIGN.md`,
`docs/ROADMAP.md` and `docs/WORKLOADS.md`. `docs/DECISIONS.md` is the append-only ledger: cite it, do not read it end to
end. `docs/PRIOR_ART.md` holds the measured evidence the design rests on; `docs/RESUME.md` is the handoff brief.

## What this is
A financial computation engine that records pricing maths written once (templated on `Scalar`) and compiles it into a
domain-typed array program executed by pre-compiled fused kernels, with mechanically derived adjoints. **No JIT.**

## Status
**Rebuilding the front half of the compiler.** `docs/PRINCIPLES.md` is the contract and is authoritative;
this paragraph is a summary of it and is rewritten, never extended (PRINCIPLES.md §9).

M1 (kill test), M2 (verification and adjoints) and M3 (the Stage A tape) passed 2026-09-23. **M4 (optimise
the totality) failed its exit gate 2026-09-24** (D59), and the post-mortem found the cause is structural,
not a tuning problem: the engine has a good back half — inference, planning, tiling, the catalogue, the
adjoint, the IFT — and **no front half at all**. There is no canonicalisation, no algebraic simplification
and no recurrence solving anywhere in it, and the tape passes were contracted to be bit-identical, which
forbids all three. D68 measured the consequence: the whole plan-level dynamic range on Stage A is
1.027x–1.042x and `exec::Interpreter` is 2.513% of the problem, so the space M4 searched was worth about
one part in a thousand of the wall clock. D78 measured what was outside it: 33x–132x on the same problem.

**The engine as M4 left it is tagged `v1.0-m4`** and stays addressable and re-measurable.

Where it stands against a specialist, measured in one process on one clock over 25 knots and 1,000 trades
(D78, `tools/h2h`): the risk ladder is **5.16x faster** and crosses over at about 32 trades because an IFT
ladder scales with the knots and not the book; cold calibration is **1.9x slower** and warm recalibration
**7.1x slower**. Those are with the coupon hand-telescoped. As the engine records it today they are 132x
and 33x worse again, which is the missing front half priced.

Work in flight, in `PRINCIPLES.md` §10 order: a compile entry point, quarantine then delete the 12,219
lines of e-graph and layout-rule machinery no execution path reaches (§7.1), then canonicalise → simplify
→ solve recurrences above the pin. Telescoping is the worked example that proves the contract end to end.
Nothing below the pin is rewritten; nothing below the pin is implicated. Nothing merges to `main` without
the owner.
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
