# EpykosEngine — Workloads

The fixed fixtures behind every milestone gate. Everything is generated from a seed; there are **no data files**.
Each fixture has a `dump` that writes CSV for debugging; the CSV is never an input.

**Common conventions**
- Seed: `20260922`. Sub-streams are derived by counter (D17), so each object's draws are independent of generation order.
- Calendar: synthetic, no holidays, day 0 = valuation date. Discount times `t = d/365`.
- Accrual: ACT/360, `τ = (d_end − d_start)/360`. An annual schedule starting on day `d0` has period ends
  `d_j = d0 + round(365.25·j)`, `j = 1..T`.
- All tolerances are relative unless stated. "E0" and "E1" are the exactness classes of D8.

---

## M1 — kill-test book

### Curve
12 knots at times (years): `7/365, 1/12, 0.25, 0.5, 1, 2, 3, 5, 7, 10, 20, 30`.
State = the 12 zero rates `z_k`. `z(t)` is linear in `t` between knots and flat beyond both ends. `DF(t) = exp(−z(t)·t)`, `DF(0) = 1`.
Record-point state:
`z = [0.0400, 0.0405, 0.0410, 0.0415, 0.0420, 0.0410, 0.0400, 0.0395, 0.0400, 0.0410, 0.0430, 0.0440]`.

### Swaps (1,000)
For `i = 0..999`, drawn from sub-stream `i`:
- tenor `T_i ∈ {1,…,30}` years, uniform integer;
- notional `N_i` log-uniform in `[1e6, 1e8]`;
- side `s_i ∈ {+1 (receive fixed), −1 (pay fixed)}`, equiprobable;
- **seasoned** if `i < 200` (the realised-fixing bucket): start day `d0 = −U{30,…,300}`; otherwise `d0 = 0`;
- fixed leg: annual, ACT/360, rate `K_i = par_i·(1 + ε_i)`, `ε_i ~ U(−0.05, 0.05)`, where `par_i` is the par rate of
  this swap on the record-point curve (including the seasoned first-fixing rule);
- float leg: annual compounded OIS, ACT/360, the same schedule as the fixed leg;
- seasoned swaps carry a realised compounded rate for the first float period, `R_i = z(1)·(1 + U(−0.1, 0.1))`, as a data column.

### Coupon formulas (the templated maths, written this way deliberately)
- fixed: `pv = N·τ_j·K·DF(e_j)`
- float: `fwd_j = (DF(s_j)/DF(e_j) − 1)/τ_j`, `pv = N·τ_j·fwd_j·DF(e_j)` — telescoping is the fusion pass's job, not the author's
- seasoned first float coupon (`s_1 < 0`): `fwd_1` is replaced by `R_i`; `τ_1` still accrues from `s_1` to `e_1`
- swap: `pv = s_i·(Σ fixed − Σ float)`; book: `pv = Σ_i pv_i`

### Expected domain structure
`Knots —linmap→ Times (unique {d_j/365}) —exp→ DF —gather→ Coupons {fixed + seasoned-first, float} —segment_sum→ Legs —segment_sum→ Swaps —segment_sum→ Book`.
The seasoned-first coupons must not fall into the float-coupon class; the signature pass is not told about them.
By construction they share the constant-rate class with the fixed coupons (`N·τ·R·DF(e)` and `N·τ·K·DF(e)` are the
same op tree with one constant slot; hash-consing modulo constants cannot separate them, D22). A separate bucket
for them is an R2 (uniform-column) decision in M3, not an M1 requirement.

### Batch
64 states: `z^(b) = z + δ^(b)`, `δ^(b)_k ~ N(0, 0.0010)` i.i.d. from sub-stream `100000 + b`; `b = 0` is the record point (`δ = 0`).

### Outputs
1,000 swap PVs and the book PV per state.

### Measurement
Single-state (`B = 1`) and batched (`B = 64`) value evaluation, tables prebuilt, state inputs written fresh each
repetition. Warm; ≥ 200 repetitions; report min, median and p90. Record the fingerprint (D13) and the 1-minute load
average; discard runs with load above `cores/2`. Interpreter vs hand-fused reference under the same fingerprint only (D9).
Tile sweep: 128 / 256 / 512 (D15).

Terms (defined 2026-09-23 by the M1/P7 review, after the M1 rounds were measured):
- A **repetition** is one timed evaluation. Google Benchmark's `--benchmark_repetitions=n` with `min_time` gives
  `n` *repetition means* (each the mean over the evaluations of ≥ `min_time`), not `n` repetitions: statistics over
  them are statistics of means, and a p90 of 20 means is much tighter than the p90 of the evaluations. A result
  states which it reports and how many. Every M1/P6 round (attempts 1–4) used 20 Google Benchmark repetitions of
  ≥ 0.2 s (P4/P5: 25), i.e. min/median/p90 over 20 means of ~5,000 (`B = 1`) or ~180 (`B = 64`) evaluations, a

  deviation from the ≥ 200 above that the results now state; the gate ratios use medians, where the difference is
  immaterial. A re-measurement at ≥ 200 timed evaluations per row is owed by the next benchmark round.
- **cores** in `cores/2` means logical CPUs (hardware threads), the unit the load average is measured against
  (`sysctl hw.logicalcpu` / `nproc`; `scripts/fingerprint.sh` records both counts). On the Mac Pro (8 cores /
  16 threads) the threshold is 8. A result also reports the physical reading (4 there) when a run's load falls
  between the two, and which rows it would discard.


---

## M2 — verification fixtures
- **State ball:** `z^(r) = z + ρ·u`, `u` uniform on `[−1, 1]^12`, `ρ = 0.0050`; 256 draws from sub-stream `200000 + r`.
- **Differential:** compiled vs templated-`double` at every draw. E0 under `-ffp-contract=off`; E1 with contraction.
- **Adjoint:** `d(book pv)/dz` vs central finite difference (`h = 1e-6`, tolerance 1e-6) and vs forward mode (1e-12).
  Per-swap adjoints on 50 swaps drawn from sub-stream `300000`.
- **Forward mode** is the same templated maths instantiated on a dual-number `Scalar`.
- **Mutation set:** for each pass that exists (fold-sum, CSE, affine collapse, expander, adjoint) at least one mutant
  (wrong constant fold, dropped gather, off-by-one segment offset, wrong transpose) that must fail a gate.
  Rewrite mutants (R1–R7, an fma-contraction rule) land in M4 with the rewrites that need them (D35 re-plan). As
  registered (`include/epykos/mutation/mutation.hpp`, pinned
  by `tests/mutation/registry_test.cpp`, run by `scripts/mutation_test.sh`; D32), in harness order:
  `cse.merge_nonequal` (a Const operand's bit pattern is ignored by the CSE key), `fold_sum.wrong_order` (Sum operands
  reversed), `affine.wrong_coefficient` (the first coefficient of the first Affine emitted is one ulp off),
  `affine.drop_offset` (c_0 dropped), `affine.single_term_unscaled` (a one-term Affine — a scaled atom that a
  non-chain op or an output reads as a value, M3/G1 — drops its coefficient: no such product on the M1 book, caught by
  the curve gates `curve_*_e0_test`), `expander.drop_gather` (gather 0 reads the identity index),
  `expander.segment_off_by_one` (every segment loses its last member), `signature.merge_classes` (the const-slot
  pattern is not part of the signature), `interpreter.tile_boundary` (the last row of every elementwise tile is
  skipped); then the adjoint mutants (M2/Q4b, D33), caught by the adjoint gates — `tests/adjoint/m1_adjoint_test.cpp`
  (vs central FD, linearity) and `m1_adjoint_vs_dual_test.cpp` (vs forward mode) on the M1 book, and
  `tests/adjoint/nearmiss_adjoint_test.cpp` (vs `Dual<6>` and FD on the near-miss shapes below) — never by a test
  written for them:

  | mutant | defect (one line in `src/adjoint/`) | exercised by |
  |---|---|---|
  | `adjoint.wrong_transpose` | gather 0's pull reads the slot of row `index[r]` (the forward index array) instead of the row `r` that read it | M1 book (the DF domain's gather of the times: no row coincides); the near-miss fixture after the passes (5 of gather 0's 10 rows coincide) — on its raw recording gather 0 is the identity index and the mutant is a no-op |
  | `adjoint.drop_broadcast` | the last member of every Sum row gets no reader entry: the broadcast of the row's adjoint skips it | M1 book (leg / book Sums), near-miss sums |
  | `adjoint.affine_not_transposed` | an Affine reader's coefficient is read from the forward table at the reader's transposed position (`W`'s entries in `Wᵀ`'s order) | M1 book (the interpolation Affine), near-miss affine chains |
  | `adjoint.select_wrong_arm` | the select rule routes the adjoint to the other arm | **not exercisable on the M1 book** (no `select`); the near-miss select / max / min / abs shapes |
  | `adjoint.recip_rule_sign` | recip's rule accumulates `+(ȳ·y)·y` instead of `−(ȳ·y)·y` | **not exercisable on the M1 book** (no `recip`); the near-miss `recip_of` shape |

  The implicit node's mutants (M3/G4, D40; one line each in `src/solver/residual.cpp`), caught by the IFT gates
  `tests/solver/m1_implicit_adjoint_test.cpp` (IFT vs bump-and-recalibrate on the M1 book, 1e-6),
  `tests/solver/curve_set_adjoint_test.cpp` (through two chained curve blocks) and
  `tests/solver/m1_implicit_vs_dual_test.cpp` (vs forward mode through the calibration, 1e-12):

  | mutant | defect | exercised by |
  |---|---|---|
  | `implicit.ift_not_transposed` | the IFT multiplier solves `F_z λ = z̄` instead of `F_zᵀ λ = z̄` | the calibrated M1 book (F_z is not symmetric) |
  | `implicit.ift_drop_fp` | the parameter pull `p̄ −= F_pᵀ λ` is skipped: the quotes receive no adjoint | the calibrated M1 book (every quote adjoint arrives through it) |
  | `implicit.stale_jacobian` | the IFT uses the last iterate's Jacobian, not the solution's | the forward-mode gate at 1e-12 (which sees it whatever the start); from the fixture's flat 3 % start the last iterate is far enough from the solution for the 1e-6 bump gates to see it too (measured: caught by all three, and by the lanes E0 gate through a NaN diagnostic under the chord policy) |

  then the scan mutants (M3/G3, D41), caught by the scan fixtures' gates below — `tests/ir/scan_roundtrip_test.cpp`,
  `tests/exec/scan_interp_e0_test.cpp`, `tests/adjoint/scan_adjoint_vs_dual_test.cpp` — **not exercisable on the M1
  book** (no scan):

  | mutant | defect (one line) | exercised by |
  |---|---|---|
  | `expander.scan_carry_from_init` | `expand`: every step of a chain reads the chain's initial value instead of the previous step (the scan unrolled without its recurrence) | the round-trip identity on both scan fixtures |
  | `interpreter.scan_drop_last_wave` | `Interpreter::run`: the last wave of every scan (the last step of its longest chains) is not evaluated | the E0 interpreter gate on both scan fixtures |
  | `adjoint.scan_forward_order` | `Adjoint::run`: the reverse scan visits the rows forwards, so the carried adjoint arrives after it was pulled | the adjoint vs forward mode / FD gate on both scan fixtures |

  then the M4/R-a rewrite mutants (R1 fold uniform columns, R2 bucket rows, R3 elide trivial maps;
  one line each in `src/rewrite/r1_fold_uniform_columns.cpp` / `r2_bucket_rows.cpp` /
  `r3_elide_trivial_maps.cpp`), caught by each rule's own `tests/rewrite/r*_e0_test.cpp` — a
  differential check via `rewrite::verify_rule` / `compare_programs` against a synthetic program
  built by hand to exercise the exact defect (none of R1-R3 currently fires on the M1 book or the
  Stage A tape as recorded; `docs/RESUME.md`'s R-a landing entry says why):

  | mutant | defect (one line) | exercised by |
  |---|---|---|
  | `r1.ignores_last_row` | the uniformity check stops one row early, so a column differing only in its last row is wrongly folded to a literal | a synthetic domain whose column matches every row but the last |
  | `r1.wrong_slot` | the fold always overwrites operand `a`, even when the uniform column was read from `b` / `c` / `konst` | a synthetic `Mul(gather, uniform column)` step (the column in `b`) |
  | `r2.wrong_run_boundary` | a run boundary compares row `r` to `r-2` instead of `r-1`, mis-sizing the buckets | a synthetic 5-row domain whose signature column is `{1,1,2,2,2}` (asserted bucket sizes `{2,3}`) |
  | `r2.column_slice_uses_wrong_bucket` | bucket `k` (`k>0`) is built from bucket `k-1`'s row range instead of its own | the same synthetic domain, checked value-for-value against the un-split program |
  | `r3.off_by_one_member` | a domain's row substitutes the NEXT row's sole member instead of its own | a synthetic length-1-Sum domain of more than one row |
  | `r3.treats_length_two_as_trivial` | a two-member segment row is wrongly accepted as trivial, dropping the second additive term | a synthetic two-member Sum domain |

  R6 / R7's mutants (M4/R-c, landed once the Stage A tape existed — the "R1-R7 land in M3" line
  above was written before D35's milestone re-plan moved them to M4), caught by each rule's own
  `rewrite::verify_rule`-based differential gate on the M1 book and the Stage A tape
  (`tests/rewrite/r6_materialise_boundaries_e0_test.cpp`, `tests/rewrite/r7_block_linmap_e0_test.cpp`):

  | mutant | defect (one line) | exercised by |
  |---|---|---|
  | `r6.ignore_reduction_boundary` | `R6MaterialiseBoundaries`: a producer feeding a Sum/Affine segment (or an output) is inlined into its gather-consumer anyway | the M1 book and Stage A tape (every DF-like domain feeds a segment_sum or an Affine somewhere) |
  | `r6.ignore_fanout_boundary` | `R6MaterialiseBoundaries`: only the first reader gather's consumer domain is checked, so a producer read by several distinct consumers is still inlined into just one | the M1 book and Stage A tape |
  | `r7.no_offset_rebase` | `R7BlockLinmap::split_linmap_domain`: a block's sliced segment keeps the original whole-domain absolute offsets instead of rebasing them to its own `members`/`coefs` arrays | the Stage A tape's multi-curve linmap domain (>= 2 blocks; not exercisable on the M1 book, which has one curve and so one block) |
  | `r7.wrong_block_value_base` | `R7BlockLinmap::split_linmap_domain`: the running `value_base` accumulator advances by a block's segment length instead of its row count | the Stage A tape's multi-curve linmap domain |

  then the M4/R-b rewrite-rule mutants (`src/rewrite/`), each caught by that rule's own `rewrite::verify_rule` gate
  (a `*_e0_test.cpp` or `*_verify_test.cpp` under `tests/rewrite/`, matched by `scripts/mutation_test.sh`'s gate
  regex) on the M1 book and the Stage A tape:

  | mutant | defect (one line) | exercised by |
  |---|---|---|
  | `r4a.wrong_literal` | `push_unary_through_gathers`: the relocated step multiplies the shared value by `0.0` instead of `1.0` | `tests/rewrite/r4a_push_unary_e0_test.cpp`'s `verify_rule` gate on the M1 book and Stage A |
  | `r4a.wrong_row_map` | `push_unary_through_gathers`: the new gather reads `S_op` row `d_row` directly instead of `row_of(S, old_gather.index[d_row])` | same gate (wrong whenever the relocated gather is not already the identity) |
  | `r4b.wrong_op` | `shared_reciprocal`: combines `a` and the reciprocal with `Add` instead of `Mul` | `tests/rewrite/r4b_shared_reciprocal_verify_test.cpp`'s `verify_rule` gate (E1, 4 ulps) |
  | `r4b.wrong_row_map` | `shared_reciprocal`: the new gather reads `S_recip` row `d_row` directly instead of `row_of(S, old_gather.index[d_row])` | same gate |
  | `r5.wrong_step_index` | `group_formation`: the consumer's replacement slot points at the producer's first step instead of its last | `tests/rewrite/r5_group_formation_e0_test.cpp`'s `verify_rule` gate |
  | `r5.drop_last_step` | `group_formation`: the merged group drops the producer's last step when splicing the two step lists together | same gate |
  | `fma.wrong_operand` | `fma_contraction`: builds `fma(a, b, b)` instead of `fma(a, b, c)`, dropping the Add's real other operand | `tests/rewrite/fma_contraction_verify_test.cpp`'s `verify_rule` gate (E1, 4 ulps) |
  | `fma.drop_remap` | `fma_contraction`: kept steps after the fused one keep their pre-removal Step-slot indices | same gate |
  | `eg.ad_mode_ignores_cost` | `rewrite::decide_ad_mode` (M4/EG): always chooses `AdMode::Reverse` regardless of the three priced modes | `tests/rewrite/ad_mode_rule_e0_test.cpp`'s `ADModeRule.PicksTheCheapestModePerBlock` (a narrow, many-input block where forward must win) |
  | `eg.ad_mode_affine_without_check` | `rewrite::decide_ad_mode` (M4/EG): marks every block `ClosedFormAffine`-eligible without checking `is_linmap_domain` | same file's `BlockJacobian.ClosedFormAffineRefusesANonAffineBlock` (the consumer's own independent check then throws where the unmutated rule would never have picked that mode) |
  | `eg.block_jacobian_wrong_coefficient_index` | `adjoint::block_jacobian` (M4/EG, `ClosedFormAffine`): reads a row's coefficient one member off | same file's `BlockJacobian.ClosedFormAffineMatchesReverseExactlyOnALinmapBlock` |
- **Near-miss shapes** (`include/epykos/fixtures/nearmiss_shapes.hpp`, the gate fixture the mutation harness showed
  was missing, D32): 42 templated shapes over six positive inputs, each an op tree that differs from a neighbour in
  exactly one respect a signature may overlook — a constant on the left or the right of `−` and `/`, a constant in
  one slot of a two-op tree or the other or neither, `select` with a constant in the condition, the true arm, the
  false arm or both arms, sums of two, three and four members with and without a constant member, affine chains
  with a leading, a trailing or no constant and with negated and subtracted terms, `exp` / `log` / `sqrt` / `neg`
  around the same trees, `fma` with a constant in each slot, `recip` vs `1/x`, the comparisons the other way round,
  `max` / `min` / `abs`. Five instances per shape (inputs rotated per instance; constants `c ~ U(0.5, 2.5)`,
  `d ~ U(−1.5, 1.5)` from sub-stream `400000 + shape`, draws `2i`, `2i + 1`), then a left fold of every row into a
  total: 211 outputs. Record point `(0.7, 1.3, 2.1, 0.4, 1.9, 1.1)`; state ball of 64 draws, each input scaled by
  `U(0.5, 1.5)` from sub-stream `410000 + r` (draw 0 is the record point). Gates: round-trip identity raw and after
  the passes (36 raw classes), E0 replay after every pass vs the `double` instantiation, expanded tape and IR
  evaluator E0, interpreter E0 at `B = 1` and `B = 64` over tiles {1, 7, 256} × lane tiles {1, 3, 8, 64}; the
  adjoint (M2/Q4b) vs the `Dual<6>` Jacobian at 1e-12 and vs central FD (`h = 1e-6`, 1e-6, where the difference does
  not straddle a kink) on the raw recording and after the passes at the record point and 15 draws, and linearity in
  the seed with the seeded adjoint checked against `Jᵀ·seed` from the Dual Jacobian.

- **Scan fixtures** (M3/G3, D41; `include/epykos/fixtures/rfr_book.hpp`, `rfr_price.hpp`, `affine_scan.hpp`;
  synthetic, from the seed, no data files):
  - the **RFR compounding book**: ten quarterly OIS swaps on the M1 12-knot curve at the M1 record point, tenor
    U{1..5} years, notional log-uniform [1e6, 1e8], side ±1, ε ~ U(−0.05, 0.05), K = par·(1 + ε) (sub-stream
    500000 + i, draws 0..4); swaps 0..2 seasoned with start offset U{10..60} days and a realised fixing per day before
    the valuation date, R_d = z(1/365)·(1 + U(−0.1, 0.1)) (sub-stream 510000 + i, draw d − d0). The synthetic M1
    calendar (day 0 = valuation, no holidays, ACT/360, t = d/365), quarterly period ends d0 + round(365.25·j/4). The
    float coupon compounds daily as the natural product loop over its accrual days (τ_d = 1/360, the daily forward
    from the discount factors, or the fixing): `pv = N·τ·((Π(1 + r_d·τ_d) − 1)/τ)·DF(e)`; the fixed coupon
    `N·τ·K·DF(e)`; swap `side·(fixed − float)`; book the sum. Outputs: the 10 swap PVs then the book PV; states: the
    64 M1 batch states. After the passes: 2 scan domains, 48 projected chains of 73–92 steps (CSE shares the
    unseasoned swaps' identical coupon periods), 3 fixing chains of 10–18 steps; raw: 156 chains;
  - the **affine scan**: `x_{k+1} = a_k·x_k + b_k` over 100 steps on 4 paths, recorded time-outer (interleaved
    chains): `a_k = exp(−κ·Δt_k)`, `drift_k = θ·(1 − a_k)`, `b_kp = drift_k + σ·sqrt(Δt_k)·ε_kp`; inputs x_0 of each
    path (0.02 + 0.005p), κ = 0.5, θ = 0.04, σ = 0.01; Δt_k ~ U(0.5, 1.5)/52 (sub-stream 520000), ε ~ N(0, 1)
    (sub-stream 520001); outputs every x_kp (time-major) or only the finals, then the mean of the finals; a ball of
    states scaling every input by U(0.8, 1.2) (sub-stream 520100 + r). One scan domain of 4 chains of 100 steps,
    the step `sum(mul(@0,^),@1,@2)` after the passes.
  Gates: round-trip identity raw and after the passes with the scan counts above and the M1 book asserted scan-free
  and unchanged; the interpreter bitwise the double instantiation and the replay at 65 states at B = 1, B = 64 lane
  for lane the single runs, tiles {1, 7, 256, 4096} × lane tiles {1, 3, 8, 32, 64}, odd B; the adjoint's Jacobian vs
  Dual at 1e-12 (relative to the row's scale, the leg scale for a swap PV) and vs central FD at 1e-6, linearity;
  the batched adjoint lane for lane the single runs, tiles, no allocation.
- **Instrument sample** (M3/G2, D43; `include/epykos/fixtures/instrument_sample.hpp`, `src/fixtures/instrument_sample.cpp`;
  synthetic, from the seed, no data files; the conventions and blueprints are the repository's data): five trades of
  every Stage A blueprint (`blueprints/instruments/stage_a.json`: SOFR OIS plain / observation shift 2 / shift 2 +
  lockout 2, SOFR averaging swap, SOFR ON deposit, SR3, SR1, €STR OIS, €STR ON deposit, EURIBOR 3M / 6M deposits,
  EURIBOR 3M / 6M IRS, 3s6s basis, FEU3) at valuation 2026-09-23 on four linear-zero curves (USD-SOFR, EUR-ESTR,
  EUR-EURIBOR-3M, EUR-EURIBOR-6M; knots 0.25, 0.5, 1, 2, 3, 5, 7, 10, 15, 20, 30 years; record point base 3.8 % /
  1.9 % / 2.1 % / 2.4 % rising 50 bp over the grid): per blueprint b (sub-stream 600000 + b, draws in order per trade)
  tenor from {1Y, 2Y, 3Y, 5Y, 7Y} (deposits their own, futures contract i), notional log-uniform [1e6, 1e8], side ±1,
  a start offset U{30..300} days back for trades 0 and 1 (seasoned, with realised fixings from `synthetic_fixings`
  2025-01-01 to the day before the valuation date, seeds 20260922 + slot), ε ~ U(−0.05, 0.05) on the par rate /
  spread, a futures traded price par ± U(−0.1, 0.1). Outputs: the 75 trade PVs then the book PV; states: a ball of
  ±50 bp on every knot (sub-stream 601000 + r). Measured: 77 compounded coupons with ≥ 3 projected days (17,439
  days), 4,352 averaged days, 225 term coupons, 32 current coupons, 8 realised term fixings; after the passes 44,516
  nodes, 33 domains, 29,369 values, ONE scan domain `mul(^,@0)@scan` of 52 chains of 47–259 steps (CSE shares the
  spot-starting trades' periods); raw 3 scan domains, 110 chains. Gates: round-trip identity raw and after the
  passes; the interpreter bitwise the double maths and the replay at 17 states at B = 1 and lane for lane at B = 17
  over tiles {1, 256, 4096} × lane tiles {1, 8, 17}; the adjoint's Jacobian vs `Dual<44>` at 1e-12 of the row scale
  (measured 2.2e-15) and vs central FD at 1e-6 (3.7e-8).
- **Stage A desk problem** (M3/G5, D44; `include/epykos/fixtures/stage_a.hpp`, `src/fixtures/stage_a_e0.cpp`;
  the definitions in `blueprints/problems/stage_a.json`, every number from the seed 20260923, synthetic, no data
  files): valuation 2026-09-23; the four curves of `blueprints/curves/` in slots USD-SOFR, EUR-ESTR, EUR-EURIBOR-3M,
  EUR-EURIBOR-6M (slot 0 optionally on its log-DF / monotone cubic / composite variant); generating zero curves
  z(t) = long + (short − long)(1 − e^{−t/3})/(t/3) at 3.80 → 4.30 %, 1.95 → 2.65 %, 2.05 → 2.75 %, 2.13 → 2.83 %,
  quotes = their par quotes ± U(1 bp) (a futures price ± 0.01; sub-stream 810000 + k), fixings from 2024-01-01
  (`synthetic_fixings`, levels 3.85 / 1.95 / 2.05 / 2.13 %, 1 bp daily vol); 2,000 trades (sub-stream 800000 + i,
  draws in order: blueprint by the mix weights, tenor from {1Y..30Y} by weight, notional log-uniform [1e6, 2e8], side,
  seasoned with probability 0.2 with an age U{30..min(1500, tenor − 30)} days, netting set U{0..39}, moneyness
  U(± 0.15) on the par rate / spread at the generating curves); 1,000 scenario lanes (sub-stream 820000 + l): parallel
  (lane 0 unshocked), twist about 5Y, butterfly (wings up, belly 5Y down), per-curve parallel, sizes U(± 100 / 100 /
  50 / 100 bp), a rate quote shifted by s(t), a futures price by −100 s(t). Measured: 378 seasoned trades, 7,537
  compounded / 1,397 averaged / 15,297 term coupons, 752 current coupons, 2,208,068 projected observation days;
  after the passes 517,036 nodes, 67 domains, 423,235 values, 5 scan domains of 1,107 chains over 255,959 rows.
  Gates (`tests/stage_a/`): records, every block converges (`‖Jᵀr‖∞` 6.8e-14 / 2.9e-14 / 6.2e-16 / 7.7e-15), round-trip
  identity after the passes (raw on 120 trades), the sharing gate (two DF domains read by both groups, 0 duplicates),
  the replay bitwise the double book at the record point, the program bitwise the double maths at each lane's solved
  knots, 8 scenario lanes bitwise the single runs and across tile configurations, the per-trade IFT ladder vs
  `Dual<70>` 5.8e-15 (gate 1e-12) and vs Richardson bump-and-recalibrate 5.1e-8 (gate 1e-6), the three USD variants.

---

## M3 — catalogue workloads
Reference workloads for the catalogue generator: the M1 book at `B = 1` and `B = 64`, forward and adjoint.
The generator emits C++ under `src/catalogue/generated/`, which is **committed**; CI checks that regeneration is a
no-op. The coverage report gives the fraction of groups, and of evaluation time, served by the catalogue vs the
interpreter. Parity gate: catalogued groups within 1.05× of the hand-fused reference.

---

## M4 — calibration
- **Instruments:** 12 OIS par swaps whose tenors are the knot tenors (`1W 1M 3M 6M 1Y 2Y 3Y 5Y 7Y 10Y 20Y 30Y`);
  sub-annual tenors are single-period swaps, the rest annual. Quotes are par rates on the M1 record-point curve, so
  the Linear scheme must recover `z` to solver tolerance.
- **Schemes**, all interpolating the zero rate on the same 12 knots: Flat (step), Linear, Hermite (Bessel/Catmull–Rom
  tangents), NaturalCubic, MonotoneCubic (Fritsch–Carlson tangents with the Hyman filter), BSpline (cubic, clamped).
  Linear-in-knots schemes must collapse to `linmap`; MonotoneCubic must appear through `select` with mask, margin
  and arm gap exported.
- **Interpolation variable**, per scheme: `zero` (interpolate `z(t)`, `DF = exp(−z·t)`), `logdf` (interpolate
  `y(t) = −z(t)·t`, `DF = exp(y)`; linear on `logdf` is piecewise-constant forwards), and for Flat and Linear also
  `forward` (interpolate the instantaneous forward, `DF = exp(−∫f)` in closed form). All six schemes on `zero` and
  `logdf`; the variable is structure, never a `select`.
- **Composite (region) curve:** an ordered list of regions `[t_a, t_b)` each with its own `(scheme, variable)`,
  knots partitioned by region, value-continuous in `DF` at region boundaries; region lookup is structure (class A,
  folded at record time). The M4 composite fixture on the 12 knots: `[0, 1Y)` Linear on `zero`, `[1Y, 10Y)`
  MonotoneCubic on `zero`, `[10Y, 30Y]` Linear on `logdf`. Required: round-trip identity; the linear regions produce
  affine rows and no `select`; `select` buckets appear only for rows in the monotone region; calibration to the 12
  quotes, optimality and IFT risk as for the single-scheme curves.
- **Optimality:** `‖Jᵀr‖∞ < 1e-12` at the solution, every scheme and the composite.
- **IFT risk:** `dz/dq` vs bump-and-recalibrate (1 bp central bumps) at 1e-6, every scheme and the composite.
- **Kink 2-cycle fixture:** the composite curve above (its MonotoneCubic region) and a quote path of ≥ 50 quote vectors, constructed so that
  frozen-Newton **without** `pin` oscillates between two Hyman states at at least one knot (this failure must be
  demonstrated by a test), and **with** `pin` + hysteresis converges from every quote vector within 5 iterations and
  no full refresh.

---

## M5 — exposure grid
- **Model:** Hull–White one-factor, `a = 0.03`, `σ = 0.0100`, `x_0 = 0`, fitted to the M4 Linear curve at the record
  point (`A(t,T)` from the curve). LGM with `ζ(t), H(t)` chosen equivalent to `(a, σ)`; must reproduce Hull–White
  prices to 1e-12 (D19).
- **Grid:** 10,000 paths from sub-streams `1000000 + path`; 100 dates, quarterly, `t_k = k/4`, `k = 1..100`;
  the M1 book (1,000 swaps). Structure at each date = the coupons remaining after `t_k`; how that structure is
  represented (per-date tables vs a mask column) is the implementer's call and must be measured.
- **Exposure:** per (path, date, swap) value; `EE(t_k) = mean_paths max(V, 0)` per swap and for the book (`max` is a
  `select`, both arms computed); positive expected exposure profile as the output.
- **Reference:** per-path scalar evaluation of the same templated maths on `double` at every date. Per-path values E0
  under `-ffp-contract=off`; `EE` to 1e-12 (fixed pairwise reduction order, D17).
- **Performance gate:** simulation + valuation of the full grid under 1 s with a pool of 8 threads on this machine,
  tables prebuilt, fingerprinted; parity with the reference is a correctness gate, not a timing one.

---

## MX — G4 multi-currency bundle (stretch)
- **Currencies:** USD, EUR, GBP, JPY. Collateral/discounting: each currency's OIS; USD SOFR as the cross-currency base.
- **Curves per currency** (researched, sources in `docs/G4_BUNDLE.md`): the OIS curve from deposits/OIS swaps (and
  futures/FRAs on the short end where that is market practice); tenor-basis curves only where the market still quotes
  them (EURIBOR 3M and 6M vs €STR; JPY as researched); cross-currency basis for EURUSD, GBPUSD, USDJPY as
  mark-to-market resetting basis swaps vs USD SOFR.
- **Conventions:** day counts, payment and fixing lags, business-day rolls, spot lags, IMM rules and holiday
  calendars (NY, TARGET, London, Tokyo) implemented from published rules, generated in code.
- **Instruments:** the calibrating set per curve at researched standard tenors; quotes: a plausible synthetic surface
  from the seed, stated as synthetic.
- **Runs:** full-bundle calibration (all curves jointly where they couple); par-delta risk ladder to every quote;
  a portfolio of 5,000 swaps across the four currencies (seeded) under 1,000 scenarios (parallel and twist shocks).
- **Comparison:** the same bundle, instruments, portfolio and scenarios on SwapEngine as a black box (D21); one
  fingerprint; each engine's own timing harness; the report states precisely what each engine computed.
