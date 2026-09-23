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
2026-09-22  M1/P2 integrate  done: record_m1 helper records the UNMODIFIED P1 maths (12 inputs, 1,001 outputs); E0 gate replays bit-identical to price_book<double> at the record point and all 64 states after each of cse/dce/fold_sum/affine_collapse/dce (412,953 nodes recorded -> 112,960 cse -> 84,094 fold_sum -> 79,432 affine -> 75,698 final); affine_collapse fixed to take shared weight·knot products so every interpolated time is one Affine (2,561); 14/14 tests pass on release, reference and debug  25a3389
2026-09-23  M1/P5 hand  E0 bitwise (reference arithmetic) and E1 gate passed at record point + 64 states; eval_batch(64) bitwise = 64 evals; zero allocations. d448afd70180 (load ~2/16): eval 46.0 us, eval_batch(64) 708 us (11.1 us/state), informational. include/epykos/hand, src/hand, tests/hand, bench/hand

2026-09-23  M1/P3 signature  done: domain IR (ir/program.hpp, text-serialisable), infer() (rules in D22), expand() and roundtrip_identical(); round-trip identity holds on the M1 book raw (412,953 nodes) and after passes (75,698 nodes -> 10 domains, 42,314 values), on 3 seeded sub-books and on the mini book with select; expected chain Inputs -> affine -> DF -> {const-rate, forward, float} coupons -> legs -> swaps -> book, no scan domains; E0: expanded tape and ir::Evaluator replay bit-identical to the tape and the double oracle at record point + 64 states; 4 test executables pass on release and reference  156e690
2026-09-23  M1/P4 interpreter  done: exec::Interpreter (include/epykos/exec, src/exec): domains in order, one op per tile of rows x L lanes with kernels picked at construction per (op, operand kinds, lane width 1/4/8/16/32/64 + runtime), batch innermost, runtime B/tile/lane tile, Sum/Affine as length-bucketed transposed whole-domain passes, zero allocations in run(), describe(). E0 bitwise vs the tape replay and price_book<double> at the record point + 64 states at B=1; B=64 = the 64 single runs; every tile in {1,7,128,256,512,4096} x lane tile in {1,3,4,8,16,32,64}; odd B; the 3 P3 sub-books; the mini book with select; 23/23 tests pass on release and reference. d448afd70180 (load 2.4-2.9/16, n=25 medians, bench/results/d448afd70180/m1_interp.json): B=1 71.8 us at tile 256 (70.5 at 512) vs hand std::exp 53.5 -> 1.32x (exp_poly 59.5/58.4 vs 45.9 -> 1.27x); B=64 2808 us at tile 256 lane_tile 8 (2741 at tile 128) = 43.9 us/state vs hand std::exp 1458 -> 1.88x (exp_poly 2107/2039 vs 704 -> 2.9x); vs the hand reference-arithmetic variant (the interpreter's own operation order) 1.08x / 1.23x. Time goes to bytes moved (every coupon row materialised then gathered back by the leg Sum: flat across lane tiles 4-16 and tiles 32-4096) plus libm exp (18% at B=1, 30% at B=64); the fix is R5 fusion (M3), not tiling.  fc5e580
2026-09-23  M1/P6 benchmark  measured on d448afd70180 (load 1.9-3.1/16, n=20, min_time 0.2 s; bench/results/d448afd70180/m1.json + README.md): like for like (std::exp both sides) B=1 interpreter 69.07 us at tile 512 (72.31/70.02/69.07 for 128/256/512) vs hand eval 53.51 us -> b1_ratio 1.291 (go <= 1.3: met); B=64 interpreter 2780.8 us at tile 128 lane_tile 8 (43.4 us/state) vs hand eval_batch 1459.7 us -> b64_ratio 1.905 (go <= 1.1: NOT met; 1.961 at tile 512). Informational: exp_poly both sides 1.27x / 2.89x; vs hand reference arithmetic 1.05x / 1.25x. Gates under the reference preset: P3 round-trip, P4 E0, P5 E1 all pass. Profile (sample): B=1 leg/book segment Sum 30%, coupon-row mul(col,gat)+mul(vec,gat) 32%, libm exp 19%; B=64 libm exp 30%, Sum 23%, coupon muls 27% - bytes moved through materialised coupon rows plus scalar exp; tile sweep flat (< 5%). Verdict left to the orchestrator.

## 6. Morning report
Written to the PR description and to §7 below: per milestone, gate results with numbers and fingerprint; kill-path
decisions taken; open findings; total agent runs.

## 7. State at end of run
(filled in when the run stops)
