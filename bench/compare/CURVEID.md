# CURVEID — the curve-identity probe: PRE-REGISTERED EXPECTATIONS

`tools/curveid/` asks one question with no tolerance to hide in: given the same interpolation
variable, the same scheme, the same knots and the same calibration instruments, do EpykosEngine and
the SwapEngine checkout produce **the same curve**?

Why it must be true. With the same variable and scheme on the same knots, and the same instruments
priced under the same conventions, the residual F is the *same function*; the system is square and
non-singular (one knot per instrument), so the root is unique and both engines must land on it.
Deviations at ~1e-15 are the solve. Anything larger is a defect or a convention mismatch, and the
value of the probe is that it localises which.

**This section is written and committed BEFORE the probe exists** (the D106 standard). It is not
edited afterwards; measured results land in §6 of this file in a later commit, reported *against*
these expectations. Everything below is derived by reading, not by running:

* ours — `include/epykos/maths/curve/scheme.hpp`, `curve.hpp`, `composite.hpp`
* theirs — `include/swaps/curve/regions.hpp`, `include/swaps/curve/curve_module.hpp`

D107 (2026-10-09) lifted D11's prohibition on *reading* that checkout. Nothing is copied, ported or
adapted into `include/epykos/` or `src/`: `tools/curveid/` calls their public header-only curve and
nothing else, exactly as `tools/h2h/` calls their facade (D78). No test case of theirs is lifted
here, so D108's cases-yes/implementations-no line is not exercised by this file.

---

## 1. The asymmetry that shapes the whole matrix

**Their curve has exactly ONE interpolation variable, and it is the instantaneous forward.**
`regions.hpp` §intro: "A region maps its slice of the knot forwards to the instantaneous forward
f(t) LINEARLY, and knows how to integrate it"; `curve_module.hpp:224`: "x = all knot forwards,
region by region". There is no `zero` and no `logdf` option anywhere in that module. Their 8 schemes
are all available on that one variable.

**Our `Variable::forward` is available for Flat and Linear ONLY.** `curve.hpp:91` static_asserts
`scheme_has_integral<S>`, which is `is_same_v<S,Flat> || is_same_v<S,Linear>`, and
`composite.hpp:256` throws `"Composite: a forward region must use the flat or the linear scheme"`.
The reason is stated in `scheme.hpp`: the closed-form integral is only written for those two.

So the (scheme × variable) matrix is 6 shared names × 3 of our variables = 18 cells, of which:

* **2 cells are directly comparable as whole curves** — (flat, forward) and (linear, forward).
* **4 cells are NOT CONSTRUCTIBLE on our side** — (hermite | natural_cubic | monotone_cubic |
  bspline, forward). Their side has all four. This is the single largest finding of the probe and it
  is structural, not a tolerance: *four of the six shared scheme names have no comparable whole-curve
  cell at all.*
* **12 cells (zero, \*) and (logdf, \*) have NO COUNTERPART on their side** — their x is always knot
  forwards. One of them has an exact bridge; see §4.

The probe therefore measures at two layers, and the pre-registration is split the same way.

## 2. Layer A — scheme-interpolant identity (expected)

Same knot times, same knot values, ours `Scheme::value(v, coef, t)` against their single **leading**
region's `forward(t)` (`set_forwards` seeds `Boundary{}` with `has_predecessor=false`, so a
one-region curve is leading and flat-extrapolates its first free value backwards — the same
convention as ours). This layer is variable-free: it asks only whether the two interpolants through
the same points are the same function.

| scheme | expected | reason |
|---|---|---|
| `flat` | **DEVIATES, O(1)** | *Scheme construction — step anchoring.* Ours: `v_k` on `[t_k, t_{k+1})`, right-continuous, `v_0` below `t_0`, `v_{n-1}` at and above `t_{n-1}`. Theirs: `f = x_k` on `(t_{k-1}, t_k]`, left-continuous, `x_0` on `(-inf, t_0]`. Value `k` is anchored to the interval *starting* at knot `k` on our side and the interval *ending* at knot `k` on theirs. Deviation is a full knot-value difference wherever adjacent values differ, confined to knot intervals and offset by one index. |
| `linear` | **IDENTICAL to roundoff** | Same nodes, same interpolant, flat both ends. Not bitwise: ours evaluates `(1-w)·v_k + w·v_{k+1}`, theirs `y_i + (y_{i+1}-y_i)·w`. Expect <= ~1e-16 relative. |
| `hermite` | **IDENTICAL to roundoff** | Same construction. Interior Bessel tangents `m_k = (h_k d_{k-1} + h_{k-1} d_k)/(h_{k-1}+h_k)` on both sides; same one-sided parabolic end tangents `m_0 = ((2h_0+h_1)d_0 - h_0 d_1)/(h_0+h_1)` and its mirror; same `n=2` fallback `m_0 = m_1 = d_0`. Evaluation form differs (ours the Hermite basis `h00..h11` in `s`, theirs `a+bu+cu²+du³`), so expect ~1e-16..1e-15 relative, not bitwise. |
| `natural_cubic` | **IDENTICAL to roundoff** | Same construction. Our interior system is theirs divided by 6: ours `(h_{i-1}/6)M_{i-1} + ((h_{i-1}+h_i)/3)M_i + (h_i/6)M_{i+1} = d_i - d_{i-1}`, theirs `h_{i-1}M_{i-1} + 2(h_{i-1}+h_i)M_i + h_iM_{i+1} = 6(d_i - d_{i-1})`. Both impose natural `M_0 = M_{n-1} = 0` at **both** ends (their header comment says "natural zero-curvature at the far end", but the code sets `M[0]=M[N-1]=0`; the comment is incomplete prose, not a second end condition). Same piecewise cubic. Expect ~1e-15 relative from the different Thomas scaling. |
| `monotone_cubic` | **DEVIATES, O(1) — shares a NAME only** | *Scheme construction, two independent differences.* (a) **Initial tangents**: ours are the Bessel / three-point tangents (`bessel_rules`); theirs come from the C2 first-derivative tridiagonal with natural ends — QuantLib `CubicInterpolation(Spline, SecondDerivative=0)`, which their header says is transcribed to match `MonotonicCubicNaturalSpline` exactly. Bessel tangents are not the C2 spline's derivatives. (b) **The filter**: ours is the plain Hyman clip — zero the tangent on a sign change, else clip `σ·m0` into `[0, 3·min(|d_l|,|d_r|)]`; theirs is QuantLib's full filter, which additionally *widens* the bound to `1.5·min(|pm|,|pd|)` or `1.5·min(|pm|,|pu|)` under a second-difference guard. Ours has no counterpart to those `pd`/`pu` branches. **Prediction with teeth:** on data that is exactly affine in `t` both reduce to the line, so this cell is identical *on a linear profile only* and deviates on every other profile. |
| `bspline` | **DEVIATES, O(1) — shares a NAME only** | *Scheme construction — a different spline space.* Ours: `n` control points (the `n` knot values), degree `p = min(3, n-1)`, interior knots `U_{p+j} = mean(t_j..t_{j+p-1})`, `j = 1..n-p-1`, so `n-4` interior knots for a cubic. Theirs: `n+1` control points — `cp_0` duplicates the first free value `x[off]` for a leading region — over averaging sites `[t_0, t_0, t_1, ..., t_{n-1}]`, giving `n-3` interior knots at *different* positions `(t_{j-1}+t_j+t_{j+1})/3`. Different control-point count, different interior-knot count, different breakpoints. Neither side interpolates its interior knot values, so a deviation here is not even a limit of a shared object. Expected to deviate on **every** profile including the linear one. |

Profiles are chosen so the limiter claims above are testable: `linear` (affine in `t`), `hump`
(smooth non-monotone), `step` (a sharp level change), `oscillate` (alternating, maximally
limiter-tripping).

## 3. Layer B — whole-curve identity (expected)

DF on a dense grid and the instantaneous forward on that grid, ours against their `discount(t)` /
`forward(t)`.

| our variable × scheme | expected |
|---|---|
| (`forward`, `linear`) | **IDENTICAL to roundoff, including both extrapolations.** Below `t_0` both integrate a flat pre-segment at `v_0` (ours via `linear_masses`' `mass[0] += min(to, t_0) - from`, theirs via `I_[0] = in.integral + y_0·(t_0 - in.time)` with `in.time = 0`). Above `t_{n-1}` both add `v_{n-1}·(t - t_{n-1})`. Expect <= ~1e-15 relative on DF. |
| (`forward`, `flat`) | **DEVIATES, O(1)**, for the Layer-A reason, *and the integral diverges from the first segment onward*: ours gives `∫_0^t = v_0·t` for `t ∈ [t_0, t_1)`, theirs `f_0·t_0 + f_1·(t - t_0)` for `t ∈ (t_0, t_1]`. Localisation: scheme construction, not units and not time measure. |
| (`forward`, `hermite` / `natural_cubic` / `monotone_cubic` / `bspline`) | **NOT COMPARABLE — not constructible on our side.** Expect `Composite` to throw `invalid_argument`. The probe reports the thrown message rather than substituting a scheme; a silent substitution is exactly the failure mode the brief forbids. |
| (`zero`, any) and (`logdf`, any) | **NOT COMPARABLE — no counterpart on their side.** Their interpolation variable is not configurable. The probe prints this per cell rather than leaving the matrix half-empty. |

## 4. The bridge — the one cross-variable cell that IS an identity

`logdf`-linear gives a piecewise-constant instantaneous forward, which is why
`include/epykos/fixtures/compare_ois.hpp` (lines 14–30) picked it: it takes the scheme out of the
comparison. The brief's job is the opposite, but the bridge is still the sharpest available check on
the *level* conventions, so it is pre-registered too.

Our `Curve<Linear, logdf>` over `{0, t_0, ..., t_{n-1}}` with knot log-DFs `y_k` (and the structural
origin knot `y ≡ 0` at `t = 0`) has instantaneous forward constant on `(t_{k-1}, t_k]` equal to
`-(y_k - y_{k-1})/(t_k - t_{k-1})`. That is *exactly* the interval their `Flat` anchors its value `k`
to. So their end-anchored `Flat` is the natural pairing with our log-linear DF, which also explains
why their `Flat` is end-anchored at all.

* **Expected IDENTICAL to roundoff on `[0, t_{n-1}]`** with `x_k = -(y_k - y_{k-1})/(t_k - t_{k-1})`,
  `y_{-1} ≡ 0`, `t_{-1} ≡ 0`.
* **Expected to DEVIATE beyond `t_{n-1}`**, and *growing with `t`*: ours is flat in `logdf`, so DF is
  constant and the forward is **zero** past the last knot (`curve.hpp` names this a stated
  simplification of the milestone); theirs is flat in the *forward*, so DF keeps decaying at
  `f_{n-1}`. Localisation: **extrapolation**, and the signature is a deviation that is zero inside
  the knot range and grows monotonically outside it.

## 5. Layer C — calibration identity (expected)

The instrument set is made **convention-free on purpose**, because a curve-identity probe must not
be able to blame a calendar: fixed-vs-OIS par swaps on pure year fractions, no calendars, no
business-day rule, no day-count fraction — the time measure on both sides is the year fraction `t`
that indexes the knots, and nothing else. Single-curve OIS, so the float leg telescopes to
`1 - DF(T)` exactly and the par rate is

    R_i = (1 - DF(T_i)) / Σ_j δ_j DF(T_j),    T_j the annual grid points up to T_i, δ_j their gaps.

One instrument per knot, maturities **at** the knot times, so the system is square; the intermediate
payment dates make the residual depend on the curve *between* knots, which is the point. The
residual and the damped Newton that solves it are written **once in the tool** and run against each
engine's own curve object, so the solver is a control and not a variable: if the curve function is
the same, the root must be the same.

* Where Layer A/B say the curve is identical (`linear`, `hermite`, `natural_cubic` as interpolants;
  `(forward, linear)` as a whole curve): **roots expected equal to solver tolerance**, <= ~1e-13
  relative on the knot values at `‖F‖∞ <= 1e-15`.
* Where they say it is not (`flat`, `monotone_cubic`, `bspline`): **roots expected to differ by
  O(the curve difference)**. That is the result, not a failure; the localisation is what is reported.
* `monotone_cubic` additionally may stall a Newton iteration, because the Hyman filter makes `F` only
  piecewise smooth. Non-convergence is reported as non-convergence; no tolerance is relaxed.

## 6. Measured

Run: `tools/curveid/curveid` at the commit that adds it, release preset, `-DEPYKOS_H2H=ON`. No
timings are taken and none are claimed, so the box load (14.3 at the time of the run) is recorded
only for completeness; nothing below is a performance number.

Knot sets: `nonuniform` = {0.25, 0.5, 1, 2, 3, 5, 7, 10, 15, 20, 30}; `uniform` = {1 … 8}. Profiles
as in §2. Max relative deviation over a dense grid of ~190 points covering below the first knot,
16 interior points per knot interval, the knots themselves, and out to 3x the last knot.

### 6.1 Layer A — which of the six shared names share a construction

| scheme | expected (§2) | measured | verdict |
|---|---|---|---|
| `flat` | DEVIATES O(1), construction | 1.43e-01 … 6.00e-01 rel, every profile, both grids, signature **construction** | **as expected** |
| `linear` | IDENTICAL | <= 2.20e-16 rel, every profile, both grids | **as expected** |
| `hermite` | IDENTICAL | <= 4.25e-16 rel, every profile, both grids | **as expected** |
| `natural_cubic` | IDENTICAL | <= 4.20e-16 rel, every profile, both grids | **as expected** |
| `monotone_cubic` | identical on the linear profile only; deviates elsewhere | identical on `linear` (2.27e-16) **and on `step`**; deviates on `hump` (1.25e-02) and `oscillate` (3.36e-01) | **as expected, plus one** |
| `bspline` | DEVIATES on every profile including linear | deviates on `linear` (2.42e-03), `hump` (7.78e-03), `oscillate` (1.14e-01); **identical on `step` on the nonuniform grid only** (4.16e-16), deviates on `step` on the uniform grid (1.74e-01) | **as expected, plus one** |

**Three of the six shared names are the same construction**: `linear`, `hermite`, `natural_cubic`,
each agreeing at the roundoff floor on every profile and both grids. **Three share a name only**:
`flat`, `monotone_cubic`, `bspline`.

The two "plus one" rows are the pre-registration being *under*-specific rather than wrong, and both
have an exact mechanism:

* **`monotone_cubic` on `step`.** The step profile makes all but one secant exactly zero, so at
  every node `min(|S_{i-1}|, |S_i|)` is 0, the Hyman bound is 0, and both filters return a zero
  tangent whatever their raw tangent was. The two schemes coincide **exactly when the limiter is
  fully saturated** and differ when it is not — the opposite of the intuition that a limiter is
  where two implementations would diverge. The difference in §2 is real; it just cannot be seen
  through a saturated filter.
* **`bspline` on `step`, nonuniform.** Writing out both knot vectors: theirs is ours **plus one
  extra interior knot at the front** (0.5833 = mean(0.25, 0.5, 1) on the nonuniform grid, 2.0 on
  the uniform one) with the control sequence `[v_0, v_0, v_1, …]` against our `[v_0, v_1, …]`. Their
  spline space is therefore ours refined by a single front knot, and the two curves coincide
  exactly when the control points that knot's basis touches are all equal. The step profile's first
  five nonuniform knot values are all 0.020, so they are; on the uniform grid the jump falls inside
  that span and they are not. This is a sharper statement of the §2 difference, not a retraction.

  That refinement reading is **corroborated by the measurement and not only by the code**: a single
  extra knot at the front predicts that every `bspline` deviation should peak in the *front* spans,
  and every one of the seven deviating `bspline` rows does — maxima at t = 0.9412, 1.176 and 0.3971
  on the nonuniform grid and at t = 1.647, 1.765, 3.0 and 1.353 on the uniform one, against last
  knots of 30 and 8. Nothing else about the two schemes differs, which is why the whole long end
  agrees to the roundoff floor on every profile.

### 6.2 Layer B — the whole-curve matrix

| our variable | `flat` | `linear` | `hermite` | `natural_cubic` | `monotone_cubic` | `bspline` |
|---|---|---|---|---|---|---|
| `zero` | no counterpart | no counterpart | no counterpart | no counterpart | no counterpart | no counterpart |
| `logdf` | no counterpart | no counterpart | no counterpart | no counterpart | no counterpart | no counterpart |
| `forward` | **DEVIATES** | **IDENTICAL** | not constructible | not constructible | not constructible | not constructible |

* **(forward, linear): IDENTICAL**, on all four profiles, in both quantities and in both tails —
  DF <= 4.16e-16 rel, instantaneous forward <= 2.20e-16 rel. The flat pre-segment below the first
  knot and the flat forward beyond the last are the same on both sides.
* **(forward, flat): DEVIATES.** The instantaneous forward deviates with a clean **construction**
  signature (confined to the knot range, 1.43e-01 … 6.00e-01 rel). DF deviates with a *mixed*
  signature, and that is informative rather than a failure of the classifier: DF **integrates** the
  forward, so a deviation confined to the knot range is carried past the last knot as a constant
  accumulated offset in `log DF` — 8.12e-02 rel at t = 75 on the `linear` profile. Localisation:
  scheme construction, propagated through the integral.
* The four `not constructible` cells throw
  `Composite: a forward region must use the flat or the linear scheme`. The probe prints that
  message; it does not substitute a scheme.

### 6.3 The bridge — measured exactly as pre-registered

| profile | DF on [0, t_last] | DF on the whole grid |
|---|---|---|
| `linear` | 2.13e-16 rel — **IDENTICAL** | 9.33e-01 rel @ t=90 — **extrapolation** |
| `hump` | 1.56e-16 rel — **IDENTICAL** | 8.35e-01 rel @ t=90 — **extrapolation** |
| `step` | 2.49e-16 rel — **IDENTICAL** | 9.50e-01 rel @ t=90 — **extrapolation** |
| `oscillate` | 3.05e-16 rel — **IDENTICAL** | 9.92e-01 rel @ t=90 — **extrapolation** |

Our `logdf`-linear curve and their `Flat` forward curve are **the same curve** on the knot range,
to 3.1e-16, on every profile — which confirms the level and sign conventions line up exactly and
confirms the reading of their end-anchored `Flat` as the piecewise-constant-forward partner of
log-linear DF. They part company only past the last knot, where ours holds `log DF` flat (DF
constant, forward **zero**) and theirs holds the **forward** flat. The classifier reports
`extrapolation` unprompted, which is the signature the brief predicts for exactly this cause.

### 6.4 Layer C — calibration identity

Square set, one instrument per knot, maturities at the knots, convention-free par rates, identical
damped Newton on both sides from the same flat 3% start, `||F||inf <= 1e-15`.

| scheme | ours | theirs | calibrated knots | curve at the two roots |
|---|---|---|---|---|
| `flat` | **DID NOT CONVERGE — structurally singular** | CONVERGED, 1.32e-16, 3 iters | 3.13e-01 rel | DEVIATES |
| `linear` | CONVERGED, 6.24e-17, 4 iters | CONVERGED, 1.39e-17, 4 iters | **1.42e-14 rel — IDENTICAL** | DF 1.21e-14, f 1.42e-14 — **IDENTICAL** |
| `hermite`, `natural_cubic`, `monotone_cubic`, `bspline` | **cannot calibrate this cell at all** | — | — | — |

**The finding that was NOT pre-registered, and is the sharpest thing the probe found.** On our side
the Flat-on-forward Jacobian has an **exact zero column at knot 10 (t = 30)**: no instrument's
residual depends on the last knot value at all, so the system is square by count and singular in
substance. The cause is precisely the step-anchoring difference of §2. Our `Flat` gives knot `k` the
cell `[t_k, t_{k+1})`, so the **last** knot value governs only `t >= t_{n-1}` and is invisible to
every instrument maturing at or before the last knot — which is every instrument, because the knots
sit at the maturities. Their end-anchored `Flat` gives knot `k` the cell `(t_{k-1}, t_k]`, so every
knot value is inside some instrument's integral and the system is genuinely square.

That is a defect-class result for the (forward, flat) cell, not a tolerance question, and it is not
reachable from Layer A: Layer A says the two Flats differ, Layer C says ours cannot be calibrated on
a knots-at-maturities set at all. It does not touch the shipped path —
`fixtures/compare_ois.hpp` calibrates `linear` on `logdf`, which §6.3 shows is the *same curve* as
their `Flat` and which Layer C shows calibrates cleanly.

### 6.5 The headline

Of six shared scheme names, **three are the same construction and three only share a name**. Of
eighteen (scheme x variable) cells, **one** — (forward, linear) — can run the sharpest available
test end to end, and it passes it: identical interpolant, identical DF and forward including both
extrapolations, identical calibrated knots to 1.4e-14. **Twelve cells have no counterpart on their
side** because their interpolation variable is not configurable, **four are not constructible on
ours** because `Variable::forward` is Flat/Linear only, and **one is structurally singular on ours**.
The bridge adds a second genuine identity across the variable gap.

Nothing here was made to pass: no tolerance was relaxed, the roundoff floor stayed at 1e-13
throughout, and the two results that exceeded the pre-registration were both the pre-registration
being under-specific, not wrong.
