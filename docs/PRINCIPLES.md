# PRINCIPLES — what the engine is allowed to do, and against what it is judged

Owner-set. Written 2026-09-25 after M4's post-mortem; **rewritten in place 2026-09-27** after the
pipeline reassessment. This is the contract the engine is built against.

**This document is rewritten, never appended to.** Where it and any older document disagree, this
one wins. `docs/DECISIONS.md` is the historical ledger of how we got here; this is the statement of
where we are. §9 governs that split. The pre-rebuild engine is tagged `v1.0-m4`.

---

## 0. The two failures this exists to prevent

**The first was a space nobody wrote down.**
`include/epykos/maths/instrument/coupon.hpp` states that "nothing telescopes, unrolls or pre-folds
by hand (that is the engine's business, M4)". The pricing layer hands the engine a naive product
loop and expects it to collapse. `docs/DESIGN.md` §6 then specifies the eight rewrites the engine
would use: fold uniform columns, bucket rows, elide trivial maps, push unary through gathers, share
reciprocals, group formation, materialise at boundaries, block linmap. **Every one is a
data-movement rewrite. Not one can collapse a product.**

Those two statements sat in two files for three days. Measured consequence: the entire plan-level
dynamic range on the Stage A tape is 1.027x–1.042x (D68), while telescoping the coupon is worth
33x–132x on the same problem (D78).

**The second was a contract applied to the wrong stage.**
Every tape pass carries, in its own header, "All four passes are exactness class E0: the replay of
the tape after a pass is bit-identical to the replay before it." Bit-identity is the right contract
for deciding *how* to evaluate an expression. It is a prohibition on deciding *which* expression to
evaluate, because every algebraic simplification re-rounds. Applied globally it forbade the only
work worth doing, and nobody noticed, because it was a default rather than a stated principle.

The lesson in both cases is the same: **what the engine may do was never written down**, so nobody
could see what it excluded. That is what this document is for.

---

## 1. The pipeline, and the pin

The engine is a compiler. It was built with a good back half and no front half. This is the shape it
is being rebuilt to, and the rest of this document follows from it.

```
  record ──► Tape
     │
     ├─ canonicalise      div→mul·recip, sub→add·neg, operand ordering, constant folding
     ├─ simplify          value numbering + algebraic identities + cancellation    ⟲ to fixpoint
     ├─ detect chains     loop-carried recurrences (the analysis ir::infer already does)
     ├─ solve recurrences closed forms: telescoping, geometric, arithmetic, linear ⟲ re-simplify
     └─ dce
     │
  ═══╪═══════════════════ THE PIN: the collapsed tape ═══════════════════════════════
     │
     ▼
  ir::infer ──► plan (fusion, inline, pairing, lane width) ──► select (catalogue,
                                        vector transcendentals) ──► execute / adjoint
```

**Above the pin the engine chooses WHICH expression to evaluate.** It is judged against the recorded
expression in exact real arithmetic. It may re-round freely. Its objective is countable — nodes,
operations, transcendental calls — not modelled.

**Below the pin the engine chooses HOW to evaluate THAT expression.** It is judged bit-identical to
the pinned tape, under the declared policy of §5.3. Its objective is wall clock.

**Both of those sentences say EXPRESSION, and that is a limit, not a figure of speech (D87).** The
pin governs recorded maths. It says nothing about maths that was never recorded — and there is a
second body of it: the engine's OWN numerics. The Newton step, the optimality diagnostic ‖JᵀF‖∞ and
the implicit-function rule's own products are written in C++ and Eigen in `src/solver/`, so no pass,
cost model or search above or below the pin can see them. That is not a slip. **A derivative cannot
be expressed as a tape node** — `Op::Linmap` is reserved with no producer anywhere, `adjoint::Adjoint`
is a compiled artifact of the pipeline, and so differentiation is a pipeline STAGE, not an
OPERATION. Every quantity whose definition mentions a derivative is therefore forced out of the
recorded world by construction.

The consequence is measured, not feared: D68 puts the hand-written half at about 95% of an O4 lane
and 46% of the whole Stage A problem, and D85's largest available win sat unseen inside it for four
milestones. **The thesis — maths is written once and compiled — has so far been applied to the
product's maths and never to the engine's own.** D87 §7 states the options and decides none of them.

The pin is a stage boundary, not a per-rule judgement. A rewrite is licensed by **which side of the
pin it is on** — a structural fact, not an argument to be had case by case. This replaces "every
rewrite declares an exactness class E0/E1", which made each rule a separate negotiation and returned
the wrong answer every time the right answer was "change the arithmetic".

Three consequences, all of which close open problems rather than open new ones:

1. **The optimiser gets wired in.** Today `optimise::` is on no execution path at all —
   `ImplicitProgram` is `infer` → `Interpreter` → `Adjoint` and consults nothing. Above the pin the
   algebra *is* the pipeline, not an apparatus that scores programs beside it.
2. **The cost model's job shrinks to one it can do.** It currently predicts interpreter wall clock
   at 58.6% error in order to choose between plans worth 1.03x. Above the pin the objective is a
   count. Below it, the greedy planner is already within a few percent of the best plan (D68).
3. **Vector transcendentals become legal** (§5.3), and that is likely the largest unclaimed win
   below the pin.

### 1a. What we have and have not got, against a real compiler

Audited against the code 2026-09-27, not recalled.

| compiler stage | EpykosEngine |
|---|---|
| canonicalisation | **absent** — and the reason `affine_collapse` cannot see `Div` |
| value numbering | hash-consing: equal *syntax*, never equal *value* |
| algebraic simplification, reassociation | **absent** |
| strength reduction | **absent** |
| scalar evolution — recurrences and their closed forms | `detect_chains` **finds** them and never solves one |
| loop fusion, tiling, unrolling | present, and good |
| vectorisation | tiling and batch; vector transcendentals **blocked by the retired contract** |
| instruction selection | the catalogue |
| fast-math | permanently off |
| phase ordering | four passes called by the recorder; no pipeline |

Everything from loop transformation down exists and works — M1 put the interpreter within 4.4% of a
hand-fused kernel. Everything above it is missing. M4 then searched the half that was already good,
and measured its remaining headroom at 1.03x.

---

## 2. Algebra first, estimation second

Two tiers, in this order, and the first is exhausted before the second is entered. Both live above
the pin.

**Tier 1 — exact algebra.** Rewrites that are identities in ℝ: cancellation, telescoping,
reassociation, distribution and factoring, strength reduction, recurrence collapse. These remove
work at no cost in the mathematics. They move floating-point results, and §5 governs by how much.

**Tier 2 — deliberate approximation.** Rewrites that change the function: Taylor expansions,
polynomial replacements for transcendentals, moment approximations. Each spends accuracy for speed
and needs a validity domain.

**Tier 2 is CLOSED until tier 1 is proven end to end** (owner, 2026-09-25). `exec::ExpMode::poly`
stays disabled, and `task_f7b9c87d` — the catalogue having no axis for which exponential a kernel
uses, so `poly` is silently a no-op whenever the catalogue is on — is a deliberate deferral, not an
open defect. It becomes tier 2's first citizen when that tier opens.

The boundary is not where it looks. Replacing libm's `exp` with a *differently approximated* `exp`
of equal or better accuracy is **not** tier 2; it is an implementation choice below the pin (§5.3).
Replacing `exp` with a truncated series that is knowingly less accurate is tier 2.

---

## 3. Discovery, not declaration

**The pricing maths declares no optimisation opportunity.** It is written as the definition reads: a
compounded coupon is the product loop of the definition, an average is the sum of the definition.

**The engine may hold general mathematical facts.** Not hints about coupons, not fixture-specific
knowledge — general truths of the same standing as associativity. "A scan whose step multiplies by a
ratio of consecutive terms collapses to a ratio of its endpoints" is such a fact. It is about scans,
not about interest rates, and the engine is entitled to know it.

**The op set does not grow for this** (owner, 2026-09-25). `Linmap`, `Affine` and `Sum` carry what
they carry; new facts live in the identity and rule set over the existing ops.

The distinction that makes this workable: a fact about a *mathematical form* is discovery; a fact
about *this instrument* is declaration and is not allowed.

### 3a. Recurrences, and where each half of the work belongs

Measured 2026-09-27 on the recorded tape. This corrects what this section said on 2026-09-25.

Telescoping is **two** transformations at **two** layers, and conflating them is what made it look
intractable.

**The cancellations are local peepholes and belong on the tape.** The recorded coupon is
`1 + ((DF(s)/DF(e) − 1)/τ)·w` with `w ≡ τ`, and because constants are hash-consed on bit pattern, τ
is literally the same node in both places. So `mul(div(x, τ), τ) → x` and `add(1, sub(y, 1)) → y`
are node-identity matches, not value comparisons. Measured on the real `compare_ois` tape:
**94,942 additive cancellations fire, exactly the recorded observation-day count.** They are linear,
they need no search, and `standard_passes` performs none of them — a 365-day chain goes in at 2,558
nodes and comes out at 2,558.

**The closed form ALSO turned out to belong on the tape, and this section predicted otherwise.**
The plan was: peepholes on the tape, then the recurrence's closed form on the detected scan domain,
where it is one rewrite instead of n. Built and measured 2026-09-27, the tape does all of it. After
the two cancellations the step is `acc ← acc·(d_i/d_{i+1})`, the accumulator is left-associated, so
the tape holds `mul(div(d_0,d_i), div(d_i,d_{i+1}))` at every step and **one more peephole,
`mul(div(p,q), div(q,s)) → div(p,s)`, telescopes the whole chain in a single forward sweep.** No
scan analysis, no closed-form solver, no IR round trip. §10's steps 4, 5 and 6 are one pass of four
rules.

Two properties worth keeping in view:

* **The pattern IS the precondition.** A lookback, observation-shift or lockout coupon's days do
  not meet — `t_next_i ≠ t_rate_{i+1}` — so `div(d_i, d_{i+1})` and `div(d_{i+1}, d_{i+2})` share
  no node and the rule does not match. The spike had to test `telescopes()` explicitly per coupon;
  a structural rewrite needs no such test, because a coupon that does not telescope cannot match
  the shape.
* **It is O(n), not O(1).** One rewrite per observation day, and n is 94,942 on `compare_ois`.
  Linear in the recording is cheap enough that the O(1) scan-domain version buys nothing today.
  If a recording ever appears where it does — a scan too long to unroll at all — `ir::infer`'s
  `detect_chains` is still there and the closed-form solver is still the right answer for it.
  That is a measurement to make before building, not a layer to add on principle.

**This is elementary and was over-theorised.** D74 and D75 framed it as creative telescoping
(Gosper/Zeilberger) and as e-graph equality saturation needing associative-commutative closure,
measured 2ⁿ−1 e-classes, and concluded it was unreachable. Both framings are withdrawn (§7.3). An
e-graph is for when you do not know which direction is better. Here you always do: fewer nodes wins.
The right tool is **directed canonicalisation**, and it is linear.

Measured once it existed: `compare_ois` at sixteen trades goes from 1,020,583 recorded nodes to
**1,183**, where the four data-movement passes alone left 81,321. The hand-written spike produces
1,313 from the same problem, so the engine's own result is smaller than the form a human wrote to
show it what it was missing, and recording both and comparing gives **0.000e+00**. The whole desk
problem, Stage A, goes 517,036 → **194,794** even though §6 notes its averaged coupons do not
telescope at all.

The recurrence library is small and enumerable for this domain: product of consecutive ratios,
geometric, arithmetic, linear with constant coefficients. That list is the work and it is finite.

---

## 4. Scope: expression, then algorithm, and now one part of the solver

**In scope.** The arithmetic the engine was handed, and the algorithmic form of how a quantity is
obtained: a closed-form Jacobian for a linear block instead of a numerical one; the implicit
function theorem instead of bump-and-recalibrate.

### 4a. The solver exclusion, partly lifted (owner, 2026-09-28)

This section said "out of scope for now: the solver itself — Jacobian policy, iteration strategy,
convergence criteria… **revisit once tier 1 is proven**". Tier 1 is proven (D81), so the condition
the exclusion set for itself has fired, and the owner has lifted it for one part.

**Now in scope: Jacobian reuse and warm-start state.** A solve may be seeded from a previous
solution and may reuse a previously factorised Jacobian, refreshing when contraction stalls.

**Still out of scope: iteration strategy and convergence criteria.** Neither is touched. The
narrowing is deliberate — the measured prize needs only the first.

**Why, measured (D82).** Of a 74.9 us warm solve, **about 65 us is a Jacobian build paid on every
call** whatever else happens: at zero quote move, with a single 2.1 us residual evaluation, the call
still costs 66.9 us. The other engine's entire warm tick is 9.3 us, so our fixed Jacobian cost alone
is seven times their whole tick. It is the largest measured loss in the repository and the algebra
phase moved it by nothing — collapsing the residual slice 200x left the ratio where it was, which is
what proves the gap is not arithmetic.

Two structural alternatives were measured and declined rather than assumed: a closed-form Jacobian
does not apply (0 of 1 residual output domains satisfy `is_linmap_domain` — the residual is a par
rate minus a quote and every discount factor is `exp` of an affine function), and a partial closed
form is bounded at 14.4% of the block's work against a cache that removes 100% of it.

**The verification obligation this section was protecting.** The old text's reason for excluding the
solver was exact and still stands: *a solver change can alter convergence rather than a value, which
is a different and harder verification problem.* Lifting the exclusion means discharging it, so the
gate is stated here rather than left to whoever writes the code:

1. **The uncached path stays the reference and stays the default.** Reuse is opt-in, carried in an
   explicit caller-held object; passing nothing must be today's behaviour bit-for-bit.
2. **The cached path lands within the block's own solve tolerance of the uncached path**, on every
   lane, over a ball of quote moves large enough to force a refresh. That is the differential test,
   and the uncached path is what it differs against.
3. **`RunStats.refreshed` becomes load-bearing and is reported**: a cache that never refreshes is
   indistinguishable from one that never goes stale until it silently fails to converge.
4. **A mutant for "never refresh on stall"**, which must show as non-convergence on a large move.

**Built, measured, declined (D85, 2026-09-29).** The cache above was written to this gate and
measured **0.99x — it collects nothing**, and it is reverted. D82's own decomposition says why: ~65
us of a 74.9 us warm solve is one Jacobian build paid on every call, and a seed cache attacks the
iteration count, not the Jacobian. Measured, the cache lands exactly on a fixed record-point seed
(183.3 us against 182.7 us), because both remove iterations and neither removes the build.

**What is in scope instead: a LAZY final Jacobian — BUILT 2026-10-03, D89.** Build `J_z` at the
solution when `adjoint` or the IFT asks for it, not eagerly at the end of every `run`. Measured on the same block, a re-quote
falls from 69.0 us to **11.4 us** and the ladder is untouched, because when the ladder is wanted the
Jacobian is still built at the solution. This is laziness, not approximation: it changes no iterate
path, needs no cache, and costs §5.2 nothing.

**The gate, restated for that shape.** Clauses 1 and 3 above are vacuous — there is no cached path
and nothing to refresh. Clause 2 becomes *stronger*: the lazily-built Jacobian must be **bitwise**
equal to the eagerly-built one, not within a tolerance of it. Clause 4's mutant becomes "never build
it when asked", and it cannot survive: dropping the final Jacobian outright leaves the O3 ladder
6.5% wrong at +25bp and 24% wrong at +100bp (D85 §3).

**One warning that survives the change of shape.** D85's first version of that ladder check was run
at the record point, where a stale Jacobian *is* the Jacobian at the solution, and it returned
`0.00e+00` for every configuration. Any gate on this must exercise a MOVED market; a check whose
control case is the only case it runs is not a check.

## 5. The error contract

### 5.1 Ground truth

**Ground truth is the recorded expression evaluated without rounding** (owner, 2026-09-25). Not the
`double` evaluation of it, which is one realisation among many and has never had its own error
characterised. Not the "financially correct" answer either: the engine reproduces the maths it was
given and may not silently repair a poor discretisation. If the method is wrong that is a bug in the
pricing code, not something the optimiser fixes.

**The oracle is a DIAGNOSTIC, not a gate** (owner, 2026-09-25: "I don't see why it is necessary at
all if the simple rules of algebra are followed"):

* a rewrite's correctness is established by its identity, once, when the identity is written —
  re-checking it at runtime against a high-precision reference re-checks algebra already known to be
  true;
* an implementation bug in the rewriter shows up as DISAGREEMENT between paths, which needs no
  notion of truth;
* truth only adds something when two paths disagree and you must know which to believe, and a person
  can settle that on the rare occasion it happens;
* a bug-detection tolerance need not be principled or tight. Something loose never fires on
  legitimate rounding and still catches a genuinely broken rewriter.

`epykos::Wide` is kept, does not run routinely, and gates nothing.

**Rigour moves from testing to the rule definitions.** When the engine holds "a scan multiplying by
consecutive ratios collapses to the ratio of its endpoints", someone must have established that
correctly, and the cost of getting it wrong is every program that matches. That is a proof
obligation discharged once, on paper, by whoever writes the rule. It is a better place for rigour
than a runtime instrument, and it is cheaper.

### 5.2 The two references, one per side of the pin

| | above the pin | below the pin |
|---|---|---|
| judged against | the recorded expression in ℝ | **the pinned tape, bitwise** |
| may change roundings | yes — that is the point | no |
| licence to proceed | a written identity plus its domain preconditions | none needed; it is an implementation |
| a failure means | the identity is wrong, or its precondition does not hold | the implementation is wrong |

Two things follow that matter in practice.

**Reproducible ≠ identical to the naive evaluation.** Above the pin is a deterministic pure function
of the tape: same recording in, same collapsed tape out, on every machine. Below the pin is
bit-identical. So the composed pipeline is **bit-reproducible** — same build, same inputs, same
bits, every run.

**Reproducible means: same build, same inputs, SAME OPTIONS.** The third clause is not a weakening;
it is a statement of what was always true. `ProgramOptions::warm_start` and `jacobian = chord`
predate this section, and both change a solve's iterate path: measured on the `compare_ois` 25-knot
block, the chord policy lands 1.2e-14 from the cold Newton default in the knots and 2.6e-12 in the
IFT ladder, each well inside the block's own 1e-13 solve tolerance (D85 §2, §3). What matters is
that both depend only on options and inputs and **never on history**, so a caller can reason about
the result from the call in front of them.

**There is no exception for call sequence, and D83's amendment making one is withdrawn (D85).** A
solve cache — seeded from, and reusing a factorisation from, whatever the previous call left — would
have made the landing point depend on the SEQUENCE of calls. One was built against §4a's gate,
measured at 0.99x, and reverted; nothing in the engine now carries solver state between calls. The
replacement, a lazily-built final Jacobian, changes no iterate path at all and is required to be
*bitwise* equal to the eager one.

What is given up is agreement with the *unoptimised* form, which pinned one arbitrary rounding
sequence as though it were the answer. Reproducibility is what people mean when they ask for
bit-identity, and it is kept.

**The bitwise gate does not disappear; it re-anchors.** `verify::Tolerance::e0()` today compares the
compiled path against the templated `double` maths. It comes to compare the executed path against
the *pinned tape's* own evaluation. That is a strictly stronger gate, because it also pins that the
algebra phase did what was intended.

Above the pin the test is path-to-path agreement within a loose tolerance, plus the identities' proof
obligations. Tolerances are **per output class, not global** (owner, 2026-09-25), and the numbers
belong in `PROBLEM.md` beside the outputs they govern. §5.5 is why: sensitivities are 62x–656x worse
conditioned than valuations.

**Execution is not a tier of its own.** The interpreter, the catalogue kernels and the adjoint are
implementations of the pinned tape and are judged against it. The standing test — **the slow and
fast paths must agree** (owner, 2026-09-25) — holds at both altitudes: generic interpreter against
catalogued kernel, unfused against fused, reference against release. Below the pin that agreement is
exact; above it, within tolerance.

That test is not redundant with correct algebra. It does not test the algebra — it tests that the
CODE implements the algebra, which is a claim about construction rather than a consequence of it.
Two defects were exactly that gap: the catalogue fingerprint was canonical by design and was not
(D61), and a harness sized a buffer from the wrong vector and overran it on every call (D65).

### 5.2a Re-anchoring an existing gate: the decision rule

Every gate written before the pin existed compared *something* to *something*. Which of the three
cases it is decides what happens to it, and the case is a fact about the test, not a judgement:

1. **Both sides evaluate the SAME pinned tape** — interpreter against replay, against
   `ir::Evaluator`, against the expanded tape, against the adjoint's forward pass; catalogue on
   against off; one lane width against another; one batch size against another. This is below the
   pin on both sides. **It stays bitwise.** If such a gate starts failing, that is a REAL DEFECT
   in the algebra phase or in execution — it is never a reason to loosen the gate.

2. **One side is the templated `double` maths** — `price_book<double>`, `price_stage_a_at`,
   `price_sample<double>`, the M1 oracle tables, a `double` instantiation of a curve. This
   comparison CROSSES the pin: the reference is the recording as written and the other side is the
   collapsed tape, so they are two different roundings of the same real number. **It becomes a
   tolerance comparison.** Measure the divergence the change actually produces, set the gate about
   an order of magnitude looser than that, and print the measured value so drift is visible. §5.1
   is why a bug-detection tolerance need not be tight.

3. **The test asserts STRUCTURE the algebra removed** — that a scan chain exists, that a domain has
   N rows, that a rule fires on M sites. The structure legitimately changed and the old assertion
   is now false about a better program. **Re-state it against what is true now**, and say in the
   test what it used to assert and why that moved. Deleting it silently is how coverage is lost.

A gate in case 1 that fails is a bug. A gate in case 2 that fails is arithmetic. A gate in case 3
that fails is the point.

### 5.3 Transcendentals: the declared policy

**`exp(x)` in a computer is not the exponential** (owner, 2026-09-27). It is libm's approximation,
accurate to about an ulp, chosen by whoever built the platform. The engine has already measured this
and not acted on it — D72 records that "libm's 64-bit `expl`/`logl` is the LESS accurate side of
that comparison".

So bit-identity to libm is not correctness; it is agreement with one approximation. Pinning it has
had a real and unquantified cost: D27's M1 kill test ran "the interpreter in its gated E0 mode
(scalar libm `std::exp`) against the hand-fused kernel using the same scalar libm" — both sides held
to the scalar transcendental so that bit-identity was achievable. Every discount factor is an `exp`.

**The policy.** The choice of transcendental implementation is a **declared build policy**, fixed
above the pin and constant below it, exactly as compilers treat `-ffp-contract`:

* A vector or otherwise different transcendental of **equal or better accuracy** than the declared
  reference is a legal implementation below the pin. It is selected by policy, not per kernel, so
  the pipeline stays bit-reproducible.
* `fma` contraction is the same kind of thing and is settled the same way. It is currently an E1
  peephole applied at kernel selection, which under a strict reading violates the rule below the
  pin. It becomes part of the declared policy, fixed before the boundary.
* Changing the policy changes the answer. It is a versioned, recorded decision — never an
  optimisation the search may make.

Below the pin therefore reads, in full: **bit-identical to the pinned tape under the declared
contraction and transcendental policy.**

**What this rule closes off, stated so it is a choice and not an accident (D88).** Between the two
sides there is a third kind of rewrite with no home: one that is *not* an identity in exact real
arithmetic but is within a stated error budget — replacing a non-linear function by a truncated
series, say. Above the pin it fails the test, because it is a different function and not a
different rounding of the same one. Below the pin the paragraphs above forbid the search from
choosing it. So it is licensed **nowhere**, and deliberately: this rule is what keeps the pipeline
bit-reproducible. Admitting that category would mean the composed error BUDGET becomes the
guarantee in place of a fixed POLICY — a larger loosening than D79 made.
`include/epykos/algebra/error.hpp` is the only design in the tree for it. D88 §5 works the example
through, including the reason it is not obviously a win: expanding the discount factors would
destroy the telescope that collapses the tape 2,565x. **Not decided.**

### 5.4 What this retires

The `-ffp-contract=off` pinning and the `*_e0.cpp` / `*_e0_test.cpp` convention exist to make results
reproducible bit for bit across compilers. Measured 2026-09-25: **14 engine sources and 37 test
files, about 9,700 lines, plus 21 lines of build routing.** None of it serves the mathematics; it
serves an equality check that is no longer the contract.

It has not been free. Two cross-compiler defects were in that machinery rather than in the maths:
FMA contraction reaching a folded constant (D25/D46) and unsequenced operand ordering leaking into
the catalogue fingerprint (D61).

**Retire it as the rebuild reaches each file.** Not as a separate sweep — a 52-file rename that
touches nothing else is a bad commit.

Where two paths happen to agree exactly and the exactness costs nothing to assert, asserting it
remains a good bug detector and is allowed; it is what caught D61. It is a test of convenience,
never a constraint. The moment a kernel wants to reorder for speed, the assertion is relaxed there
and the error measured.

### 5.5 The measurements this rests on

Naive path against truth, fingerprint `d448afd70180` (D72):

| | valuation | sensitivities |
|---|---|---|
| M1 book | 1.0e-15 to 1.8e-15 | 7.2e-15 to 1.1e-13 |
| Stage A | 7.9e-14 to 1.6e-13 | **5.2e-11 to 2.1e-10** |

Sensitivities are 62x worse than valuation on the M1 book and 656x worse on Stage A. The retired
bitwise contract was demanding exact reproduction of a risk number good to about ten significant
figures.

And the direction of the trade, measured (D74): the telescoped form is **39.7x more accurate** on
calibration par rates and **53.5x** on trade PVs than the product loop it replaces, closer to truth
in 16 of 16 and 16 of 16. Tier-1 algebra is not a concession of accuracy for speed. It is usually
both.

---

## 6. What the engine is graded on

**An arithmetic-dominated workload is added** (owner, 2026-09-25): Monte Carlo exposure or XVA, long
elementwise chains over large arrays. No current fixture has that shape and it is the shape the
design targets. It is Stage D in `PROBLEM.md` §8 and it moves forward.

**Stage A is kept as the realism and correctness gate, not as the scoreboard.** It is researched,
cited and end to end. It is also dominated by a calibration solve, so the plan stage touches 2.513%
of it (D68).

**The head-to-head is the honest scoreboard.** `tools/h2h` (D78) runs both engines in one process on
one clock and refuses to report a timing unless they agree first. It is the only instrument here
that can say whether the engine is good, rather than whether it improved.

---

## 7. What is removed, and when

Audited against the code 2026-09-27. Line counts measured, not estimated.

### 7.1 Quarantined now — no execution path reaches it

| | engine | tests |
|---|---|---|
| `optimise/` — e-graph, cost model, extraction, plan_bridge | 2,098 | 3,983 |
| `rewrite/` minus `planner` — R1–R7, cross-stage, greedy, ir_edit, verifier | 3,417 | 2,721 |
| | **5,515** | **6,704** |

**12,219 lines.** `grep -rn "optimise::" src/solver src/exec` returns nothing; the only references
outside the subsystem are comments, the mutation registry's strings, and two measurement tools.
Quarantined, not deleted — see below.

**Quarantine behind a default-OFF build flag. Do NOT delete.** One commit; nothing that runs is
touched; fully recoverable.

**Why not delete, corrected 2026-09-27 (owner: "Do we truly not need the e-graph at all? I thought
this was how we search the space of optimisations after the algebra is fully optimised down").** He
is right and the first draft of this section conflated two things:

* **The e-graph as a TECHNIQUE has a future here.** Equality saturation exists for rewrites where
  you do not know which direction is better, and the algebra phase has exactly such a residual after
  canonicalisation: factoring against distribution (`a·b + a·c` ↔ `a·(b+c)`), reassociation, and
  common subexpressions that only appear under a different association. Those are context-dependent
  and phase-ordering-sensitive, which is the problem equality saturation solves and directed
  rewriting does not. D75's own verdict was **"write the term e-graph, do not wrap one"**, and D73's
  `include/epykos/algebra/egraph.hpp` is the design for it.
* **The e-graph we HAVE is at the wrong altitude.** Its e-nodes are whole `ir::Program`s (D62
  recorded this as its own known limitation), it sits below the pin, and its rule set is layout-only
  by specification. Nothing in it is reusable for term-level algebra, which is why it is quarantined
  rather than extended.

So the sequence is: canonicalise and simplify by DIRECTED rewriting first, because that is linear and
because every e-graph needs a canonical form underneath it anyway; then measure what bidirectional
space is left; then build the term-level e-graph against that measurement rather than against a
guess. Deleting the quarantined code is a decision to take **after** step 6, with that measurement in
hand — not tonight, and not on a trigger set before the evidence exists.

### 7.2 The structures that caused it, for the record

1. **A rewrite must return a whole `ir::Program`.** `rewrite::Proposal` holds an optional Program; a
   rule may point at a step but cannot replace a term, so every match materialises an entire
   program. This was called "the blocker" and it was a blocker for the *wrong approach*.
2. **The e-graph's e-nodes are whole Programs.** The same root cause.
3. **No algebraic identities exist anywhere** — not in the op set, not in a tape pass beyond
   `affine_collapse`, not in a rule. **This is the real one**, and the only item here the rebuild
   fixes by building rather than by deleting.
4. **`affine_collapse` does not recognise `Div`.** It matches `Add`, `Sub`, `Sum`, `Mul` and never
   `Div`, so `div(x, c)` is not seen as a scale by `1/c`. Canonicalisation (§1) fixes this, and half
   the τ cancellation falls out of a pass that already exists.
5. **The cost model cannot see the catalogue, and models `exec::Interpreter` only** — blind to the
   reverse ladder (24.66%) and the calibration solve (~48%).
6. **`standard_passes` is called by the recorder, not by a compile entry point.** One engine call
   site, `src/solver/residual.cpp:46`; every other caller is a fixture. There is no `compile(tape)`
   front door for a new phase to be added to. Build one first.

### 7.3 Retracted

Stated plainly so they are not cited again:

* **D74's framing** of telescoping as creative telescoping (Gosper/Zeilberger). It is a first-order
  product recurrence with an elementary closed form.
* **D75's argument** that no e-graph can find telescoping by AC-closing products. True, and the
  wrong question — its measurements stand, the conclusion drawn from them does not. Note what D75
  itself concluded and which this document does NOT retract: *write* the term e-graph. §7.1.
* **D77's headline** of 2.82x, superseded by D78's in-process measurement: 1.49x at the same book
  size.
* **"The tape is the right layer"**, the steer given to the P1 term-rewriting design. It is both
  layers, with the division of labour in §3a.
* **E0 as a global contract.** Right below the pin, wrong above it, and wrong for transcendentals
  anywhere.

---

## 8. What is kept

The recorder and the tape. Domain IR inference, including `detect_chains`, which the recurrence work
needs. The executor, tiling, the catalogue and the planner. The mechanical adjoint, forward mode and
the implicit function theorem. The researched conventions, the blueprints and the Stage A fixture.
The differential tester and the mutation harness, which are tier-agnostic; only the default
tolerance moves. The fingerprinted performance discipline. `tools/compare` and `tools/h2h`.

That is roughly 21,500 lines on the execution path, all of it measured: the interpreter within 4.4%
of a hand-fused kernel, the adjoint at 2.9e-9 against finite differences, the IFT ladder at 5.16x a
specialist's on a thousand trades (D78).

**The telescoping spike is kept, and D74's instruction to delete it is withdrawn** (D81). It was
written to measure a prize the engine has now claimed, and its own header said to delete it at
that point. It turns out to be the only source of the accuracy comparison §5.5 rests on: showing
that the telescoped form is 39.7x closer to truth needs two INDEPENDENTLY WRITTEN forms evaluated
on `double`, and the engine's collapse happens below `Rec`, so it cannot supply the second one. It
also became the regression gate that says, in terms of the maths rather than of node counts, that
`simplify` still derives the collapse. Still test-only fixture code; §2 is unchanged.

The seven layout rules are **demoted, then deleted with the rest of §7.1**. They are cheap and
tested and worth 1.73x–1.88x on a workload with that structure; that workload is not the one we
have, and they can be rebuilt above a working pipeline if it ever is.

---

## 9. How this document and the ledger are maintained

The accretion is worse in the documents than in the code (owner, 2026-09-27). `DECISIONS.md` stands
at **4,293 lines and 73 entries**, and the rule that produced it — "changing a decision means
appending a new entry that supersedes it" — is an accretion machine by construction. Knowing the
current truth about the optimiser requires reading nineteen entries in order, of which one is
withdrawn and four are now retracted (§7.3).

**The split, from here:**

* **`DECISIONS.md` is a ledger.** Append-only, historical, the evidence and the reasoning as it stood
  at the time. It is cited, not read end to end. An entry is never edited after the day it lands,
  except to add a one-line pointer to whatever superseded it.
* **This document is the state.** Rewritten in place. It carries no history, no "formerly", no record
  of what each section used to say. If a principle changes, the old text goes — `git log -p` is the
  history.
* **`CLAUDE.md`'s Status paragraph is state too**, and comes under the same rule: rewritten to say
  what is true now, not extended with what was found most recently.

A decision entry that changes a principle lands in the same commit as the rewrite of this document.

---

## 10. Order of work

Steps 1–7 were built on 2026-09-27 and are recorded in D81. What they cost and what they returned:

1. **This contract.** *(done)*
2. **A compile entry point.** `epykos::compile(Tape&)` — the pin's front door. A pure refactor:
   the engine and eight fixture recording paths route through it, test callers of the individual
   passes do not. *(done, 91a44f8)*
3. **Quarantine §7.1** behind `EPYKOS_LEGACY_SEARCH`, default OFF. 12,126 lines left the default
   build; 104/104 tests pass with it off, 129/129 with it on. Nothing deleted. *(done, 824a628)*
4–6. **Canonicalise, simplify, solve recurrences — one pass of five peepholes**, not three stages.
   The engine derives telescoping: compare_ois 1,020,583 recorded nodes → **1,183** where the
   data-movement passes alone left 81,321, against 1,313 for the hand-written spike; Stage A
   517,036 → **174,388**. It costs 1.03–1.08x the old pass time and reaches its fixpoint in three
   rounds. §3a records why no recurrence solver was needed. *(done, 001845b and c8ed80a)*
7. **The residual bidirectional space, measured** rather than deleted — see §7.1. 549 factorable
   `Mul` pairs under a `Sum` at 16 trades, 15,865 at 256; and 40 `div(exp,exp)` sites where the
   obvious rule loses at all 40 because the exps are shared. That last is why the quarantined code
   is quarantined and not gone. *(done, ae7b66d)*

**Measured against a specialist, both engines in one process on one clock** (D81 §7): 161x faster
cold calibration, 150x warm, 36.5x on the risk ladder, every one of them better than the hand
spike. The risk ladder is **5.70x in our favour** where the engine as it wrote its own maths was
0.16x. Calibration is still **1.7x slower cold and 7.1x warm**, unchanged by the collapse — so the
arithmetic was never what that gap was made of, and what is left is solver algorithm.

What is next, in order:

8. **The solver gap — IN PROGRESS, and re-aimed 2026-09-29 (D85).** §4a lifted the exclusion on
   2026-09-28 for Jacobian reuse and warm-start state, and only those. D82 decomposed the gap: ~65
   us of a 74.9 us warm solve is one Jacobian build paid every call, against a 9.3 us tick on the
   other side. The first shape tried — an explicit caller-held solve cache — was built to §4a's
   gate, **measured 0.99x, and is reverted**. The shape now is a **lazy final Jacobian**: build it
   when the adjoint or the IFT asks, not at the end of every `run`. Measured 69.0 us → **11.4 us**
   on a re-quote with the ladder untouched, and it changes no iterate path, so §5.2 pays nothing
   and §4a's gate restates as a *bitwise* equality. Not yet implemented — D85 is the measurement
   and the design, not the landing.

   **LANDED 2026-10-03 (D89), and the measurement beat the design's own projection.** Paired
   before/after, same box, three alternating rounds at load 1.95–1.97: the h2h "hot"
   configuration goes **69.0–70.7 us → 15.1–15.5 us, 4.6x**, and the cold default 241 → 188
   (1.28x), which was not predicted. The ladder is unchanged and asserted *bitwise* through
   history independence. `final_jacobian = false` is 3.7 us SLOWER and correctly so: it was fast
   and wrong.
9. **Vector transcendentals**, under the §5.3 policy. 81 of the pinned tape's 1,183 nodes are
   `exp`, and it has never been tried.
10. **The arithmetic-dominated workload** of §6 — and note it is now also the only workload that
    would feed the quarantined layout rules, which find zero sites on both current fixtures.
11. **The term-level e-graph**, built against §7's measurement rather than a guess.
12. **Tier 2 opens**, with `ExpMode::poly` as its first citizen — blocked on its own entry
    criterion, that it become lane-width independent (D80).

**Not in this list, because it is not scheduled and the owner has not decided it (D87).** The
engine's own numerics are not recorded, and cannot be, because a derivative is not expressible as a
tape node. That is where D68 measures ~95% of an O4 lane to be and where D85's win was hiding. D87
§7 sets out three options — leave it and instrument by hand; build a derivative operator and
self-host the thesis; or give the cost model a solver (D68's recommendation 3). It is listed here
rather than numbered because its priority against steps 8–12 is a decision, not an ordering.

Nothing in this list rewrites anything below the pin, because nothing below the pin is implicated —
with one exception already taken: D81 §6(a), a pre-existing interpreter defect the collapse exposed.
