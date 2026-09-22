# CLAUDE.md — EpykosEngine

Read `docs/DESIGN.md` first, then `docs/ROADMAP.md`, `docs/DECISIONS.md` and `docs/WORKLOADS.md`. `docs/PRIOR_ART.md`
holds the measured evidence the design rests on. `docs/RESUME.md` is the current handoff / launch brief.

## What this is
A financial computation engine that records pricing maths written once (templated on `Scalar`) and compiles it into a
domain-typed array program executed by pre-compiled fused kernels, with mechanically derived adjoints. **No JIT.**

## Status
M0 design complete. M1–M5 in progress on branch `integrate/m1-m5` (see `docs/RESUME.md`). Nothing merges to `main`
without the owner.

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
scripts/bootstrap.sh            # fetch pinned third_party (Eigen, GoogleTest, Google Benchmark)
cmake --preset release && cmake --build --preset release
ctest --preset release
cmake --preset reference        # -ffp-contract=off build for E0 gates
```
Flags per D13. Fingerprint: `scripts/fingerprint.sh`.

## Commits
`type(scope): summary`, type ∈ `feat|perf|fix|test|bench|refactor|build|docs|chore`. `perf` commits include measured
before/after. Do not put model identifiers in commits or code.
