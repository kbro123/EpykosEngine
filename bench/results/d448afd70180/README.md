# M1 gate benchmark (P6) — fingerprint `d448afd70180`

Intel(R) Xeon(R) W-3223 CPU @ 3.50GHz, 8 physical / 16 logical cores, Apple clang version 21.0.0 (clang-2100.1.1.101), flags `-O3 -march=x86-64-v3 -fno-math-errno`. 1-minute load before measuring: 4.73, after: 3.17.
Engine commit measured: `c4fc0d7` (integrate/m1-m5 after P4-opt-3 e361ee5: an Exp/Log step read only by the next step is applied in place by the pair kernel on each row while it is in L1 (the exp domain runs as mul(neg(gat),col) => exp(vec)); a domain read only through one elementwise domain's gathers is evaluated per tile of that consumer into a tile temporary and never materialised (the knots -> times affine, 2,561 rows, inside the exp tiles); output rows of a producer fused into a whole-domain Sum are written to out from the reduction block that computes them (the 969 swap PVs of d7 inside the book sum: no output copy pass); all three applied at 1 lane and at 16 lanes or more only. Attempt 3 measured 40af34e (fused step pairs), attempt 2 363321f (reduction fusion), attempt 1 1805412).
Google Benchmark, `--benchmark_repetitions=20 --benchmark_min_time=0.2s --benchmark_report_aggregates_only=false`; tables prebuilt, state written fresh each iteration; stats over the 20 repetition means (real time, us; see Measurement statistic). Interpreter and hand kernel compiled with the release preset (D13). Correctness gates run under the reference preset.

## Read first: the outcome depends on the pairing

ROADMAP.md M1 Go and RESUME.md P6 say "interpreter within 1.3x of the hand-fused kernel single-state, within 1.1x batched" and name no hand variant or exp implementation. The table below gives the ratio under every candidate denominator (medians, us). The gate table that follows uses the pairing P4/P6 chose (row 1); which pairing is the gate is a DECISIONS entry the orchestrator owes before the M1 verdict (M1/P7 review).

| pairing | exactness | B=1 interp / hand | B=1 ratio (<= 1.3) | B=64 interp / hand | B=64 ratio (<= 1.1) |
|---|---|---:|---:|---:|---:|
| interpreter std::exp vs hand variant 2 (fused/shared-recip/std::exp): the P4/P6 like-for-like pairing, libm exp on both sides | interpreter E0 vs hand E1 | 56.72 / 54.17 | **1.047** (met) | 1577.5 / 1467.5 | **1.075** (met) |
| interpreter exp_poly vs hand variant 0 (default: fused/shared-recip/exp_poly): the same exactness class on both sides | E1 vs E1 | 50.20 / 46.32 | **1.084** (met) | 881.3 / 709.7 | **1.242** (NOT met) |
| interpreter std::exp (the gated E0 mode) vs hand variant 0 (default, as specified and delivered by P5) | E0 vs E1 | 56.72 / 46.32 | **1.225** (met) | 1577.5 / 709.7 | **2.223** (NOT met) |
| interpreter std::exp vs hand variant 3 (reference arithmetic: the interpreter's own operation order, std::exp) | E0 vs E0 | 56.72 / 66.01 | **0.859** (met) | 1577.5 / 2244.7 | **0.703** (met) |

Row 1 puts the identical scalar libm exp on both sides: at B=64 it is 696 us of the interpreter's 1577.5 (exp:0 minus exp:1 at the gate point) and 758 us of the hand kernel's 1467.5 (variant 2 minus variant 0), so the non-exp remainder ratio is 1.242, the same as the E1-vs-E1 row. Against the hand kernel as specified and delivered (variant 0, exp_poly, E1) the interpreter's gated E0 mode is 2.223x batched.

## Gate (like for like: interpreter std::exp vs hand fused/shared-recip/std::exp)

| row | config | min | median | p90 | ratio | go |
|---|---|---:|---:|---:|---:|---|
| interpreter B=1 | tile 512, lane_tile 1, std::exp | 56.2 | 56.7 | 57.8 | **1.047** | <= 1.3: yes |
| hand eval B=1 | variant 2 | 53.8 | 54.2 | 54.4 | 1 | |
| interpreter B=64 | tile 256, lane_tile 32, std::exp | 1568.3 | 1577.5 | 1585.3 | **1.075** | <= 1.1: yes |
| hand eval_batch B=64 | variant 2 | 1462.2 | 1467.5 | 1475.3 | 1 | |

B=64 at the B=1-best tile (512): best lane_tile 32, median 1583.2 us, ratio 1.079.

## Measurement statistic

min / median / p90 over n Google Benchmark repetitions; each repetition's real_time is the mean of its iterations (>= min_time of evaluations), so p90 is a p90 of repetition means, not of evaluations. docs/WORKLOADS.md M1 Measurement asks for >= 200 repetitions (timed evaluations); this round has n = 20 repetition means per row, a stated deviation; the gate uses medians. Gate rows:

| row | n (repetition means) | iterations per repetition (min..max) | timed evaluations |
|---|---:|---:|---:|
| interpreter B=1 | 20 | 4961..4961 | 99220 |
| hand eval B=1 | 20 | 5117..5117 | 102340 |
| interpreter B=64 | 20 | 177..177 | 3540 |
| hand eval_batch B=64 | 20 | 192..192 | 3840 |

## Load rule

docs/WORKLOADS.md Terms: cores = logical CPUs (hardware threads): threshold 8 (physical reading: 4). 1-minute loads during measurement: 4.73, 4.29, 3.47, 3.17; within the logical threshold: yes; within the physical threshold: no. Under the physical reading the before-sweep hand run (load 4.73) would be discarded and the after-sweep re-run (load 3.47) used as the denominator: ratios 1.054 / 1.082.

## CI

GitHub Actions (ubuntu GCC 13 release + reference, macOS Apple clang release): green: all 3 jobs pass (macos-latest clang release, ubuntu-latest GCC 13 release, ubuntu-latest GCC 13 reference; 24/24 tests each) at `a8f3e6a (m1/p7-fix = integrate/m1-m5 4a4bb87 + the P7 review fixes)` (https://github.com/kbro123/EpykosEngine/actions/runs/35828819535). The first green run since P0 (2026-09-22T21:42Z): every push to integrate/m1-m5 from P2 to P6 attempt 4 (32 runs, 2026-09-22T22:13Z to 2026-09-23T05:54Z) was red because the reference TUs were pinned to -ffp-contract=off with a clang-only pragma, so under GCC 13 release exec_m1_interp_e0_test, hand_m1_hand_e0_test and exec_interp_test failed at rounding level, and tape_replay_e0_test failed under both GCC presets on argument evaluation order (D25). The numbers in this README were measured on Apple clang at c4fc0d7 and are unaffected: the fix pins on GCC what clang already had pinned and changes no arithmetic; 'exec_interp_test' in the gates evidence is now exec_interp_e0_test.

## Correctness gates (reference preset, -ffp-contract=off)

| gate | test executable | result |
|---|---|---|
| P3 round-trip identity | ir_roundtrip_test | pass |
| P4 E0 (interpreter vs replay and oracle, B=1 and B=64) | exec_m1_interp_e0_test | pass |
| P5 E1 (hand kernel vs double maths, 1e-12) | hand_m1_hand_test | pass |

`ir_roundtrip_test`: exit 0; [  PASSED  ] 5 tests.

`exec_m1_interp_e0_test`: exit 0; [  PASSED  ] 5 tests.

`hand_m1_hand_test`: exit 0; [  PASSED  ] 9 tests.

`hand_m1_hand_e0_test`: exit 0; [  PASSED  ] 4 tests.

`exec_interp_test`: exit 0; [  PASSED  ] 6 tests.

`ctest_reference`: 100% tests passed out of 23

`ctest_release`: 100% tests passed out of 23

## Hand kernel (all variants)

| benchmark | variant | min | median | p90 | us/state |
|---|---|---:|---:|---:|---:|
| BM_HandEval/0 | 0 fused/shared-recip/exp_poly | 46.0 | 46.3 | 46.6 | 46.32 |
| BM_HandEval/1 | 1 fused/per-row-div/exp_poly | 46.7 | 47.2 | 47.5 | 47.17 |
| BM_HandEval/2 | 2 fused/shared-recip/std::exp | 53.8 | 54.2 | 54.4 | 54.17 |
| BM_HandEval/3 | 3 reference-arith/std::exp | 65.4 | 66.0 | 66.2 | 66.01 |
| BM_HandEvalBatch/0 | 0 fused/shared-recip/exp_poly | 704.4 | 709.7 | 713.0 | 11.09 |
| BM_HandEvalBatch/1 | 1 fused/per-row-div/exp_poly | 909.3 | 915.9 | 922.0 | 14.31 |
| BM_HandEvalBatch/2 | 2 fused/shared-recip/std::exp | 1462.2 | 1467.5 | 1475.3 | 22.93 |
| BM_HandEvalBatch/3 | 3 reference-arith/std::exp | 2236.9 | 2244.7 | 2254.5 | 35.07 |
| BM_HandBuild | table build | 2339.9 | 2406.6 | 2433.3 | |

## Interpreter tile sweep (D15), medians in us

B=1 (lane_tile 1):

| tile | std::exp min | median | p90 | exp_poly min | median | p90 |
|---:|---:|---:|---:|---:|---:|---:|
| 128 | 59.5 | 60.3 | 61.0 | 53.3 | 53.8 | 54.1 |
| 256 | 57.7 | 58.2 | 58.7 | 51.2 | 51.6 | 52.0 |
| 512 | 56.2 | 56.7 | 57.8 | 49.5 | 50.2 | 50.6 |

B=64, std::exp (median us per call of 64 states; us/state = median/64):

| tile \ lane_tile | 4 | 8 | 16 | 32 | 64 |
|---:|---:|---:|---:|---:|---:|
| 128 | 2085.1 | 1788.1 | 1611.1 | 1585.7 | 1814.1 |
| 256 | 2029.6 | 1746.7 | 1604.7 | 1577.5 | 1818.2 |
| 512 | 1988.0 | 1733.5 | 1603.3 | 1583.2 | 1827.3 |

B=64, exp_poly (median us):

| tile \ lane_tile | 4 | 8 | 16 | 32 | 64 |
|---:|---:|---:|---:|---:|---:|
| 128 | 1328.8 | 1035.1 | 922.5 | 886.6 | 1095.9 |
| 256 | 1281.9 | 1006.3 | 915.2 | 881.3 | 1103.0 |
| 512 | 1248.0 | 1006.8 | 918.5 | 886.0 | 1109.6 |

## Informational ratios (D9)

- exp_poly both sides: B=1 1.084 (interp tile 512 50.2 us vs hand variant 0 46.3 us); B=64 1.242 (tile 256 lane_tile 32 881.3 us vs 709.7 us).
- vs the hand reference-arithmetic variant 3 (the interpreter's own operation order, no fma, division, std::exp): B=1 0.859, B=64 0.703.
- BM_InterpBuild: median 2317.831 us (min 2240.878, p90 2384.628).
- BM_InterpPipeline: median 141.980 ms (min 137.576, p90 143.981).

## Compared with the previous measurement

Attempt 3 (commit 10e44e2 results, engine 40af34e, same fingerprint, same flags and repetitions) -> attempt 4 (engine c4fc0d7, after P4-opt-3 e361ee5: exp tail inside the pair kernel, inlined single-reader producers, output rows emitted from the reduction blocks), medians in us, like for like (std::exp both sides):

| row | attempt 3 | attempt 4 | change |
|---|---:|---:|---:|
| interpreter B=1 (best tile) | 59.04 (tile 512) | 56.72 (tile 512) | -3.9% |
| hand eval variant 2 | 53.61 | 54.17 | +1.0% |
| b1_ratio | 1.101 | 1.047 | |
| interpreter B=64 (best point) | 1640.0 (tile 256, lane_tile 32) | 1577.5 (tile 256, lane_tile 32) | -3.8% |
| hand eval_batch variant 2 | 1456.8 | 1467.5 | +0.7% |
| b64_ratio | 1.126 | 1.075 | |
| interpreter B=1 exp_poly | 50.59 (tile 512) | 50.20 (tile 512) | -0.8% |
| interpreter B=64 exp_poly | 947.1 (tile 128, lane_tile 32) | 881.3 (tile 256, lane_tile 32) | -6.9% |
| BM_InterpBuild | 1959.6 | 2317.8 | +18.3% |
| BM_InterpPipeline (ms) | 135.3 | 142.0 | +4.9% |

The hand kernel is unchanged; all nine of its rows are 0.7-1.4% slower in this session than in attempt 3 (eval v0/v1/v2/v3 46.32/47.17/54.17/66.01 vs 45.96/46.51/53.61/65.26; eval_batch 709.7/915.9/1467.5/2244.7 vs 704.65/905.06/1456.8/2226.1; HandBuild +2.3%), a session-level drift that the after-sweep re-run of variant 2 (53.83 / 1457.4, within 0.4% / 0.04% of attempt 3) confirms; with the after-sweep denominators the ratios are 1.054 and 1.082, so the outcome does not depend on which hand run is the denominator. The P4-opt-3 commit measured the same engine in its own session (n=10) at 56.71 us (B=1, tile 512) and 1570.7 us (B=64, tile 256, lane_tile 32) against hand variant 2 at 53.95 / 1466.6, ratios 1.051 / 1.071: this run reproduces the B=1 point to 0.02% and the B=64 point to 0.4%, with ratios 1.047 / 1.075. BM_InterpBuild is +18% (the inlined producer's transposed tables), a one-off structure cost (DESIGN.md section 9), not on the gate. The B=64 lane-tile optimum stays at 32 (16 within 1.7%: 1604.7); lane_tile 8 is now +10.7% (was +5.3%) and 4 is +28.7% because the three P4-opt-3 fusions are off at 4 and 8 lanes by design (their per-row work does not amortise there); 64 is +15.3%. B=64 is flat across tiles at lane_tile 32 (1585.7 / 1577.5 / 1583.2, within 0.5%); B=1 stays monotone in tile (60.32 / 58.17 / 56.72).

## Profile

`sample(1)` (macOS, 6 s at 1 ms, main thread, release binary without debug info; symbol template arguments decoded with Op {3 Sub, 4 Mul, 5 Div, 6 Neg, 7 Exp}, operand Kind {0 Vec, 1 Lit, 2 Col, 3 Gat} and pair-operand PK {0 S = literal/column scalar, 1 G = gathered row, 2 none}) of the two gate configurations, share of samples by top-of-stack frame (raw output in tmp/profile_b1.txt, tmp/profile_b64.txt, gitignored).

B=1, tile 512, lane_tile 1, std::exp (4667 samples): the fused float-coupon pair folded through the leg-sum epilogue `k_pair_acc<Mul,S,G,Mul,G>` (`N*tau x DF(e) x fwd` accumulated straight into the leg sum) 30.0%; libm `exp` 21.5%; the fixed-coupon step through the epilogue `k_binary_acc<Mul,Col,Gat>` 16.4%; `seg_block_fused<false,1>` (the leg-sum block driver) 7.9%; the forwards domain d4 (`k_pair<Div,G,G,Sub,S>` = `sub(div(gat,gat),lit)` 5.1%, `k_binary<Div,Vec,Col>` = `/tau` 2.8%) 7.9%; the exp-argument pair with the exp tail `k_pair<Neg,G,None,Mul,S>` (`mul(neg(gat),col) => exp`, the frame that calls libm) 4.7%; the book sum `seg_block_gathered<false,1>` 4.0%; the inlined knots -> times affine `k_seg_list<true,1>` (evaluated per exp tile, replaces last attempt's 7.4% `seg_block_gathered<true,1>` pass) 3.9%; the swap-PV pair `k_pair<Sub,G,G,Mul,S>` 1.6%; `copy_out` 1.0%; `Interpreter::run` + `eval_group` dispatch 0.7%.

B=64, tile 256, lane_tile 32, std::exp (4693 samples): libm `exp` 52.6% (+0.3% stub; scalar libm, 2564 x 64 = 164k calls per run, about 830 us of the 1577); the float-coupon pair `k_pair_acc<Mul,S,G,Mul,G,32>` 11.4%; the fixed-coupon step `k_binary_acc<Mul,Col,Gat,32>` 8.2%; the forwards domain d4 (`k_pair<Div,G,G,Sub,S>` 5.5%, `k_binary<Div,Vec,Col>` 5.4%) 10.9% (about 172 us: 2 x 2,432 true IEEE divisions per lane, 311k per run); `seg_block_fused<false,32>` 4.5%; the exp-argument pair with the exp tail 4.4%; the swap-PV pair `k_pair<Sub,G,G,Mul,S,32>` (d7, now evaluated inside the book-sum block) 2.4%; the inlined affine `k_seg_list<true,32>` 2.0%; `memmove` called from `emit_run_outputs<32>` (the 969 swap-PV rows written to `out` from the book-sum block) 1.8% (+0.2% in `emit_run_outputs` itself); `k_load_acc` 0.4%; `Interpreter::run` 0.3%; no `copy_out` frame (32 of the 1001 outputs are copied, 969 emitted).

What dominates: at B=1 the coupon arithmetic inside the leg-sum blocks (46%) and libm exp (22%); at B=64 libm exp at 53%, the coupon arithmetic at 20% and the forwards' divisions at 11%. The three P4-opt-3 fusions removed the frames they targeted: last attempt's separate knots -> times affine pass (7.4% at B=1, 3.4% at B=64) is replaced by the cheaper inlined `k_seg_list` (3.9% / 2.0%), the `k_unary<Exp,Vec>` wrapper (2.2% / 5.1%) is gone (exp is called from the pair kernel while the row is in L1), and the swap-PV domain pass plus the output copy (1.4% + 0.8% at B=1; 2.9% + 2.1% at B=64) became the in-block swap-PV pair plus the memmove of its rows (1.6% + 1.0%; 2.4% + 2.0%). At B=64 exp is at parity with the hand std::exp variant (interpreter exp:0 - exp:1 at the same point = 696 us; hand v2 - v0 = 758 us), so it is not part of the like-for-like gap. That gap is now 110 us at B=64 (1577.5 vs 1467.5; was 183) and 2.6 us at B=1 (56.72 vs 54.17; was 5.4): the E0 arithmetic the hand kernel replaces in E1 - the forwards' two IEEE divisions per element (about 172 us at B=64, at the divide-throughput floor) against one reciprocal per unique time plus an fma, and the fixed and float coupons at three roundings as recorded against gather -> fma -> segment sum - plus the reduction block drivers with no hand-loop counterpart. Consistent with this, against the hand reference-arithmetic variant 3 (the interpreter's own operation order) the interpreter is faster: 0.859 at B=1 and 0.703 at B=64. The remaining candidates are unchanged: R4b shared reciprocal (E1), fma contraction in the fused coupon pair (E1), and a cross-lane-vectorised exp (exp_poly, E1: 881.3 us at B=64, 1.242x the hand exp_poly variant).

## Caveats

- Same fingerprint as P4/P5/attempts 1-3 (d448afd70180): Intel Xeon W-3223, Apple clang 21, -O3 -march=x86-64-v3 -fno-math-errno; release preset for timings, reference preset (every TU -ffp-contract=off) for the gates; machine otherwise idle (1-minute load 4.73 before the hand bench, 4.29 before the interpreter sweep, 3.47 before the hand re-run, 3.17 after; the cores/2 = 8 discard threshold was never approached during measurement; nothing was built or tested during measurement: both presets were built and all tests run first, and the load had decayed below 5 before the first benchmark started; the only other CPU users were the desktop's own processes at < 20% of one core).
- Google Benchmark real time over 20 repetitions of >= 0.2 s each; p90 is the nearest-rank 18th of 20 sorted repetitions. Both benchmarks write the state fresh each iteration (verified by reading bench/exec/m1_interp_bench.cpp and bench/hand/m1_hand_bench.cpp at c4fc0d7: z = z0 + 1e-9 * k drift inside the timed loop, DoNotOptimize + ClobberMemory) and build tables in the constructor outside the timed loop; nothing is cached across iterations; no fix was needed.
- Hand kernel variant 2 re-measured after the interpreter sweep (tmp/m1_p6_hand_after.json, gitignored): eval 53.83 us (min 53.47, p90 54.04) vs 54.17 before (-0.6%), eval_batch 1457.4 (min 1449.8, p90 1463.4) vs 1467.5 before (-0.7%); the ratios use the before-sweep run (the measurement protocol's order); with the after-sweep denominators they would be 1.054 and 1.082, both still within the go criteria.
- The like-for-like pairing is interpreter exp:0 (libm std::exp, the E0 mode the gates use) vs hand variant 2 (fused/shared-recip/std::exp). The hand kernel's default (variant 0, exp_poly, E1) and its reference-arithmetic variant 3 are informational rows (D9). The hand kernel always computes 64 lanes at B=64; the interpreter runs 64/lane_tile chunks of the whole program. Options::fuse_reductions, fuse_pairs and inline_producers are at their defaults (on) in every row; the P4-opt-3 fusions apply at lane_tile 1 and >= 16 only, so the lane_tile 4 and 8 rows run without them by design.
- Interpreter tile sweep per D15: 128/256/512, lane tiles 4/8/16/32/64 at B=64. B=1 improves monotonically with tile (60.32 / 58.17 / 56.72 us; exp_poly 53.79 / 51.62 / 50.20). B=64 std::exp at lane_tile 32 is flat across tiles (1585.7 / 1577.5 / 1583.2, within 0.5%; lane_tile 16: 1611.1 / 1604.7 / 1603.3) with 4 and 64 worst (+28.7% and +15.3% at tile 256). The gate's b64_ratio uses the best sweep point (tile 256, lane_tile 32); at the B=1-best tile 512 the best B=64 point is lane_tile 32 at 1583.2 us (ratio 1.079), so the choice of tile does not change the B=64 outcome.
- Gates were run once each under the reference preset at c4fc0d7 before measuring: ir_roundtrip_test (P3, 5 tests), exec_m1_interp_e0_test (P4 E0, 5 tests), hand_m1_hand_test (P5 E1, 9 tests) all pass; supplementary hand_m1_hand_e0_test (4) and exec_interp_test (6) pass; the full ctest suite is 23/23 under both the reference and the release preset. Logs in tmp/ (gitignored).
- The plans measured (m1_p6_plan.txt): describe() at the two gate configurations, printed by a throwaway tool linked against the release libepykos (not in the repository), then the default-options plan from EPYKOS_DESCRIBE=1 (tile 256, lane_tile 8, which does not show the P4-opt-3 fusions). At tile 512 / lane_tile 1: 10 domains, 42,314 rows of which 34,305 are fused or inlined (d3/d5 coupons into the 129 leg-sum blocks of d6, d1 affine inlined into the 6 exp tiles of d2 with the exp tail), d7 swap PVs materialised (2 tiles) and copied out with d8 and the book. At tile 256 / lane_tile 32: 35,274 rows fused or inlined (d7 also folded into the book-sum block, its 969 output rows emitted from there), 176 leg-sum blocks, 27 tile passes and 48 kernel calls outside reductions per lane chunk (attempt 3: 53), 32 outputs copied from the value buffer and 969 emitted.

Raw data: `m1_p6_hand.json`, `m1_p6_interp.json` (20 Google Benchmark repetitions each), `m1_p6_gates.json`, `m1_p6_fingerprint.json`, `m1_p6_notes.json`; everything above in `m1.json`. Regenerate with `bench/results/m1_summarise.py bench/results/d448afd70180`.
