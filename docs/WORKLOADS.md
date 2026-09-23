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
  Rewrite mutants (R1–R7) land in M3 with the rewrites. As registered (`include/epykos/mutation/mutation.hpp`, pinned
  by `tests/mutation/registry_test.cpp`, run by `scripts/mutation_test.sh`; D29), in harness order:
  `cse.merge_nonequal` (a Const operand's bit pattern is ignored by the CSE key), `fold_sum.wrong_order` (Sum operands
  reversed), `affine.wrong_coefficient` (the first coefficient of the first Affine emitted is one ulp off),
  `affine.drop_offset` (c_0 dropped), `expander.drop_gather` (gather 0 reads the identity index),
  `expander.segment_off_by_one` (every segment loses its last member), `signature.merge_classes` (the const-slot
  pattern is not part of the signature), `interpreter.tile_boundary` (the last row of every elementwise tile is
  skipped). The adjoint mutants (wrong transpose, dropped pull) are added by M2/Q4b once the adjoint lands.
- **Near-miss shapes** (`include/epykos/fixtures/nearmiss_shapes.hpp`, the gate fixture the mutation harness showed
  was missing, D29): 42 templated shapes over six positive inputs, each an op tree that differs from a neighbour in
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
  evaluator E0, interpreter E0 at `B = 1` and `B = 64` over tiles {1, 7, 256} × lane tiles {1, 3, 8, 64}.

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
