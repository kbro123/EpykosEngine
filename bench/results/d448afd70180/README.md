# M1 gate benchmark (P6) — fingerprint `d448afd70180`

Intel(R) Xeon(R) W-3223 CPU @ 3.50GHz, 8 physical / 16 logical cores, Apple clang version 21.0.0 (clang-2100.1.1.101), flags `-O3 -march=x86-64-v3 -fno-math-errno`. 1-minute load before measuring: 2.38, after: 3.39.
Engine commit measured: `b32182a` (integrate/m1-m5 tip after the M1/P7 review fixes (D23 recurrent flags per domain, D24 tape serial stamped into Rec/RecBool, D25 reference TUs pinned to -ffp-contract=off by the src/**/*_e0.cpp convention on both compilers), CI green on all three jobs at this commit. The run() path is the one attempt 4 measured at c4fc0d7 (P4-opt-3 e361ee5: exp tail in the pair kernel, inlined single-reader producers, outputs emitted from the reduction blocks); attempt 3 measured 40af34e, attempt 2 363321f, attempt 1 1805412).
Google Benchmark, `--benchmark_repetitions=20 --benchmark_min_time=0.2s --benchmark_report_aggregates_only=false`; tables prebuilt, state written fresh each iteration; stats over the 20 repetition means (real time, us; see Measurement statistic). Interpreter and hand kernel compiled with the release preset (D13). Correctness gates run under the reference preset.

## Read first: the outcome depends on the pairing

ROADMAP.md M1 Go and RESUME.md P6 say "interpreter within 1.3x of the hand-fused kernel single-state, within 1.1x batched" and name no hand variant or exp implementation. The table below gives the ratio under every candidate denominator (medians, us). The gate table that follows uses the pairing P4/P6 chose (row 1); which pairing is the gate is a DECISIONS entry the orchestrator owes before the M1 verdict (M1/P7 review).

| pairing | exactness | B=1 interp / hand | B=1 ratio (<= 1.3) | B=64 interp / hand | B=64 ratio (<= 1.1) |
|---|---|---:|---:|---:|---:|
| interpreter std::exp vs hand variant 2 (fused/shared-recip/std::exp): the P4/P6 like-for-like pairing, libm exp on both sides | interpreter E0 vs hand E1 | 56.60 / 54.23 | **1.044** (met) | 1584.0 / 1467.7 | **1.079** (met) |
| interpreter exp_poly vs hand variant 0 (default: fused/shared-recip/exp_poly): the same exactness class on both sides | E1 vs E1 | 49.99 / 46.42 | **1.077** (met) | 879.0 / 708.7 | **1.240** (NOT met) |
| interpreter std::exp (the gated E0 mode) vs hand variant 0 (default, as specified and delivered by P5) | E0 vs E1 | 56.60 / 46.42 | **1.219** (met) | 1584.0 / 708.7 | **2.235** (NOT met) |
| interpreter std::exp vs hand variant 3 (reference arithmetic: the interpreter's own operation order, std::exp) | E0 vs E0 | 56.60 / 65.90 | **0.859** (met) | 1584.0 / 2246.3 | **0.705** (met) |

Row 1 puts the identical scalar libm exp on both sides: at B=64 it is 705 us of the interpreter's 1584.0 (exp:0 minus exp:1 at the gate point) and 759 us of the hand kernel's 1467.7 (variant 2 minus variant 0), so the non-exp remainder ratio is 1.240, the same as the E1-vs-E1 row. Against the hand kernel as specified and delivered (variant 0, exp_poly, E1) the interpreter's gated E0 mode is 2.235x batched.

## Gate (like for like: interpreter std::exp vs hand fused/shared-recip/std::exp)

| row | config | min | median | p90 | ratio | go |
|---|---|---:|---:|---:|---:|---|
| interpreter B=1 | tile 512, lane_tile 1, std::exp | 56.3 | 56.6 | 57.1 | **1.044** | <= 1.3: yes |
| hand eval B=1 | variant 2 | 53.7 | 54.2 | 54.5 | 1 | |
| interpreter B=64 | tile 256, lane_tile 32, std::exp | 1571.5 | 1584.0 | 1591.7 | **1.079** | <= 1.1: yes |
| hand eval_batch B=64 | variant 2 | 1460.5 | 1467.7 | 1477.1 | 1 | |

B=64 at the B=1-best tile (512): best lane_tile 32, median 1586.5 us, ratio 1.081.

## Measurement statistic

min / median / p90 over n Google Benchmark repetitions; each repetition's real_time is the mean of its iterations (>= min_time of evaluations), so p90 is a p90 of repetition means, not of evaluations. docs/WORKLOADS.md M1 Measurement asks for >= 200 repetitions (timed evaluations); this round has n = 20 repetition means per row, a stated deviation; the gate uses medians. Gate rows:

| row | n (repetition means) | iterations per repetition (min..max) | timed evaluations |
|---|---:|---:|---:|
| interpreter B=1 | 20 | 4960..4960 | 99200 |
| hand eval B=1 | 20 | 5168..5168 | 103360 |
| interpreter B=64 | 20 | 176..176 | 3520 |
| hand eval_batch B=64 | 20 | 192..192 | 3840 |

## Load rule

docs/WORKLOADS.md Terms: cores = logical CPUs (hardware threads): threshold 8 (physical reading: 4). 1-minute loads during measurement: 2.38, 1.89, 3.24, 3.39; within the logical threshold: yes; within the physical threshold: yes.

## CI

GitHub Actions (ubuntu GCC 13 release + reference, macOS Apple clang release): green: all 3 jobs pass (macos-latest clang release, ubuntu-latest GCC 13 release, ubuntu-latest GCC 13 reference) at `b32182a (the engine commit measured; integrate/m1-m5 tip at the start of this attempt)` (https://github.com/kbro123/EpykosEngine/actions/runs/35829425538). Checked with gh run list at measurement time: conclusion success, created 2026-09-23T06:59:59Z; the previous run on the branch (4a4bb87, attempt 4) was red for the reason D25 records. The results in this README are the first measured at a commit with green CI.

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

`exec_interp_e0_test`: exit 0; [  PASSED  ] 7 tests.

`ctest_reference`: 100% tests passed out of 24

`ctest_release`: 100% tests passed out of 24

## Hand kernel (all variants)

| benchmark | variant | min | median | p90 | us/state |
|---|---|---:|---:|---:|---:|
| BM_HandEval/0 | 0 fused/shared-recip/exp_poly | 46.1 | 46.4 | 46.8 | 46.42 |
| BM_HandEval/1 | 1 fused/per-row-div/exp_poly | 46.5 | 47.1 | 47.4 | 47.13 |
| BM_HandEval/2 | 2 fused/shared-recip/std::exp | 53.7 | 54.2 | 54.5 | 54.23 |
| BM_HandEval/3 | 3 reference-arith/std::exp | 65.6 | 65.9 | 66.2 | 65.90 |
| BM_HandEvalBatch/0 | 0 fused/shared-recip/exp_poly | 701.5 | 708.7 | 711.9 | 11.07 |
| BM_HandEvalBatch/1 | 1 fused/per-row-div/exp_poly | 906.9 | 912.1 | 916.5 | 14.25 |
| BM_HandEvalBatch/2 | 2 fused/shared-recip/std::exp | 1460.5 | 1467.7 | 1477.1 | 22.93 |
| BM_HandEvalBatch/3 | 3 reference-arith/std::exp | 2231.7 | 2246.3 | 2252.3 | 35.10 |
| BM_HandBuild | table build | 2320.8 | 2395.1 | 2420.0 | |

## Interpreter tile sweep (D15), medians in us

B=1 (lane_tile 1):

| tile | std::exp min | median | p90 | exp_poly min | median | p90 |
|---:|---:|---:|---:|---:|---:|---:|
| 128 | 59.9 | 60.3 | 60.7 | 53.4 | 53.8 | 54.2 |
| 256 | 57.9 | 58.5 | 58.7 | 51.2 | 51.7 | 51.9 |
| 512 | 56.3 | 56.6 | 57.1 | 49.3 | 50.0 | 50.4 |

B=64, std::exp (median us per call of 64 states; us/state = median/64):

| tile \ lane_tile | 4 | 8 | 16 | 32 | 64 |
|---:|---:|---:|---:|---:|---:|
| 128 | 2100.4 | 1790.6 | 1610.6 | 1587.5 | 1809.3 |
| 256 | 2035.4 | 1743.3 | 1608.5 | 1584.0 | 1821.8 |
| 512 | 1990.6 | 1738.3 | 1604.9 | 1586.5 | 1821.7 |

B=64, exp_poly (median us):

| tile \ lane_tile | 4 | 8 | 16 | 32 | 64 |
|---:|---:|---:|---:|---:|---:|
| 128 | 1344.9 | 1036.8 | 919.2 | 883.1 | 1098.4 |
| 256 | 1288.1 | 1008.0 | 912.9 | 879.0 | 1103.5 |
| 512 | 1254.0 | 1006.1 | 916.0 | 886.5 | 1111.8 |

## Informational ratios (D9)

- exp_poly both sides: B=1 1.077 (interp tile 512 50.0 us vs hand variant 0 46.4 us); B=64 1.240 (tile 256 lane_tile 32 879.0 us vs 708.7 us).
- vs the hand reference-arithmetic variant 3 (the interpreter's own operation order, no fma, division, std::exp): B=1 0.859, B=64 0.705.
- BM_InterpBuild: median 2299.119 us (min 2147.890, p90 2396.784).
- BM_InterpPipeline: median 140.482 ms (min 135.502, p90 143.716).

## Compared with the previous measurement

Attempt 4 (commit 4a4bb87 results, engine c4fc0d7, same fingerprint, same flags and repetitions) -> attempt 5 (engine b32182a = integrate/m1-m5 after the M1/P7 review fixes: D23 recurrent flags, D24 tape serial in Rec, D25 `*_e0.cpp` build convention for the kernel TUs and the hand kernel; no change to the run() path), medians in us, like for like (std::exp both sides):

| row | attempt 4 | attempt 5 | change |
|---|---:|---:|---:|
| interpreter B=1 (best tile) | 56.72 (tile 512) | 56.60 (tile 512) | -0.2% |
| hand eval variant 2 | 54.17 | 54.23 | +0.1% |
| b1_ratio | 1.047 | 1.044 |  |
| interpreter B=64 (best point) | 1577.5 (tile 256, lane_tile 32) | 1584.0 (tile 256, lane_tile 32) | +0.4% |
| hand eval_batch variant 2 | 1467.5 | 1467.7 | +0.0% |
| b64_ratio | 1.075 | 1.079 |  |
| interpreter B=1 exp_poly | 50.20 (tile 512) | 49.99 (tile 512) | -0.4% |
| interpreter B=64 exp_poly | 881.3 (tile 256, lane_tile 32) | 879.0 (tile 256, lane_tile 32) | -0.3% |
| BM_InterpBuild | 2317.8 | 2299.1 | -0.8% |
| BM_InterpPipeline (ms) | 142.0 | 140.5 | -1.1% |

Nothing measurable changed, as expected from the diff: the interpreter gate rows moved -0.2% (B=1) and +0.4% (B=64), the hand rows -0.4% to +0.2% (eval v0/v1/v2/v3 46.42/47.13/54.23/65.90 vs 46.32/47.17/54.17/66.01; eval_batch 708.7/912.1/1467.7/2246.3 vs 709.7/915.9/1467.5/2244.7), and the 36 interpreter sweep rows moved between -0.4% and +1.2% (all within the run-to-run band of earlier sessions). The after-sweep re-run of hand variant 2 (53.77 / 1457.3) reproduces attempt 4's after-sweep re-run (53.83 / 1457.4) to 0.1% / 0.01%, and the before-sweep rows (54.23 / 1467.7) reproduce attempt 4's (54.17 / 1467.5) to 0.1% / 0.01%, so the ~0.8% before/after-sweep drift of the hand kernel is a repeatable session effect (the hand bench is the first benchmark run after the builds and tests; the cause was not investigated), not noise. The B=64 lane-tile optimum stays at 32 (16 within 1.5%: 1608.5); lane_tile 8 is +10.1% and 4 +28.5% (the P4-opt-3 fusions are off at 4 and 8 lanes by design), 64 +15.0%. B=64 is flat across tiles at lane_tile 32 (1587.5 / 1584.0 / 1586.5, within 0.2%); B=1 stays monotone in tile (60.27 / 58.50 / 56.60).

## Profile

`sample(1)` (macOS, 6 s at 1 ms, main thread, release binary without debug info; symbol template arguments decoded with Op {3 Sub, 4 Mul, 5 Div, 6 Neg, 7 Exp}, operand Kind {0 Vec, 1 Lit, 2 Col, 3 Gat} and pair-operand PK {0 S = literal/column scalar, 1 G = gathered row, 2 none}) of the two gate configurations, share of main-thread samples by top-of-stack frame (raw output in tmp/profile_b1.txt, tmp/profile_b64.txt, gitignored).

B=1, tile 512, lane_tile 1, std::exp (4691 samples): the fused float-coupon pair folded through the leg-sum epilogue `k_pair_acc<Mul,S,G,Mul,G>` (`N*tau x DF(e) x fwd` accumulated straight into the leg sum) 27.6%; libm `exp` 21.9% (+0.1% stub); the fixed-coupon step through the epilogue `k_binary_acc<Mul,Col,Gat>` 17.4%; `seg_block_fused<false,1>` (the leg-sum block driver) 7.8%; the forwards domain d4 (`k_pair<Div,G,G,Sub,S>` = `sub(div(gat,gat),lit)` 5.5%, `k_binary<Div,Vec,Col>` = `/tau` 2.8%) 8.2%; the book sum `seg_block_gathered<false,1>` 4.4%; the exp-argument pair with the exp tail `k_pair<Neg,G,None,Mul,S>` (`mul(neg(gat),col) => exp`, the frame that calls libm) 4.3%; the inlined knots -> times affine `k_seg_list<true,1>` 3.9%; the swap-PV pair `k_pair<Sub,G,G,Mul,S>` 1.9%; `copy_out` 0.9%; `Interpreter::run` + `eval_group` dispatch 0.9%; `k_seg_whole` 0.4%; the kept (not fused) coupon rows 0.3%.

B=64, tile 256, lane_tile 32, std::exp (4689 samples): libm `exp` 53.7% (+0.3% stub; scalar libm, 2564 x 64 = 164k calls per run, about 855 us of the 1584.0); the float-coupon pair `k_pair_acc<Mul,S,G,Mul,G,32>` 10.9%; the fixed-coupon step `k_binary_acc<Mul,Col,Gat,32>` 8.0%; the forwards domain d4 (`k_pair<Div,G,G,Sub,S>` 5.7%, `k_binary<Div,Vec,Col>` 5.3%) 11.0% (about 175 us: 2 x 2,432 true IEEE divisions per lane, 311k per run); `seg_block_fused<false,32>` 5.1%; the exp-argument pair with the exp tail 4.4%; the inlined affine `k_seg_list<true,32>` 2.3%; the swap-PV pair `k_pair<Sub,G,G,Mul,S,32>` (d7, evaluated inside the book-sum block) 2.2%; `memmove` called from `emit_run_outputs<32>` (the 969 swap-PV rows written to `out` from the book-sum block) 1.1% (+0.2% in `emit_run_outputs` itself); `k_load_acc` 0.3%; dispatch 0.1%; no `copy_out` frame (32 of the 1001 outputs are copied, 969 emitted).

What dominates: at B=1 the coupon arithmetic inside the leg-sum blocks (45%) and libm exp (22%); at B=64 libm exp at 54%, the coupon arithmetic at 19% and the forwards' divisions at 11%. The profile is the same as attempt 4's frame for frame (every share within 1.5 points), as expected: the engine changed only in build convention since c4fc0d7 (D25: the kernel TUs and the hand kernel are now `*_e0.cpp` pinned with the `-ffp-contract=off` flag instead of the clang pragma they already carried) and in recording-side code (D23, D24), none of it on the run() path. At B=64 exp is at parity with the hand std::exp variant (interpreter exp:0 - exp:1 at the gate point = 705 us; hand v2 - v0 = 759 us), so it is not part of the like-for-like gap. That gap is 116 us at B=64 (1584.0 vs 1467.7) and 2.4 us at B=1 (56.60 vs 54.23): the E0 arithmetic the hand kernel replaces in E1 - the forwards' two IEEE divisions per element (at the divide-throughput floor) against one reciprocal per unique time plus an fma, and the fixed and float coupons at three roundings as recorded against gather -> fma -> segment sum - plus the reduction block drivers with no hand-loop counterpart. Consistent with this, against the hand reference-arithmetic variant 3 (the interpreter's own operation order) the interpreter is faster: 0.859 at B=1 and 0.705 at B=64. The remaining candidates are unchanged: R4b shared reciprocal (E1), fma contraction in the fused coupon pair (E1), and a cross-lane-vectorised exp (exp_poly, E1: 879.0 us at B=64, 1.240x the hand exp_poly variant).

## Caveats

- Same fingerprint as P4/P5/attempts 1-4 (d448afd70180): Intel Xeon W-3223, Apple clang 21, -O3 -march=x86-64-v3 -fno-math-errno; release preset for timings, reference preset (every TU -ffp-contract=off) for the gates; machine otherwise idle (1-minute load 2.38 before the hand bench, 1.89 before the interpreter sweep, 3.24 before the hand re-run, 3.39 after; the cores/2 = 8 discard threshold was never approached; nothing was built or tested during measurement: both presets were built and all tests run first, then the machine was checked 96% idle with no XProtect scan of the new binaries still running before the first benchmark started; the only other CPU users were the desktop's own processes at < 20% of one core).
- Google Benchmark real time over 20 repetitions of >= 0.2 s each; p90 is the nearest-rank 18th of 20 sorted repetitions. Both benchmarks write the state fresh each iteration (verified by reading bench/exec/m1_interp_bench.cpp and bench/hand/m1_hand_bench.cpp at b32182a: z = z0 + 1e-9 * k drift inside the timed loop, DoNotOptimize + ClobberMemory) and build tables in the constructor outside the timed loop; nothing is cached across iterations; no fix was needed.
- Hand kernel variant 2 re-measured after the interpreter sweep (tmp/m1_p6_hand_after.json, gitignored): eval 53.77 us (min 53.50, p90 54.21) vs 54.23 before (-0.9%), eval_batch 1457.3 (min 1450.9, p90 1462.6) vs 1467.7 before (-0.7%); the ratios use the before-sweep run (the measurement protocol's order); with the after-sweep denominators they would be 1.053 and 1.087, both still within the go criteria.
- The like-for-like pairing is interpreter exp:0 (libm std::exp, the E0 mode the gates use) vs hand variant 2 (fused/shared-recip/std::exp). The hand kernel's default (variant 0, exp_poly, E1) and its reference-arithmetic variant 3 are informational rows (D9). The hand kernel always computes 64 lanes at B=64; the interpreter runs 64/lane_tile chunks of the whole program. Options::fuse_reductions, fuse_pairs and inline_producers are at their defaults (on) in every row; the P4-opt-3 fusions apply at lane_tile 1 and >= 16 only, so the lane_tile 4 and 8 rows run without them by design.
- Interpreter tile sweep per D15: 128/256/512, lane tiles 4/8/16/32/64 at B=64. B=1 improves monotonically with tile (60.27 / 58.50 / 56.60 us; exp_poly 53.79 / 51.71 / 49.99). B=64 std::exp at lane_tile 32 is flat across tiles (1587.5 / 1584.0 / 1586.5; lane_tile 16: 1610.6 / 1608.5 / 1604.9). The gate's b64_ratio uses the best sweep point (tile 256, lane_tile 32); at the B=1-best tile 512 the best B=64 point is lane_tile 32 at 1586.5 us (ratio 1.081), so the choice of tile does not change the B=64 outcome.
- Gates were run once each under the reference preset at b32182a before measuring: ir_roundtrip_test (P3, 5 tests), exec_m1_interp_e0_test (P4 E0, 5 tests), hand_m1_hand_test (P5 E1, 9 tests) all pass; supplementary hand_m1_hand_e0_test (4) and exec_interp_e0_test (7; was exec_interp_test with 6 before the P7 fixes) pass; the full ctest suite is 24/24 under both the reference and the release preset (23 before P7 added tests/maths/m1_fixture_e0_test.cpp). Logs in tmp/ (gitignored).
- The plans measured (m1_p6_plan.txt): describe() at the two gate configurations, printed by a throwaway tool linked against the release libepykos (scratchpad, not in the repository), then the default-options plan (tile 256, lane_tile 8, which does not show the P4-opt-3 fusions). Identical to attempt 4's plans: at tile 512 / lane_tile 1, 10 domains, 42,314 rows of which 34,305 are fused or inlined, 19 tile passes and 30 kernel calls outside reductions per chunk, all 1001 outputs copied; at tile 256 / lane_tile 32, 35,274 rows fused or inlined, 27 tile passes and 48 kernel calls outside reductions per lane chunk, 32 outputs copied and 969 emitted from the book-sum block.
- This attempt re-measures the same engine as attempt 4 plus the M1/P7 review fixes; it changes no code. The first (attempt 4) numbers already met both criteria, and CI was red at that commit for the GCC contraction reason D25 records; this run is the measurement at the first commit where CI is green on all three jobs.

Raw data: `m1_p6_hand.json`, `m1_p6_interp.json` (20 Google Benchmark repetitions each), `m1_p6_gates.json`, `m1_p6_fingerprint.json`, `m1_p6_notes.json`; everything above in `m1.json`. Regenerate with `bench/results/m1_summarise.py bench/results/d448afd70180`.
