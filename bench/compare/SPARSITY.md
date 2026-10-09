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

Filled by `tools/sparsity/`. Counts and patterns only: nothing here is a timing, so no reserved box
and no load gate (`tools/coverage/`'s position, for the same reason).

<!-- MEASURED -->

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

## 4. Verdict

See §2's measured table. The honest direction is built into the method: `colour_rows` is **greedy**,
so its answer is an upper bound on the chromatic number and therefore a **lower** bound on the
saving — a "no" from a greedy colouring is weaker evidence than a "yes" would be, and is reported
as such.
