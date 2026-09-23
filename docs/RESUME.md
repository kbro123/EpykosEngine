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
- **Layout is by library structure, never by milestone.** Engine code under `include/epykos/<component>/` and
  `src/<component>/`; seeded fixture books, record helpers and oracles under `include/epykos/fixtures/` and
  `src/fixtures/` (test-only, not engine API); hand-written reference kernels under `bench/hand/`. Milestone names
  appear only as fixture names (`fixtures/m1_book.hpp`) and in test/bench file names. M2/Q0 consolidates what M1
  left under `maths/m1/`, `tape/record_m1.hpp` and `hand/`.

## 2. Repository layout (fixed by M1/P0)
```
CMakeLists.txt  CMakePresets.json        presets: release (D13 flags), reference (+ -ffp-contract=off), debug
scripts/bootstrap.sh                     fetch pinned third_party with checksums
scripts/fingerprint.sh                   CPU brand, cores, compiler, flags → id; prints 1-min load
include/epykos/                          public headers, namespace epykos, macros EPY_*
  scalar/     Rec, RecBool, Dual, scalar traits, select/structural_if
  maths/      templated pricing maths: calendar, schedules; curve/ (schemes, variables, composite); swap/ (legs, swaps); model/ (M5)
  fixtures/   seeded test books (m1_book), record helpers, oracles - test-only, not engine API
  tape/       node table, opcode enum (one for every pass), CSE/DCE, fold-sum, affine collapse
  ir/         domain IR (plain data, serialisable), signature pass, expander (round-trip)
  exec/       tiled interpreter, tile/batch layout, thread pool (M5)
  adjoint/    M2      rewrite/  M3      catalogue/  M3      solver/  M4      mc/  M5
src/                                     non-template implementation
src/catalogue/generated/                 committed generated kernels (M3)
tests/                                   gtest: unit, roundtrip, differential, adjoint, mutation
bench/                                   Google Benchmark; results in bench/results/<fingerprint>/; bench/hand/ = hand-fused reference kernels (D9)
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
| P5 hand reference | hand-fused kernel for the same book (gather → fma → segment_sum, shared reciprocal, block GEMV), from `DESIGN.md` only | E1 vs P1 `double`: ≤ 1e-12 relative to the leg scale `|fixed|+|float|` per swap and `Σ|pv|` for the book (D26); the literal ≤ 1e-12·`|pv|` holds only where `|pv| ≥ 1e-2 × scale` (worst literal 2.4e-13 there; 1.46e-12 on the book over all states) |
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
| S3 variables + regions | interpolation variable (`zero`/`logdf`/`forward`) per scheme; composite curve by region, boundaries as structure | round-trip; no `select` on linear regions; composite calibrates |
| I1 implicit | `implicit` node; least-squares calibration to the 12 quotes; IFT risk | `‖Jᵀr‖∞ < 1e-12`; IFT vs bump 1e-6 |
| A1 active set | `pin`, `rank_update`, hysteresis; frozen-Newton streaming | kink 2-cycle fixture on the composite: failure shown, then converges ≤ 5 iters |
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
Mac Pro, Xeon W-3223 (8 cores / 16 threads, x86-64-v3 + AVX-512), Apple clang 21, cmake 4.4, ninja 1.13, Docker
available for Linux/GCC checks (used by M1/P7 to reproduce the GCC 13 CI jobs). No SwapEngine access for agents.

## 5. Progress log
(appended by packages as they merge: `date  milestone/pkg  result  commit`)
2026-09-22  M1/P2 recorder  landed: Rec/RecBool, Tape, cse/dce/fold_sum/affine_collapse (E0), Replayer; 6/6 tests pass in release, reference and debug  498afc8
2026-09-22  M1/P1 maths  done: calendar, Philox/AS241, seeded book+batch, templated curve/pricing, double oracle table; 9 test executables pass on release/reference/debug (Apple clang) and the oracle digests are bit-identical across the three presets  b2debe4
2026-09-22  M1/P2 integrate  done: record_m1 helper records the UNMODIFIED P1 maths (12 inputs, 1,001 outputs); E0 gate replays bit-identical to price_book<double> at the record point and all 64 states after each of cse/dce/fold_sum/affine_collapse/dce (412,953 nodes recorded -> 112,960 cse -> 84,094 fold_sum -> 79,432 affine -> 75,698 final); affine_collapse fixed to take shared weight·knot products so every interpolated time is one Affine (2,561); 14/14 tests pass on release, reference and debug  25a3389
2026-09-23  M1/P5 hand  E0 bitwise (reference arithmetic) and E1 gate passed at record point + 64 states; eval_batch(64) bitwise = 64 evals; zero allocations. d448afd70180 (load ~2/16): eval 46.0 us, eval_batch(64) 708 us (11.1 us/state), informational. include/epykos/hand, src/hand, tests/hand, bench/hand  [P7 amendment: the E1 gate is relative to the leg scale (|fixed|+|float| per swap, Σ|pv| for the book), D26; the literal 1e-12·|pv| is asserted only where |pv| >= 1e-2 x scale and is exceeded elsewhere: worst literal 2.4e-13 (well-conditioned swaps), 1.46e-12 (book, all 65 states, reference preset)]


2026-09-23  M1/P3 signature  done: domain IR (ir/program.hpp, text-serialisable), infer() (rules in D22), expand() and roundtrip_identical(); round-trip identity holds on the M1 book raw (412,953 nodes) and after passes (75,698 nodes -> 10 domains, 42,314 values), on 3 seeded sub-books and on the mini book with select; expected chain Inputs -> affine -> DF -> {const-rate, forward, float} coupons -> legs -> swaps -> book, no scan domains; E0: expanded tape and ir::Evaluator replay bit-identical to the tape and the double oracle at record point + 64 states; 4 test executables pass on release and reference  156e690
2026-09-23  M1/P4 interpreter  done: exec::Interpreter (include/epykos/exec, src/exec): domains in order, one op per tile of rows x L lanes with kernels picked at construction per (op, operand kinds, lane width 1/4/8/16/32/64 + runtime), batch innermost, runtime B/tile/lane tile, Sum/Affine as length-bucketed transposed whole-domain passes, zero allocations in run(), describe(). E0 bitwise vs the tape replay and price_book<double> at the record point + 64 states at B=1; B=64 = the 64 single runs; every tile in {1,7,128,256,512,4096} x lane tile in {1,3,4,8,16,32,64}; odd B; the 3 P3 sub-books; the mini book with select; 23/23 tests pass on release and reference. d448afd70180 (load 2.4-2.9/16, n=25 medians, bench/results/d448afd70180/m1_interp.json): B=1 71.8 us at tile 256 (70.5 at 512) vs hand std::exp 53.5 -> 1.32x (exp_poly 59.5/58.4 vs 45.9 -> 1.27x); B=64 2808 us at tile 256 lane_tile 8 (2741 at tile 128) = 43.9 us/state vs hand std::exp 1458 -> 1.88x (exp_poly 2107/2039 vs 704 -> 2.9x); vs the hand reference-arithmetic variant (the interpreter's own operation order) 1.08x / 1.23x. Time goes to bytes moved (every coupon row materialised then gathered back by the leg Sum: flat across lane tiles 4-16 and tiles 32-4096) plus libm exp (18% at B=1, 30% at B=64); the fix is R5 fusion (M3), not tiling.  fc5e580
2026-09-23  M1/P6 benchmark  measured on d448afd70180 (load 1.9-3.1/16, n=20, min_time 0.2 s; bench/results/d448afd70180/m1.json + README.md): like for like (std::exp both sides) B=1 interpreter 69.07 us at tile 512 (72.31/70.02/69.07 for 128/256/512) vs hand eval 53.51 us -> b1_ratio 1.291 (go <= 1.3: met); B=64 interpreter 2780.8 us at tile 128 lane_tile 8 (43.4 us/state) vs hand eval_batch 1459.7 us -> b64_ratio 1.905 (go <= 1.1: NOT met; 1.961 at tile 512). Informational: exp_poly both sides 1.27x / 2.89x; vs hand reference arithmetic 1.05x / 1.25x. Gates under the reference preset: P3 round-trip, P4 E0, P5 E1 all pass. Profile (sample): B=1 leg/book segment Sum 30%, coupon-row mul(col,gat)+mul(vec,gat) 32%, libm exp 19%; B=64 libm exp 30%, Sum 23%, coupon muls 27% - bytes moved through materialised coupon rows plus scalar exp; tile sweep flat (< 5%). Verdict left to the orchestrator.

2026-09-23  M1/P4-opt-1 interpreter (kill-path iteration 1)  done: reduction-only elementwise domains are no longer materialised — each whole-domain Sum/Affine block evaluates its fused producers for the member rows (operand tables permuted into member order, contiguous rows) and folds the last step through a reduction epilogue with rows in flight in local accumulators; rows read by a gather/output/per-row Sum are kept and evaluated on their own; E0 unchanged (exec_m1_interp_e0_test + exec_interp_test bitwise, 23/23 ctest in release and reference). On M1 the two coupon domains (31,744 of 42,314 rows) fold into the leg Sum. d448afd70180 (load 2.7-3.3/16, n=20, min_time 0.2 s, std::exp both sides): B=1 64.6 us at tile 512 (69.8/66.9/64.6 for 128/256/512) vs hand eval 53.5 -> 1.21 (was 1.29, go <= 1.3 met); B=64 1968 us at tile 512 lane_tile 32 (flat 1969-1970 at tiles 128/256; lane_tile 16-32 best) vs hand eval_batch 1456 -> 1.35 (was 1.88; go <= 1.1 NOT met; kill threshold 2x cleared). Informational: exp_poly both sides B=1 53.4 vs 45.8 -> 1.17, B=64 1160 vs 704 -> 1.65; vs hand reference-arithmetic variant 3: B=1 0.99, B=64 0.89. Remaining B=64 gap is exp (~750 us on both sides, at parity) plus E0 arithmetic the hand kernel avoids in E1 (d4's 311k true divisions ~200 us vs a shared reciprocal). BM_InterpBuild 867 -> 2072 us (permuted tables). ab452ca
2026-09-23  M1/P6 benchmark (attempt 2, after P4-opt-1)  measured on d448afd70180 (load 2.5-2.9/16, n=20, min_time 0.2 s; bench/results/d448afd70180/m1.json + README.md; engine 363321f): like for like (std::exp both sides) B=1 interpreter 64.75 us at tile 512 (69.72/66.95/64.75 for 128/256/512) vs hand eval 53.66 us -> b1_ratio 1.207 (go <= 1.3: met; was 1.291); B=64 interpreter 1963.5 us at tile 256 lane_tile 32 (30.7 us/state; flat 1966.6/1963.5/1964.9 across tiles at lane_tile 32) vs hand eval_batch 1464.7 us -> b64_ratio 1.340 (go <= 1.1: NOT met; was 1.905; 1.341 at tile 512). Informational: exp_poly both sides 1.166 / 1.640; vs hand reference arithmetic 0.986 / 0.880. Gates under the reference preset: P3 round-trip, P4 E0, P5 E1 all pass (23/23 ctest in reference and release). Profile (sample): B=1 libm exp 20%, fused coupon steps inside the leg-sum blocks 45%, gathered reductions 12%, forwards (div/sub/neg) 9%; B=64 libm exp 44% (at parity with the hand std::exp variant, ~760-860 us both sides), fused coupon steps 27%, forwards 14%; the like-for-like gap (499 us at B=64, 11 us at B=1) is the E0 arithmetic the hand kernel replaces in E1 (311k true divisions vs a shared reciprocal, mul+mul+add vs fma). Verdict left to the orchestrator.
2026-09-23  M1/P4-opt-2 interpreter (kill-path iteration 2)  done: two consecutive chained steps whose middle value has no other reader are one kernel (fused pair: op in {add,sub,mul,div,neg} over literal/column/gather operands, then op2 of that value and one more operand; 143 shapes x plain/indirect/Sum-epilogue/Affine-epilogue per lane width, Options::fuse_pairs), the per-row reduction helpers are forced inline (each row was a stack-argument call), the output copy has the chunk's compile-time width; E0 bitwise on both presets (ctest 23/23). d448afd70180 (load 3.0-3.3/16, n=20, min_time 0.2 s, release): like for like (std::exp both sides) B=1 57.75 us at tile 512 (60.67/59.09/57.75 for 128/256/512) vs hand eval 53.79 -> 1.074 (go <= 1.3: met; was 1.207); B=64 1639.9 us at tile 256 lane_tile 32 (25.6 us/state; lane 16: 1663.9) vs hand eval_batch 1456.7 -> 1.126 (go <= 1.1: NOT met; was 1.340; < 2x kill). Informational: exp_poly both sides 1.101 / 1.345; vs hand reference-arithmetic v3 0.883 / 0.737. Per-domain timers at B=64: libm exp 941 us (at parity: interpreter exp:0-exp:1 = 737 vs hand v2-v0 = 751), leg sums with fused coupons 375 (was 522; a naive fused loop over the same tables measures 366-410), forwards 180 (two IEEE divisions per element, at the divide-throughput floor; the hand kernel's E1 reciprocal is the remaining like-for-like gap), affine 57, swap PVs 50, output copy 32. Tried and dropped (measured no gain or a loss): interleaving same-length reduction blocks across producer patterns, staging the outputs per chunk, folding a single-reader whole-domain Affine into its reader's tiles (+4.7 us at B=1).  be63c95
2026-09-23  M1/P6 benchmark (attempt 3, after P4-opt-2)  measured on d448afd70180 (load 2.3-4.7/16, n=20, min_time 0.2 s; bench/results/d448afd70180/m1.json + README.md; engine 40af34e): like for like (std::exp both sides) B=1 interpreter 59.04 us at tile 512 (62.18/60.28/59.04 for 128/256/512) vs hand eval 53.61 us -> b1_ratio 1.101 (go <= 1.3: met; was 1.207); B=64 interpreter 1640.0 us at tile 256 lane_tile 32 (25.6 us/state; 1646.6/1640.0/1649.6 across tiles at lane_tile 32) vs hand eval_batch 1456.8 us -> b64_ratio 1.126 (go <= 1.1: NOT met; was 1.340; 1.132 at tile 512). Informational: exp_poly both sides 1.101 / 1.344; vs hand reference arithmetic 0.905 / 0.737. Gates under the reference preset: P3 round-trip, P4 E0, P5 E1 all pass (23/23 ctest in reference and release). Profile (sample): B=1 fused float-coupon pair through the leg-sum epilogue 29%, libm exp 20%, fixed coupon 17%, block driver 8%, affine 7%, forwards 6.5%; B=64 libm exp 50% (~820 us, at parity: hand v2-v0 = 752, interpreter exp:0-exp:1 = 692), coupon pairs 18%, forwards 11% (~175 us, two IEEE divisions per element), exp wrapper 5%, output copy 2%; the like-for-like gap is 183 us at B=64 and 5.4 us at B=1: the E0 divisions and three-rounding coupons the hand kernel replaces in E1, plus the output copy. Verdict left to the orchestrator.

2026-09-23  M1/P4-opt-3 interpreter (kill-path iteration 3)  done: three plan-level fusions, applied at 1 lane and at 16 lanes or more (at 4 and 8 lanes their per-row work does not amortise: lane sweep at tile 256, medians us, lane 4/8/16/32/64 = 2024 / 1737 / 1594 / 1571 / 1807 vs P6 attempt 3's 2024 / 1727 / 1656 / 1640 / 1924), all E0 (23/23 ctest under release and reference): (1) an Exp / Log step that only the next step reads and whose operand is a fused pair's value is applied by the pair kernel in place on each stored row while it is in L1 (the exp domain runs as "mul(neg(gat),col) => exp(vec)"; arithmetic tails tried and dropped: no gain over a vectorised pass); (2) a domain read only through the gathers of one materialised elementwise domain (no output, no segment membership, no other reader; a whole-domain Sum / Affine only with uniform materialised members) is evaluated per tile of that consumer into a tile temporary its operand reads by tile row (the interpolated rates, 2,561 rows / 656 KB at 32 lanes, never written to the value buffer: a microbenchmark of that pass measured 13 us hot vs 56 us with its destination evicted, which was its cost), Options::inline_producers; (3) output rows of a producer fused into a whole-domain Sum are written to `out` from the reduction block that computes them when their region is >= 64 KB (the 1,000 swap PVs at 32 lanes: no 248 KB region, no output copy pass). d448afd70180 (load 3.3-4.8/16, n=10, min_time 0.2 s, release, std::exp both sides, same session): B=1 56.71 us at tile 512 (60.22/58.13/56.71 for 128/256/512) vs hand eval 53.95 -> 1.051 (go <= 1.3: met; P6 attempt 3: 1.101); B=64 1570.7 us at tile 256 lane_tile 32 (24.5 us/state) vs hand eval_batch 1466.6 -> 1.071 (1.077 against the after-sweep hand row 1458.8) (go <= 1.1: met in this session; P6 attempt 3: 1.126). Informational: exp_poly both sides 1.091 (50.30 vs 46.11) / 1.245 (879.0 vs 706.3); vs hand reference arithmetic v3 0.865 / 0.704. Per-domain timers (EPYKOS_EXEC_PROFILE) at B=64 lane 32: exp domain 985 -> 938 us (libm floor 845-866 for 164k calls), interpolation pass 57 -> 0, swap PVs 49 -> 0 and output copy 31 -> 0 (the out write moves into the book sum, 7 -> 70), forwards 180 and leg sums 375 unchanged: the remaining like-for-like gap is the E0 forwards (two IEEE divisions per element) and the E0 coupon arithmetic. BM_InterpBuild 3154 (was 1960: the inlined producer's transposed tables) us. e361ee5

## 6. Morning report
Written to the PR description and to §7 below: per milestone, gate results with numbers and fingerprint; kill-path
decisions taken; open findings; total agent runs.

## 7. State at end of run
(filled in when the run stops)
2026-09-23  M1/P6 benchmark (attempt 4, after P4-opt-3)  measured on d448afd70180 (load 3.2-4.7/16, n=20, min_time 0.2 s; bench/results/d448afd70180/m1.json + README.md; engine c4fc0d7): like for like (std::exp both sides) B=1 interpreter 56.72 us at tile 512 (60.32/58.17/56.72 for 128/256/512) vs hand eval 54.17 us -> b1_ratio 1.047 (go <= 1.3: met; was 1.101); B=64 interpreter 1577.5 us at tile 256 lane_tile 32 (24.6 us/state; 1585.7/1577.5/1583.2 across tiles at lane_tile 32) vs hand eval_batch 1467.5 us -> b64_ratio 1.075 (go <= 1.1: met under this pairing; was 1.126; 1.079 at tile 512; 1.054 / 1.082 against the after-sweep hand re-run 53.83 / 1457.4). Informational: exp_poly both sides 1.084 / 1.242; vs hand reference arithmetic 0.859 / 0.703. [P7 amendment: the docs name no hand variant; the "met" holds for the pairing P4/P6 chose (libm std::exp on both sides, hand variant 2); against the hand kernel as delivered (variant 0, exp_poly, E1) the E1-vs-E1 ratio is 1.242 and the gated E0 mode is 2.223 at B=64, both NOT met - the pairing is an orchestrator decision pending in DECISIONS; bench/results/d448afd70180/README.md leads with that table.]
 Gates under the reference preset: P3 round-trip, P4 E0, P5 E1 all pass (23/23 ctest in reference and release). Profile (sample): B=1 fused float-coupon pair through the leg-sum epilogue 30%, libm exp 22%, fixed coupon 16%, block driver 8%, forwards 8%, exp-argument pair with the exp tail 5%, inlined affine 4%; B=64 libm exp 53% (~830 us, at parity: hand v2-v0 = 758, interpreter exp:0-exp:1 = 696), coupon pairs 20%, forwards 11% (~172 us, two IEEE divisions per element), in-block swap-PV pair + emitted-output memmove 4%; the like-for-like gap is 110 us at B=64 and 2.6 us at B=1 (was 183 / 5.4): the E0 divisions and three-rounding coupons the hand kernel replaces in E1. Verdict left to the orchestrator.
2026-09-23  M1/P7-fix review fixes  landed: (1) Domain::recurrent per domain, class flag as scan_class (D23; exp(exp(x)), (a*b)*c shared, level-split chains now run in exec::Interpreter, bitwise vs replay); (2) long-group names "op[N]" so deserialize(serialize(p)) == p (format epykos-ir 2); (3) Rec/RecBool carry the tape serial, foreign-tape and foreign-id use throws RecordError (D24); (4) reference TUs pinned to -ffp-contract=off by the src/**/*_e0.cpp CMake convention on both compilers, false GCC comments deleted, tests/exec/interp_test -> interp_e0_test, new maths_m1_fixture_e0_test (book arithmetic recomputed bitwise in a contraction-free TU; literal digests of the libm-independent fixture, identical on Apple clang 21 and GCC 13; libm-dependent digests differ across libms and are printed only) (D25); (5) D26 defines the E1 leg-scale tolerance and the P5 row/line state it (worst literal 2.4e-13 swaps, 1.46e-12 book); (6) the P6 README leads with the ratio under every pairing (std::exp both sides 1.047/1.075 met; E1 vs E1 1.084/1.242 NOT met at B=64; gated E0 vs hand default 1.225/2.223; vs reference arithmetic 0.859/0.703) - which pairing is the gate is an orchestrator DECISIONS entry still owed; (7) replay_e0_test inputs in separate statements (GCC argument order); (8) WORKLOADS "repetition" defined, the 20-repetition-means deviation stated, statistic table in the README; (9) cores = logical (16; physical reading 4 would use the after-sweep hand denominator: 1.054/1.082), RESUME says 8 cores / 16 threads; (10) WORKLOADS/DESIGN seasoned-first wording per D22. CI: GitHub Actions was red on every integrate/m1-m5 push from P2 to P6 attempt 4 and no package reported it; m1/p7-fix run 35828819535 is green on all 3 jobs (ubuntu GCC 13 release + reference, macOS clang release; 24/24 each), also reproduced locally in Docker (ubuntu 24.04, g++-13.3: 24/24 both presets) and on Apple clang 21 (24/24 both presets). No performance-relevant code changed on the gated toolchain; the interpreter/hand kernel numbers stand.
2026-09-23  M1/P6 benchmark (attempt 5, after the P7 review fixes)  measured on d448afd70180 (load 1.9-3.4/16, n=20, min_time 0.2 s; bench/results/d448afd70180/m1.json + README.md; engine b32182a, the first measured commit with green CI): like for like (std::exp both sides) B=1 interpreter 56.60 us at tile 512 (60.27/58.50/56.60 for 128/256/512) vs hand eval 54.23 us -> b1_ratio 1.044 (go <= 1.3: met; attempt 4: 1.047); B=64 interpreter 1584.0 us at tile 256 lane_tile 32 (24.8 us/state; 1587.5/1584.0/1586.5 across tiles at lane_tile 32) vs hand eval_batch 1467.7 us -> b64_ratio 1.079 (go <= 1.1: met; attempt 4: 1.075; 1.081 at tile 512; 1.053 / 1.087 against the after-sweep hand re-run 53.77 / 1457.3). Informational: exp_poly both sides 1.077 / 1.240; vs hand reference arithmetic 0.859 / 0.705. Gates under the reference preset: P3 round-trip, P4 E0, P5 E1 all pass (24/24 ctest in reference and release). Every row within 1.2% of attempt 4 (the P7 fixes touch no run()-path code). Profile (sample): B=1 fused float-coupon pair through the leg-sum epilogue 28%, libm exp 22%, fixed coupon 17%, block driver 8%, forwards 8%; B=64 libm exp 54% (~855 us, at parity: hand v2-v0 = 759, interpreter exp:0-exp:1 = 705), coupon pairs 19%, forwards 11% (~175 us, two IEEE divisions per element); the like-for-like gap is 116 us at B=64 and 2.4 us at B=1: the E0 divisions and three-rounding coupons the hand kernel replaces in E1. Verdict left to the orchestrator.
