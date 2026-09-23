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
| `scan` | cumulative `⊕` along a sequence (product, sum, affine step) | reverse scan | compounding, survival, path evolution |
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

As implemented (M3/G4, D37; `solver/implicit.hpp`, `solver/residual.hpp`, `solver/implicit_program.hpp`,
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
6. **Recurrence detection:** if instances of one class depend on each other, the class is a `scan` candidate (sequential
   along the chain; parallel only across the batch axis). As implemented (D23): the class is flagged `scan_class`, its
   instances are split by dependency level so every domain reads only earlier domains, and `recurrent` (a domain whose
   rows read that same domain, which the interpreter refuses) is never produced by inference; M1 asserts no scan class
   on its book.
7. **Round-trip check:** expand the domain IR back to a scalar tape and compare node-for-node with the recording.
   Identity, not tolerance.

Expected outcome on a rates book: `Knots —linmap→ Times —exp→ DF —gather→ Subs —segment_sum→ Coupons —segment_sum→ Legs →
Rows`, with buckets falling out as separate signatures where their op trees differ (a coupon with a realised fixing
has the fixed coupon's tree with a constant in the rate slot, so it shares that class and is separated, if wanted,
by the uniform-column bucketing of §6 R2, not by the signature; D22).


Across recordings the signature → domain map persists: a new trade of a known shape adds rows, not code.

---

## 6. Fusion rewrites

Applied in order. Each has an **exactness class**: *E0* bit-identical, *E1* ≤ 1 ulp per op (tolerance-gated; for a value
that is a difference of terms, such as a swap PV, the tolerance is relative to the scale of the terms, D26).

| # | rule | class | recovers (in SwapEngine terms) |
|---|---|---|---|
| R1 | fold uniform columns (`k==1`, `konst==0`, `w==1`) | E0 | `cpn_is_plain` |
| R2 | bucket rows by uniform-column signature | E0 | per-kind batches |
| R3 | elide trivial maps (length-1 segments, identity gathers ⇒ merge domains) | E0 | `sub_is_identity` |
| R4a | push pure unary ops through gathers toward the smaller domain, then CSE (`exp`) | E0 | `exp` once per time |
| R4b | same for `recip` (`a/b → a·recip(b)`) | E1 | shared `INV` |
| R5 | group formation: maximal elementwise region per domain; gathers in; segment-sum epilogue | E0 | fused coupon→leg loop |
| R6 | materialise only at domain boundaries (before `linmap`, at reductions, on profitable fan-out) | E0 | "materialise before a sparse reduce" (a measured 1.28× trap becomes unrepresentable) |
| R7 | block `linmap` by the column span its rows read | E0 | per-curve block GEMV |

Flip classification (from measured degenerate-tie flips): a `select`/guard flip is significant only if the **arm gap**
exceeds a threshold, never on the bit alone.

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
- **mutation testing** on rewrite rules (a mutated rule must fail a gate): every pass carries its mutants as one-line
  defects behind `epykos::mutant("<pass>.<defect>")` (`include/epykos/mutation/`), compiled in only by the `mutation`
  preset and selected one per process by `EPYKOS_MUTANT`; `scripts/mutation_test.sh` runs the gates above once per
  registered mutant and fails if any survives — a survivor is a gap in the gates, never a job for a mutant-specific
  test (D32); the adjoint's mutants are caught by the adjoint gates, which are part of the harness's gate set (D33);
- **external oracles** (QuantLib and others) added per product, test-only.

Performance gates: per machine+toolchain fingerprint; fail on > 1.25× self-regression or an absolute target miss. As
built (M2/Q5, D29, D34): `bench/run.sh` writes `bench/results/<fingerprint>/<run>.json` (refusing at a 1-minute load
above cores/2) and `scripts/perf_gate.py` gates it against `baseline.json` of the same fingerprint — keyed by the run
name derived from the binary, moved only by `--accept` in a perf commit — and against the absolute targets of
`bench/targets.json`; the M1 numbers are the first baseline.
Reference implementations (QuantLib, hand-fused kernels) are informational tables, never the gate.

---

## 12. Where this is expected to be weak

- **Irregular code with a batch axis** (scripted exotics): low catalogue coverage → interpreter-bound; a JIT (AADC-style)
  likely wins by 1.3–2× (estimate).
- **Recurrences** need scan-domain detection, unprototyped.
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
