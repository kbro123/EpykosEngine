# PRINCIPLES — what the engine is allowed to do, and against what it is judged

Owner-set, 2026-09-25, after M4's post-mortem. This document is the contract the optimiser is built
against. It supersedes the unexamined assumption that governed M1–M4, which was that a rewrite must
be bit-identical to the naive recorded evaluation. That assumption was never stated as a principle;
it was a default that nobody questioned, and it silently restricted the optimiser to rewrites that
cannot remove work. See `docs/DECISIONS.md` for the entry that records this and the evidence behind
it.

Where this document and an older one disagree, this one wins.

---

## 0. The failure this exists to prevent

`include/epykos/maths/instrument/coupon.hpp` states that "nothing telescopes, unrolls or pre-folds by
hand (that is the engine's business, M4)". The pricing layer therefore hands the engine a naive
product loop and expects it to collapse. `docs/DESIGN.md` §6 then specifies the eight rewrites the
engine would use: fold uniform columns, bucket rows, elide trivial maps, push unary through gathers,
share reciprocals, group formation, materialise at boundaries, block linmap. **Every one is a
data-movement rewrite. Not one can collapse a product.**

Those two statements sat in two files for three days. The consequence, measured: the entire
plan-level dynamic range on the Stage A tape is 1.027x–1.042x, while the telescoping the coupon
header expects is worth about 250:1 on the calibration side.

The lesson is not "we picked the wrong rules". It is that **the space the search may explore was
never written down**, so nobody noticed it excluded the only thing worth finding.

---

## 1. Algebra first, estimation second

Two tiers, in this order, and the first is exhausted before the second is entered.

**Tier 1 — exact algebra.** Rewrites that are identities in ℝ: cancellation, telescoping,
reassociation, distribution and factoring, strength reduction, recurrence collapse. These remove
work at no cost in the mathematics. They still move floating-point results, and §4 governs by how
much.

**Tier 2 — deliberate approximation.** Rewrites that change the function: Taylor expansions,
polynomial replacements for transcendentals, moment approximations. Each spends accuracy for speed
and needs a validity domain.

**Tier 2 is CLOSED until tier 1 is proven end to end** (owner, 2026-09-25). `exec::ExpMode::poly`
therefore stays disabled, and `task_f7b9c87d` — the catalogue having no axis for which exponential a
kernel uses, so `poly` is silently a no-op whenever the catalogue is on — is reclassified from an
open defect to a deliberate deferral. It becomes tier 2's first citizen when that tier opens.

---

## 2. Discovery, not declaration

**The pricing maths declares no optimisation opportunity.** It is written as the definition reads:
a compounded coupon is the product loop of the definition, an average is the sum of the definition.
This is unchanged from the original design and is now a principle rather than an accident.

**The engine may hold general mathematical facts.** Not hints about coupons, not fixture-specific
knowledge — general truths, of the same standing as associativity. "A scan whose step multiplies by
a ratio of consecutive terms collapses to a ratio of its endpoints" is such a fact. It is about
scans, not about interest rates, and the engine is entitled to know it.

**The op set does not grow for this** (owner, 2026-09-25, choosing against the option that allowed
it). `Linmap`, `Affine` and `Sum` already carry the structures they carry; new general facts live in
the identity and rule set, expressed over the existing ops, not as new primitives.

The distinction that makes this workable: a fact about a *mathematical form* is discovery; a fact
about *this instrument* is declaration and is not allowed.

### 2a. What follows, and it is not free

A recurrence closed form is a proof about a recurrence, not a chain of local rewrites. Collapsing a
250-step scan by applying cancellation 249 times will exhaust any e-graph. So the engine needs a
rule KIND it does not have: one that analyses a detected scan's step and recognises it as a member
of a known recurrence class, rather than pattern-matching a term. The scan detection that would feed
it already exists (D41).

The recurrence library is small and enumerable for this domain. Product of consecutive ratios
(telescoping). Geometric. Arithmetic. Linear with constant coefficients. That list is the work, and
it is finite.

---

## 3. Scope: expression, then algorithm. Not the solver, yet

**In scope.** The arithmetic the engine was handed, and the algorithmic form of how a quantity is
obtained: a closed-form Jacobian for a linear block instead of a numerical one; the implicit
function theorem instead of bump-and-recalibrate. The reverse ladder is reachable under this and is
about a quarter of the recurring work.

**Out of scope for now.** The solver itself — Jacobian policy, iteration strategy, convergence
criteria. Roughly half the recurring work sits there and the optimiser currently cannot see any of
it. It is excluded because a solver change can alter convergence rather than a value, which is a
different and harder verification problem, not because it is unimportant. Revisit once tier 1 is
proven.

---

## 4. The error contract: per output class, against the expression at infinite precision

**Ground truth is the recorded expression evaluated without rounding** (owner, 2026-09-25). Not the
`double` evaluation of it, which is merely one realisation and has never had its own error
characterised. Not the "financially correct" answer either: the engine reproduces the maths it was
given, faithfully, and may not silently repair a poor discretisation. If the method is wrong, that
is a bug in the pricing code, not something the optimiser fixes.

The oracle is the templated maths instantiated at wider precision. That the maths is templated on
`Scalar` was decided on day one (D3) and is what makes this nearly free.

**Tolerances are per output class, not global** (owner, 2026-09-25). Valuation is held tight;
sensitivities are allowed more, because risk numbers tolerate error that P&L does not. The specific
numbers belong in `PROBLEM.md` beside the outputs they govern, not here.

**Bit-identity is demoted from an admission criterion to an execution property.** Three tiers:

| tier | what it covers | contract |
|---|---|---|
| structural | `expand(infer(tape))` reproduces the tape; signature canonicality | exact, unchanged |
| execution | interpreter, catalogue kernels and adjoint realise the compiled program | **bitwise**, unchanged |
| mathematical | rewrites of the recorded maths | characterised error against §4's oracle |

Only the third tier changes. The execution tier is where E0 earns its keep and is what makes
swapping in a specialised kernel safe; a difference there is still a bug.

**Reproducibility is a separate property and is kept.** Same build, same inputs, same answer, every
time. It does not require agreeing with the naive ordering, and it is what people usually mean when
they ask for bit-identity.

---

## 5. What the optimiser is graded on

**An arithmetic-dominated workload is added** (owner, 2026-09-25): Monte Carlo exposure or XVA, long
elementwise chains over large arrays. No current fixture has that shape, and it is the shape the
engine's whole design targets. Note the sequencing consequence: that workload is Stage D, currently
the LAST milestone in `PROBLEM.md` §8. It moves forward.

**Stage A is kept, as the realism and correctness gate, not as the optimiser's scoreboard.** It is
researched, cited and end to end. It is also dominated by a calibration solve, so the optimiser
touches about 2.5% of it and grading optimisation on it measures the workload rather than the
engine.

---

## 6. Structures that limited the search, and are to be removed

Audited 2026-09-25 against the code, not recalled.

1. **A rewrite must return a whole `ir::Program`.** `rewrite::Proposal` holds an optional Program.
   A rule may POINT at a step (`SiteKind::Step` exists) but cannot REPLACE a term. Every match
   materialises an entire program, so identities that match everywhere are combinatorially
   impossible. Measured: without its structural floor, R2 produced 10,111 site-matches and blew the
   e-graph's node bound in two rounds after 700 s. **This is the blocker.**
2. **The e-graph's e-nodes are whole Programs** — the same root cause, already recorded in D62 as
   the known limitation with a term-level e-graph named as the proper fix.
3. **No algebraic identities exist anywhere**, in the op set, in a tape pass beyond `affine_collapse`,
   or in a rule.
4. **The oracle is the naive `double` path**, so a rewrite that is MORE accurate fails the gate.
5. **The cost model cannot see the catalogue.** One mention, in a comment, about a future consumer.
   So the search prices every candidate as if it runs on the generic interpreter and is blind to the
   1.11x–1.16x that catalogued shapes get — the only measured win the project has.
6. **The cost model models `exec::Interpreter` only**, so it is blind to the reverse ladder (24.66%)
   and the calibration solve (~48%).

**Not a blocker, contrary to expectation:** the cost model's own arithmetic pricing. Its fit puts
essentially all predicted time into the per-op rates, which is exactly why it is useless for layout
(every data-movement coefficient fits to zero) and roughly right for algebra. Removing 249 multiplies
of 250 would register strongly. The instrument is wrong for the work that was done and adequate for
the work that should be.

---

## 7. What is kept

The recorder and tape passes. Domain IR inference. The executor, including tiling and the catalogue.
The mechanical adjoint, forward mode and the implicit function theorem. The researched conventions
and the Stage A fixture. The mutation harness, which is tier-agnostic and unaffected. The
fingerprinted performance discipline.

The seven layout rules are kept and **demoted**: they are cheap, tested and worth 1.73x–1.88x on a
workload that has that structure. They stop being what the optimiser IS.

---

## 8. Order of work

1. This contract, and the oracle. Nothing downstream can be judged before both exist.
2. Term-level rewriting: a proposal that replaces a sub-term, e-classes over terms.
3. The identity set, and the recurrence rule kind of §2a.
4. Telescoping, end to end, as the worked example — it exercises every part of this contract and is
   worth 250:1.
5. Teach the cost model the catalogue and the adjoint.
6. The arithmetic-dominated workload of §5.
7. Tier 2 opens, with `ExpMode::poly` as its first citizen.
