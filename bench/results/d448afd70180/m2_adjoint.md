# M2/Q3 adjoint benchmark — fingerprint `d448afd70180` (informational, load-tainted)

Intel Xeon W-3223, 8 physical / 16 logical cores, Apple clang 21.0.0, flags `-O3 -march=x86-64-v3 -fno-math-errno`,
release preset, engine commit `77a9ad5` (branch m2/q3-adjoint). `adjoint_m1_adjoint_bench`, Google Benchmark
`--benchmark_repetitions=20 --benchmark_min_time=0.2s`, min / median / p90 over the 20 repetition means of real time
(us); raw output in `m2_adjoint.json`, fingerprint with load in `m2_adjoint_fingerprint.json`.

**1-minute load 13.71 before and 10.22 after measuring, above the cores/2 = 8 discard threshold of
WORKLOADS.md §M1** (other M2 packages were building and testing in parallel): these rows are informational
(D9) and must be re-measured on a quiet machine before Q5 takes them as a baseline. The M2 gates for Q3 are the
tests, not these numbers.

One `Adjoint::run` = the 1,001 outputs plus d(book pv)/dz (`out_bar = e_book`) per lane; the state is written fresh
each repetition, tables in the constructor.

| row | config | min | median | p90 |
|---|---|---:|---:|---:|
| value + adjoint, B=1 | tile 256, lane_tile 1 | 416.0 | **422.5** | 425.5 |
| value + adjoint, B=1 | tile 128 / 512 | 418.6 / 419.8 | 426.9 / 426.0 | 436.9 / 433.8 |
| value + adjoint, B=64 | tile 128, lane_tile 8 (252.7 us/state) | 15985.5 | **16172.9** | 16349.3 |
| value + adjoint, B=64 | tile 128, lane_tile 4 / 16 / 32 / 64 | 15936 / 20129 / 24409 / 18630 | 16492 / 27159 / 32009 / 18968 | 17059 / 29616 / 32915 / 19385 |
| value + adjoint, B=64 | tile 256, lane_tile 4 / 8 / 16 / 32 / 64 | 16486 / 17264 / 18506 / 22492 / 18990 | 16884 / 18035 / 19253 / 23050 / 19506 | 17070 / 18406 / 19540 / 23368 / 19674 |
| value + adjoint, B=64 | tile 512, lane_tile 4 / 8 / 16 / 32 / 64 | 16772 / 16765 / 17944 / 23689 / 22588 | 16910 / 17069 / 18873 / 25680 / 24191 | 17116 / 17317 / 19584 / 26661 / 24718 |
| AdjointBuild (constructor) | | 4134 | 4702 | 5772 |

Against the M1 forward-only rows of `m1.json` (interpreter, std::exp, quiet machine: 56.60 us at B=1, 1584.0 us
at B=64): value + adjoint is 7.5x the value at B=1 and 10.2x at B=64 (per state 252.7 vs 24.8 us), on a loaded
machine. Where the time goes, by construction (D31): the adjoint materialises every row value (42,314 rows; the
interpreter fuses the 31,806 coupon rows into the leg sums and never stores them), recomputes the intermediate
steps of every group in the reverse, and pulls each value's adjoint through 56,937 gather and 37,866 segment
reader entries; the lane-tile 16/32 rows at B=64 are the edge buffers (3.9 MB at 8 lanes, scaling with the
lanes) leaving L2. None of the M1 interpreter's fusions is applied to the reverse pass yet; that is M3's
catalogue / rewrite work (the reverse of a fused group) and Q5's baseline, not a Q3 gate.
