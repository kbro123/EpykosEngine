# CLAUDE.md — EpykosEngine

Read `docs/DESIGN.md` first, then `docs/ROADMAP.md`, `docs/DECISIONS.md` and `docs/WORKLOADS.md`. `docs/PRIOR_ART.md`
holds the measured evidence the design rests on. `docs/RESUME.md` is the current handoff / launch brief.

## What this is
A financial computation engine that records pricing maths written once (templated on `Scalar`) and compiles it into a
domain-typed array program executed by pre-compiled fused kernels, with mechanically derived adjoints. **No JIT.**

## Status
M0 design complete. **M1 (kill test) passed 2026-09-23**: verdict go — the tiled interpreter at 1.044× the hand-fused
kernel single-state and 1.079× batched on fingerprint d448afd70180, libm `std::exp` on both sides (D27; `docs/RESUME.md`
§5 "M1 result"). M2–M5 in progress on branch `integrate/m1-m5` (see `docs/RESUME.md`). Nothing merges to `main` without
the owner.

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
- **Fixtures are generated from the seed in `docs/WORKLOADS.md`.** No data files in the repo.
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
```
Presets build into `build/<preset>/`. Flags per D13; the `-march=x86-64-v3` flag is dropped on non-x86-64 hosts (the
arm64 CI runner) and `build/<preset>/epykos_flags.txt` / `epykos::build_flags()` state the flags actually used.
Fingerprint: `scripts/fingerprint.sh`.
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
