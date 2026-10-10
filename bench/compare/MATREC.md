# Materialise vs recompute: does reader count predict the right choice, at both ends of the sharing spectrum?

`tools/matrec/` (counts, refuses no box) and `tools/matprobe/` (times, refuses a loaded box) emit
everything below. Fingerprint `d448afd70180`, release preset, `-O3 -march=x86-64-v3
-fno-math-errno`, Apple clang 21.0.0, 16 logical cores, load threshold 8.0; loads stated per table.

**The one-line answer: no — and that is a theorem, not a weak correlation. Reader count is what
`ir::infer` already SPENT to construct the two classes, so inside either one it is a constant. What
does predict the winner, at both ends, is the recomputed cone's transcendental content, which the
engine also already holds; it needs ONE coarse machine constant, and a per-step arm would need only
the ordering `transcendental > copy > flop`. Residency was tested as the alternative explanation by
forced eviction and ruled out. See §5 for the verdict and what it does not license.**

---

## 0. What the decision actually is, and the brief this answers was wrong about where it lives

The brief that commissioned this said the engine makes the choice in "two contradictory places,
each a human judgement in a header comment, neither derived". All three clauses are wrong, and
establishing that changed the question.

**There is one policy, stated in three places that agree, and it is derived in D31 item 1** with a
cost model and a pre-registered revisit condition ("Not a checkpointing scheme: at MC scale (M6)
this changes" — which is precisely D106's path grid).

| cited as | what it actually says |
|---|---|
| `src/adjoint/adjoint_e0.cpp:482` | about **fuse/inline**: this file applies none of `exec::Interpreter`'s, so a catalogued kernel is an unconditional drop-in. A statement about domain ROWS existing in the value buffer at all, not about intra-chain steps. |
| `include/epykos/adjoint/adjoint_to_program.hpp` | "**Q's forward half** … intermediates are MATERIALISED rather than recomputed **per target**". The "~44k steps" is the emitted *step count* of Q, and the reason is that a Program's domains are addressable so "recompute per target" duplicates a domain per consumer. Not a runtime performance judgement, and not about the reverse pass. |
| `include/epykos/adjoint/plan.hpp:30-36` | the actual policy, citing D31: "the forward pass stores every row value… Intermediate steps of a group are not stored". `adjoint.hpp` says the same. |

So the two quoted comments are about different objects and do not contradict each other. What the
split really is:

* `ir::infer` makes a tape node a **domain value** — addressable in the value buffer — exactly when
  it is structural, is a segment member or an output, or **has a use count other than one**
  (`src/ir/signature.cpp`'s `initial_boundaries`: `boundary_[i] = (structural || use_count_[i] != 1
  || member_use_[i])`). Everything else is folded into a group's chain.
* A group's **last** step is therefore always materialised: other domains address it.
* Steps **0 .. last-1** are the only open question, and they are what
  `adjoint::Options::materialise_steps` flips.

---

## 1. Why reader count cannot predict it — a theorem, not a correlation

An intra-chain step has a use count of **exactly one**, by the boundary rule above. Measured, on
every fixture, the in-group reader count of the always-recomputed class is the constant 1:

| fixture | intra-chain step instances | exactly one in-group reader |
|---|---|---|
| `compare_ois`(256) | 3,029 | **3,029 — 100.00%** |
| `stage_a` | 32,602 | **32,602 — 100.00%** |
| `exotic_path`(256×52) | 29,188 | **29,188 — 100.00%** |
| `exotic_path`(1024×252) | 526,340 | **526,340 — 100.00%** |
| `exotic_path`(64×12) | 2,180 | **2,180 — 100.00%** |

**Reader count is what CONSTRUCTS the partition, so inside either class it carries no information
about the remaining choice.** On the materialised side the choice is not open either — a value with
two readers, or one used as a segment member, must have an address. The signal the engine already
holds (`AdjointPlan`'s four "who reads me" CSR lists) is indexed by value id, so it is *defined
only on the class that is unconditionally materialised* and is silent on the other.

This is the same shape as D102 §3's observation that "a value with exactly one reader needs no pull
at all, and `plan.cpp` already has the reader count" — but one level down, and with the opposite
conclusion: there the count discriminated, here it cannot, because it was already spent.

For completeness the materialised class's distribution, which is not constant but governs no
choice (modal share, 93.61% / 60.13% at one reader on `compare_ois`(256) / `exotic_path`(1024×252)):

| fixture | p50 | p90 | p99 | max | modal |
|---|---|---|---|---|---|
| `compare_ois`(256) | 1 | 1 | 20 | 514 | **1 at 93.61%** |
| `stage_a` | 1 | 2 | 17 | 2,491 | **1 at 79.30%** |
| `exotic_path`(1024×252) | 1 | 5 | 5 | 258,048 | **1 at 60.13%** |

Those two modal shares are a cross-check worth stating: D102 §3 and D105 §7 measured "79.3% of
Stage A's pulls … a single unit-coefficient reader (93.6% of `compare_ois`'s)" by a different route
(`AdjointPlan`'s CSR lists at the pull site, filtered on the coefficient). This tool reaches
**79.30%** and **93.61%** on the same two fixtures from the reader lists alone, so the two censuses
agree and the small residual is exactly the non-unit coefficients D105 filtered on.

---

## 2. What does vary: the recomputed cone's transcendental content

D31's stated reason for recomputing is that "a group whose value is an `exp` / `log` / `sqrt` never
recomputes the libm call; only the arithmetic before it is recomputed". **That holds exactly on the
rates book and fails on a path grid**, and this is the first time either has been measured:

| fixture | recompute as % of the forward's arithmetic | recomputed transcendentals |
|---|---|---|
| `stage_a` | **8.3%** | **0 of 11,087** |
| `compare_ois`(256) | **21.2%** | **0 of 81** |
| `exotic_path`(64×12) | 44.3% | 832 of 1,601 (52.0%) |
| `exotic_path`(256×52) | 42.6% | 13,568 of 26,881 (50.5%) |
| `exotic_path`(1024×252) | **42.2%** | **259,072 of 517,121 (50.1%)** |

D31's reason is a *rates-book* property, in the same way and for the same reason that D81's 2,565x
collapse is (D106 §4): on a curve every `exp` is a discount factor, which is a value many trades
read, so the boundary rule makes it a domain value. On a path grid the step's own `exp` feeds
exactly one successor, so the boundary rule folds it into the chain — and then the reverse pays for
it twice.

Cone cost is **bimodal on the path grid and unimodal on the rates book**, and the split is exactly
the transcendental (cost scalar = flops + 20·trans + 0.5·loads, nominal weights, an ESTIMATE):

| fixture | cone of an intra-chain step |
|---|---|
| `compare_ois`(256) | **2 at 100.00%** — one bucket |
| `stage_a` | 2 at 64.68%, 3 at 33.45%, rest ≤ 8 |
| `exotic_path`(1024×252) | **2 at 49.61% and 21 at 49.22%** — two buckets, nothing between |

---

## 3. Reuse distance and working set: measured, and neither discriminates

Under the shipped schedule (forward domains ascending, then reverse descending) a domain value's
live range almost always ends in the reverse, so it spans most of the two-pass timeline whatever
the value is. Measured in value slots touched, over a timeline of 2N:

| fixture | p50 | p90 | p99 | mean | last reader is another domain's reverse |
|---|---|---|---|---|---|
| `compare_ois`(256) | 29% | 71% | 99% | 40.0% | 97.8% |
| `stage_a` | 35% | 90% | 94% | 46.2% | 72.6% |
| `exotic_path`(256×52) | 39% | 85% | 90% | 42.5% | 100.0% |

The three distributions are the same shape at both ends of the spectrum while the winners differ,
so **reuse distance does not discriminate either**. The mechanism is structural: the reverse's
recompute re-loads the same operands the forward loaded, so every materialised value is live from
its definition until the reverse reaches its lowest-numbered reader.

**Working set, and a correction to D31's own figure.** D31 priced the materialise arm at
`max_steps × rows × L` doubles, "4–6× the value buffer on M1". That bound charges every domain the
*longest* group's chain length. The true requirement is `Σ_d (steps_d − 1) · rows_d · L`:

Denominator stated, because D31's is not: the "value buffer" column below is `value_bytes()` —
the value array **and** the value-adjoint array, both `num_values × L` doubles — which is the
figure `Adjoint::describe()` prints.

| fixture | values + value adjoints | edge slots | materialise buffer | × the value buffer |
|---|---|---|---|---|
| `compare_ois`(256) | 0.88 MB | 0.64 MB | **+0.18 MB** | **0.21x** |
| `stage_a` | 14.59 MB | 9.56 MB | **+1.99 MB** | **0.14x** |
| `exotic_path`(256×52) | 8.41 MB | 7.63 MB | **+1.78 MB** | **0.21x** |

On `compare_ois`(256) the `max_steps` bound is `2 × 7,209 × 8` doubles = 0.92 MB, **1.05x**, against
a true 0.21x — a **5x overestimate** on this fixture. The gap is `max_steps / mean steps`: 57.98% of
`compare_ois`(256)'s rows are in one-step groups and 42.02% in two-step groups, so the bound charges
every row the longest chain.
The footprint cost of the arm is 8.2%–12.1% of values+edges and is nearly identical at both ends of
the spectrum, so it is not what separates them either.

---

## 3b. The same distributions over Q = adjoint(P), so "forward half" and "reverse half" have domains

For the runtime there is one program and `Adjoint::run` walks it twice, so §1–§3's "halves" are
passes. For a domain-level reader count of the reverse **as an object**, `tools/matrec/` also builds
Q = `adjoint(P)` (invariant I1, D98–D100) and splits Q's domains by the cone of P's own outputs —
the same split `tools/revcollapse/` makes at tape level.

| fixture | Q values / domains | forward half | reverse half | readers, fwd half | readers, rev half |
|---|---|---|---|---|---|
| `compare_ois`(256) | 31,299 / 86 | 10,238 / 27 | 21,061 / 59 | p50 1, p90 2, p99 5, max 772, **modal 1 at 65.88%** | p50 1, p90 2, p99 10, max 256, **modal 1 at 81.28%** |
| `stage_a` | 468,790 / 316 | 152,105 / 106 | 316,685 / 210 | p50 1, p90 5, p99 14, max 2,677, **modal 1 at 60.26%** | p50 1, p90 2, p99 3, max 1,999, **modal 1 at 83.32%** |
| `exotic_path`(256×52) | 321,079 / 988 | 98,068 / 295 | 223,011 / 693 | p50 2, p90 7, p99 7, max 13,312, **modal 1 at 43.08%** | p50 1, p90 2, p99 2, max 256, **modal 1 at 74.74%** |

Two things fall out of the table that are not this probe's question and are reported rather than
pursued:

* **The reverse half is 67%–69% of Q's values on every fixture**, and its reader distribution is
  *more* concentrated at one than the forward half's on all three. That is the single-reader pull
  chain D102 §3 measured from the other side (79.4% of Stage A's reverse-half pull `Affine`s are
  `+0.0 + 1.0·x`), seen as a distribution.
* **6.43% of Q's reverse half on `exotic_path` has ZERO readers** — emitted values nothing consumes.
  D102 §3 measured `dce` removing 0.1% on Stage A and exactly `affine_scan`'s 400 scan rows, and
  called it "a small emission inefficiency, reported not fixed". On a path grid it is two orders of
  magnitude larger. Still an emitter property and still not fixed here.

---

## 4. The counterfactual, measured: both arms in one process

`adjoint::Options::materialise_steps`, **false by default**, both arms interleaved A,B,B,A in one
binary on one `ir::Program` — D105's pattern, which exists because a two-binary comparison of this
very subsystem carried a 6.6% codegen confound that collapsed to 0.2% once both arms sat behind one
option. `--swap` controls allocation order. Load 2.64–3.41 throughout, stated per run.

**The flip is BITWISE: 7,949,088 `memcmp` comparisons, 0 mismatches**
(`tests/adjoint/materialise_steps_e0_test.cpp`) over 12 fixtures × tile ∈ {1, 3, 256, 100000} ×
lane_tile ∈ {1, 3, 8} × B including the split-lane widths 2, 5, 7 × both `use_catalogue` settings ×
2 states × 4 seeds. That is a construction, not an argument about arithmetic: both arms call the
same `forward_step` with the same operands, because an intra-chain step reads only its own group's
earlier steps and values of **earlier** domains (`ir::validate` forbids a forward read), so the
value buffer holds the same bits at those addresses whether the step runs in the forward or in the
reverse. It is the same fact that makes today's recompute bitwise.

### 4a. The winner flips between the two ends

| fixture | recomputed trans (share of intra) | B | off (D31) us | on us | paired delta | % of call | winner |
|---|---|---|---|---|---|---|---|
| `compare_ois`(256) | **0%** | 1 | 64.64 | 68.45 | +3.452 | **+5.34%** | RECOMPUTE |
| `compare_ois`(256) | 0% | 8 | 254.98 | 283.42 | +27.899 | **+10.94%** | RECOMPUTE |
| `compare_ois`(256) | 0% | 64 | 2046.09 | 2269.96 | +228.727 | **+11.18%** | RECOMPUTE |
| `stage_a` | **0%** | 1 | 1284.45 | 1313.75 | +29.611 | **+2.31%** | RECOMPUTE |
| `stage_a` | 0% | 8 | 7255.26 | 7764.15 | +486.169 | **+6.70%** | RECOMPUTE |
| `exotic_path`(256×52) | **46.5%** | 1 | 892.83 | 815.68 | −71.147 | **−7.97%** | MATERIALISE |
| `exotic_path`(256×52) | 46.5% | 8 | 5160.55 | 4645.63 | −512.128 | **−9.92%** | MATERIALISE |

Swap control, same configurations: `compare_ois` +3.66% / +12.48%, `exotic_path` −8.76% / −9.78%.
Signs and magnitudes hold, so this is not the allocation-order confound.

**Note what does NOT separate these rows.** The recompute's share of the forward's arithmetic is
19.3% on `compare_ois`(256) and 23.5% on `exotic_path`(256×52) — four points apart, and the winners
are opposite. An arithmetic share threshold would have to sit inside that gap, and `stage_a`'s
12.1% is on the same side as `compare_ois`'s 19.3% while `exotic_path`'s `barrier` leg at 34.2% is
on the other. **The operation count does not decide it** — D103 §3 and D105 §2 again.

### 4b. Within one fixture: the delta tracks the transcendental share

The leg mask (`--legs`) varies the transcendental share of the intra-chain class inside one fixture,
which is what turns a two-point correlation into a swept one. `exotic_path`(256×52), B=8, reps 31:

| legs | intra instances | trans share **of intra** | paired delta us | delta per intra lane-element |
|---|---|---|---|---|
| `spot` | 13,315 | **99.98%** | −590.8 | **−5.55 ns** |
| `arith` | 13,828 | 96.27% | −595.8 / −593.7 | **−5.39 / −5.37 ns** |
| `geom` | 14,084 | 96.34% | −548.2 / −520.1 | **−4.87 / −4.62 ns** |
| `barrier` | 27,908 | 47.70% | −479.1 | **−2.15 ns** |
| `all` | 29,188 | 46.48% | −483.9 | **−2.07 ns** |
| `compare_ois`(256) | 3,029 | **0%** | +27.9 | **+1.15 ns** |
| `stage_a` | 32,602 | **0%** | +486.2 | **+1.86 ns** |

Seven points, monotone in the share, crossing zero. A two-constant model

    delta per intra lane-element  =  c·(1 − f)  −  S·f,     f = transcendental share of the class

reproduces every point with **c ≈ 1.15–1.86 ns** (the copy-back, paid per non-transcendental
element) and **S ≈ 4.7–5.6 ns** (the libm call avoided). S is a plausible `std::exp` at 3.5 GHz
(≈16–20 cycles). *Both constants are fitted to these seven points and are ESTIMATES*; the model's
implied break-even, `f = c/(c+S) ≈ 21–28%`, is stated as the model's consequence and **is not a
tuned threshold and must not be shipped as one.**

**A method note, because one reading nearly became a finding.** The first `geom` measurement
(reps 25, single run, no swap) came back at −3.05% of the call and looked like a hard anomaly
breaking the model. Re-run at reps 31 with the swap control it is −8.70% / −9.15%. It was noise.
Nothing was concluded from it, and the un-replicated number is recorded here rather than deleted.

### 4c. Residency against arithmetic: the false positive this could have been, ruled out

D91 §3 put ~40% of the head-to-head calibration figure in cache refill and reproduced it by forced
eviction; D103 §4 measured 1.15x decaying to 1.01x on the ladder. Materialising **trades arithmetic
for footprint**, so if the winner moved with pressure the deciding quantity would be residency and
no structural signal could carry the decision. Same paired arms, forced flush between consecutive
calls, B=8, reps 21:

`exotic_path`(256×52), load 2.91:

| evict MB | 0 | 1 | 4 | 16 | 64 |
|---|---|---|---|---|---|
| off us | 5106.26 | 5135.67 | 5245.74 | 5258.05 | 5231.64 |
| on us | 4695.14 | 4610.20 | 4687.53 | 4722.60 | 4766.38 |
| paired | −403.2 | −525.5 | −548.1 | −481.2 | −477.0 |
| % of call | −7.90 | −10.23 | −10.45 | −9.15 | −9.12 |
| winner | MAT | MAT | MAT | MAT | MAT |

`compare_ois`(256), load 2.76:

| evict MB | 0 | 1 | 4 | 16 | 64 |
|---|---|---|---|---|---|
| off us | 256.70 | 256.96 | 260.78 | 341.83 | 359.27 |
| on us | 282.75 | 284.22 | 286.49 | 370.81 | 393.91 |
| paired | +27.8 | +26.6 | +27.4 | +32.8 | +34.5 |
| % of call | +10.84 | +10.35 | +10.52 | +9.59 | +9.62 |
| winner | RECOMP | RECOMP | RECOMP | RECOMP | RECOMP |

**The verdict does not flip at any pressure on either fixture, and the percentage barely moves.**
Eviction does what D91 and D103 said it does to the absolute times — `compare_ois`'s whole call goes
**1.40x** at 64 MB, `exotic_path`'s only **1.02x**, the large-working-set amortisation D103 §4
measured — but it leaves the *paired* delta alone. **Residency is not what decides this one.**

---

### 4d. What this settles about D103 §3's open attribution

D103 §3 measured the reverse pass running **~1.90x less efficiently per operation** than the forward
(2.74x the arithmetic, 5.20x the time, on the same program) and named the recompute as the candidate
cause, explicitly labelling it unresolved: *"That attribution is an estimate; nothing counts the
recomputed ops."* This counts them, and then removes them.

On `compare_ois`(256) — D103's own program, 12,280 pinned nodes, 24 domains, 7,209 values — the
recompute is **3,029 flops and 6,058 loads per lane**, against the domain-value steps' 12,537 and
16,587: **19.3% of the forward's arithmetic, and zero transcendentals.**

The stronger bound is the time, because `materialise_steps` deletes that arithmetic outright and the
paired delta prices the whole trade. At B=8 the arm is **+27.9 us** net on a 255.0 us call, so the
recompute's arithmetic is worth at most the copy that replaced it plus that margin — bounded above
by the 24,232 flop-elements it comprises, i.e. **≤ ~24 us, ≤ 9.5% of the call and ≤ 11% of D103's
210.8 us reverse-pass bound** (the upper bound is an ESTIMATE; the +27.9 us is measured).

**So the recompute explains at most ~1.1x of D103's 1.90x, and the rest is something else.** The
candidate named in D103 §3 is real but small, and the gap stays open. D105 §7's per-row four-way
pull dispatch and D102 §4 item 2's single-reader pull aliasing remain the unexamined items on that
list; nothing here touches either.

---

## 5. Verdict

**Is a generic policy reachable from structural information the engine already holds?** Partly, and
not from any of the four signals the commissioning brief named.

**What does not work.** Reader count is spent constructing the partition (§1) — a theorem, so no
amount of data will rescue it. Reuse distance is a near-constant of the shipped schedule and has the
same distribution at both ends while the winners differ (§3). The footprint ratio is 8.2%–12.1% at
both ends (§3). And the operation count gets the winner **wrong**: 19.3% of the forward's arithmetic
on `compare_ois`(256) → recompute wins, 23.5% on `exotic_path`(256×52) → materialise wins, with
`stage_a`'s 12.1% and the `barrier` leg's 34.2% on the opposite sides of that four-point gap
(§4a). That is D103 §3 and D105 §2 for the fifth time: an operation count treats a store, a branch
and a libm call as commensurable, and they are not.

**What does work, and it is already in the IR.** The deciding variable is the **op** of each
intra-chain step — `ir::Group::steps[k].op`, which `ir::infer` has recorded since M2 and which no
analysis is needed to obtain. Seven points, both ends of the spectrum, sign and rough magnitude
from one model (§4b).

**How many calibrated constants.** Fewer than the model's two, and the count depends on the
granularity of the arm:

* **The all-or-nothing arm measured here** needs **one**: the decision is `S·f > c·(1−f)`, i.e.
  `f > c/(c+S)`, so only the RATIO `S/c` enters — measured at **3–5** on this fingerprint. Not two
  constants; the absolute ns values cancel.
* **A per-step arm needs none that is machine-specific in any sharp sense.** Materialising step *k*
  alone saves that step's own work and costs one copy, so the rule is "store a step whose own cost
  exceeds a copy". Measured, the three quantities are **≈5.5 ns (a libm call), ≈1.5 ns (the copy),
  ≈0.3 ns (one flop)** — an order of magnitude between the transcendental and the copy, so any
  threshold in a wide band gives the same answer on every fixture here, and the ORDERING
  `transcendental > copy > flop` is a property of every machine this engine would run on rather
  than of this one. The constant is needed in principle and is coarse in practice.

  **That arm is NOT measured.** `materialise_steps` is all-or-nothing, and a per-step version has a
  complication this probe does not touch: with every intra-chain step at exactly one reader (§1), a
  step whose successor is materialised may not need computing at all — unless its own rule needs
  its value (`Div`, `Exp`, `Sqrt`, `Recip`) or its reader's rule reads it as an operand. Stated as
  a consequence of the model and labelled unmeasured.

**What this does not say. It does not say build it.** The arm is worth **−8% to −10%** of an
`Adjoint::run` on a workload the engine is **already 0.45–0.78x of naive scalar C++ on** (D106), and
**+2.3% to +11.2% against** it on the workloads the engine ships. A policy that got this right
everywhere would buy roughly a tenth of the reverse pass at the low-sharing end and **nothing** at
the high-sharing end, because there the recomputed cone contains no transcendental to save — which
is the fifth lead in this programme to come back at or near zero on the workloads that matter
(D92, D104, D105, D111). It belongs with D106's open question — whether the engine's claimed domain
includes path grids at all — and not with the rates book's optimisation ledger.

**What is built and shipped off:** the option, the bitwise gate, two mutants and both tools. Shipped
behaviour is D31's, byte for byte, and D31's decision is **confirmed correct for the rates book on
its own stated reasoning**, which is more than it had before: its "a group whose value is an exp
never recomputes the libm call" was an argument, and is now a measurement (0 of 11,087 and 0 of 81),
together with the boundary of that argument (50.1%).

---

## 6. Loose ends

* The two new mutants have **no line in `scripts/mutation_catchers.tsv`**, so they take the
  full-gate fallback. That file is generated by `scripts/mutation_test.sh --full` and its header
  states a missing line *"can never turn a survivor into a pass"* — it costs time, never
  correctness. Same position D111 §7 took, and now two more lines owed; a `--full` run on an idle
  box tidies all four. Both mutants were verified to die against this gate directly
  (**38,368** and **146,576** mismatches of 2,668,528 comparisons).
* `adjoint(adjoint(P))` **does not exist for a scanning program**, found while building
  `tools/matrec/`: `build_plan(Q)` throws *"domain 63 (cbar9) is recurrent but not a scan"*, because
  `adjoint_to_program` emits the reverse of a scan as a recurrent NON-scan domain by design
  (`adjoint_to_program.hpp`, "Scans (C3b)") and `build_plan` refuses exactly that shape. A real hole
  in I1's closure, one level up from where D100 asserted it; `tools/matrec/` computes Q's reader
  counts directly from Q's own gathers and segments instead. Reported, not this probe's question,
  and not touched.
