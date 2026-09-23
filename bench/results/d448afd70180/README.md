# M1 gate benchmark (P6) — fingerprint `d448afd70180`

Intel(R) Xeon(R) W-3223 CPU @ 3.50GHz, 8 physical / 16 logical cores, Apple clang version 21.0.0 (clang-2100.1.1.101), flags `-O3 -march=x86-64-v3 -fno-math-errno`. 1-minute load before measuring: 1.87, after: 2.86.
Google Benchmark, `--benchmark_repetitions=20 --benchmark_min_time=0.2s --benchmark_report_aggregates_only=false`; tables prebuilt, state written fresh each iteration; stats over the 20 repetitions (real time, us). Interpreter and hand kernel compiled with the release preset (D13). Correctness gates run under the reference preset.

## Gate (like for like: interpreter std::exp vs hand fused/shared-recip/std::exp)

| row | config | min | median | p90 | ratio | go |
|---|---|---:|---:|---:|---:|---|
| interpreter B=1 | tile 512, lane_tile 1, std::exp | 68.4 | 69.1 | 69.5 | **1.291** | <= 1.3: yes |
| hand eval B=1 | variant 2 | 53.4 | 53.5 | 53.7 | 1 | |
| interpreter B=64 | tile 128, lane_tile 8, std::exp | 2755.6 | 2780.8 | 2794.3 | **1.905** | <= 1.1: NO |
| hand eval_batch B=64 | variant 2 | 1453.9 | 1459.7 | 1463.0 | 1 | |

B=64 at the B=1-best tile (512): best lane_tile 8, median 2862.2 us, ratio 1.961.

## Correctness gates (reference preset, -ffp-contract=off)

| gate | test executable | result |
|---|---|---|
| P3 round-trip identity | ir_roundtrip_test | pass |
| P4 E0 (interpreter vs replay and oracle, B=1 and B=64) | exec_m1_interp_e0_test | pass |
| P5 E1 (hand kernel vs double maths, 1e-12) | hand_m1_hand_test | pass |

`ir_roundtrip_test`: exit 0; PASSED

`exec_m1_interp_e0_test`: exit 0; PASSED

`hand_m1_hand_test`: exit 0; PASSED

`hand_m1_hand_e0_test`: exit 0; PASSED

`exec_interp_test`: exit 0; PASSED

## Hand kernel (all variants)

| benchmark | variant | min | median | p90 | us/state |
|---|---|---:|---:|---:|---:|
| BM_HandEval/0 | 0 fused/shared-recip/exp_poly | 45.3 | 45.5 | 45.8 | 45.53 |
| BM_HandEval/1 | 1 fused/per-row-div/exp_poly | 46.0 | 46.3 | 46.4 | 46.28 |
| BM_HandEval/2 | 2 fused/shared-recip/std::exp | 53.4 | 53.5 | 53.7 | 53.51 |
| BM_HandEval/3 | 3 reference-arith/std::exp | 65.3 | 65.5 | 65.7 | 65.54 |
| BM_HandEvalBatch/0 | 0 fused/shared-recip/exp_poly | 699.7 | 704.6 | 708.2 | 11.01 |
| BM_HandEvalBatch/1 | 1 fused/per-row-div/exp_poly | 903.4 | 907.2 | 910.3 | 14.18 |
| BM_HandEvalBatch/2 | 2 fused/shared-recip/std::exp | 1453.9 | 1459.7 | 1463.0 | 22.81 |
| BM_HandEvalBatch/3 | 3 reference-arith/std::exp | 2222.6 | 2227.9 | 2235.5 | 34.81 |
| BM_HandBuild | table build | 2343.2 | 2388.6 | 2423.6 | |

## Interpreter tile sweep (D15), medians in us

B=1 (lane_tile 1):

| tile | std::exp min | median | p90 | exp_poly min | median | p90 |
|---:|---:|---:|---:|---:|---:|---:|
| 128 | 71.7 | 72.3 | 72.9 | 60.1 | 60.6 | 60.9 |
| 256 | 69.3 | 70.0 | 70.3 | 58.4 | 58.9 | 59.2 |
| 512 | 68.4 | 69.1 | 69.5 | 56.7 | 57.7 | 58.5 |

B=64, std::exp (median us per call of 64 states; us/state = median/64):

| tile \ lane_tile | 4 | 8 | 16 | 32 | 64 |
|---:|---:|---:|---:|---:|---:|
| 128 | 2829.4 | 2780.8 | 2960.0 | 3225.5 | 3783.3 |
| 256 | 2824.0 | 2839.1 | 2978.7 | 3231.2 | 3810.9 |
| 512 | 2886.4 | 2862.2 | 2984.3 | 3237.6 | 3855.4 |

B=64, exp_poly (median us):

| tile \ lane_tile | 4 | 8 | 16 | 32 | 64 |
|---:|---:|---:|---:|---:|---:|
| 128 | 2079.5 | 2033.2 | 2205.7 | 2422.9 | 2982.6 |
| 256 | 2122.9 | 2106.6 | 2220.2 | 2417.3 | 2999.7 |
| 512 | 2151.3 | 2132.1 | 2219.8 | 2439.7 | 3049.6 |

## Informational ratios (D9)

- exp_poly both sides: B=1 1.268 (interp tile 512 57.7 us vs hand variant 0 45.5 us); B=64 2.886 (tile 128 lane_tile 8 2033.2 us vs 704.6 us).
- vs the hand reference-arithmetic variant 3 (the interpreter's own operation order, no fma, division, std::exp): B=1 1.054, B=64 1.248.
- BM_InterpBuild: median 866.781 us (min 743.072, p90 912.234).
- BM_InterpPipeline: median 136.491 ms (min 133.066, p90 138.103).

## Profile

`sample(1)` (macOS, 6 s at 1 ms, main thread; release binary, no debug info) of the winning configurations, share of samples by top-of-stack frame.

B=1, tile 512, lane_tile 1, std::exp (4662 samples): `k_seg_whole<Sum,1>` (the whole-domain segment sums: coupons -> legs, swaps -> book) 29.8%; `k_binary<Mul,Col,Gat,1>` (the materialised coupon rows `N*tau*K x DF(e)` and `N*tau x DF(e)`, gathered from the DF domain) 21.5%; libm `exp` 18.9%; `k_binary<Mul,Vec,Gat,1>` (the float-coupon second multiply by the gathered forward) 10.4%; `k_seg_whole<Affine,1>` (knots -> times) 6.1%; `Interpreter::run` dispatch 2.5%; `div(gat,gat)`, `div(vec,col)` (forwards) 2.3% + 2.1%; exp wrapper 2.0%; `neg`, `sub(gat,gat)`, `mul(vec,col)` 1-1.3% each.

B=64, tile 128, lane_tile 8, std::exp (4672 samples): libm `exp` 30.0% (scalar libm, one call per (time, lane): 2564 x 64 per run, it does not vectorise across lanes); `k_seg_whole<Sum,8>` 22.9%; `k_binary<Mul,Col,Gat,8>` 17.8%; `k_binary<Mul,Vec,Gat,8>` 9.1%; `div(gat,gat)` 3.3%; exp wrapper 3.2%; `k_seg_whole<Affine,8>` 3.0%; `div(vec,col)` 2.9%; `Interpreter::run` 2.8%; `sub(gat,gat)` 1.6%; `neg` 1.2%.

What dominates: bytes moved, not arithmetic. Every coupon row (16,103 fixed + 15,703 float) is materialised into the value buffer by the elementwise kernels and then gathered back by the leg Sum (31,744 members), so the Sum plus the two coupon multiply kernels are 60% of B=1 and 50% of B=64; the hand kernel fuses gather -> fma -> segment sum in one pass and never stores the coupon rows. The second component is libm exp (19% / 30%); with exp_poly (E1) the interpreter drops to 57.7 us / 2033 us, but the hand kernel's exp_poly variant drops further (45.5 / 704.6), so exp_poly does not close the gap. The tile sweep is flat (B=1: 72.3 / 70.0 / 69.1 us; B=64 within 3% across tiles) and lane_tile 4-8 is best at B=64 (wider lane tiles overflow L1 with the 42,314-row value buffer: 42,314 x 8 x 8 B = 2.6 MB at lane_tile 8). The remedy is R5 group fusion (M3), not tile or layout tuning; consistent with P4's analysis.

## Caveats

- Same fingerprint as P4/P5 (d448afd70180): Intel Xeon W-3223, Apple clang 21, -O3 -march=x86-64-v3 -fno-math-errno; the machine was otherwise idle (load 1.9-3.1 / 16 logical cores throughout; the cores/2 = 8 discard threshold was never approached).
- Google Benchmark real time over 20 repetitions of >= 0.2 s each; p90 is the nearest-rank 18th of 20 sorted repetitions. Both benchmarks write the state fresh each iteration (verified by reading bench/exec/m1_interp_bench.cpp and bench/hand/m1_hand_bench.cpp: z = z0 + 1e-9 * k drift, DoNotOptimize + ClobberMemory) and prebuild tables outside the timed loop; nothing is cached across iterations.
- Hand kernel medians were re-measured after the interpreter sweep (tmp/m1_p6_hand_after.json, not committed): eval variant 2 53.43 us vs 53.51 us before, eval_batch variant 2 1456.7 us vs 1459.7 us; < 0.3% drift, so the sweep was taken in a stable thermal state.
- The like-for-like pairing is interpreter exp:0 (libm std::exp, the E0 mode the gates use) vs hand variant 2 (fused/shared-recip/std::exp). The hand kernel's default (variant 0, exp_poly, E1) and its reference-arithmetic variant 3 are reported as informational rows (D9). B=64 for the hand kernel always computes 64 lanes; the interpreter runs 64/lane_tile chunks of the whole program.
- Interpreter tile sweep per D15: 128/256/512; best B=1 median is tile 512 (69.07 us), best B=64 median is tile 128 with lane_tile 8 (2780.8 us); at tile 512 the best B=64 is lane_tile 8 at 2862.2 us (ratio 1.961). The gate's b64_ratio uses the best sweep point.
- Gates were run once each under the reference preset (every TU -ffp-contract=off): ir_roundtrip_test (P3), exec_m1_interp_e0_test (P4 E0), hand_m1_hand_test (P5 E1) all pass; supplementary hand_m1_hand_e0_test and exec_interp_test also pass. Full logs in tmp/ (gitignored).

Raw data: `m1_p6_hand.json`, `m1_p6_interp.json` (20 repetitions each), `m1_p6_gates.json`, `m1_p6_fingerprint.json`; everything above in `m1.json`. Regenerate with `bench/results/m1_summarise.py bench/results/d448afd70180`.
