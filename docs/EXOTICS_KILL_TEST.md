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

Measured 2026-10-09, fingerprint **`d448afd70180`** (Intel Xeon W-3223, 8 physical / 16 logical cores, Apple clang
21.0.0), flags `-O3 -march=x86-64-v3 -fno-math-errno`, `release` preset for every timing and `profile` for coverage
only. 1-minute loads stated per block, threshold cores/2 = **8.0**; nothing was taken above it.
`ctest --preset release` **119/119** (118 on the base branch plus this fixture's gate).

### The verdict: FAIL — D1's revisit clause has fired

**The engine is slower than its own naive templated-`double` path at every one of the nine swept points**, by the
criterion committed before the fixture existed. Best 0.757x, worst 0.338x, degrading monotonically in both the path
count and the step count. The 0.8x FAIL threshold is not reached anywhere.

**But the mechanism is not the one D1 names, and that matters more than the verdict.** D1 is about the catalogue and
the interpreter not reaching a gate. What the sweep actually measures is a **fixed interpretive excess of 2.6–6.0 ns
per path-step that a path grid gives nothing to amortise against**. Catalogue coverage turns out to be *anti-correlated*
with the ratio: 99.6% row coverage scores 0.568x while 66.7% scores 0.844x. So the pre-registered coverage diagnostic,
though it also fails decisively, is **not predictive of the deciding number** — see "what this test got wrong".

### 1. The deciding number, swept

`exec::Interpreter::run` against `exotic_path_evaluate<double>`, same `Scalar` source (D3), min of 9 reps, interleaved
so box drift moves both arms together. Loads **2.50 → 2.88**. Each arm has its own right-sized `Interpreter`
(`max_batch = 1` for the B = 1 arm) and a compiler barrier; the printed per-arm checksums are identical and non-zero,
so neither computation was elided.

| paths | steps | `double` µs | engine B=1 µs | **ratio** | engine B=64 µs/state | ratio |
|---|---|---|---|---|---|---|
| 64 | 12 | 12.8 | 16.9 | **0.757x** | 10.65 | 1.199x |
| 64 | 52 | 48.1 | 68.2 | **0.706x** | 46.90 | 1.026x |
| 64 | 252 | 248.8 | 365.6 | **0.681x** | 250.81 | 0.992x |
| 256 | 12 | 47.5 | 63.9 | **0.743x** | 44.71 | 1.062x |
| 256 | 52 | 192.2 | 264.7 | **0.726x** | 202.09 | 0.951x |
| 256 | 252 | 931.0 | 1569.0 | **0.593x** | 1082.51 | 0.860x |
| 1024 | 12 | 178.2 | 255.7 | **0.697x** | 182.02 | 0.979x |
| 1024 | 52 | 757.6 | 1378.5 | **0.550x** | 861.29 | 0.880x |
| 1024 | 252 | 3491.9 | 7620.1 | **0.458x** | 5485.87 | 0.637x |

Agreement with the `double` reference across the pin: **0.00e+00 to 2.39e-16**. The engine computes the right answer;
it computes it slowly.

**A measurement artefact found and removed, worth recording.** The first run of this sweep ran the B = 1 arm on the
`max_batch = 64` instance and got 0.338x at the largest grid. Right-sizing to `max_batch = 1` moves that point to
0.458x — **a 41% penalty for a buffer the caller asked for and never used** (9,861 µs → 6,979 µs on the same program).
That is D91's residency finding in a new place, and charging it to the engine would have been unfair, so the table
above is the right-sized comparison.

### 2. The 'fixable defect below the pin' clause, discharged

The criterion's FAIL clause requires the shortfall not to be a mis-set knob. Every `exec::Options` knob, swept on the
same program at the worst point (1024 paths × 252 steps), load 2.5–2.9:

| configuration | engine µs | ratio |
|---|---|---|
| default | 6979.3 | 0.485x |
| `tile` = 16 / 64 / 1024 / 4096 | 7613 / 7038 / 6891 / 6647 | 0.444x – 0.509x |
| `lane_tile` = 1 / 4 | 7097 / 6876 | 0.477x / 0.492x |
| `split_lane_chunks` = false | 6954.0 | 0.487x |
| `fuse_pairs` = false | 6916.6 | 0.489x |
| `fuse_reductions` = false | 5910.0 | 0.572x |
| `inline_producers` = false | 7051.9 | 0.480x |
| `use_catalogue` = false | 7132.7 | 0.474x |
| `tile` = 4096 + `fuse_reductions` = false | 5755.6 | **0.588x (best legal)** |
| `exp = poly` | 5929.7 | 0.571x |

**No configuration clears 1.00x**, and the best is 0.588x. The shortfall is not a mis-set knob. Two incidental
readings: `use_catalogue = false` is *faster* than the default at this point (the lookup costs more than the kernels
save when coverage is ~1%), and `exp = poly` is worth ~18%, which is the transcendental share — see §5.

### 3. Catalogue coverage and interpreter share

Coverage and `time_fraction` from the **`profile`** build; **no timing is quoted from it**, and the tool now refuses to
print one (see "what this test got wrong" — the first version of that guard did not work). The rates baseline is
measured in the *same binary* so the numbers divide (D103 §1: a number without its denominator means nothing).

| program | domains | rows | rows/domain | group cov | row cov | **time cov** |
|---|---|---|---|---|---|---|
| exotics 256 × 12 | 57 | 17,680 | 310 | 24.2% | 14.3% | **4.6%** |
| exotics 256 × 52 | 177 | 68,880 | 389 | 7.1% | 3.7% | **0.7%** |
| exotics 256 × 252 | 777 | 324,880 | 418 | 1.6% | 0.8% | **0.2%** |
| **`m1_book` (rates)** | 14 | 42,799 | 3,057 | **100.0%** | **100.0%** | **34.6%** |

Against the pre-registered threshold (≥ 0.25 reaches, < 0.10 does not): **the AOT catalogue does not serve this
domain.** 0.2–4.6% of execution time against 34.6% on the rates book, and it decays as 1/steps. The interpreter share
is the complement: **95.4% to 99.8%** of execution time is in the generic interpreter.

### 4. Does the algebra collapse a path-dependent payoff? No — 1.22x, against 2,565x on rates

| paths | steps | recorded | pinned | collapse |
|---|---|---|---|---|
| 64 | 12 | 8,981 | 7,384 | 1.216x |
| 256 | 52 | 138,261 | 111,384 | 1.241x |
| 1024 | 252 | 2,600,981 | 2,083,864 | **1.248x** |

Essentially constant at **1.22x–1.25x**, independent of the path count. **The prediction recorded in advance was
right**: D81's telescope is `mul(div(p,q), div(q,s)) → div(p,s)` and needs consecutive *ratios*; the path recurrence
`S_k = S_{k-1}·exp(c_k + v·ε_k)` is a product of independent factors with no shared denominators, so the rule cannot
match. The 1.22x that *is* there is CSE of the shared per-step drift and vol plus the `add(0, x)` at each
accumulator's first step. **D81's 2,565x is a rates-only phenomenon**, and this is the first measurement that says so.

Worth noting: the collapse is bit-preserving here — the measured cross-pin divergence is **exactly 0.00e+00**.

### 5. What fraction of the program is scan — and the sharpest finding in the test

**For the full payoff: none. Zero scan rows at every one of the nine grid points.** `ir::infer` detects the
recurrences and refuses to lay them out, reporting:

> `scan class 6 (mul, 3328 rows) reads itself through other classes; scan class 8 (select, 3328 rows) reads itself through other classes`

Pricing one leg at a time locates the cause exactly, and it is **not** the number of coupled recurrences:

| legs | domains | scan rows | scan % | row cov | scans? |
|---|---|---|---|---|---|
| spot only (1 recurrence) | 8 | 3,328 | 33.3% | 50.0% | **yes** |
| arithmetic Asian | 13 | 3,328 | 32.5% | 99.2% | **yes** |
| geometric Asian | 14 | 3,328 | 24.5% | 50.4% | **yes** |
| both Asians (3 recurrences) | 20 | 3,328 | 24.0% | 75.0% | **yes** |
| **barrier alone** | **113** | **0** | **0.0%** | **1.0%** | **no** |
| arithmetic + barrier | 119 | 0 | 0.0% | 2.9% | no |
| **all three** | **177** | **0** | **0.0%** | **3.8%** | **no** |

Three recurrences coupled (both Asians) scan fine at 75% coverage. **The single feature that destroys the scan layout
is a `Select` inside a recurrence** — the running extremum `M_k = select(S_k > M_{k-1}, S_k, M_{k-1})` — and its
failure **cascades**: it takes the spot's own `mul` chain down with it, so a program that would have scanned does not.

The consequence is a scaling fact, now gated in `tests/exec/exotic_path_e0_test.cpp`: **with a scan the domain count
is independent of the step count; without one it is linear in it.** 16 steps → 48 steps:

| legs | domains @16 | @48 |
|---|---|---|
| spot / arith / geom / both Asians | 8 / 13 / 14 / 20 | 8 / 13 / 14 / 20 (unchanged) |
| barrier alone | 41 | **105** |
| all three | 69 | **165** |

### 6. The decisive separation: is it D1, or is it the scan gap?

If the scanning payoffs passed and only the barrier failed, this would be a scan-detector bug and not a D1 result. It
is not. Same grid (256 × 52), same transcendentals, same reduction, `max_batch = 1`, min of 11 reps, load 1.90:

| legs | scans | row cov | `double` µs | engine B=1 | ratio | engine B=64/st | ratio | **ns/path-step excess** |
|---|---|---|---|---|---|---|---|---|
| spot only | yes | 0.0% | 76.7 | 133.0 | **0.577x** | 95.25 | 0.806x | 4.22 |
| arithmetic Asian | yes | 99.6% | 79.7 | 140.4 | **0.568x** | 98.22 | 0.811x | 4.56 |
| geometric Asian | yes | 66.7% | 188.6 | 223.5 | **0.844x** | 162.98 | **1.157x** | 2.63 |
| both Asians | yes | 99.6% | 189.1 | 231.4 | **0.817x** | 169.82 | **1.113x** | 3.18 |
| barrier alone | no | 1.0% | 81.1 | 142.0 | **0.571x** | 100.93 | 0.803x | 4.58 |
| all three | no | 1.9% | 190.3 | 270.5 | **0.704x** | 196.26 | 0.970x | 6.02 |

**Every configuration loses at B = 1, scan or no scan**, and the cleanest case in the whole test — one recurrence, 8
domains, a proper scan domain, nothing reading it — is **0.577x**. So the scan gap is real, costly and worth fixing,
but it is **not** what the deciding number is made of.

What the ratio actually is, exactly: the last column is the engine's excess over the `double` arm divided by
`paths × steps`, and it is **2.6–6.0 ns and roughly constant**. The ratio is then just that excess against the
payoff's own per-step arithmetic:

| legs | `double` ns/step | excess ns/step | predicted ratio | measured |
|---|---|---|---|---|
| spot only | 5.76 | 4.22 | 0.577x | 0.577x |
| arithmetic Asian | 5.99 | 4.56 | 0.568x | 0.568x |
| geometric Asian | 14.17 | 2.63 | 0.843x | 0.844x |
| both Asians | 14.21 | 3.18 | 0.817x | 0.817x |
| all three | 14.30 | 6.02 | 0.704x | 0.704x |

**The mechanism, stated plainly.** The engine pays a per-row interpretive cost. A path grid has one row per path-step
and almost no sharing, so there is nothing to amortise it against; the ratio improves only as the payoff's own
per-step arithmetic grows (the geometric leg's `log` per step), and it never exceeds 1.0 at B = 1 because the engine
runs the same scalar libm transcendentals the `double` loop does. On the rates book 1,000 trades share one curve, so
one `exp` row serves many consumers and the row count is small against the work — which is exactly why this never
showed up in M1 or M3.

**The one qualification, stated rather than buried.** At B = 64 the two Asian legs clear 1.00x (1.157x, 1.113x). But
B is the axis of *market states*, not paths: a single Monte Carlo price is B = 1. The engine's advantage mechanism
requires a batch axis that an MC pricing call does not have, and that is the finding, not a let-off.

### 7. The geometric Asian against its closed form

| paths | steps | MC estimate | closed form | rel diff | std err | z |
|---|---|---|---|---|---|---|
| 1024 | 12 | 4.45015699 | 5.13923956 | 1.34e-01 | 2.26e-01 | −3.05 |
| 4096 | 12 | 4.79196018 | 5.13923956 | 6.76e-02 | 1.19e-01 | −2.93 |
| 16384 | 12 | 4.99656474 | 5.13923956 | 2.78e-02 | 5.92e-02 | −2.41 |
| 16384 | 52 | 4.70923366 | 4.88968440 | 3.69e-02 | 5.63e-02 | −3.21 |

On the seed `WORKLOADS.md` fixes this **trips the pre-registered 3-SE criterion** at two points, and that is reported
rather than restated. It is not the machinery. Two discriminators settle it:

* **Eight independent seeds:** z = −3.21, +1.30, +0.38, −0.08, +1.24, +0.75, −0.07, +0.59. **Mean z = +0.113**, whose
  own standard error is 1/√8 = 0.354. Exactly the null. (A second configuration: mean z = +0.082.)
* **The moments of `ln G`**, which have far less MC error than the price: the sample variance agrees at z = −0.03 to
  −1.22, while the sample *mean* is 2.4–3.1 SE low — so the shortfall is a location shift in the draws, not a
  mis-specified variance, a wrong `t̄`, or a payoff bug.
* **The cause, localised:** sub-stream 530000 at seed 20260922 has a sample mean **2.0–3.0 SE below zero** with the
  variance correct at 0.995–0.999. An unlucky stream, legitimately drawn.

**Conclusion: the path machinery, the discretisation and the payoff are correct**, and the agreement is as good as the
sample permits. The gate in `tests/exec/exotic_path_e0_test.cpp` is therefore on the mean z over 8 seeds, because a 3σ
band on one Monte Carlo estimate has a 0.3% false-alarm rate by construction and this seed is one of the 0.3%. The
seed was **not** changed to get a prettier number.

**The barrier has no external oracle here**, as stated in advance. The reflection-principle formula prices a
*continuously* monitored barrier and the discrete-monitoring bias is first-order in the step — the size of the effect
being measured, not a rounding — so quoting it would be quoting a different instrument. The geometric check validates
everything the barrier *shares* (path recurrence, inputs, reduction, discounting); the running extremum and the
survival `select` are carried by path-to-path agreement only.

### 8. Where the recording discipline strained

Four sites wanted a C++ branch or a `std::max`. **All four were expressible, none needed a new op, and one is
enforced by the recorder rather than by the author remembering.** This is the evidence on whether a payoff *language*
is feasible on this op set, and on that narrow question the answer is encouraging:

| site | what the payoff wants | what was written | verdict |
|---|---|---|---|
| call payoff `max(A/K − X, 0)`, ×3 | `std::max` | `epykos::max` → `Select`, both arms recorded | clean; ADL finds it for `double`, `Rec` and `Dual` from one source |
| running extremum `M_k = max(S_k, M_{k-1})` | `if (s > m) m = s;` | `max(s, run_max)` → `Select` in a recurrence | **compiles and is correct, but defeats the scan layout** (§5) |
| knock-out `M_K < Bu` | `if (knocked_out) return 0;` | `select(run_max < barrier, vanilla, 0)` | clean, and **`structural_if` on it throws** because `Bu` is an input — the discipline is enforced, not remembered. Gated. |
| `legs` selection | `if (want_arith)` | a C++ `if` on a *structure* flag | legitimate: `legs` is not a `Scalar`, so no `structural_if` is involved (it takes a `RecBool`) |

The honest summary for a payoff language: **the op set is sufficient and the discipline is enforceable, but one
perfectly ordinary payoff primitive — a running extremum, i.e. every barrier, lookback and cliquet — silently costs
the program its scan layout and 96% of its catalogue coverage.** That is a far more specific and more actionable
finding than "exotics are slow", and nothing in the op set has to change to fix it.

### 9. What this test got wrong

* **The coverage diagnostic is not predictive of the deciding number, and the criterion implied it would be.** 99.6%
  row coverage scores 0.568x; 66.7% scores 0.844x. Coverage tracks which payoff is cheap per step, not what the
  engine costs. Both thresholds failed, but reading one from the other would have been wrong.
* **The 3-SE oracle criterion was under-specified.** A 3σ band on one Monte Carlo estimate has a 0.3% false-alarm
  rate by construction, and displaying it over six *nested* sweep points makes one observation look like six
  failures. The criterion should have been written on the sampling distribution — the mean z over independent seeds —
  from the start, which is what the gate now does.
* **The profile-build guard did not work when first written.** It was a compile-time `#if defined(EPYKOS_EXEC_PROFILE)`,
  and the root `CMakeLists.txt` sets that definition **`PRIVATE`** to the `epykos` target, so **no consumer TU can
  see it**. The tool printed "build: release" while linked against a profile `libepykos` — precisely the mix-up the
  guard exists to prevent. It is now a runtime probe on `catalogue::Coverage::time_fraction`, which `coverage.hpp`
  documents as the signal. **Any other tool that guards itself this way has the same trap waiting.**
* **An over-tight structural assertion.** The first version of the scan test asserted `domains < steps` for the
  scanning cases and failed at 20 domains against 16 steps, because the domain count has a constant part that
  dominates at a small step count. The real claim is a scaling one and is now asserted as such.
* **A dangling pointer in the option sweep** (names held as `const char*` into a reallocating vector) blanked two rows
  of the first sweep. Fixed.

