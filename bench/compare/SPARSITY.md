# The structural sparsity of ∂F/∂z, and whether a sparse-Jacobian colouring is worth building

`tools/sparsity/` emits the measured half of this file. Everything in §1 was written and committed
**before the binary existed** — the same discipline `tools/curveid/` used (D110) — so the
predictions can be wrong on the record rather than adjusted after the fact.

The relation under measurement is `solver::structural_pattern` (`include/epykos/solver/sparsity.hpp`):
which unknowns of a declared `ImplicitBlock` reach which residuals, by reachability over the
**recorded forward program**. It is a question about F and not about ∂F, which is why it is
answerable with no I1, no I3 and no new op (`PRINCIPLES.md` §1, D87).

---

## 1. Pre-registered predictions

### 1a. The orchestrator's prediction (quoted as given)

> - **Single curve: colouring buys nothing, exactly.** Instrument *i* at maturity `T_i` depends on
>   every knot up to `T_i`, so row *i* is nonzero in columns `0..i` — a dense lower triangle.
>   Column 0 appears in every row, so no two columns are ever orthogonal and **colours = n_r**.
> - **Multi-curve: colours ≈ the largest per-curve block, not the sum**, because USD instruments do
>   not touch EUR knots. On Stage A's 4 curves that bounds the prize near **4x**, degraded by real
>   coupling — 3s6s basis spans two EURIBOR curves, and the EUR curves discount off ESTR.

### 1b. This agent's prediction, where it differs

**Single curve: agreed, and for the stated reason.** `compare_ois` is `(logdf, linear)` with knots
at the maturities, so instrument *i*'s annuity reads DF at every annual point up to `T_i = t_i` and
a log-DF-linear curve's DF(t ≤ t_i) depends on knots `0..i`. Dense lower triangle, density
`(n+1)/2n` ≈ 52% at n = 25, **colours = n_r = 25, lane reduction exactly 1.00x**. Already
demonstrated on the 5-knot analogue in `tests/solver/structural_gate_test.cpp`.

**Multi-curve: the 4x is predicted to be ALREADY BANKED, and not by colouring.** The orchestrator's
prediction implicitly treats Stage A as one block with 70 residuals over 4 curves' knots. It is not,
in the mode everything ships in. `CurveSet::calibrate` defaults to `Mode::sequential`, which records
**one `ImplicitBlock` per strongly connected component** of the curve dependency graph, in
dependency order; a later block reads an earlier block's unknowns **as parameters p, never as
unknowns z** (`solver/implicit.hpp`: "a later block may read an earlier block's unknowns, never the
other way round"). So ∂F/∂z of each block is confined to that curve's own knots by construction —
the block-diagonal structure the colouring would have to discover was already extracted at
recording time, by the dependency analysis, four milestones ago.

Prediction, therefore:

| object | predicted colours | predicted lane reduction |
|---|---|---|
| `compare_ois`, 25 knots | 25 = n_r | **1.00x** |
| `stage_a_h2h`, `Mode::sequential` (the shipped mode), each block | n_r of that block | **1.00x, every block** |
| `stage_a_h2h`, `Mode::joint` (one block, all curves) | ≈ the largest per-curve block | ~3x–4x, degraded by the ESTR discounting and the 3s6s basis |

The sharp claim is the middle row: **on the path the engine actually runs, the prize is not 4x
degraded by coupling, it is 1.00x, because the 4x is not available to be won twice.** `Mode::joint`
exists and would show the orchestrator's number, which makes the two predictions distinguishable
rather than a matter of framing.

Secondary prediction: the per-curve blocks are **not** perfectly triangular in Stage A the way
`compare_ois` is, because Stage A's USD set mixes deposits and SR3 futures with OIS, and a futures
contract's accrual spans an interval that need not end at a knot. Expect a near-triangle with a
little fill above the diagonal, and no change to the colour count — one dense column is enough to
force `colours = n_r` and column 0 will be in nearly every row regardless.

---

## 2. Measured

`./build/release/tools/sparsity/sparsity all`, fingerprint `d448afd70180`, flags
`-O3 -march=x86-64-v3 -fno-math-errno`. Counts and patterns only: nothing here is a timing, so no
reserved box and no load gate (`tools/coverage/`'s position, for the same reason). Every structural
count below was cross-checked against the **numerical** nonzero count of ∂F/∂z at the record point,
through the engine's own batched adjoint: **TIGHT on every block of every fixture**, so the
conservative superset is exact here and the shape claims are claims about the derivative.

| fixture / block | n_r | n_z | nonzeros | density | shape | greedy colours | lane reduction |
|---|---|---|---|---|---|---|---|
| `compare_ois`, 16 tenors (default) | 16 | 16 | 136 | 53.12% | lower triangular, 1 component | **16** | **1.000x** |
| `compare_ois`, 25 tenors (the head-to-head's) | 25 | 25 | 325 | 52.00% | lower triangular, 1 component | **25** | **1.000x** |
| `stage_a_h2h` seq. — `USD-SOFR-LOGDF` | 22 | 22 | 142 | 29.34% | lower triangular, 1 component | 14 | 1.571x |
| `stage_a_h2h` seq. — `EUR-ESTR-LOGDF` | 18 | 18 | 133 | 41.05% | lower triangular, 1 component | 18 | 1.000x |
| `stage_a_h2h` seq. — `EUR-EURIBOR-3M-LOGDF` | 18 | 18 | 146 | 45.06% | lower triangular, 1 component | 12 | 1.500x |
| `stage_a_h2h` seq. — `EUR-EURIBOR-6M-LOGDF` | 12 | 12 | 78 | 54.17% | lower triangular, 1 component | 12 | 1.000x |
| **`stage_a_h2h`, `Mode::sequential` — all four blocks** | **70** | **70** | 499 | — | four independent blocks | **56** | **1.250x** |
| `stage_a_h2h`, `Mode::joint` — one block | 70 | 70 | 788 | 16.08% | banded (44 below, 12 above), **block diagonal in 2 components of 22 and 48** | **35** | **2.000x** |
| `stage_a` (default, linear-**zero**), `Mode::sequential` | 70 | 70 | 499 | — | identical to `stage_a_h2h`, block for block | **56** | **1.250x** |

### 2a. The two offered routes, measured rather than reasoned about

The brief offered two routes to the relation and asked for both to be verified.

**`dce`'s live-marking (`src/tape/passes.cpp`) — the reading is correct and the route is sound**, but
it is not the one to use: `dce` seeds from **all** of `old.outputs()`, so using it would mean
changing it, and inputs must keep their ordinals. The same backward closure already exists per-root
in `slice()` (`src/tape/slice.cpp`), which is why `ResidualProgram`'s bind failure has always caught
a dead column. `structural_pattern` does the equivalent walk with one bitset per residual, which
gives the full bipartite pattern instead of a single live/dead bit.

**`ir::sharing::reach(p, groups)` — UNSOUND for this question, measured.** `ir/program.hpp` says
"inputs: value id of every Input ordinal (**they are rows of the Input domain**)", so every unknown
is a row of one domain and a per-**domain** mask cannot separate unknown *j* from unknown *k*.
The tool prints what the route actually returns:

```
ir::reach route: 50 slice inputs occupy 1 domain(s); that one domain's mask has 25 of 25
residual bits set, so the route reports a DENSE pattern and cannot separate one unknown
from another: UNSOUND here
```

A dense pattern is a sound *superset*, so nothing would crash — the gate would simply never fire
and a colouring would report `colours = n_r` and look like an honest negative result. That is the
worst available failure for this analysis. `reach` is not wrong; it answers a per-domain question
correctly, and this is a per-ordinal question. It also caps at 32 groups, where `stage_a`'s joint
block has 70 residuals.

### 2b. Both pre-registered predictions were wrong, and the real mechanism was in neither

**The orchestrator's single-curve prediction is confirmed exactly**, including its reasoning:
`compare_ois` is a perfect dense lower triangle at both 16 and 25 tenors, density `(n+1)/2n`, column
0 present in every row, **colours = n_r and lane reduction 1.000x — not approximately, exactly.**
The same holds for `EUR-ESTR`, which is also a pure OIS strip.

**The orchestrator's multi-curve prediction attributes the prize to the wrong thing.** It expected
colours ≈ the largest per-curve block, bounding the prize near 4x on four curves. Measured: in
`Mode::sequential` — the mode everything ships in — there is **no cross-curve prize to win at all**,
because `CurveSet::calibrate` already records one `ImplicitBlock` per strongly connected component
and a later block reads an earlier block's unknowns as **parameters p, never as unknowns z**. Each
block's ∂F/∂z is confined to its own curve by construction. The per-curve block diagonality a
colouring would have to discover was extracted at recording time, by the dependency analysis.
`Mode::joint` is the only object where the question even arises, and there the answer is **2.000x,
not 4x** — and the two connected components (22 and 48) are exactly the coupling the orchestrator
named: USD stands alone, and the three EUR curves fuse into one component because the EURIBOR
curves discount off ESTR.

**This agent's prediction — 1.00x on every sequential block — is wrong on two of the four.**
`USD-SOFR` gives 1.571x and `EUR-EURIBOR-3M` gives 1.500x, for a reason neither prediction
contained: **instrument heterogeneity within a single curve**, not curve separation. The USD
pattern shows it directly:

```
    0 |#.....................| 1     rows 0-3   deposits: knots 0..i, a small dense triangle
    1 |##....................| 2
    2 |###...................| 3
    3 |####..................| 4
    4 |..###.................| 3     rows 4-11  the SR3 FUTURES STRIP: two bracketing knots each,
    5 |....##................| 2               pairwise DISJOINT -- this is the whole prize
    6 |.....##...............| 2
   ...
   11 |..........##..........| 2
   12 |##....##..###.........| 7     rows 12-21 the OIS swaps: the annuity touches the annual
   13 |##....##..####........| 8               payment dates, which land on a SUBSET of knots
   ...
   21 |##....##..############| 16
```

A futures residual depends only on the knots bracketing its own accrual period, so rows 5 and 7 are
structurally orthogonal and can share a seed lane. `compare_ois` has no such rows — every one of
its instruments is a par-quoted OIS whose annuity starts at the valuation date — which is why it
measures 1.000x and Stage A does not. **The colouring prize on this engine is a futures-strip
phenomenon.** `EUR-ESTR` (pure OIS) and `EUR-EURIBOR-6M` (12 instruments, no strip) both measure
1.000x, which is the control.

Secondary prediction, also wrong in detail: this agent expected "a near-triangle with a little fill
above the diagonal" from the futures. There is **no fill above the diagonal anywhere** — every block
of every fixture is exactly lower triangular. The futures rows are sparser *below* it, not wider
above it.

**And the pattern is a property of the instruments, not of the interpolation variable.**
`stage_a` (linear-**zero**) and `stage_a_h2h` (log-DF-**linear**) give byte-identical counts, block
for block, because both are local two-knot interpolants.

---

## 3. Sizing the prize

D92 measured where a residual Jacobian build actually sits:

- **87% of cold calibration**;
- **~0% of the warm path** — `JacobianPolicy::chord` builds none;
- **0.5% of a 256-row risk ladder** — `Factors` is built once per solve and reused across every
  seed, so the earlier 63.7% was a one-row artefact.

A colouring reduces the lane count of a Jacobian **build**. So whatever it saves lands on cold
calibration and essentially nowhere else, and it is bounded above by 87% × (1 − 1/reduction) of the
cold number. Any figure below is an **estimate** derived from D92's shares, not a measurement of a
built colouring — and D92, D104 and D105 each looked like a win from an operation count and measured
at zero or negative (D105's was the wrong sign).

---

### 3a. The decisive arithmetic

D92 priced the Jacobian **on `compare_ois` at 25 knots** — 25 batched reverse lanes, 229 us of a
262.7 us cold calibration, saving 2.0% of one 256-row ladder if it were *free*. And `compare_ois` at
25 knots is measured above at **1.000x: colouring removes exactly zero lanes from the workload D92
priced.** That is a measurement, not an estimate.

On Stage A's shipped `Mode::sequential` the reduction is 1.250x, which removes
`1 − 1/1.25 = 20%` of the Jacobian build. Carrying D92's 87% share across:

- ≈ **17.4% of cold calibration**, once per session;
- **0% of the warm path** — `JacobianPolicy::chord` builds no Jacobian at all;
- **0.1% of a 256-row risk ladder** — 20% of D92's 229 us is ~46 us against 11,250 us, so **0.41%**
  of one ladder, decaying as 1/N in the ladders a session runs.

Both of those are **estimates**, derived from D92's measured shares and this file's measured colour
counts, not measurements of a built colouring. The only figure here that is a measurement is the
zero.

---

## 4. Verdict: NO. Do not build the colouring.

Five reasons, in descending weight.

1. **On the workload the prize was priced against, the reduction is exactly 1.000x.** D92's 229 us
   is a `compare_ois` 25-knot number and that configuration is a perfect dense lower triangle.
   Colouring saves zero there, measured.
2. **Where it is non-trivial it is worth ~0.4% of one ladder, once, decaying as 1/N.** Stage A's
   1.250x lands on cold calibration and nowhere else, because the chord builds no Jacobian on the
   warm path and `Factors` is built once per solve and reused across every ladder seed.
3. **The 2.000x exists only in `Mode::joint`, which nothing ships** — and joint mode solves one
   70x70 system in place of four smaller ones, so the mode a colouring would most reward is the
   mode you would not choose for other reasons.
4. **The cost side is a new seeding path through `adjoint::Adjoint`, and it is the hazard class this
   repository keeps paying for.** Several residuals per lane means each Jacobian entry becomes an
   accumulation rather than a single write, and `0.0 + (−0.0)` is `+0.0` while `−0.0` is not:
   `a2p.drop_zero_start` and `adjoint.seed_input_pull` are both registered mutants that exist
   because of exactly that. It would need a bitwise gate and is therefore a §5.2 question for the
   owner, not a local optimisation.
5. **The prior is bad and it is measured, not felt.** D92 (87% share, built nothing), D104 and D105
   each looked like a win from an operation count; D105's measured with the **wrong sign**
   (+11–13% where 26% was expected).

The honest direction is built into the method: `colour_rows` is **greedy**, so its answer is an
upper bound on the chromatic number and therefore a **lower** bound on the saving. A better
colouring exists in principle. But the binding constraint is not the colour count — it is that
the Jacobian build is 0% of the warm path and 0.5% of a ladder, so even a perfect colouring on a
perfectly block-diagonal problem would be arguing over a few tenths of a percent of one ladder,
once per session.

**What the measurement is worth keeping for** is not the colouring. It is §2b's mechanism: the
engine's calibration Jacobian is a **lower triangle with a sparse futures strip in the middle**, the
structural pattern is **exactly** the numerical one on every shipped block, and the per-curve block
decomposition a sparse-AD paper would recommend discovering was already done at recording time by
`CurveSet`'s dependency analysis. If a future piece of work wants sparsity, that is the ground
truth it should start from, and `solver::structural_pattern` is where it now lives.
