# M1 gate benchmark (P6) — fingerprint `d448afd70180`

Intel(R) Xeon(R) W-3223 CPU @ 3.50GHz, 8 physical / 16 logical cores, Apple clang version 21.0.0 (clang-2100.1.1.101), flags `-O3 -march=x86-64-v3 -fno-math-errno`. 1-minute load before measuring: 4.69, after: 2.3.
Engine commit measured: `40af34e` (integrate/m1-m5 after P4-opt-2 be63c95, fused step pairs: two consecutive chained IR steps whose middle value has no other reader run as one kernel (d2 exp argument, d4 forwards, d5 float coupons and d7/d8 swap PVs on the M1 book), the reduction row helpers are inlined and the output copy has the chunk's compile-time width; attempt 2 measured 363321f (reduction fusion only), attempt 1 measured 1805412).
Google Benchmark, `--benchmark_repetitions=20 --benchmark_min_time=0.2s --benchmark_report_aggregates_only=false`; tables prebuilt, state written fresh each iteration; stats over the 20 repetitions (real time, us). Interpreter and hand kernel compiled with the release preset (D13). Correctness gates run under the reference preset.

## Gate (like for like: interpreter std::exp vs hand fused/shared-recip/std::exp)

| row | config | min | median | p90 | ratio | go |
|---|---|---:|---:|---:|---:|---|
| interpreter B=1 | tile 512, lane_tile 1, std::exp | 58.4 | 59.0 | 59.3 | **1.101** | <= 1.3: yes |
| hand eval B=1 | variant 2 | 53.3 | 53.6 | 54.0 | 1 | |
| interpreter B=64 | tile 256, lane_tile 32, std::exp | 1630.3 | 1640.0 | 1646.7 | **1.126** | <= 1.1: NO |
| hand eval_batch B=64 | variant 2 | 1450.2 | 1456.8 | 1462.5 | 1 | |

B=64 at the B=1-best tile (512): best lane_tile 32, median 1649.6 us, ratio 1.132.

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

## Hand kernel (all variants)

| benchmark | variant | min | median | p90 | us/state |
|---|---|---:|---:|---:|---:|
| BM_HandEval/0 | 0 fused/shared-recip/exp_poly | 45.4 | 46.0 | 46.2 | 45.96 |
| BM_HandEval/1 | 1 fused/per-row-div/exp_poly | 45.9 | 46.5 | 46.7 | 46.51 |
| BM_HandEval/2 | 2 fused/shared-recip/std::exp | 53.3 | 53.6 | 54.0 | 53.61 |
| BM_HandEval/3 | 3 reference-arith/std::exp | 65.0 | 65.3 | 65.5 | 65.26 |
| BM_HandEvalBatch/0 | 0 fused/shared-recip/exp_poly | 697.9 | 704.7 | 708.6 | 11.01 |
| BM_HandEvalBatch/1 | 1 fused/per-row-div/exp_poly | 899.9 | 905.1 | 907.5 | 14.14 |
| BM_HandEvalBatch/2 | 2 fused/shared-recip/std::exp | 1450.2 | 1456.8 | 1462.5 | 22.76 |
| BM_HandEvalBatch/3 | 3 reference-arith/std::exp | 2218.9 | 2226.1 | 2230.8 | 34.78 |
| BM_HandBuild | table build | 2301.1 | 2351.9 | 2384.0 | |

## Interpreter tile sweep (D15), medians in us

B=1 (lane_tile 1):

| tile | std::exp min | median | p90 | exp_poly min | median | p90 |
|---:|---:|---:|---:|---:|---:|---:|
| 128 | 61.7 | 62.2 | 62.4 | 53.6 | 53.9 | 54.3 |
| 256 | 60.0 | 60.3 | 60.8 | 51.4 | 51.6 | 52.0 |
| 512 | 58.4 | 59.0 | 59.3 | 50.0 | 50.6 | 50.8 |

B=64, std::exp (median us per call of 64 states; us/state = median/64):

| tile \ lane_tile | 4 | 8 | 16 | 32 | 64 |
|---:|---:|---:|---:|---:|---:|
| 128 | 2084.3 | 1769.2 | 1665.2 | 1646.6 | 1912.6 |
| 256 | 2023.8 | 1726.6 | 1655.6 | 1640.0 | 1923.9 |
| 512 | 1975.4 | 1720.1 | 1662.2 | 1649.6 | 1936.1 |

B=64, exp_poly (median us):

| tile \ lane_tile | 4 | 8 | 16 | 32 | 64 |
|---:|---:|---:|---:|---:|---:|
| 128 | 1334.8 | 1032.6 | 952.8 | 947.1 | 1227.7 |
| 256 | 1275.3 | 1004.9 | 953.5 | 948.3 | 1226.2 |
| 512 | 1245.3 | 1003.8 | 952.0 | 954.0 | 1245.9 |

## Informational ratios (D9)

- exp_poly both sides: B=1 1.101 (interp tile 512 50.6 us vs hand variant 0 46.0 us); B=64 1.344 (tile 128 lane_tile 32 947.1 us vs 704.7 us).
- vs the hand reference-arithmetic variant 3 (the interpreter's own operation order, no fma, division, std::exp): B=1 0.905, B=64 0.737.
- BM_InterpBuild: median 1959.646 us (min 1903.441, p90 2001.901).
- BM_InterpPipeline: median 135.320 ms (min 131.671, p90 137.279).

## Compared with the previous measurement

Attempt 2 (commit 48cb578 results, engine 363321f, same fingerprint, same flags and repetitions) -> attempt 3 (engine 40af34e, after P4-opt-2 be63c95: fused step pairs, inlined reduction row helpers, compile-time-width output copy), medians in us, like for like (std::exp both sides):

| row | attempt 2 | attempt 3 | change |
|---|---:|---:|---:|
| interpreter B=1 (best tile) | 64.75 (tile 512) | 59.04 (tile 512) | -8.8% |
| hand eval variant 2 | 53.66 | 53.61 | -0.1% |
| b1_ratio | 1.207 | 1.101 | |
| interpreter B=64 (best point) | 1963.5 (tile 256, lane_tile 32) | 1640.0 (tile 256, lane_tile 32) | -16.5% |
| hand eval_batch variant 2 | 1464.7 | 1456.8 | -0.5% |
| b64_ratio | 1.340 | 1.126 | |
| interpreter B=1 exp_poly | 53.30 | 50.59 | -5.1% |
| interpreter B=64 exp_poly | 1158.0 (tile 256, lane_tile 8) | 947.1 (tile 128, lane_tile 32) | -18.2% |
| BM_InterpBuild | 2091.2 | 1959.6 | -6.3% |
| BM_InterpPipeline (ms) | 137.8 | 135.3 | -1.8% |

The hand kernel is unchanged and reproduces within 0.5% (all nine of its rows), so the whole change is the interpreter's fused pairs. The P4-opt-2 commit message measured the same engine in its own session at 57.75 us (B=1, tile 512) and 1639.9 us (B=64, tile 256, lane_tile 32) against hand variant 2 at 53.79 / 1456.7: this run reproduces the B=64 point to 0.01% and the B=1 point within 2.2% (59.04 here), with ratios 1.074 / 1.126 there and 1.101 / 1.126 here. The B=64 lane-tile optimum stays at 32 (16 within 1-1.5%); lane_tile 4 is now the worst point (+20-27%: with the coupon and forward pairs in registers, per-kernel dispatch and the gathered loads are the cost that 4 lanes do not amortise) and 64 is +16-18% (value buffer 10,570 materialised rows x 64 lanes x 8 B = 5.4 MB per chunk).

## Profile

`sample(1)` (macOS, 6 s at 1 ms, main thread, release binary without debug info; symbol template arguments decoded with Op {3 Sub, 4 Mul, 5 Div, 6 Neg, 7 Exp}, operand Kind {0 Vec, 1 Lit, 2 Col, 3 Gat} and pair-operand PK {0 S = literal/column scalar, 1 G = gathered row, 2 none}) of the two gate configurations, share of samples by top-of-stack frame (raw output in tmp/profile_b1.txt, tmp/profile_b64.txt, gitignored).

B=1, tile 512, lane_tile 1, std::exp (4640 samples): the fused float-coupon pair folded through the leg-sum epilogue `k_pair_acc<Mul,S,G,Mul,G>` (`N*tau x DF(e) x fwd` accumulated straight into the leg sum, one kernel per block) 29.2%; libm `exp` 20.2% (+0.4% stub); the fixed-coupon step through the epilogue `k_binary_acc<Mul,Col,Gat>` (`N*tau*K x DF(e)`) 16.8%; `seg_block_fused<false,1>` (the leg-sum block driver: member loop, accumulator stores) 7.9%; `seg_block_gathered<true,1>` (knots -> times affine) 7.4%; `seg_block_gathered<false,1>` (the book sum) 3.9%; the forwards domain d4 (`k_pair<Div,G,G,Sub,S>` = `sub(div(gat,gat),lit)` 4.5%, `k_binary<Div,Vec,Col>` = `/tau` 2.0%) 6.5%; the exp-argument pair `k_pair<Neg,G,None,Mul,S>` (`mul(neg(gat),col)`) 2.9%; `k_unary<Exp,Vec>` wrapper 2.2%; the swap-PV pair `k_pair<Sub,G,G,Mul,S>` 1.4%; `copy_out` 0.8%; `Interpreter::run` dispatch 0.3%.

B=64, tile 256, lane_tile 32, std::exp (4660 samples): libm `exp` 49.8% (+0.6% stub; scalar libm, one call per (time, lane) = 2564 x 64 = 164k calls per run, about 820 us of the 1640); the fused float-coupon pair `k_pair_acc<Mul,S,G,Mul,G,32>` 11.2%; the fixed-coupon step `k_binary_acc<Mul,Col,Gat,32>` 7.2%; the forwards domain d4 (`k_binary<Div,Vec,Col>` 5.5%, `k_pair<Div,G,G,Sub,S>` 5.2%) 10.7% (about 175 us: 2 x 2,432 true divisions per lane, 311k per run); `k_unary<Exp,Vec,32>` wrapper 5.1%; `seg_block_fused<false,32>` 3.7%; `seg_block_gathered<true,32>` (affine) 3.4%; the swap-PV pair 2.9%; the exp-argument pair 2.5%; `copy_out` 2.1%; the book sum 0.5%; `Interpreter::run` 0.2%.

What dominates now: every scratch round trip the fused pairs removed is gone from the profile (attempt 2's separate `mul(col,gat)` first step and the d4 `div`/`sub`/`neg` singles no longer appear); the coupon arithmetic inside the leg-sum blocks is 46% of samples at B=1 and 18% at B=64, one kernel per block per producer. At B=64 the single largest item is libm exp at half the time, which the hand kernel's std::exp variant pays equally (hand v2 - v0 = 752 us; interpreter exp:0 - exp:1 at the same tile/lane point = 692 us), so exp is at parity and is not part of the like-for-like gap. The like-for-like gap is now 183 us at B=64 (1640.0 vs 1456.8) and 5.4 us at B=1 (59.04 vs 53.61); it sits in E0 arithmetic the hand kernel replaces in E1 - the forwards domain's two IEEE divisions per element (about 175 us at B=64, at the divide-throughput floor) against one reciprocal per unique time plus an fma, and the fixed and float coupons at three roundings as recorded against gather -> fma -> segment sum - plus the interpreter's output copy (2.1%, about 35 us) and block-driver traffic with no hand-loop counterpart. Consistent with this, against the hand reference-arithmetic variant 3 (the interpreter's own operation order) the interpreter is faster: 0.905 at B=1 and 0.737 at B=64. Tile and lane-tile tuning cannot close the remainder (B=64 within 0.6% across the three tiles at lane_tile 32); the remaining candidates are the same as before: R4b shared reciprocal (E1), fma contraction in the fused coupon pair (E1), and a cross-lane-vectorised exp (exp_poly, E1: 947.1 us at B=64, 1.344x the hand exp_poly variant).

## Caveats

- Same fingerprint as P4/P5/attempts 1 and 2 (d448afd70180): Intel Xeon W-3223, Apple clang 21, -O3 -march=x86-64-v3 -fno-math-errno; release preset for timings, reference preset (every TU -ffp-contract=off) for the gates; machine otherwise idle (load 2.3-4.7 / 16 logical cores; the cores/2 = 8 discard threshold was never approached during measurement). Both presets were built, the correctness gates and the full ctest suites were run, and the load had decayed below 5 before any measurement started; nothing was built or tested during measurement.
- Google Benchmark real time over 20 repetitions of >= 0.2 s each; p90 is the nearest-rank 18th of 20 sorted repetitions. Both benchmarks write the state fresh each iteration (verified again by reading bench/exec/m1_interp_bench.cpp and bench/hand/m1_hand_bench.cpp at 40af34e: z = z0 + 1e-9 * k drift inside the timed loop, DoNotOptimize + ClobberMemory) and build tables in the constructor outside the timed loop; nothing is cached across iterations; no fix was needed.
- Hand kernel variant 2 re-measured after the interpreter sweep (tmp/m1_p6_hand_after.json, gitignored): eval 53.58 us (min 53.32, p90 53.79) vs 53.61 before (-0.0%), eval_batch 1453.4 (min 1447.1, p90 1460.2) vs 1456.8 before (-0.2%); the ratios use the before-sweep run; with the after-sweep denominators they would be 1.102 and 1.128.
- The like-for-like pairing is interpreter exp:0 (libm std::exp, the E0 mode the gates use) vs hand variant 2 (fused/shared-recip/std::exp). The hand kernel's default (variant 0, exp_poly, E1) and its reference-arithmetic variant 3 are informational rows (D9). The hand kernel always computes 64 lanes at B=64; the interpreter runs 64/lane_tile chunks of the whole program. Options::fuse_pairs and fuse_reductions are at their defaults (on) in every row.
- Interpreter tile sweep per D15: 128/256/512, lane tiles 4/8/16/32/64 at B=64. B=1 improves monotonically with tile (62.18 / 60.28 / 59.04 us; exp_poly 53.87 / 51.65 / 50.59). B=64 std::exp at lane_tile 32 is flat across tiles (1646.6 / 1640.0 / 1649.6, within 0.6%; lane_tile 16: 1665.2 / 1655.6 / 1662.2) with 4 and 64 worst (+20-27% and +16-18%). The gate's b64_ratio uses the best sweep point (tile 256, lane_tile 32); at the B=1-best tile 512 the best B=64 point is lane_tile 32 at 1649.6 us (ratio 1.132), so the choice of tile does not change the B=64 outcome.
- Gates were run once each under the reference preset at 40af34e: ir_roundtrip_test (P3, 5 tests), exec_m1_interp_e0_test (P4 E0, 5 tests), hand_m1_hand_test (P5 E1, 9 tests) all pass; supplementary hand_m1_hand_e0_test (4) and exec_interp_test (6) pass; the full ctest suite is 23/23 under both the reference and the release preset. Logs in tmp/ (gitignored).
- The plan measured (m1_p6_plan.txt, describe() at the default options, same structure at every tile / lane tile): 10 domains, 42,314 rows of which 31,744 (d3 fixed coupons, d5 float coupons) are fused into the 189 leg-sum blocks of d6, the float coupon as one fused pair per block; d2 (exp argument), d4 (forwards) and d7/d8 (swap PVs) are pairs; 53 kernel calls outside reductions per lane chunk (was 80); 37 + 25 coupon rows that a gather reads (the 31 single-period swaps of d8) stay materialised.

Raw data: `m1_p6_hand.json`, `m1_p6_interp.json` (20 repetitions each), `m1_p6_gates.json`, `m1_p6_fingerprint.json`; everything above in `m1.json`. Regenerate with `bench/results/m1_summarise.py bench/results/d448afd70180`.
