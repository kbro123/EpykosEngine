# EpykosEngine — Design

Status: M0 design; **M1 (kill test) built and passed 2026-09-23** — the recorder, tape passes, signature pass, domain IR,
tiled interpreter and hand-fused reference exist, and the interpreter is within 1.044× / 1.079× (single-state / batched)
of the hand kernel (`ROADMAP.md` §M1 result, D27); **M2 (verification harness and adjoints) built and passed 2026-09-23** —
the differential tester, `Dual` forward mode, the mechanical adjoint over the domain IR, the mutation harness and the
perf-gate tooling exist (`ROADMAP.md` §M2 result, D29–D34); M3 onward in progress. Numbers quoted as *measured* without a
fingerprint come from the SwapEngine research branch `research/aad-graph-kernels` and SwapEngine's gated baselines (see
`PRIOR_ART.md`); M1's and M2's own measurements are in `bench/results/d448afd70180/` and `RESUME.md` §5; everything else is
a target.

---

## 1. Thesis

A financial calculation is a function of inputs that change at three different rates:

| axis | examples | changes | role in the engine |
|---|---|---|---|
| **structure** | schedules, curve/vol topology, netting sets, payoff logic | daily / on booking | *compile time*: recorded once, folded away |
| **state** | quotes, curve knots, model parameters | per tick | *run time*: the inputs of the compiled program |
| **batch** | scenarios, MC paths, trades sharing a template | never within a run | *lanes*: the innermost, SIMD/GPU axis |

Recording the pricing maths against a fixed structure yields a straight-line program over state. EpykosEngine turns that
program into a **domain-typed array program** (a few array ops over index spaces such as *times*, *coupons*, *legs*), fuses it
into a small number of pre-compiled kernels, and derives the adjoint mechanically. No JIT.

What that buys, generically (no per-product code):
- value, gradient (reverse), Hessian-vector (forward-over-reverse);
- batching across scenarios/paths/trades;
- composition: calibration → pricing → exposure → margin as one graph, with calibrations as **implicit nodes**;
- incremental re-evaluation, lineage, structural diffing, bitwise reproducibility.

The pricing maths is written **once**, as ordinary C++ templated on `Scalar`. It is simultaneously the reference
implementation (instantiated on `double`) and the source the compiler records.

---

## 2. Pipeline

```
Blueprint (instruments, curves, models as DATA)
   │  (M3/G0, D42: blueprints/conventions/*.json read by the strict in-house JSON reader into the
   │   conventions registry — calendars from rules, day counts, schedules, IMM, RFR windows are code;
   │   M3/G2, D43: blueprints/instruments/*.json name a convention and the coupon kind per leg,
   │   blueprints/curves/*.json a scheme, variable, regions and the calibration instrument set;
   │   maths/instrument/builder.hpp resolves a trade into the row tables the coupon maths prices from)
   │
   ▼
Templated maths  ── instantiated on double ──►  reference values (the oracle)
   │  instantiated on Rec
   ▼
Scalar tape      (ops over leaves; constants are leaves, not immediates)
   │  CSE · DCE · fold-sum (left-deep add chains → variadic SUM) · affine collapse
   ▼
Signature pass   (hash-cons modulo constants → isomorphism classes)
   │
   ▼
Domain IR        (domains = index spaces; columns = varying constants; gathers = cross-domain operands;
   │              segments = SUM children; scan domains = loop-carried recurrences)
   │  rewrites (§6)
   ▼
Fused groups     (one iteration domain each; gathers in, segment-sum epilogue out)
   │
   ├─► tier 1: catalogue kernel (AOT-generated C++, matched by group signature)
   ├─► tier 2: tiled vector interpreter (per-op dispatch amortised over a tile; intermediates in L1)
   └─► tier 3: batch mode (batch axis innermost: each element is a SIMD vector of scenarios)
   │
   ▼
Adjoint          (each group reversed op-by-op; scatters turned into pulls via precomputed transposes;
                  the same rewrites and tiers apply)
   │
   ▼
Solver layer     (implicit / pin / rank_update: calibration, streaming, active sets)
```

---

## 3. The op set

An op earns primitive status only if it has at least one of: **(a)** an adjoint cheaper than differentiating its
insides, **(b)** a dedicated vector kernel, **(c)** semantics the rest of the system needs (linearity, a mask, implicitness).
Domain operations (`discount`, `par_rate`, `annuity`, `leg_pv`) are **not** ops: they are compositions the fusion pass
recognises.

### 3.1 Pricing graph (state → model quantities)

| op | semantics | adjoint | why primitive |
|---|---|---|---|
| `linmap` | `y = W·x`, W block-sparse, structure-only | `Wᵀ` | every linear interpolation scheme, spread ancestry and turn window lowers to it; its Jacobian is W itself; batch turns GEMV into GEMM |
| `ew` (elementwise) | `add sub mul div neg exp log sqrt recip fma …` within one domain | local rules | the arithmetic; fused, never dispatched per element |
| `gather` | `y[i] = v[idx[i]]` across domains | scatter-add, executed as a pull (§7) | all instrument structure reduces to index arrays = data |
| `segment_sum` | ragged reduce fine → coarse by offsets | broadcast | sub-period → coupon → leg → row → portfolio |
| `scan` | cumulative `⊕` along a sequence (product, sum, affine step); as built (M3/G3, D41) a recurrent domain whose rows are the steps of every chain, chain-major, with a carry gather reading the previous step — not a step op | reverse scan (the same pull, rows backwards) | compounding, survival, path evolution |

**As built, Stage A scale (M3/G5, D44):** the 2,000-trade book's compounding is **5 scan domains** of 1,107 chains
over 255,959 rows — not investigated why 5 domains arise from 3 scan classes rather than one, flagged for M4. A
chain that starts inside another chain (a branch: two lockout coupons over the same start whose ends differ by a
business day) is not folded into the scan; its own steps up to the divergence are retried as ordinary elementwise
rows (§5 point 6), and without that retry the whole book has no scan domain at all (it splits into ~330 level
domains). The scan is never fused or inlined by the planner (§7) and is 67–77% of every Stage A evaluation
regardless of lane tile — the standing hot spot named for M4's cost model.
| `quot` | `a / b` with a fused quotient-rule adjoint | 2-line rule | par rate / par spread without per-kind partials |
| `quadform` | `½·xᵀQx` | `Q·x` | moment-integrated averaging (the one non-DF shape) |
| `select` | `c ? a : b`, c value-dependent; both arms computed | adjoint of the selected arm | value branches; exports mask + margin + arm gap |
| `smooth_step` | smoothed indicator of width ε | analytic | digitals/barriers; ε is a graph parameter (bias reportable) |
| `frozen(v, guard)` | value fixed at record time; guard checked each replay | zero (stop-gradient) | LSM exercise boundaries, adaptive grids |

### 3.2 Solver layer (quotes → state)

| op | semantics | why primitive |
|---|---|---|
| `implicit(F)` | forward: any solver, untaped; backward: `dx/dp = −F_x⁺ F_p` at the solution | calibrations, implied vols, yields, regressions; tape length independent of iterations |
| `pin(v, edge, mask)` | clamp to an edge while an **externally owned** mask says so | band edges and interpolation kinks as one active-set mechanism, with hysteresis; the graph stays pure |
| `rank_update` | rank-k update of a factored operator | a mask flip costs a rank-k update, not a refresh |
| `row_map` | per-row transform after accumulation, depending on the live quote (`band`, `log`, …) | depends on q, which is not state |

`select` vs `pin`: the **value** owns a `select`'s mask; the **solver** owns a `pin`'s mask. That distinction is what
removes Newton 2-cycles at kinks (see `PRIOR_ART.md`, desk_mixed).

As implemented (M3/G4, D40; `solver/implicit.hpp`, `solver/residual.hpp`, `solver/implicit_program.hpp`,
`solver/curve_set.hpp`): an implicit node is a *block* on the one tape — its unknowns are Inputs the solver fills in,
its residuals outputs it drives to zero, its O1 diagnostics (`‖Jᵀr‖∞`, iterations) solved inputs registered as outputs —
kept in an `ImplicitRegistry` of ordinals that survives every pass; `Op::Implicit` stays reserved. The residual
sub-program is the backward slice of the tape from the residual outputs (`tape/slice.hpp`), with its own interpreter
and adjoint; the untaped solve (Gauss–Newton / LM, `‖F‖∞ < 1e-14` or 50 iterations) takes values from the one and
Jacobians from the other (one adjoint lane per residual). Backward is the IFT rule `λ = F_z⁻ᵀ z̄`, `p̄ −= F_pᵀ λ` per
lane with the solution's factorised Jacobian, blocks in reverse order so a curve's unknowns pull into the curves it
read; forward mode is `implicit_dual<N>` on the templated maths. A `CurveSet` infers each instrument's curve
dependencies from a scratch recording and solves the strongly connected components in order (or all curves jointly).
Batch lanes recalibrate independently (bitwise the single-lane runs), identical lanes share one solve, and the
record-point factorisation may drive every lane's steps (`chord`) with a per-lane refresh on a stall. `ir/sharing.hpp`
asserts the cross-stage sharing: every DF domain feeds both the residuals and the book, and no DF is computed twice.

**As built, Stage A scale (M3/G5, D44):** `CurveSet` is Composite-aware (`CurveSpec::regions`; a spec without
regions keeps the exact M1 linear path unchanged, bitwise) — four curves (SOFR, ESTR, EURIBOR 3M/6M) solved in
dependency order inside one tape, 4 blocks, 70 free knots, optimality `‖Jᵀr‖∞` worst 1.235e-13 (gate 1e-12) at the
record point, at the O2 run and on 32 sampled lanes; the whole 1,000-lane grid recalibrates with 0 not converged.
The solve tolerance is 1e-13, not this section's 1e-14, on the 2,000-trade book: the compounded residuals of
30-year daily products carry a 3–5e-14 rounding floor in rate units, stated in the test. Forward mode for the O3
comparison is `Dual<70>` on the templated maths (not a tangent interpreter over the IR) — 6.2× cheaper than the
reverse IFT ladder for the per-trade shape, 40× more expensive for the book alone; the choice is reported per
Jacobian block, not automated (M4's rule).

---

## 4. Recording

- **`Rec`** is an operator-overloading scalar: `{double v; node_id id; tape_serial tape}` (D14, D24) — it carries the serial
  of the tape it was recorded on, and using it on another tape, or after a pass or `clear()`, throws `RecordError` — plus an
  Eigen `NumTraits` specialisation.
- **Constants are leaves.** `0.25*y` records `mul(const#17, y)`. Eager constant folding into op immediates is forbidden:
  it makes every coupon unique and defeats domain inference. Folding of *uniform* columns happens later, in §6.
- **No implicit conversion to `double`.** Comparisons return `RecBool`, which does not convert to `bool`, so
  `if (a < b)` on active values fails to compile. The maths must write `select(c, a, b)` or
  `structural_if(c)` (which throws at record time if `c` depends on an input).
- **Taint.** Every value carries "depends on input". `.value()` on a tainted value throws in record mode.
- **Overloads.** `max/min/abs` on `Rec` record `select`; a lint forbids `std::max` on a `Scalar`.
- **Provenance side table** (optional): `(op → template call site, instrument, row)` for debugging.
- **Scope markers** (optional hints): `EPY_DOMAIN(s, "coupon", i)` — no-ops for `double`; the signature pass verifies them.

**As built, Stage A scale (M3/G5, D44):** recording the 2,000-trade book raw is 27,459,283 nodes (→ 517,036 after
the E0 passes of §6) and takes 7–13 s / peaks ~1.5 GB depending on load — a record-time memo of the book's
`df(slot, t)` callable (one `Rec` per distinct `(slot, t)`) keeps the *raw* recording from repeating a discount
factor already looked up for an earlier coupon; the calibration instruments' own DFs are not memoised, they are
merged by `cse` regardless. The post-`cse` tape is identical either way — the memo shrinks recording time and raw
node count only, it folds none of the maths, and every compounding loop, average and forward is still recorded
per coupon exactly as `maths/instrument/coupon.hpp` writes it (no telescoping, no pre-folding by hand, D44).

---

## 5. Domain inference (the signature pass)

1. Record with constants as leaves.
2. Fold left-deep `add` chains whose inner nodes have a single use into a variadic `SUM` (left-fold order preserved →
   bit-identical).
3. Hash-cons modulo constants: `sig(n) = H(op, sig(operands), const_slot_pattern)`, commutative operands canonically
   ordered. Boundaries: fan-out > 1, `SUM`, `linmap` outputs, inputs.
4. Partition by signature: domain = class; rows = instances; constant slots = columns; operands in another domain =
   gather indices; `SUM` children = segment offsets.
5. Column classification: identical across instances → literal (folded); varying → data column.
6. **Recurrence detection:** chains `x_{k+1} = f(x_k, …)` are found without hints before the partition (M3/G3, D41):
   a node's tree with one node cut out and replaced by a carry token hashes like the cut node's own tree with its
   own cut, at the same operand path, three or more steps in a row. The steps become one *scan domain* — recurrent,
   rows chain-major, `Program::scans` naming the carry gather and the chains — whose group is the step (a Sum on
   the carry's path is a fixed-arity Sum step: fold_sum's `+` of `a·x + b`). Sequential along a chain, parallel
   across chains and batch lanes. An inner running sum is fold_sum's reduction, never a scan; a chain the builder
   cannot lay out (the carry inside an Affine, a step reading its own earlier steps through another class, fewer
   than three steps) is flagged `scan_class` and split by dependency level as before (D23), so every domain still
   reads only earlier domains. The M1 book has no chain and its program is unchanged.
   **How to write a recurrence so it records as a scan:** as the definition states it, one Scalar carried through
   the loop — `Scalar acc = 1.0; for (i) acc = acc * (1.0 + r[i] * tau[i]);` (`maths/swap/compounding.hpp`),
   `x = a * x + drift + noise` for a path — never telescoped, unrolled or pre-folded by hand (the collapse of the
   compounded product to `DF(s)/DF(e)` is the engine's business, M4), never an `if` on the Scalar (it does not
   compile). Realised fixings as plain doubles in the same loop form a chain of their own (a constant per step)
   whose last value starts the projected chain; two step shapes in one loop are two chains; every partial value
   may be an output (each step a boundary) and it is still one scan. A chain that starts inside another chain
   (a branch: two lockout coupons over the same start whose ends differ by a day share their steps up to the
   divergence) is not a chain of the scan: its steps are retried as straight-line rows reading the trunk's row, and
   the trunk and every other chain of the class stay a scan (M3/G5, D44).
7. **Round-trip check:** expand the domain IR back to a scalar tape and compare node-for-node with the recording.
   Identity, not tolerance.

Expected outcome on a rates book: `Knots —linmap→ Times —exp→ DF —gather→ Subs —segment_sum→ Coupons —segment_sum→ Legs →
Rows`, with buckets falling out as separate signatures where their op trees differ (a coupon with a realised fixing
has the fixed coupon's tree with a constant in the rate slot, so it shares that class and is separated, if wanted,
by the uniform-column bucketing of §6 R2, not by the signature; D22).


Across recordings the signature → domain map persists: a new trade of a known shape adds rows, not code.
Measured on the instrument sample (M3/G2, D43: 75 trades of every Stage A type on four curves): 33 domains, one
scan domain `mul(^,@0)@scan` holding every compounded coupon's projected days (52 chains of 47–259 steps after CSE),
the averaged coupons as Sum reductions, the term coupons and deposits as elementwise rows. On the Stage A tape (M3/G5,
D44: the four calibrations and the 2,000-trade book on one tape, 517,036 nodes after the passes): 67 domains, 423,235
values, 5 scan domains of 1,107 chains over 255,959 rows, two DF domains (knot-time and interpolated, D22) read by both
the calibration residuals and the book, no discount factor computed twice.

---

## 6. Fusion rewrites

Applied in order. Each has an **exactness class**: *E0* bit-identical, *E1* ≤ 1 ulp per op (tolerance-gated; for a value
that is a difference of terms, such as a swap PV, the tolerance is relative to the scale of the terms, D26).

| # | rule | class | recovers (in SwapEngine terms) | fires? (M1 book / Stage A, D51/D52/D50, measured) |
|---|---|---|---|---|
| R1 | fold uniform columns (`k==1`, `konst==0`, `w==1`) | E0 | `cpn_is_plain` | M1 book 0 sites / Stage A 0 sites on an unrewritten tape (the signature pass already folds this at construction time) — but **Stage A 16 sites** on the 80 domains the tape becomes after an R2 pass, which is the pairing R1 exists for (D65) |
| R2 | bucket rows by uniform-column signature | E0 | per-kind batches | **M1 book 0 sites / Stage A 3 sites** (D65). The `adjoint::` crash D52 pt.4 blocked this on was a test-harness buffer-sizing defect, not an adjoint defect; the gate is now two structural floors (consolidate something; be a per-kind partition). Without the per-kind floor: M1 book 5 sites / Stage A 23 sites, all bit-exact but fragmenting rather than batching, and enough proposals to blow the e-graph's node bound |
| R3 | elide trivial maps (length-1 segments, identity gathers ⇒ merge domains) | E0 | `sub_is_identity` | no / no (0/0) |
| R4a | push pure unary ops through gathers toward the smaller domain, then CSE (`exp`) | E0 | `exp` once per time | no / no (0/0 — a record-time df memo already removes the redundancy on both fixtures) |
| R4b | same for `recip` (`a/b → a·recip(b)`) | E1 | shared `INV` | no / no (0/0 — heterogeneous per-curve gathers avoid the shared-site shape) |
| R5 | group formation: maximal elementwise region per domain; gathers in; segment-sum epilogue | E0 | fused coupon→leg loop | no / **yes** (0/3 producer→consumer merges, 423,235 → 406,661 recorded values on Stage A) |
| R6 | materialise only at domain boundaries (before `linmap`, at reductions, on profitable fan-out) | E0 | "materialise before a sparse reduce" (a measured 1.28× trap becomes unrepresentable) | no / **yes** (0/3 domains, 16,574 rows, into 3 consumers on Stage A) |
| R7 | block `linmap` by the column span its rows read | E0 | per-curve block GEMV | **yes** / **yes** (2 splits on the M1 book, 5 on Stage A — rows `[7456, 7668, 48, 3, 1742]`) |
| — | `fma_contraction`: `a·b+c` chains folded into one `Op::Fma` (D51; not in the original R1–R7 numbering) | E1 (scale abs(a·b)+abs(c), D57) | — | no / no (0/0 on both; latent only — D57 found the originally-declared flat-ulp tolerance unsound under cancellation before any real fire) |

Flip classification (from measured degenerate-tie flips): a `select`/guard flip is significant only if the **arm gap**
exceeds a threshold, never on the bit alone.

**As built (M4/R-c, D49; `include/epykos/rewrite/r6_materialise_boundaries.hpp`, `r7_block_linmap.hpp`):** R6 is
`planner.inline_producers`' own InlineIntoConsumer test (`rewrite/planner.hpp`, D47), reused directly, minus its
`row_fusion_pays(lane_tile)` gate and with a stricter (1.0, not 1.25) per-row reference cap — the annotation
vocabulary `merge_annotations` gives a rule has no way to say "materialise, overriding a fuse/inline decision
already made" (Materialize is the un-annotated default, indistinguishable from "this rule has nothing to say"),
so R6's own candidates are exactly the cases where nothing else has decided otherwise yet. Measured: 0 candidates
on the M1 book (every intermediate is a reduction, a reduction's member, or read far more than once per row), 3
domains (16,574 rows) into 3 consumers on the Stage A tape. R7 is a real domain split (`Proposal::program`, not an
annotation — there is no annotation slot for "this domain is really several"), preserving every value id exactly
so no gather or segment elsewhere in the program needs remapping; column-span blocks only, `min_block_rows` a
rule parameter. Measured: the M1 book's one-curve linmap domain still splits in two (D22's "t = 0" row, isolated);
the Stage A tape's shared-Input-domain linmap domain splits into 5 (rows `[7456, 7668, 48, 3, 1742]` on the full
tape). Not landed: R7's own DESIGN.md text also names exposing a linmap's Jacobian as `AdMode::ClosedFormAffine`
for the IFT — scoped out because `ir::PlanAnnotations::JacobianBlockPlan::mode` has no consumer yet (D47), so
nothing could gate a rule that populated it (D49 point 5).
**As built (M4/R-a, D52):** R1-R3 against the M4/R0 Rule interface (`rewrite::R1FoldUniformColumns`,
`R2BucketRows`, `R3ElideTrivialMaps`). R1 never fires on a Program straight out of `ir::infer` -- the signature
pass's own column classification (§5.5) already performs R1's fold at construction time -- but IS needed after a
rewrite that introduces a fresh Column without classifying it, which R2 does by construction (a bucket's
signature columns are uniform within it by definition). R2 buckets by MAXIMAL CONTIGUOUS RUNS of matching
signature, not a sort (a sort would need renumbering every downstream reader of the domain, program-wide).

**Amended by D65.** R2's gate used to reject any split with a singleton run, partly on quality grounds and partly
because a large singleton-heavy split reproducibly aborted with heap corruption while the rewritten program's
adjoint ran. That abort is a caller-side buffer-sizing defect in R-a's own Stage A test -- it handed
`adjoint::Adjoint::run` a 70-entry `state_bar` for a tape with 148 Inputs -- and nothing in `src/adjoint/` is
wrong; the harness now derives state and buffer lengths from the Program so the class is unreachable, and
`tests/adjoint/state_bar_bounds_e0_test.cpp` pins the write-bounds clause the caller broke. R2's gate is now two
structural floors: the split must consolidate something (fewer buckets than rows), and it must be a PER-KIND
PARTITION -- every distinct signature in exactly one contiguous run, which is what "per-kind batches" above
means. Singleton buckets are allowed. Measured: R2 fires on the M1 book's 10 domains 0 times and on the full
Stage A tape's 67 domains 3 times, every site bit-identical interpreter and adjoint; R1, given that R2 pass,
fires 0 times on the M1 book and 16 times on the 80 domains Stage A becomes. With the per-kind floor removed R2
fires 5 and 23 times and is still bit-exact, but the splits fragment rather than batch (M1 domain 4: 2,432 rows,
2 kinds, 1,243 contiguous runs) and the resulting proposal count blows `optimise::EGraph`'s node bound. R3 does
not fire on either fixture regardless. `rewrite::detail` (`bucket_split_edit.hpp`) holds the
editing primitives this package's three rules share (`drop_owned_entries`, `shift_domain_ids`, `eliminate_domain`)
-- renumbering domain ids after a domain is split or removed, and redirecting every reference to an eliminated
domain's rows to the value it now equals; `recompute_reads` itself is M4/R-b's own `ir_edit.hpp` (landed the same
function, same contract, for R4a/R4b/R5's domain insert/merge edits), reused here rather than duplicated.

**As built (M4/R-b, D51):** R4a (`include/epykos/rewrite/r4a_push_unary_through_gathers.hpp`), R4b
(`r4b_shared_reciprocal.hpp`), R5 (`r5_group_formation.hpp`) and a fourth rewrite this table does
not name — `fma_contraction.hpp`, `PROBLEM.md` §7's own text, `a*b+c` chains folded into one
`Op::Fma` — are implemented against M4/R0's `rewrite::Rule` interface, sharing one structural-
surgery primitive for the value-space renumbering a domain insertion or removal always needs
(`include/epykos/rewrite/ir_edit.hpp`). Measured on the Stage A tape and the M1 book (D51): only R5
fires on either (3 producer/consumer pairs on Stage A, 423,235 → 406,661 recorded values); R4a,
R4b and fma-contraction each find zero profitable sites on both fixtures, for three different,
independently measured and documented reasons (D51 point 5) — none of the three targets is wrong,
each fixture simply already avoids the specific redundancy that rewrite removes, by a different
mechanism (a record-time memo, heterogeneous per-curve gathers, and `affine_collapse` respectively).
CORRECTED BY D57 (a review finding on the M4-gate-1 landing): fma-contraction's declared E1 class
is only sound when checked relative to the pre-fusion operands' own scale (`|a*b| + |c|`, D26's
"difference of legs" pattern restated for this rule), never a flat ulps-of-VALUE bound — under
catastrophic cancellation of the Sum's two members the unscaled ulp distance is unbounded, proved
directly (not merely argued) in `tests/rewrite/fma_contraction_verify_test.cpp`. This is currently
latent (0/0 fires on both shipped fixtures, above), not a live defect in anything shipped.

---

## 7. Execution tiers

1. **Catalogue.** Each fused group has a canonical signature. A build-time tool runs the reference workloads, collects hot
   signatures, emits C++, and compiles it into the engine. AOT code generation, not a JIT.
2. **Tiled interpreter.** For uncatalogued groups: run the group's ops one at a time over a tile (~256 elements; M1
   measured 512 best single-state and flat across 128–512 at 32 lanes batched) with intermediates in an L1 scratch —
   dispatch per op per tile, not per element (the X100/DuckDB model).
3. **Batch mode.** Batch axis innermost on every domain; each element is a SIMD vector of scenarios, so the interpreter's
   dispatch amortises over the batch as well as the tile.

Adjoints (§3 rules applied group by group in reverse): scatter-add is replaced by a **pull** through the precomputed
transpose of each index array (CSR "who reads me"), so adjoint groups are conflict-free gathers and vectorise. At the
`Times` boundary `linmapᵀ` gives `−(G·diag(DF))·W` — the analytic calibration Jacobian, derived. As implemented
(M2/Q3, D31; `adjoint::Adjoint` over the domain IR, the interpreter's batch layout): the forward pass materialises
every row value, the reverse recomputes a group's intermediate steps per tile and reads its value from the buffer,
every (gather, row) and (segment, row) is an edge slot the reading group's reverse accumulates into, and a domain
pulls its rows' adjoints from those slots in one fixed order (seeds, gathers, Sum members, Affine members × coefficient).
Bits are independent of B, tile and lane tile by construction, so the E0 gates (forward bitwise vs the replay, a batched
lane bitwise the single-state run) hold under every preset. Measured on the M1 book (d448afd70180, informational, D9):
value + adjoint of the book PV 418.5 µs at B = 1, 7.38× the value-only interpreter, and 229.7 µs per state at B = 64,
9.31× — every row value is materialised and none of the interpreter's fusions apply in the reverse; the reverse of a
fused group is rewrite / catalogue work (M4).

Scan domains (M3/G3, D41): the interpreter evaluates a scan wave by wave — wave w is the rows whose carry chain has
depth w — in tiles of the indirect row mode (the carry is a gather like any other), so a wave of forty coupons' step
k runs as one tile and the batch lanes are the innermost axis; a scan is never fused into a reduction nor inlined.
The adjoint runs a scan's forward one row per tile in row order and its reverse one row at a time backwards: the
carry's edge slot (carry, r + 1) is one of row r's readers, so the reverse scan is the ordinary pull. Bits are
independent of B, tile and lane tile as for every other domain (the scan fixtures' E0 gates).

**As built (M4/R0, D47):** the tiled interpreter's own planning — which domain materialises, folds into a
reduction or inlines into a consumer's tiles; which consecutive steps run as one fused-pair kernel and which
further steps chain on as a tail; which output rows a reduction block emits directly — is no longer decided inside
`exec::Interpreter`'s constructor. Five pure functions of the domain IR (`rewrite::planner::reduction_fusion_plan`,
`fused_pairs_plan`, `chain_tails_plan`, `inline_producers_plan`, `emit_outputs_plan`, `include/epykos/rewrite/
planner.hpp`) compute exactly what `decide_fusion` / `decide_inline` / the fused-pair loop computed before this
package, wrapped as `rewrite::Rule` objects (`planner.reduction_fusion` etc., `planner_rules.hpp`) that write an
`ir::PlanAnnotations` (`ir/annotate.hpp`) rather than deciding for one Interpreter alone; `exec::Interpreter` reads
that plan (a caller's `program.plan`, or its own default pass over `Options` — PROBLEM.md §7's "Options flags keep
working by selecting the greedy pass") instead. The default plan is bit-identical and time-identical to the one
before this package (`tests/rewrite/planner_rules_m1_e0_test.cpp`; the M1 gates re-run unchanged). `rewrite::Rule`
(`include/epykos/rewrite/rule.hpp`) is the interface DESIGN.md §6's own R1-R7 will implement: a matcher over the
IR plus the annotations decided so far, and a constructor of the rewritten form (a Program, for a structural
rewrite; a `PlanAnnotations` delta, for a planning decision), usable both as a greedy pass (`rewrite/greedy.hpp`)
and, unbuilt, as an e-graph (M4/EG): "produce the alternative without discarding the original" is the contract's
own words, not this package's implementation of one.

**As built (M4/CM, D48):** `PROBLEM.md` §7's cost model (`include/epykos/optimise/cost.hpp`) prices an `ir::Program`
per domain — rows x lanes x per-op cost, tile-aware L1/L2/L3/DRAM byte traffic (the working set is one tile's
worth, `tile rows x lane width`, not the whole domain: DESIGN.md §4's own scratch is sized that way), a gather cost
per indirect read, a per-kernel dispatch cost, a reduction-epilogue cost per fold step, and a Jacobian block's cost
by AD mode (forward / reverse / closed-form affine, §7's own "the forward-versus-reverse choice per Jacobian
block"). It prices its own `Treatment` / `Plan` annotation rather than reading M4/R0's `ir::PlanAnnotations` above,
because the two packages ran in parallel from the same base and CM's cost function must also be able to price a
*candidate* program EG has not built a real `Interpreter` for; `infer_plan` reproduces the planner's fusion /
inlining rules as IR-structure predicates for that case, and a live `ir::PlanAnnotations` (when a caller has one)
is the more accurate source the two should be reconciled onto in a follow-up. `tools/costmodel/` calibrates the
coefficients per fingerprint by relative-error least squares on the M1 book and the Stage A tape under a new
`profile` preset; on this machine the fit falls short of the package's own < 25% mean-relative-error target (84%
overall, 69% restricted to domains that are a material share of their config's time), reported as such rather than
narrowed to a friendlier number — `bench/results/<fingerprint>/cost_model_validation.md` names the causes.

**As built (M4/EG-core, D49):** `include/epykos/optimise/egraph.hpp` is the e-graph M4/R0's own
`rewrite::Rule` contract was written for ("produce the alternative without discarding the
original", D47) and M4/CM's `optimise::Plan` was written to cost: two tiers of whole-value
alternatives — a PROGRAM tier (`ir::Program` values reachable by structural Proposals, hash-consed
by exact `ir::serialize` identity) and, one per program node, a PLAN tier (`ir::PlanAnnotations`
values reachable by annotation Proposals folded with `merge_annotations`, hash-consed by this
package's own structural equality since `PlanAnnotations::operator==` is unconditionally `true` by
D47's own design). Saturation runs every registered `rewrite::Rule` to a fixpoint or a stated
bound, queuing every site's Proposal without replacing the node it came from and deferring
hash-consing to one `rebuild()` per round (real congruence: two derivations converging on the same
content in ONE round are unioned there); a persistent-table pre-check keeps an always-matching
rule (every one of R0's five) from re-queuing its own already-known result forever, so the
`saturate()` loop reaches an actual fixpoint rather than running to its iteration bound every time.
**Amended (M4/EG-scale, D62):** that pre-check was still pay-first-check-later — a proposal was
cloned, validated and serialised before the duplicate was found — and nothing stopped the node
count growing multiplicatively once several structural rules fire at once. `saturate()` now
memoises each (program node, rule, site) application (both of a node's inputs to `match`/`propose`
are fixed for its whole life, so the answer is), matches only a program CLASS's representative,
and takes a `RefirePolicy`: `AllRules` is D49's complete shape, `NoFreshCrossRule` (the default)
lets a structural rule extend only its own output once it has fired, and `PipelineOrderedPlans`
additionally runs plan derivations in the caller's declared rule order. The last two are a stated
completeness tradeoff in the SEARCH, never in what is extracted; both are documented with what
they give up in `egraph.hpp`'s own header.
`include/epykos/optimise/extract.hpp` picks the cheapest (program-class, plan-class) pair by
CM's `estimate_program`, filtering out any node whose accumulated `rewrite::Exactness` (the max
over every rule in its own derivation) exceeds the caller's request — E0 extraction can never
select an E1 rewrite, even the cost model's own favourite one — with a deterministic tie-break.
`include/epykos/optimise/plan_bridge.hpp` is D48 point 6's own follow-up: a live
`ir::PlanAnnotations` (what every real plan-tier e-node actually is) translates 1:1 into CM's
`optimise::Plan`, and `infer_plan`'s structural guess is now only ever the fallback for the empty
root plan node ("nothing decided yet"), not every candidate the way it was before this package.
First experiment (PROBLEM.md §7's own ask, not a gate): extracting over ONLY R0's five rules at
several `lane_tile` choices at once and comparing against `default_plan`'s own greedy pipeline on
the M1 book — the extracted plan passes the same E0 verification at every configuration tried, but
does not yet consistently beat the greedy default on REAL measured time, consistent with (not a
contradiction of) D48's own reported cost-model accuracy shortfall: an extraction search is only
as good as the cost it argmins over. Rules present: R0's planner rules and the R1-R7 identity
stubs; adding a rule (R1-R7's real bodies, R-a/R-b/R-c's job) needs no change to this file's own
machinery, by design — `egraph.hpp` never names a rule by name. Not done in EG-core (a later
package's scope, `RESUME.md` §3's EG row): the AD-mode-per-Jacobian-block rule, cross-stage sharing
rules, and reconciling the plan tier's per-node keying onto program-tier CLASSES (flagged, not
silently assumed, in `egraph.hpp`'s own header comment).

**As built (M4/EG integration, D54):** two new rule families and one extraction-time guard, on top
of the now-real R1-R7/fma (R-a/R-b/R-c) registered alongside R0's planner rules.
`rewrite::ADModePerBlockRule` (`include/epykos/rewrite/ad_mode_rule.hpp`) fills the
`jacobian.mode` slot D47 left empty: given a caller-named `JacobianBlockSpec` (output domains +
input/output counts — HARD RULE 9, no problem-specific magic inside the rule itself), it prices
Forward / Reverse / ClosedFormAffine by `optimise::estimate_jacobian_ns` (ClosedFormAffine
eligible only when every domain is a `rewrite::is_linmap_domain`, R7's own fact) and picks the
cheapest. Its REQUIRED consumer (R7's own header and D50 point 5: a rule proposing this slot
"cannot be checked by CLAUDE.md's own gates" without one) is `adjoint::block_jacobian`
(`include/epykos/adjoint/block_jacobian.hpp`): Reverse via the real `adjoint::Adjoint`, one lane
per output; Forward via one-sided finite differences of `exec::Interpreter` (a stated stand-in —
no generic IR-level tangent evaluator exists, RESUME.md's own M3/G5 simplification — priced with
the cost model's own "n_inputs passes" shape, gated at FD tolerance, never bitwise);
ClosedFormAffine by reading a linmap's constant Affine coefficients directly (exact, no
evaluation), scoped to members that are themselves Program Inputs and throwing, not
mis-differentiating silently, otherwise. `rewrite::cross_stage_sharing_guard`
(`include/epykos/rewrite/cross_stage_sharing.hpp`) turns `ir::assert_all_shared` into the new
`optimise::ExtractOptions::reject` predicate (additive, default `nullptr`, no change to any
existing caller): a candidate program failing it is rejected outright, at every one of its plan
candidates, the same treatment an exactness overrun already got — PROBLEM.md §7's "a rewrite that
would split [the shared DF domain] is rejected", literally. The IFT product-order question
(materialise `F_z^{-T} z̄` once vs re-solve per output) and sharing a scenario lane's factored
Jacobian by its select mask were NOT reached (see this decision's own experiment notes below);
flagged as open, not silently dropped.

Experiments (`bench/results/d448afd70180/m4_experiments.json`; full notes in this decision's own
entry): (1) REDISCOVERY meets the cost-model's own "never worse than the default" bar (ratio
1.0, tied) but NOT the 1.02x measured-wall-clock target (measured 1.081x-1.104x), traced to
`plan_bridge.hpp`'s own pre-existing, documented gap (`plan.group` unpriced) combined with
extraction's tie-break; (2) CROSS-STAGE found a candidate the fixed single-pass pipeline cannot
express (`r5.group_formation` applied twice) on a SMALL Stage A fixture — CORRECTED BY D57 (a
review finding on the M4-gate-1 landing): the 0.977x this package originally reported as "a real,
verified win" was priced with `optimise::CostCoefficients::defaults()`, a synthetic, never-fitted,
uniform-per-op model, not this fingerprint's actual `bench/results/d448afd70180/cost_model.json`
(D48); under the FITTED model the ratio is 0.9997, i.e. noise, and no wall-clock bench corroborates
either number (contrast experiment (1), which has one). PROBLEM.md §7's cross-stage-win gate item
is NOT considered satisfied by this experiment (D57). Saturating the FULL rule set past a few
rounds also grows unboundedly on Stage-A-shaped programs (measured: 1.87 GB and still climbing by
round 4 on a 60-trade fixture, killed) — this package's most significant finding, left for whoever
adds a redundancy/subsumption check next; (3) AD MODE reproduces Stage A's own already-measured
Forward/Reverse choice correctly under both the M1-calibrated and Stage-A-measured adjoint
multiplier, with two now-documented cost-model gaps (reverse's linear-in-outputs formula misses
batching/chord sharing; forward's formula
understates a wide `Dual<N>` pass); (4) E1 EXTRACTION is informational only, same root cause as
(1).

**As built (M4/C1, D55; `Signature` made canonical under commutativity by D61):** §7 tier 1's
catalogue is `include/epykos/catalogue/` (`Signature`: the
op at each step plus each operand slot's KIND — Step/Literal/Column/Gather/Segment — and, for a
Step operand, how many steps back it points; no row count, no table index, no literal/column/
gather value, and (D61) no dependence on WHICH ORDER a commutative step's two operands are in —
`ir::infer` already treats `a op b` and `b op a` as one isomorphism class, but which order the
emitted Step carries is the first recorded instance's, and tape order is compiler-dependent
because C++ leaves a binary operator's operands unsequenced, so `signature_of` and `bind_domain`
order them canonically; `Kernel`, `bind_domain`, `registry.hpp`'s hash-then-full-equality lookup) plus
`tools/catalogue/`'s generator (`scripts/catalogue_regen.sh`), which walks the Stage A tape and
the M1 book straight out of `ir::infer` (no R1-R7 rewrite or EG extraction first — either is a
Program a caller supplies, not what a default `Interpreter`/`Adjoint` construction runs today; a
later regen against one needs a different `record_and_infer`, not a different `Signature` or
codegen) and writes one kernel per distinct signature found, deterministically. `exec::Interpreter`
dispatches to it only for a plain per-tile Materialize domain (not fused into a reduction, not
inlining a producer, not itself inlined, not a scan — its own three optimisations are left alone);
`adjoint::Adjoint`, which applies none of those (§7's own "the reverse of a fused group is rewrite
/ catalogue work (M4)"), dispatches unconditionally for every catalogue-eligible domain, scan
included. Measured coverage and its one real limit (a per-netting-set fixed-arity Sum's exact
arity depends on the trade-to-netting-set draw, so a Stage A instance other than the two reference
workloads reaches ~87-93%, not 100%, of candidate domains — unchanged by D61, which only made
those same numbers the same on every compiler) are in D55, D61 and `docs/RESUME.md` §5.

**As built (M4-close, D59): the search, cost model and catalogue, verdict fail.** All four §7 mechanisms described
above shipped — the `rewrite::Rule` interface and planner-as-rules (D47), the fitted per-domain cost model (D48), the
two-tier e-graph with saturation and cost-based extraction (D49), the AD-mode-per-Jacobian-block rule and
cross-stage-sharing guard (D54), and the catalogue (D55) — each with its exactness class, differential test and
mutation test (43/43 registered mutants caught, 0 survivors, `ctest` 107/107 under both release and reference at
fingerprint `d448afd70180`). Against `PROBLEM.md` §7's own exit gate: every e-graph-extracted program verifies at its
declared class against the true unrewritten original (D57's own added `verify_extraction` check) and the
self-regression gate against the M3 baseline passes (17/17 benchmarks, 0 regressions — catalogue coverage gives a real
1.11–1.16x win on the reverse risk ladder, `adjoint::Adjoint` now dispatching to a catalogued kernel for ~99.9% of its
own Stage A wall time); but rediscovery of M1's three kill-path fusions ties the cost model's own estimate exactly
(ratio 1.0, `plan_bridge.hpp`'s documented unpriced-terms gap) while missing its own 1.02x measured-wall-clock target
(1.0277x at B=1, 1.0427x at B=64), and the one cross-stage candidate the search found beyond the fixed pipeline
(`r5.group_formation` applied twice on a bounded Stage A fixture) is, once D57 corrected the pricing bug that made it
look like a 0.977x win, actually 0.999716x — noise under this fingerprint's real fitted cost model, not a win. Two
mechanism-level gaps explain most of the shortfall: the cost model's mean relative error (84.4% overall, 69.3%
restricted to domains material to their own config's time) is well outside its own <25% target, so extraction argmins
over a materially wrong cost surface; and `EGraph::saturate` has no redundancy or subsumption check (D12 rules out an
external e-graph library), so it grows unboundedly on Stage-A-shaped programs past a small, deliberately safe bound —
the full 517,036-node Stage A tape was never saturated. Verdict: **fail** (`docs/RESUME.md` §5 "M4 result",
`docs/DECISIONS.md` D59).

---

## 8. Value-dependent behaviour

| class | example | treatment |
|---|---|---|
| A structural | region lookup, schedule shape, turn windows | folded at record time; a change is a re-record |
| B limiter (min/max/abs/sign) | Hyman monotonicity filter | `select`; mask + margin + arm gap exported (as outputs: `tape/select_export.hpp`, D39; the safe-arm table of §10); kinks become an active set in the solver via `pin` |
| C formula selection on a quote | Huber bid/offer band | outside the pricing graph: `row_map` + solver active set |
| D iterative / implicit | yield, implied vol, calibration, regressions | `implicit`; never unrolled |
| E discrete argmin | bond-future CTD | `select` over a small candidate set, or a `frozen` index with a guard |
| F discontinuous payoff | digital, barrier | conditional one-step survival (Brownian bridge) where available, else `smooth_step`; vibrato MC when bias is unacceptable |
| G early exercise | Bermudan (LSM) | regression β `frozen`, exercise as `select` on the frozen rule (envelope theorem ⇒ first-order Greeks exact); low-dim via a fixed-grid PDE |
| H adaptive numerics | adaptive quadrature/ODE/PDE grids | freeze the grid as structure; a-posteriori error estimate as a guard; or fixed high-order rules |

**As built, Stage A scale (M3/G5, D44):** the base Stage A tape (linear-zero curves) records **no class-B `select`
at all** — every discount factor, coupon and leg PV is a plain elementwise or scan row — so it produces no O6
select exports; class B is exercised by the SOFR curve's monotone-cubic and composite recordings, gated separately
on the 300-trade scheme-sweep sample (126 / 36 selects exported respectively, D44), not by the 2,000-trade book.

---

## 9. Structure churn

- Record per **template**, with trade data (notionals, rates, year fractions, gather indices) as table **inputs**.
- New trade of a known template: append a row. Date roll: times are data; kernels unchanged; segment indices recomputed at
  table build. Fixings: a coupon row moves between buckets in O(1).
- New structure: re-record (ms) off-thread; diff signatures against the persistent map; swap atomically.
- Cache layers: topology (daily) → trade tables (intraday) → state (tick).

---

## 10. Numerics and determinism

- FMA contraction changes bits. Reference TUs used for bit-identity gates build with `-ffp-contract=off`, pinned by name
  (`src/**/*_e0.cpp`, `tests/**/*_e0_test.cpp`) in every preset on every compiler (D25: a clang pragma is not enough, GCC
  contracts in ISO C++ mode); production builds may contract, and gates then use E1 tolerances.
- Every rewrite declares its exactness class; the verifier applies the matching tolerance.
- `select` evaluates both arms: arms must be NaN-safe (safe-arm discipline; e.g. `m/|m|` → `copysign`). The rewrites
  used by the MonotoneCubic scheme's Hyman limiter (`maths/curve/scheme.hpp`, M3/G1, D38), the first class-B maths in
  the engine, are the table below; every arm is a product or a selection of finite values, so no arm can overflow,
  divide by zero or produce a NaN for finite inputs, and the recorded Select nodes export mask, margin and arm gap
  (`tape/select_export.hpp`, D39):

  | naive form | hazard in the unselected arm | safe form recorded |
  |---|---|---|
  | `sign(x) = x / |x|` | `0 / 0` at `x = 0` | `select(x < 0, −1, +1)` (`+1` at 0; the sign only ever multiplies a bound that vanishes with `x`) |
  | `|x|` as `sqrt(x²)` or `std::fabs` | a branch on a value / a non-`Scalar` call | `select(x < 0, −x, x)` (`abs(−0.0)` is `−0.0`) |
  | `min(a, b)`, `max(a, b)` | a branch on a value | `select(b < a, b, a)`, `select(a < b, b, a)` |
  | `clamp(x, 0, L)` | a branch on a value | `select(L < m, L, m)` with `m = select(x < 0, 0, x)` |
  | the Hyman bound as a ratio, `m · min(1, 3·min(|dl|, |dr|) / |m|)` | division by `|m| = 0` | `σ · min(max(σ·m, 0), 3·min(|dl|, |dr|))`, `σ = sign(dr)`: products only |
  | zero at an extremum, `if (dl·dr <= 0) m = 0` | a branch on a value | `select(dl·dr > 0, limited, 0)`; the limited arm is finite at `dl·dr = 0` |
  | the harmonic-mean tangent, `(w1 + w2) / (w1/dl + w2/dr)` | `1/0` at a zero secant, `inf − inf` | not used: the three-point (Bessel) tangent is a weighted sum with structural weights |
  | `min(|dl|, |dr|)` at an end knot, where `dl = dr` | a Select whose two arms are one value: a permanent tie with zero margin | structural: `3·|d|` at an end knot, no Select written |

  A flip of such a select between two states is classified by the arm gap, never by the bit (§6): the limiter's
  selects are ties (`min`, `max`, `abs`, the sign), so their gap vanishes with the margin and a flip is degenerate; a
  select whose arms differ by a finite amount at the boundary is a jump (measured: `classify_flip`).
- No `-ffast-math`.

**As built, Stage A scale (M3/G6, D45):** the differential ball (64 draws, ρ 0.005 over the 70 quotes) is bitwise
E0 under both the release and reference presets on all 8,191 outputs — 0 mismatches, max ulps 0 — confirming §11's
E0 discipline holds through an implicit block, a scan and the full instrument set, not only on the M1/M2 fixtures.
Every other §6 gate agrees release vs reference to within the last few digits of its worst number, as expected from
`-ffp-contract=off` (D45; e.g. the adjoint-vs-Dual worst is 2.79e-15 release / 2.25e-15 reference).

---

## 11. Verification

Correctness gates:
- **round-trip identity** of domain IR vs recording (§5.7);
- **differential**: compiled vs templated-`double` at randomised state in a ball around the record point (E0 exact under
  `-ffp-contract=off`, else E1 tolerance: D26's bound at the scale of the terms, D30; `verify/differential.hpp`,
  M2/Q1: the ball is computed in an E0 TU, the compiled side is batched, the report names the worst output and draw);
  this also catches missed branches;
- **adjoint** vs central finite difference and vs forward mode (`Dual`), and linearity in the seed — on the M1 book
  and on the near-miss shapes fixture, whose `select` / `recip` / `fma` / `log` / `sqrt` rules the book cannot
  exercise (`adjoint::Adjoint` and `scalar/dual.hpp`, the same templated maths on `Dual<N>`; M2/Q2, Q3, Q3b, Q4b:
  FD within 2.9e-9, forward mode within 2.1e-13 on the M1 book);
- **scan gates** (M3/G3, D41): the RFR compounding book and the affine scan (`fixtures/rfr_book.hpp`,
  `fixtures/affine_scan.hpp`; WORKLOADS §M2) — detection without hints on the raw recording and after the passes,
  round-trip identity, the interpreter bitwise the double maths at B = 1 and B = 64 over tiles and lane tiles, the
  adjoint vs forward mode at 1e-12 and vs FD at 1e-6, the batched adjoint lane for lane; the M1 book asserted free
  of scans and unchanged;
- **mutation testing** on rewrite rules (a mutated rule must fail a gate): every pass carries its mutants as one-line
  defects behind `epykos::mutant("<pass>.<defect>")` (`include/epykos/mutation/`), compiled in only by the `mutation`
  preset and selected one per process by `EPYKOS_MUTANT`; `scripts/mutation_test.sh` runs the gates above once per
  registered mutant and fails if any survives — a survivor is a gap in the gates, never a job for a mutant-specific
  test (D32); the adjoint's mutants are caught by the adjoint gates, which are part of the harness's gate set (D33);
- **error against truth** (P0/oracle, D72; `PRINCIPLES.md` §4): the gates above compare one `double` evaluation with
  another, which says nothing about how far either is from the recorded expression's true value. `verify/oracle.hpp`
  compares a `double` path with the SAME templated maths instantiated on `epykos::Wide` (`scalar/wide.hpp`), an
  in-repo double-double carrying 106 significand bits against `double`'s 53 — an instantiation, never a second copy
  of the maths (D3). `long double` is NOT the oracle (it is `double` on Apple arm64, one of CI's four jobs) but its
  witness, skipped loudly where it cannot judge; a `static_assert` on the `Oracle` alias makes the degraded
  configuration a compile error. Reporting is per output class, because `PRINCIPLES.md` §4 makes tolerances per class
  and not global, and the harness returns NO verdict: the numbers belong in `PROBLEM.md` and are set from the
  measurement rather than before it. `oracle_jacobian` is the sensitivity channel, a Richardson-extrapolated central
  difference taken at 106 bits. Measured on `d448afd70180` (D72 §4): the naive path's valuation error is 1.792e-15 of
  the D26 leg scale on the M1 book and 7.854e-14 to 1.616e-13 on Stage A, and its SENSITIVITY error is 62x and 656x
  larger respectively;
- **external oracles** (QuantLib and others) added per product, test-only.

Performance gates: per machine+toolchain fingerprint; fail on > 1.25× self-regression or an absolute target miss. As
built (M2/Q5, D29, D34): `bench/run.sh` writes `bench/results/<fingerprint>/<run>.json` (refusing at a 1-minute load
above cores/2) and `scripts/perf_gate.py` gates it against `baseline.json` of the same fingerprint — keyed by the run
name derived from the binary, moved only by `--accept` in a perf commit — and against the absolute targets of
`bench/targets.json`; the M1 numbers are the first baseline, and the Stage A tape's `stage_a_stage_a` run (record, one
calibration, O2 evaluation, the O3 ladder in both AD modes, O4 per scenario; M3/G6, D45) is the baseline M4 is gated against.
Reference implementations (QuantLib, hand-fused kernels) are informational tables, never the gate.

---

## 12. Where this is expected to be weak

- **Irregular code with a batch axis** (scripted exotics): low catalogue coverage → interpreter-bound; a JIT (AADC-style)
  likely wins by 1.3–2× (estimate).
- **Recurrences**: scan-domain detection exists (M3/G3, D41) for chains of identical steps with the carry at most
  four operands deep; a recurrence through an Affine, or a step that reads its own earlier steps through another
  class, falls back to level splitting (correct, one domain per step); the scan's step is not yet fused with its
  neighbours and the adjoint reverses it one row at a time (M4).
- **Brownfield**: requires `Scalar`-templated maths; cannot accelerate an existing OO library.
- **Compiler risk**: bugs are wrong numbers, not crashes. The verification harness precedes the compiler.
- **Linear-path parity**: the generic path must match a hand-fused kernel; the scalar tape measured 1.5–2.5× behind. M1
  settled this on the linear book (2026-09-23, d448afd70180): 1.044× single-state and 1.079× batched with `std::exp` on
  both sides, after three tile/layout iterations (reduction fusion, fused step pairs, exp tails + inlined producers);
  E1-vs-E1 against the hand kernel's polynomial exp it is 1.240× batched, the residual being E0 arithmetic (two IEEE
  divisions per forward, three-rounding coupons) that R4b and fma contraction address in M3.
- **Adjoint MC at scale** (checkpointing, per-thread accumulators) is unbuilt.

## 13. Non-goals

- A runtime JIT (revisit only if M1/M3 show the catalogue cannot close the gap).
- Accelerating third-party OO libraries.
- A GUI or web layer in this repository.
