| d | shape | rows | kind | planner treatment | reason not fused (plain tiles only) | readers (gather / segment) | outputs | time us | share |
|---|---|---:|---|---|---|---|---:|---:|---:|
| d0 | `input` | 148 | plain | 1 tile(s) | 78 of its rows are outputs; gathered by 5 domains {d1 d7 d12 d57 d66}: the inliner needs exactly one consumer | d1 d7 d12 d57 d66 / d4 | 78 | 30.9 | 0.1% |
| d1 | `neg(@0)` | 70 | plain | 1 tile(s) | inlinable into d3, but row fusion is off at lane tile 8 (row_fusion_pays: L = 1 or L >= 16) | d3 / - | 0 | 5.8 | 0.0% |
| d2 | `const($0)` | 511 | plain | 2 tile(s) | read by scan domain(s) {d10} (a scan is never fused or inlined into) | d10 / d9 d24 d27 d35 d38 d39 d47 | 0 | 16.6 | 0.0% |
| d3 | `exp(mul(@0,$0))` | 177 | plain | 1 tile(s) | gathered by 7 domains {d6 d8 d26 d32 d37 d42 d45}: the inliner needs exactly one consumer | d6 d8 d26 d32 d37 d42 d45 / - | 0 | 71.6 | 0.2% |
| d4 | `affine(#0;%0)` | 16917 | reduction | 33834 members, all gathered | - | d5 / - | 0 | 759.7 | 1.7% |
| d5 | `exp(mul(neg(@0),$0))` | 16917 | plain | 67 tile(s) | gathered by 9 domains {d6 d8 d16 d26 d32 d37 d42 d45 d55}: the inliner needs exactly one consumer | d6 d8 d16 d26 d32 d37 d42 d45 d55 / - | 0 | 5711.4 | 12.4% |
| d6 | `div(sub(div(@0,@1),#0),$0)` | 17083 | plain | 67 tile(s) | gathered by 3 domains {d7 d8 d12}: the inliner needs exactly one consumer | d7 d8 d12 / d24 | 0 | 1124.1 | 2.4% |
| d7 | `sub(@0,@1)` | 4 | plain | 1 tile(s) | 4 of its rows are outputs | - / - | 4 | 3.5 | 0.0% |
| d8 | `mul(@0,$0)` | 34685 | plain | 136 tile(s) | 226 of its rows are outputs; read by scan domain(s) {d13} (a scan is never fused or inlined into); gathered by 8 domains {d16 d29 d30 d33 d43 d53 d58 d66}: the inliner needs exactly one consumer | d13 d16 d29 d30 d33 d43 d53 d58 d66 / d9 d17 d27 d35 d38 d39 d47 d48 d49 d56 | 226 | 1418.6 | 3.1% |
| d9 | `sum(%0)` | 17076 | reduction | 91930 members, all gathered | - | d10 d14 d15 d18 d19 d21 d29 d32 d43 d50 d53 d57 d58 d60 d66 / d17 | 1562 | 1617.9 | 3.5% |
| d10 | `mul(^,@0)` | 255687 | scan | scan: 1102 chain(s), 262 wave(s) in 1126 tile(s) (indirect rows, sequential along a chain, parallel across chains and lanes) | - | d11 d19 / - | 0 | 31561.5 | 68.6% |
| d11 | `div(sub(@0,#0),$0)` | 1138 | fused | fused into {d35} (evaluated per reduction block, not materialised; 8 rows read elsewhere kept in 1 tile(s)) | - | d12 / d35 | 0 | 6.4 | 0.0% |
| d12 | `sub(@1,sub(#1,div(@0,#0)))` | 16 | plain | 1 tile(s) | 16 of its rows are outputs | - / - | 16 | 10.1 | 0.0% |
| d13 | `sum(^,@0)` | 8 | scan | scan: 2 chain(s), 4 wave(s) in 4 tile(s) (indirect rows, sequential along a chain, parallel across chains and lanes) | - | d66 / d17 | 0 | 6.8 | 0.0% |
| d14 | `mul(@0,#0)` | 1 | plain | 1 tile(s) | read by scan domain(s) {d15} (a scan is never fused or inlined into) | d15 / - | 0 | 2.5 | 0.0% |
| d15 | `mul(^,@0)` | 256 | scan | scan: 1 chain(s), 256 wave(s) in 256 tile(s) (indirect rows, sequential along a chain, parallel across chains and lanes) | - | d20 / - | 0 | 49.7 | 0.1% |
| d16 | `mul(@0,@1)` | 120 | fused | fused into {d17 d27 d35 d38 d39 d47 d48 d49 d56} (evaluated per reduction block, not materialised) | - | - / d17 d27 d35 d38 d39 d47 d48 d49 d56 | 0 | 0.8 | 0.0% |
| d17 | `sum(%0)` | 7 | reduction | 12 of 30 members evaluated in the block from {d16 (operands in member order, epilogue)}, 18 gathered | - | d34 d57 d66 / d27 | 0 | 11.1 | 0.0% |
| d18 | `div(@0,$0)` | 213 | fused | fused into {d27} (evaluated per reduction block, not materialised) | - | - / d27 | 0 | 1.0 | 0.0% |
| d19 | `mul(@0,@1)` | 102 | plain | 1 tile(s) | gathered by 2 domains {d20 d21}: the inliner needs exactly one consumer | d20 d21 / - | 0 | 10.8 | 0.0% |
| d20 | `div(sub(@0,#0),$0)` | 10 | fused | fused into {d38} (evaluated per reduction block, not materialised) | - | - / d38 | 0 | 0.4 | 0.0% |
| d21 | `mul(@0,@1)` | 94 | plain | 1 tile(s) | gathered by 2 domains {d22 d32}: the inliner needs exactly one consumer | d22 d32 / - | 0 | 5.7 | 0.0% |
| d22 | `div(sub(@0,#0),$0)` | 88 | fused | fused into {d39} (evaluated per reduction block, not materialised) | - | - / d39 | 0 | 0.3 | 0.0% |
| d23 | `div(sub(#0,#1),#2)` | 1 | fused | fused into {d24} (evaluated per reduction block, not materialised) | - | - / d24 | 0 | 1.7 | 0.0% |
| d24 | `sum(%0)` | 6639 | reduction | 1 of 13278 members evaluated in the block from {d23 (operands in member order, epilogue)}, 13277 gathered | - | d25 / - | 0 | 286.1 | 0.6% |
| d25 | `mul(@0,$0)` | 15358 | plain | 60 tile(s) | its only reader d26 is itself fused into reductions (evaluated per reduction block: no tiles of its own to inline into) | d26 / - | 0 | 531.5 | 1.2% |
| d26 | `mul(@0,@1)` | 15358 | fused | fused into {d27 d38 d39 d47 d48 d49 d56} (evaluated per reduction block, not materialised; 30 rows read elsewhere kept in 1 tile(s)) | - | d28 d29 d30 / d27 d38 d39 d47 d48 d49 d56 | 22 | 12.9 | 0.0% |
| d27 | `sum(%0)` | 1025 | reduction | 15494 of 15723 members evaluated in the block from {d26 (operands in member order, epilogue) d18 (operands in member order, epilogue) d16 (operands in member order, epilogue)}, 229 gathered | - | d28 d29 d31 d57 d66 / d35 | 806 | 574.1 | 1.2% |
| d28 | `sum(^,@0,@1)` | 4 | scan | scan: 1 chain(s), 4 wave(s) in 4 tile(s) (indirect rows, sequential along a chain, parallel across chains and lanes) | - | d57 d66 / d38 | 0 | 5.3 | 0.0% |
| d29 | `mul($0,sub(@0,@1))` | 605 | plain | 3 tile(s) | 605 of its rows are outputs; gathered by d36 and also a member of reduction(s) {d56} | d36 / d56 | 605 | 46.0 | 0.1% |
| d30 | `mul($0,sub(@0,@1))` | 11 | plain | 1 tile(s) | 11 of its rows are outputs; gathered by d31 and also a member of reduction(s) {d56} | d31 / d56 | 11 | 3.6 | 0.0% |
| d31 | `mul(@0,$0)` | 1406 | plain | 6 tile(s) | 11 of its rows are outputs; gathered by d32 and also a member of reduction(s) {d59 d61} | d32 / d59 d61 | 11 | 49.3 | 0.1% |
| d32 | `mul(@0,@1)` | 1401 | fused | fused into {d39 d47} (evaluated per reduction block, not materialised; 26 rows read elsewhere kept in 1 tile(s)) | - | d33 d46 / d39 d47 | 20 | 8.2 | 0.0% |
| d33 | `mul($0,sub(@0,@1))` | 20 | plain | 1 tile(s) | 40 of its rows are outputs | - / d59 d61 | 40 | 3.5 | 0.0% |
| d34 | `div(@0,#0)` | 2 | fused | fused into {d35} (evaluated per reduction block, not materialised) | - | - / d35 | 0 | 1.1 | 0.0% |
| d35 | `sum(%0)` | 1137 | reduction | 1136 of 2287 members evaluated in the block from {d11 (operands in member order, epilogue) d34 (operands in member order, epilogue) d16 (operands in member order, epilogue)}, 1151 gathered | - | d36 d57 d66 / d38 | 0 | 215.1 | 0.5% |
| d36 | `mul(@0,$0)` | 6989 | plain | 28 tile(s) | 605 of its rows are outputs; gathered by d37 and also a member of reduction(s) {d59 d61} | d37 / d59 d61 | 605 | 235.6 | 0.5% |
| d37 | `mul(@0,@1)` | 6384 | fused | fused into {d47 d48 d49 d56 d59 d61 d62 d63 d64 d65} (evaluated per reduction block, not materialised; 149 rows read elsewhere kept in 1 tile(s)) | - | d40 d43 d66 / d47 d48 d49 d56 d59 d61 d62 d63 d64 d65 | 136 | 31.6 | 0.1% |
| d38 | `sum(%0)` | 16 | reduction | 22 of 59 members evaluated in the block from {d20 (operands in member order, epilogue) d26 (operands in member order, epilogue) d16 (operands in member order, epilogue)}, 37 gathered | - | d41 d57 d66 / d39 | 0 | 12.7 | 0.0% |
| d39 | `sum(%0)` | 271 | reduction | 1461 of 1575 members evaluated in the block from {d32 (operands in member order, epilogue) d22 (operands in member order, epilogue) d26 (operands in member order, epilogue) d16 (operands in member order, epilogue)}, 114 gathered | - | d43 d44 d57 d66 / d47 | 177 | 105.3 | 0.2% |
| d40 | `sum(^,@0)` | 4 | scan | scan: 1 chain(s), 4 wave(s) in 4 tile(s) (indirect rows, sequential along a chain, parallel across chains and lanes) | - | d66 / d48 | 0 | 2.6 | 0.0% |
| d41 | `mul(@0,$0)` | 13 | plain | 1 tile(s) | its only reader d42 is itself fused into reductions (evaluated per reduction block: no tiles of its own to inline into) | d42 / - | 0 | 1.8 | 0.0% |
| d42 | `mul(@0,@1)` | 13 | fused | fused into {d48 d49 d56 d62} (evaluated per reduction block, not materialised) | - | - / d48 d49 d56 d62 | 0 | 0.7 | 0.0% |
| d43 | `mul($0,sub(@0,@1))` | 313 | plain | 2 tile(s) | 589 of its rows are outputs; gathered by d52 and also a member of reduction(s) {d56 d59 d61} | d52 / d56 d59 d61 | 589 | 25.4 | 0.1% |
| d44 | `mul(@0,$0)` | 1203 | plain | 5 tile(s) | its only reader d45 is itself fused into reductions (evaluated per reduction block: no tiles of its own to inline into) | d45 / - | 0 | 47.0 | 0.1% |
| d45 | `mul(@0,@1)` | 1203 | fused | fused into {d49 d56} (evaluated per reduction block, not materialised; 22 rows read elsewhere kept in 1 tile(s)) | - | d53 / d49 d56 | 22 | 6.5 | 0.0% |
| d46 | `div(sub(@0,#0),$0)` | 6 | fused | fused into {d47} (evaluated per reduction block, not materialised) | - | - / d47 | 0 | 0.5 | 0.0% |
| d47 | `sum(%0)` | 769 | reduction | 4998 of 5045 members evaluated in the block from {d37 (operands in member order, epilogue) d26 (operands in member order, epilogue) d46 (operands in member order, epilogue) d32 (operands in member order, epilogue) d16 (operands in member order, epilogue)}, 47 gathered | - | d50 d52 d57 d66 / d48 | 755 | 267.1 | 0.6% |
| d48 | `sum(%0)` | 10 | reduction | 86 of 117 members evaluated in the block from {d37 (operands in member order, epilogue) d42 (operands in member order, epilogue) d26 (operands in member order, epilogue) d16 (operands in member order, epilogue)}, 31 gathered | - | d53 d57 d66 / d49 | 4 | 17.6 | 0.0% |
| d49 | `sum(%0)` | 269 | reduction | 2314 of 2365 members evaluated in the block from {d37 (operands in member order, epilogue) d45 (operands in member order, epilogue) d42 (operands in member order, epilogue) d26 (operands in member order, epilogue) d16 (operands in member order, epilogue)}, 51 gathered | - | d57 d58 d66 / d56 | 263 | 134.2 | 0.3% |
| d50 | `mul($0,sub(@0,@1))` | 755 | plain | 3 tile(s) | 1249 of its rows are outputs; gathered by d51 and also a member of reduction(s) {d56 d59 d61} | d51 / d56 d59 d61 | 1249 | 56.0 | 0.1% |
| d51 | `mul(@0,#0)` | 261 | plain | 2 tile(s) | 261 of its rows are outputs | - / d59 d61 | 261 | 10.9 | 0.0% |
| d52 | `mul(@0,$0)` | 44 | plain | 1 tile(s) | 37 of its rows are outputs; gathered by d55 and also a member of reduction(s) {d59 d61} | d55 / d59 d61 | 37 | 3.1 | 0.0% |
| d53 | `mul($0,sub(@0,@1))` | 26 | plain | 1 tile(s) | 50 of its rows are outputs; gathered by d54 and also a member of reduction(s) {d56 d59 d61} | d54 / d56 d59 d61 | 50 | 4.0 | 0.0% |
| d54 | `mul(@0,#0)` | 2 | plain | 1 tile(s) | 2 of its rows are outputs | - / d59 d61 | 2 | 1.3 | 0.0% |
| d55 | `mul(@0,@1)` | 7 | fused | fused into {d56} (evaluated per reduction block, not materialised; 2 rows read elsewhere kept in 1 tile(s)) | - | d58 / d56 | 2 | 2.5 | 0.0% |
| d56 | `sum(%0)` | 11 | reduction | 143 of 1069 members evaluated in the block from {d37 (operands in member order, epilogue) d45 (operands in member order, epilogue) d55 (operands in member order, epilogue) d42 (operands in member order, epilogue) d26 (operands in member order, epilogue) d16 (operands in member order, epilogue)}, 926 gathered | - | d57 d60 d66 / d59 | 6 | 59.8 | 0.1% |
| d57 | `sub(div(sub(@0,@1),@2),@3)` | 9 | plain | 1 tile(s) | 9 of its rows are outputs | - / - | 9 | 5.7 | 0.0% |
| d58 | `mul($0,sub(@0,@1))` | 265 | plain | 2 tile(s) | 530 of its rows are outputs | - / d59 d61 | 530 | 19.5 | 0.0% |
| d59 | `sum(%0)` | 37 | reduction | 6 of 1756 members evaluated in the block from {d37 (operands in member order, epilogue)}, 1750 gathered | - | d66 / d61 | 35 | 50.6 | 0.1% |
| d60 | `mul($0,sub(@0,@1))` | 5 | plain | 1 tile(s) | 10 of its rows are outputs | - / d61 | 10 | 2.0 | 0.0% |
| d61 | `sum(%0)` | 9 | reduction | 7 of 3345 members evaluated in the block from {d37 (operands in member order, epilogue)}, 3338 gathered | - | d66 / d62 | 7 | 75.6 | 0.2% |
| d62 | `sum(%0)` | 2 | reduction | 13 of 15 members evaluated in the block from {d37 (operands in member order, epilogue) d42 (operands in member order, epilogue)}, 2 gathered | - | d66 / d63 | 0 | 5.7 | 0.0% |
| d63 | `sum(%0)` | 1 | reduction | 5 of 6 members evaluated in the block from {d37 (operands in member order, epilogue)}, 1 gathered | - | d66 / d64 | 0 | 3.1 | 0.0% |
| d64 | `sum(%0)` | 1 | reduction | 5 of 6 members evaluated in the block from {d37 (operands in member order, epilogue)}, 1 gathered | - | d66 / d65 | 0 | 2.8 | 0.0% |
| d65 | `sum(%0)` | 1 | reduction | 5 of 6 members evaluated in the block from {d37 (operands in member order, epilogue)}, 1 gathered | - | d66 / - | 0 | 2.6 | 0.0% |
| d66 | `sub(div(@0,@1),@2)` | 41 | plain | 1 tile(s) | 41 of its rows are outputs | - / - | 41 | 7.4 | 0.0% |

Summary by treatment (domains, rows, time share; lane tile 8, row fusion off):
  reduction  18 domains     44198 rows    9.1% of the run
  fused      14 domains     25944 rows    0.2% of the run
  scan        5 domains    255959 rows   68.8% of the run
  plain      30 domains     97134 rows   20.6% of the run
  output copy: 634.0 us (1.4%)
  total per run: 46001.1 us over 116 runs (per-run divisor 116)
