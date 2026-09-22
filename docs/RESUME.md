# RESUME — EpykosEngine overnight run (launched 2026-09-22, Mac Pro)

This is the launch brief for the M1–M5 run and the handoff for the morning. Agents: read §0–§3 before touching code.

## 0. Read first
`CLAUDE.md` → `docs/DESIGN.md` → `docs/ROADMAP.md` → `docs/DECISIONS.md` (D1–D20) → `docs/WORKLOADS.md`.
`docs/PRIOR_ART.md` is informational only.

## 1. Rules for every agent
- **No SwapEngine (D11).** Do not open `../SwapEngine` or any other checkout. Everything is written from these docs.
- Maths once, templated on `Scalar`; recording discipline as in `CLAUDE.md`; verification before features (D10).
- Fixtures come from the seed in `WORKLOADS.md`; no data files.
- Dependencies are fixed by D12: Eigen (double side only), GoogleTest, Google Benchmark, nothing else.
- Numbers: fingerprinted (`scripts/fingerprint.sh`), load-checked, flags stated; estimates labelled (D9, D13).
- Git: work on a package branch cut from `integrate/m1-m5`; commit as `type(scope): summary`; no model identifiers;
  merge back only when the package's own tests pass; never touch `main` (D18).
- Docs: op-set / pipeline / milestone changes update `DESIGN.md` / `ROADMAP.md` in the same commit; new decisions
  append to `DECISIONS.md`; each package appends one line to §5 of this file when it merges.
- Report honestly: a failed gate is reported as failed with the numbers, never softened.

## 2. Repository layout (fixed by M1/P0)
```
CMakeLists.txt  CMakePresets.json        presets: release (D13 flags), reference (+ -ffp-contract=off), debug
scripts/bootstrap.sh                     fetch pinned third_party with checksums
scripts/fingerprint.sh                   CPU brand, cores, compiler, flags → id; prints 1-min load
include/epykos/                          public headers, namespace epykos, macros EPY_*
  scalar/     Rec, RecBool, Dual, scalar traits, select/structural_if
  maths/      templated pricing maths: calendar, schedules, curve, legs, swaps, book (M1); schemes (M4); models (M5)
  tape/       node table, opcode enum (one for every pass), CSE/DCE, fold-sum, affine collapse
  ir/         domain IR (plain data, serialisable), signature pass, expander (round-trip)
  exec/       tiled interpreter, tile/batch layout, thread pool (M5)
  adjoint/    M2      rewrite/  M3      catalogue/  M3      solver/  M4      mc/  M5
src/                                     non-template implementation
src/catalogue/generated/                 committed generated kernels (M3)
tests/                                   gtest: unit, roundtrip, differential, adjoint, mutation
bench/                                   Google Benchmark; results in bench/results/<fingerprint>/
tools/catalogue/                         M3 generator
third_party/                             gitignored
.github/workflows/ci.yml                 tests only: ubuntu-latest GCC 13, macos-latest Apple clang
```

## 3. Milestones, packages, gates
Each milestone is one workflow. Packages run in parallel where their interfaces allow; a milestone's review pass
runs last and its findings are fixed before the gate is judged. M(n+1) launches only when M(n) passes (D18).

### M1 — kill test (`WORKLOADS.md` §M1)
| pkg | deliverable | gate |
|---|---|---|
| P0 scaffold | CMake/Ninja, presets, bootstrap, fingerprint, gtest/benchmark wiring, CI workflow, layout above | builds + empty test passes on macOS; CI file lints |
| P1 maths | templated `Scalar` kernel for the M1 book; book generator from seed; `double` instantiation = oracle; `dump` | unit tests vs closed forms; par rates reproduce K/(1+ε) |
| P2 recorder | `Rec`, `RecBool`, taint, constants-as-leaves, `select`/`structural_if`, node table, CSE/DCE, fold-sum, affine collapse | records P1 unmodified; tape replay E0 vs `double` under reference preset |
| P3 signature pass | hash-cons modulo constants → domain IR; columns, gathers, segments; expander | **round-trip identity** on the M1 book and on 3 smaller books; expected domain chain appears |
| P4 interpreter | tiled vector interpreter over IR groups; runtime `B`; tile parameter | E0 vs P2 replay at `B=1`; `B=64` matches 64 single-state runs E0 |
| P5 hand reference | hand-fused kernel for the same book (gather → fma → segment_sum, shared reciprocal, block GEMV), from `DESIGN.md` only | E1 vs P1 `double` (≤ 1e-12) |
| P6 benchmark | `B=1` and `B=64`, tile sweep, fingerprint + load, JSON results | **go: P4 ≤ 1.3× P5 single-state, ≤ 1.1× batched** |
| P7 review | adversarial review of P2–P5: missed branches, hidden allocations, non-determinism, D11 compliance | findings fixed or filed |
Order: P0 → {P1, P2, P5} → P3 → P4 → P6 → P7. Kill path: > 2× ⇒ up to 3 tile/layout iterations; then M3 before M2.

### M2 — verification and adjoints (`WORKLOADS.md` §M2)
| pkg | deliverable | gate |
|---|---|---|
| Q1 differential | state-ball tester; E0 under reference preset, E1 under release | passes on M1 book |
| Q2 forward mode | `Dual` scalar; same maths | tangent vs FD 1e-6 |
| Q3 adjoint | per-group reverse rules; pull transposes (CSR "who reads me"); `linmapᵀ` | vs FD 1e-6, vs Dual 1e-12 |
| Q4 mutation | mutation harness + mutants for fold-sum/CSE/affine/expander/adjoint | every mutant caught |
| Q5 perf gate tooling | baselines per fingerprint, self-regression 1.25×, absolute targets, report | M1 numbers become the baseline |
| Q6 review | adversarial review of Q3/Q4 | findings fixed or filed |

### M3 — rewrites and catalogue (`WORKLOADS.md` §M3)
| pkg | deliverable | gate |
|---|---|---|
| R-a | R1, R2, R3 (uniform columns, buckets, trivial maps) with E0 diff + mutation tests | E0 |
| R-b | R4a, R4b (push unary through gathers; recip), R5 (group formation) | E0 / E1 |
| R-c | R6 (materialise at boundaries), R7 (block linmap) | E0 |
| C1 catalogue generator | hot signatures → C++ → committed; regeneration no-op check in CI; coverage report | catalogued groups ≤ 1.05× hand-fused |
| C2 review | rewrites reviewed for hidden exactness violations | fixed or filed |

### M4 — curves and calibration (`WORKLOADS.md` §M4)
| pkg | deliverable | gate |
|---|---|---|
| S1 linear schemes | Flat, Linear, NaturalCubic, BSpline as templated maths; collapse to `linmap` | round-trip; linmap recovered |
| S2 value-dependent | Hermite, MonotoneCubic (Hyman via `select`); mask/margin/arm-gap export | round-trip; select buckets |
| I1 implicit | `implicit` node; least-squares calibration to the 12 quotes; IFT risk | `‖Jᵀr‖∞ < 1e-12`; IFT vs bump 1e-6 |
| A1 active set | `pin`, `rank_update`, hysteresis; frozen-Newton streaming | kink 2-cycle fixture: failure shown, then converges ≤ 5 iters |
| M4 review | | fixed or filed |

### M5 — batch axis, scan, exposure (`WORKLOADS.md` §M5)
| pkg | deliverable | gate |
|---|---|---|
| T1 scan | recurrence detection → `scan` domains; reverse scan adjoint | round-trip on a compounding fixture |
| T2 parallel | thread pool; Philox RNG; AS241 inverse CDF; fixed-order reductions | bit-identical across threads/tiles |
| T3 models | Hull–White 1F + LGM sharing the affine kernel; `A`, `B` from the M4 curve | LGM = HW to 1e-12 |
| T4 grid | exposure grid tables (per-date vs mask measured); EE via `select` | per-path E0 vs scalar reference; EE 1e-12 |
| T5 bench | 10k × 100 × 1k, 8 threads, fingerprinted | **< 1 s** |
| M5 review | | fixed or filed |

### MX — stretch: G4 bundle comparison (`WORKLOADS.md` §MX, `ROADMAP.md` §MX, D21) — only after M5 passes
| pkg | deliverable | gate |
|---|---|---|
| X1 research | `docs/G4_BUNDLE.md`: per-currency build characteristics, instruments, conventions, calendars, with sources | reviewed for correctness by a second agent |
| X2 conventions | calendars, rolls, day counts, lags, IMM as templated-free data + code | unit tests vs published examples |
| X3 bundle | curves, instruments, joint calibration in EpykosEngine; portfolio + scenarios | calibrates; risk ladder vs bump 1e-6 |
| X4 compare | SwapEngine built and run as a black box on the same bundle; report under `bench/compare/` | informational table with caveats |

## 4. Environment
Mac Pro, Xeon W-3223 (16 cores, x86-64-v3 + AVX-512), Apple clang 21, cmake 4.4, ninja 1.13, Docker available for
Linux/GCC checks. No SwapEngine access for agents.

## 5. Progress log
(appended by packages as they merge: `date  milestone/pkg  result  commit`)
2026-09-22  M1/P2 recorder  landed: Rec/RecBool, Tape, cse/dce/fold_sum/affine_collapse (E0), Replayer; 6/6 tests pass in release, reference and debug  498afc8
2026-09-22  M1/P1 maths  done: calendar, Philox/AS241, seeded book+batch, templated curve/pricing, double oracle table; 9 test executables pass on release/reference/debug (Apple clang) and the oracle digests are bit-identical across the three presets  b2debe4

## 6. Morning report
Written to the PR description and to §7 below: per milestone, gate results with numbers and fingerprint; kill-path
decisions taken; open findings; total agent runs.

## 7. State at end of run
(filled in when the run stops)
