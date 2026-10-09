# The exotics kill test — the criterion, written down BEFORE the measurement

**Commissioned 2026-10-09.** This file states what counts as pass and what as fail. It is committed
**before any number is taken**, so that the criterion cannot be fitted to the result. The git history
of this file is the evidence of that; the results section is appended afterwards and says so.

## The question

**D1 ("No JIT", 2026-09-22)** carries its own revisit clause: *"Revisit only if M1/M3 show the
catalogue + interpreter cannot reach the gates."* M1 and M3 are interest-rate swaps — affine,
shallow, heavily shared. Every number this project quotes for the back half (D81's 2,565x collapse,
D90's ~48x pricing) was measured there.

Nothing in the engine has ever recorded a **path-dependent payoff**: `include/epykos/mc/` is empty
and no fixture carries a path grid. So D1's revisit clause has never been tested on the domain where
the catalogue + interpreter is most likely to fail — a scripted payoff over a path grid, where the
structure is deep, the sharing is low, and the branches are per-path and data-dependent.

## The criterion

### The deciding measurement: engine against its own templated `double`

`exec::Interpreter::run` on the pinned, inferred program against a straight `double` evaluation of
the **same** `Scalar` template (D3), one state each (B = 1), same build (`release`), same box, same
clock, on **every** point of a (paths × steps) sweep. On the rates book the engine is ~48x *faster*
than a specialist on pricing (D90); against its own naive `double` path it must at the very least
not lose.

| ratio (engine / double, >1 = engine faster) | verdict |
|---|---|
| **≥ 1.0x at every swept point** | **PASS** — D1 holds for this domain. The catalogue + interpreter reaches the gate on a path-dependent payoff. |
| **0.8x – 1.0x anywhere** | **MARGINAL** — D1 is not refuted, but the back half only reaches parity with naive scalar C++ on the workload the design targets (§6). Not a validation. |
| **< 0.8x anywhere** | **FAIL** — D1's revisit clause has fired. A payoff language on this op set needs a JIT below the pin before it is built. |

The ratio is taken at B = 1 because that is the apples-to-apples comparison: one state of work on
each side. B = 64 is reported alongside as the amortised figure, and is **not** the criterion — the
`double` path has no batch axis, so a batched comparison measures a different thing.

### Diagnostic, and explicitly NOT independently deciding

These two interpret the deciding number; they do not override it. Thresholds are stated anyway so
they are not fitted afterwards.

**1. Catalogue coverage** (`catalogue::Coverage`, `profile` preset only for `time_fraction`):

| `time_fraction` | reading |
|---|---|
| ≥ 0.25 | the AOT catalogue reaches this domain |
| 0.10 – 0.25 | it reaches it weakly |
| < 0.10 | the catalogue does not serve this domain, which is D1's specific mechanism |

**2. Interpreter share** — the complement of the above. `row_fraction` is the always-available
proxy (coverage.hpp) and is reported from the `release` build; `time_fraction` only from `profile`.

**Never mix the two builds.** Coverage and `time_fraction` come from `profile`; **every** timing
comes from `release`. No time from a `profile` build is reported anywhere (`CLAUDE.md` Build).

### Predictions recorded in advance

Stated now so that the diagnostic results can be scored rather than narrated:

* **Collapse.** D81's telescope is `mul(div(p,q), div(q,s)) → div(p,s)`, which needs consecutive
  *ratios*. The path recurrence `S_k = S_{k-1}·exp(c_k + v·ε_k)` is a product of independent
  factors with no shared denominators, so the rule **cannot match**. Predicted: little or no
  collapse beyond CSE of the per-step drift and vol constants, i.e. a ratio near **1.0x**, against
  2,565x on `compare_ois`. If it collapses more than ~1.5x, this prediction was wrong and that is
  the finding.
* **Scan fraction.** Predicted the large majority of rows are scan rows, and the scan is therefore
  the program — which is the opposite of the rates fixtures, where the scans collapsed away.
* **Recording discipline.** Predicted the payoff wants a C++ `if` in at least three places (the
  call payoff's `max(·,0)`, the running extremum, the barrier's survival test) and that all three
  are expressible as `select`. The running extremum is the one at risk, because it puts a `Select`
  *inside a recurrence*.

### The independent correctness check, without a dependency (D12)

D2 makes the engine's own templated-`double` path the oracle. For a path-dependent payoff that is
circular — a bug in the path machinery moves both sides together. So:

A **geometric** Asian call on a discretely-monitored GBM has an exact closed form, because
`ln G = (1/n)Σ ln S_{t_i}` is exactly normal when the path is simulated with exact log increments
(which it is here — log-Euler with exact Gaussian increments is the exact GBM law, so there is
**no discretisation bias to acknowledge**):

    ln G ~ N(mu_G, s_G^2),   mu_G = ln S0 + (r − q − sigma^2/2)·tbar,   tbar = (1/n)·sum t_i
    s_G^2 = (sigma^2/n^2)·sum_i sum_j min(t_i, t_j)
          = sigma^2·dt·n(n+1)(2n+1)/(6 n^2)     for t_i = i·dt
    price = exp(−rT)·[ exp(mu_G + s_G^2/2)·Phi(d+) − X·Phi(d−) ],
            d− = (mu_G − ln X)/s_G,  d+ = d− + s_G

Standard library only: `Phi` is `0.5·erfc(−x/sqrt(2))`.

**Criterion:** the Monte Carlo estimate must agree with the closed form within **3 Monte Carlo
standard errors** of the estimate. That is the only defensible threshold for an MC estimate; a
tighter one would be fitting to noise and a looser one would not catch a bug. The standard error
is computed from the same sample and reported, so the reader can see the width of the test.
Reported as a **z-score**, with the relative difference alongside for scale.

**The barrier has no comparable free check and that is stated, not hidden.** The reflection-principle
closed form prices a *continuously* monitored barrier; this payoff is monitored at the `n` fixing
dates, and the discrete-monitoring bias is first-order in the step — of the same size as the effect
being measured, not a rounding. Quoting the continuous formula as the barrier's oracle would be
quoting a different instrument. What the barrier gets instead: the geometric-Asian check validates
the **shared** machinery (the path recurrence, the inputs, the reduction, the discounting) and the
barrier shares all of it; what it does not validate is the running extremum and the survival
`select`, and those are carried by path-to-path agreement only. Said plainly: the barrier's payoff
logic has **no external oracle here**.

### Rules held to

* No tolerance relaxed, no test skipped, no gate weakened. `ctest --preset release` green, count as
  seen.
* Every timing on a reserved box: 1-minute load read and stated before and after each figure, and
  refused above cores/2 = 8.0, which is `bench/run.sh`'s own rule (D29).
* Estimates labelled as estimates (§4).
* D11: nothing read, copied or ported from the SwapEngine checkout. The payoff maths and the
  geometric closed form are standard Black–Scholes results.

---

## Results

Appended after the fact. See the section below, added in a later commit than the criterion above.
