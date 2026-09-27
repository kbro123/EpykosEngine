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

## 4. Scope: expression, then algorithm. Not the solver, yet

**In scope.** The arithmetic the engine was handed, and the algorithmic form of how a quantity is
obtained: a closed-form Jacobian for a linear block instead of a numerical one; the implicit
function theorem instead of bump-and-recalibrate.

**Out of scope for now.** The solver itself — Jacobian policy, iteration strategy, convergence
criteria. A solver change can alter convergence rather than a value, which is a different and harder
verification problem. Revisit once tier 1 is proven.

**Named exception, on evidence.** D78 measured warm recalibration at **7.1x slower than a
specialist** — 114 µs against 16 µs, flat in book size, with our calibration Jacobian rebuilt on
every call. That is a pure engine-against-engine loss and the clearest single target in the
repository. It is solver work and therefore excluded here. Recorded so the exclusion is a decision
and not an oversight.

---

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
bits, every run. What is given up is agreement with the *unoptimised* form, which pinned one
arbitrary rounding sequence as though it were the answer. Reproducibility is what people mean when
they ask for bit-identity, and it is kept.

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

1. **This contract.** Nothing downstream can be judged before it exists. *(done, 2026-09-27)*
2. **A compile entry point.** `standard_passes` is called by fixtures; there is no front door for a
   phase to be added to (§7.2 item 6). Build it, and route every existing caller through it.
3. **Quarantine §7.1** behind a default-OFF flag. One commit.
4. **Canonicalise.** `div(x,c) → mul(x,1/c)`, `sub → add·neg`, commutative operand ordering,
   constant folding. Cheap, and it makes the existing `affine_collapse` more capable for free.
5. **Simplify.** Value numbering plus algebraic identities and cancellation, iterated with CSE to a
   fixpoint. Measured to fire 94,942 times on the real tape before any recurrence work at all (§3a).
6. **Solve recurrences.** The closed-form rule kind on detected scans, product-of-consecutive-ratios
   first. Telescoping is the worked example and the end-to-end proof: it exercises every part of
   this contract and is worth 33x–132x (D78).
7. **Measure the residual bidirectional space**, now that directed rewriting has taken everything
   it can. Factoring against distribution, reassociation, association-dependent CSE. That
   measurement is what a term-level e-graph (D73's `algebra/egraph.hpp`) is built against, and it
   is also what decides whether the quarantined §7.1 code is deleted or partly revived.
8. **Vector transcendentals**, under the §5.3 policy. Likely the largest remaining win below the pin
   and currently untried.
9. **The arithmetic-dominated workload** of §6.
10. **Tier 2 opens**, with `ExpMode::poly` as its first citizen.

Steps 4–6 are the front half of the compiler. Steps 3 and 7 are the cull, and step 7 is a
measurement before it is a deletion. Nothing in this list
rewrites anything below the pin, because nothing below the pin is implicated.
