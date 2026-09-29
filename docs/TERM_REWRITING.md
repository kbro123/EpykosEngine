# TERM_REWRITING — symbolic algebraic reduction on the tape

Design package P1, 2026-09-25. Implements `docs/PRINCIPLES.md` §8 item 2 ("a proposal that replaces
a sub-term, e-classes over terms") and §8 item 3 (the identity set and the recurrence rule kind of
§2a). Decision entry: **D73**.

> **This is a DESIGN. Nothing here is implemented.** The deliverable is this document plus interface
> headers — `include/epykos/algebra/{term,rule,error,recurrence,egraph,reduce}.hpp` and
> `include/epykos/ir/analysis.hpp` — so the shape can be reviewed before roughly six thousand lines
> are written against it. One `.cpp` ships (`src/algebra/names.cpp`, six `to_string` tables) so the
> enumerations are linkable and testable. No existing rule, pass, cost model or e-graph is touched.

> **Written against `PRINCIPLES.md` §4 as amended at `f8849b4`, which retires bit-identity as a
> contract anywhere.** Two tiers and a test: structural is exact because it is a graph identity;
> mathematical is characterised error against the oracle and now covers every execution path too.
> Nothing here admits a rewrite *because* it preserves bits. §3.6 is what that retirement buys,
> measured. The oracle is real as of today (`epykos::Wide`, D72), and §5 is written against its
> numbers rather than against a hypothesis.

Throughout, **design** means a proposed mechanism, **assertion** means a claim I have not measured,
and **measured** means a number from this repository's own records with its citation. §9 separates
them again and §8 names the experiments.

---

## 0. The one-paragraph summary

**The term graph already exists and it is the tape.** `tape/tape.hpp` is a straight-line program in
topological order, already hash-consed by `cse`, already rewritten in place by `tape/passes.cpp`,
and `affine_collapse` is already an algebraic simplification on it. So this package adds a stage:

```
record -> tape passes -> SYMBOLIC ALGEBRAIC REDUCTION -> ir::infer -> plan -> execute
```

A rewrite replaces one tape node's definition. E-classes are over tape nodes, which is the textbook
case. The identity set is thirty axioms over the existing twenty ops, in three buckets, with
`Op::Sum` and `Op::Affine` deliberately excluded from associative-commutative matching. The
recurrence kind of §2a proves a telescope by **node-id identity** — `cse` has already merged equal
computations, so "step k's denominator is step k+1's numerator" is an integer comparison, not a
tolerance. Extraction minimises cost subject to a per-output-class error budget, composing error by
the adjoint the engine already has. Saturation is bounded by four mechanisms, of which the load-
bearing one is saturating over one representative per **shape class** — the same deep-hash partition
that turns 517,036 tape nodes into 67 domains. Nothing below the stage changes: inference, planning
and execution receive a smaller tape and run exactly as they do now, and with an empty rule set the
stage is the identity.

Two findings outrank the design, and both are in §8: the recurrence work does not depend on the
e-graph and should be attempted first (§8.1), and **the ~48% of Stage A where the telescoping win
lives is code no rule is applied to** (§8.2).

---

## 0.1 What this document previously said, and why it was wrong

The first two versions of this design targeted **`ir::Program`** and proposed a term-level e-graph
over it, replacing `rewrite::Proposal`, `MatchSite` and `optimise::EGraph`. That was one stage too
late, and recording the correction is more useful than pretending it was always obvious.

**Why it was wrong.** `ir::infer` groups structurally identical nodes into classes and domains, so a
step applies to every row of a domain at once. At that layer there are no terms left to rewrite,
only arrays — which is the real reason `Proposal` has to return a whole `ir::Program`. **That is not
a flaw in the interface. It is the interface being correct for the layer it sits at.** `Proposal`,
`MatchSite` and the program tier are not replaced by this package and need no apology.

**What the wrong layer forced into the design, all of which is now deleted:**

| carried at the IR layer | at the tape |
|---|---|
| an **anchor domain** in every e-node's identity, because a term was a row-indexed *vector* and two structurally identical terms in two domains denoted different values | gone: a node is one scalar |
| **row-universality** — "a term rewrite applies to every row and a rule can never rewrite a subset" — and the whole argument that row-subset algebra must be a domain split first | gone as a constraint. It reappears as a driver *optimisation* a rule opts into (§2.3), which is a strictly better place for it |
| **index maps**: a `Gather` is a per-row value id, so crossing a domain boundary meant composing with an index map, and `gather_push_unary` / `gather_push_binary` had to exist as rules with a quarantined saturation phase | gone: an operand is a node id. Two axioms deleted, one saturation bound deleted |
| a **three-tier migration** (term / program / plan) and a wrapper strategy for the seven layout rules | gone: one new stage, one call site, nothing below it changes |

**The one argument that has to be retracted rather than simplified.** The IR-layer draft argued
tractability from the 517,036 → 67-domain collapse: "the sharing has already happened before the
optimiser is reached, so the term graph is ~10³ nodes". At the tape layer **that collapse has not
happened yet — it is the thing we run before**, and the seed is the full 517,036 nodes. §2.3 is the
replacement argument, and it is weaker: it uses the same repetition, but as an *optimisation the
driver applies* rather than as a property of the input.

**What transferred unchanged**, because it was never about the layer: the error model and its
adjoint-weighted composition (§5), the identity set minus the two gather axioms (§3), the refusal to
AC-match `Op::Sum` (§3.4), the saturation bounds (§6), and both of §8's findings. The recurrence
kind transferred and got *better*: at the IR layer it could not be a term rule at all, because a
closed form has one value per chain while a scan domain has one row per step; at the tape a chain is
a chain of nodes and collapsing it is ordinary (§4.1).

---

## 1. What a rewrite is, at the tape

### 1.1 A term is a node

`tape/tape.hpp`, in its own words: *"A Tape is a straight-line program in topological order: node i
only reads nodes < i. Constants are leaves, one node per distinct bit pattern. Variadic operands
live in a side array."* That is a term DAG. `cse` already hash-conses it — nodes with the same op,
operands (commutative operands in canonical order) and constant bit pattern are merged — and
`tape/passes.cpp` already rewrites it in place. `affine_collapse` is already an algebraic
simplification on it, and per `docs/PRIOR_ART.md` it is what recovers SwapEngine's hand-built
W-cache exactly, 857 rows.

So there is nothing to invent: `TermRef == node_id`, and the term is the sub-DAG under it.

### 1.2 The replacement

`algebra::Rewrite` is an e-class id plus an `Expr`: a small node arena whose operands may be other
arena nodes, **e-classes the match bound** (pattern holes — `mul(a, add(b,c)) → add(mul(a,b),
mul(a,c))` names a, b and c three times and materialises nothing), an existing node verbatim, or a
new constant. New constants are interned through the tape's own one-node-per-bit-pattern rule, so
`+0.0` and `-0.0` stay distinct exactly as `Tape::constant` makes them.

`site` and `replacement` are an **equality claim**, not an instruction: the driver unions them and
keeps both. Extraction chooses later.

### 1.3 What the layer gives, and what it takes away

**Gives: `Node::tainted`.** Computed at record time — "depends on an Input". An untainted sub-DAG is
a compile-time constant and may be *folded at rewrite time*. That is a capability the IR layer does
not have in this form, `affine_collapse` already keys on it, and it is why `fold_untainted` is in
the axiom set and in `axiom_improves_accuracy`: replacing a computed value with its exact constant
removes rounding as well as work.

**Gives: cheap, exact structural side conditions.** `cse` means equal constants are *one node*, so
"are these two coefficients equal?" is an integer comparison. §4.2 leans on this hard.

**Takes away: array-level side conditions.** At the IR layer, "x > 0 for every row" was a scan of a
`std::vector<double>` sitting in the Program — exact, cheap, and *strong*. At the tape there is no
array of values, only structure and constants, so `provably_positive` is weaker: it can discharge
`exp(anything)`, `x*x`, a positive Const, and not much else. Conditional axioms (`log_product`,
`sqrt_product`, `exp_log`) will fire less often here than the IR-layer draft assumed. That is a real
cost of the placement, it is not recoverable at this layer, and §8.7 records it as an open risk.

### 1.4 Composing with the passes that already run

`standard_passes` is cse, dce, fold_sum, affine_collapse, dce. The stage runs after all of it:

- **`cse` is this graph's hash-consing, already done.** Seeding from a post-cse tape means no two
  seeded nodes are congruent, so the initial e-graph is already canonical. This is a real saving and
  it is why the stage goes *after* the passes rather than replacing them.
- **`dce` is how a win is realised.** Collapsing a chain does not delete the nodes it replaced; it
  makes them unreachable. The stage ends by re-running cse then dce, and the measured shrink is the
  deliverable.
- **`fold_sum` and `affine_collapse` have already normalised the shapes rules match against**, which
  is a convenience — a rule can expect `Sum`/`Affine` rather than left-deep Add chains. It is also a
  hazard: `affine_collapse` has already rewritten `1 + f·w` into an `Affine` before the recurrence
  classifier sees it, which is why (P1) in §4.2 says "modulo the affine rearrangement the recorder
  and `affine_collapse` left behind". A classifier written against the un-collapsed shape will
  match nothing, and that is the most likely way the first implementation fails.

---

## 2. E-classes over tape nodes

### 2.1 The ordinary case

An e-node is `(op, child e-classes, constant, variadic operands)`. An e-class is a set of e-nodes
asserted equal. Hash-consing makes two nodes with equal content one node; congruence is the usual
closure, maintained by deferred canonicalisation — a whole round's matches are queued, then one
rebuild hash-conses and unions them all. That is egg's pattern (Willsey et al. 2021), it is what
`optimise::EGraph` already does at its own layer, and none of it is novel here. **An e-graph over
tape nodes is the ordinary thing rather than the exotic one**, which is the point: the interesting
content of this package is the rules (§3), the recurrence proof (§4), the error model (§5) and the
bounds (§6), not the graph.

### 2.2 D62's mechanisms

D62 built an application memo, representative-only matching and a re-firing policy around
whole-`Program` e-nodes. At this layer:

- **The memo survives, re-keyed** to `(e-node, rule, binding)`. E-nodes are immutable under
  hash-consing; *classes* merge, so a class-keyed memo would go unsound on the first union. Bindings
  are canonicalised to representatives at lookup, so a merge costs redundant work, never wrong work.
  Sound for exactly D62's reason: rules are pure.
- **Representative-only matching is true by construction.** Hash-consing leaves no congruent
  duplicate to skip.
- **`RefirePolicy` is not needed and is not ported.** It existed to bound D62's cause (B) — *"k
  independent local edits cost k whole clones and, over the rounds, 2^k of them, because this is a
  whole-program-valued e-graph"*. At the tape, k independent edits are k choice points: O(k) nodes.
  The completeness D62 booked away — *"any genuine optimum that needs two different structural
  rewrites composed after both have already fired once is no longer reachable at all"* — is simply
  not given up here. **The problem is bypassed rather than solved**, and D62's own note that "a real
  term-level e-graph is the proper fix" is satisfied by moving layers, not by rebuilding that graph.

### 2.3 Scale: the honest version

**Measured** (D44, M3-gate-1): the Stage A tape is **517,036 nodes** after the E0 passes, down from
27,459,283 recorded. That is a large seed, and §0.1 records that the IR-layer draft's tractability
argument does not transfer.

The replacement argument uses the same repetition differently. The tape is repetitive — that is
*why* `ir::infer` finds 67 domains in it — so the deep-hash partition puts those 517,036 nodes into
a few hundred distinct **shapes**. The driver saturates over **one representative per shape class**
and applies the winner to every member. A rule opts in with `applies_class_uniformly()`, which is
true for every axiom in §3 because an axiom is a fact about a term's shape, and false for a rule
whose side condition depends on a particular node's constants.

Two things are worth naming precisely:

1. **This is the IR layer's row-universality, reappearing as a choice.** At the IR layer the domain
   structure *forced* every rewrite to apply to every row. Here the driver *elects* to exploit the
   same repetition, and a rule that cannot take it is matched node by node instead. Same physics,
   better place to put it — and it is evidence that row-universality was never an artefact of the
   IR layer but the structure of the problem.
2. **The numbers are not measured and I am not going to pretend otherwise.** "A few hundred shapes"
   is inferred from the 67-domain count, not counted. And what the graph saturates *to* is a
   different question from what it seeds at: associativity and distributivity over a ten-node term
   is the shape that grows fastest and nobody has run it. What *is* structural is that the size is
   (number of shapes) × (per-shape saturation) and the second factor does not depend on 517,036.
   §8.9 is the experiment that would put a number on both.

---

## 3. The identity set

Thirty axioms, enumerated in `algebra::Axiom` (rule.hpp) so the list is readable in one place and a
test can assert the enum and this table have not drifted apart (`tests/algebra/interface_test.cpp`).
Three buckets. Two axioms from the IR-layer draft are **gone, not deferred** — `gather_push_unary`
and `gather_push_binary` — because the tape has no gathers; `r4a` remains a perfectly good IR-layer
rule and is unaffected. One is **new**: `fold_untainted`, which only this layer makes available.

**Read the buckets as a statement about floating-point behaviour, not about admissibility.** Since
`PRINCIPLES.md` §4 retired bit-identity, an axiom in §3.1 has **no privilege** over one in §3.3:
both are admitted or refused by their composed error against the output class's budget. The split is
kept because it is true and useful in two narrower ways — the exact ones can be asserted for free as
bug detectors (§4: "a test of convenience, never a constraint"), and the §3.2 conditions name an
obligation a rule must discharge *before it may fire at all*, which is a soundness question and not
a rounding one.

### 3.1 Exact in IEEE-754 double, unconditionally

| axiom | statement | ops | note |
|---|---|---|---|
| `neg_neg` | `neg(neg(x)) = x` | Neg | negation is a sign flip; exact for every double including NaN and ±0 |
| `sub_as_add_neg` | `sub(a,b) = add(a, neg(b))` | Sub, Add, Neg | exact: the negation is exact and the addition is the same operation |
| `neg_sub` | `neg(sub(a,b)) = sub(b,a)` | Neg, Sub | |
| `mul_one` | `mul(x,1) = x` | Mul | |
| `div_one` | `div(x,1) = x` | Div | |
| `select_same_arms` | `select(p,x,x) = x` | Select | |
| `select_push_unary` | `f(select(p,a,b)) = select(p, f(a), f(b))` | Select + any unary | exact because `Select` *picks*; D5 records both arms regardless. Can be a pessimisation (two `exp`s) or an optimisation (hoisting) — the cost model decides, which is the point of putting it in an e-graph rather than a greedy pass |
| `commute_add_mul` | operand order of `Add`, `Mul`, `CmpEq` | | `op_is_commutative` asserts this bitwise, and `cse` already canonicalises it on the tape |
| `fold_untainted` | an untainted sub-DAG → one `Const` leaf | any | the tape's own gift: `Node::tainted` is computed at record time, so a sub-DAG that does not depend on an Input has a value *now*. Exact by construction (the constant IS the value), removes work **and** rounding, which is why it is in `axiom_improves_accuracy`. `affine_collapse` already keys on untaintedness; this generalises it |

### 3.2 Admissible only once a row-universal side condition is discharged

Exact once discharged, except the five transcendental entries at the foot of the table
(`log_product`, `log_recip`, `exp_log`, `log_exp`, `sqrt_product`), which are also inexact (§3.3) — the
condition governs whether the axiom may fire at all, the `ErrorTerm` governs what firing costs, and
the two are independent.

These are the ones a careless implementation gets wrong, and each names its obligation.

| axiom | statement | obligation | why |
|---|---|---|---|
| `add_zero` | `add(x,0) = x` | `provably_not_negative_zero(x)` | `(−0) + 0 = +0 ≠ −0`. And the engine **can** observe the difference: `recip(−0) = −inf`, `sqrt(−0) = −0`, `div(1,−0) = −inf`. Comparisons cannot see it (`−0 == 0`), which is why this is easy to miss |
| `sub_zero` | `sub(x,0) = x` | same | |
| `mul_zero` | `mul(x,0) = 0` | `provably_finite(x)` | `inf·0 = NaN`; and the sign of the zero depends on the sign of x |
| `sub_self` | `sub(x,x) = 0` | `provably_finite(x)` | `inf − inf = NaN` |
| `div_self` | `div(x,x) = 1` | `provably_nonzero` **and** `provably_finite` | `0/0` and `inf/inf` are NaN |
| `affine_drop_zero_coef` | drop an `Affine` member with `c_i = 0` | `provably_finite` of that member | same reason as `mul_zero` |
| `log_product` | `log(a·b) = log a + log b` | `provably_positive` on both | inexact as well (see §3.3); the condition is separate from the error |
| `log_recip` | `log(1/a) = −log a` | `provably_positive` | |
| `exp_log` | `exp(log a) = a` | `provably_positive` | |
| `log_exp` | `log(exp a) = a` | no overflow in `exp(a)` — a Column/Literal range check | |
| `sqrt_product` | `sqrt(a)·sqrt(b) = sqrt(a·b)` | both provably non-negative | |

### 3.3 Identities in ℝ, not in float: each carrying an `ErrorTerm`

`add_assoc`, `mul_assoc`, `mul_distrib_add` (and the factoring direction, which is the valuable one),
`div_as_mul_recip` / `mul_recip_as_div`, `fma_contract` / `fma_expand`, `recip_recip`, `exp_product`,
`exp_sum_segment`, `sum_factor_common`, plus the conditional ones in §3.2 that are also inexact.

Two of these deserve calling out because under the M1–M4 contract they were *failures* and under
`PRINCIPLES.md` §4 they are **improvements**:

- **`fma_contract`** (`add(mul(a,b),c) → fma(a,b,c)`): one rounding instead of two. Faster *and*
  more accurate. Already exists as a rule and was E1-classified, i.e. barely admissible under the old contract.
- **`mul_recip_as_div`** (`mul(a, recip(b)) → div(a,b)`): one rounding instead of two. This is R4b's
  trade **run backwards**. R4b exists to share a reciprocal and is measured worth ~175 µs at B=64 on
  the M1 book (D27's profile: "forwards 11%, two IEEE divisions per element"). So the two directions
  are a genuine speed/accuracy trade-off and the search should hold both and let the budget choose —
  which is precisely what §5 is for.

The largest single win in the list is **`exp_product`** / **`exp_sum_segment`**:
`Π exp(x_i) = exp(Σ x_i)` turns *n* transcendentals into one. **Measured** relevance: `exp` was 50–54%
of the M1 book's B=64 time (D27, P6 attempt 5: "libm exp 54% (~855 µs)"), and the Stage A tape's
domain 5 is `exp(mul(neg(@0),$0))` with 16,917 rows. Whether the *shape* `Π exp(·)` actually occurs
there is §8's question, not an assertion here.

### 3.4 Where an axiom is NOT safe: `Op::Sum` and `Op::Affine`

**`Op::Sum` is a fixed-arity left fold in operand order** — `((x₀ + x₁) + x₂) …`, stated in
`ir/program.hpp`'s own evaluation contract and in the `Op` enum's comment. `Op::Affine` is the same
with coefficients. Three independent reasons not to put associative-commutative matching on them,
any one of which is sufficient:

1. **The error is unbounded and the family is infinite.** Reordering a Sum's members changes the
   rounding by an amount nothing bounds. "AC on Sum" is not one identity; it is an infinite family of
   inexact rewrites with unbounded error, since a sum of
   mixed-magnitude terms can lose everything to cancellation under one order and nothing under
   another.
2. **D61 decided this already, in the other direction.** It canonicalised a *commutative step's* two
   operands and deliberately did *not* canonicalise a Sum's member order. Introducing AC matching
   would silently reverse that decision and reopen exactly the class of compiler-dependent defect
   `task_919ea449` was.
3. **There is no bound to set.** The Stage A leg sums have segments of hundreds of members. An
   *n*-member AC term has *n*! orderings and Catalan(*n*−1) associations. A depth bound does not help;
   a node bound just means the graph fills with permutations of one sum and finds nothing.

So `TermSaturationLimits::ac_matching_on_sum` exists, defaults to `false`, and is gated by a test —
the field is there so that a future package must *change a default* rather than *discover an
absence*, which is the failure mode `PRINCIPLES.md` §0 exists to prevent.

What *is* allowed on a Sum is the one rewrite that preserves the fold order exactly:
**`sum_factor_common`**, `Sum(c·x₀, …, c·xₙ) = c · Sum(x₀, …, xₙ)`, removing *n* multiplies. It is inexact
(the products round differently) but its error is bounded and it reorders nothing.

### 3.5 What the frozen op set costs, named

`PRINCIPLES.md` §2 is explicit: "The op set does not grow for this." Working the identity set through,
here is the bill, which the owner asked to see:

- **No `Abs`.** `sqrt(x·x) = |x|` is inexpressible. So is `max(x,0)`, so is any identity whose normal
  form involves an absolute value. Workaround: `select(cmpge(x,0), x, neg(x))` — three ops and a
  branch, so the rewrite is usually a pessimisation and will not be selected.
- **No `Pow`.** This is the expensive one, and §4.4 quantifies it: two of §2a's four recurrence classes
  need `rⁿ`, which must be spelled `exp(n·log r)` — legal, but inexact, requiring `provably_positive(r)`,
  and turning ~*n* multiplies into one `exp` plus one `log`. On the measured libm cost that is a
  small constant-factor win, not an asymptotic one.
- **No `Log1p`/`Expm1`.** The forward rate's `(DF_s/DF_e − 1)` is a catastrophic cancellation with a
  standard fix that the op set cannot express. It is also tier 2 (accuracy, not speed), so it is
  closed anyway.
- **No `Min`/`Max`, no `Rsqrt`.** No identity in the current set needs them.

None of this blocks the telescope, which needs only `Div` — and that is not a coincidence, it is
*why* the telescope is the one class with an asymptotic win.

---

### 3.6 What retiring bit-identity buys, audited against the code

`PRINCIPLES.md` §4a costs the retirement at **14 engine sources, 37 test files, ~9,700 lines and 21
lines of build routing** for the `-ffp-contract=off` / `*_e0` machinery alone. That is the visible
bill. The invisible one is more interesting, because it is *complexity inside working rules that
exists for no mathematical reason*. Audited against the code, not recalled:

**1. `r2.bucket_rows` expands its entries in place instead of appending at the tail** — the clearest case, and
the one D52 already wrote down. D52 finding 3, measured: the first implementation did the
simpler thing (compact the split domain's `Column`/`Gather`/`Segment` entries away and append the
bucket's at the tail). It passed every forward and round-trip gate and **failed the adjoint gate by
36–68 ulps on 2 of the M1 book's 5 matching domains, with bit-identical forward values.** The cause
is not in R2 at all: `adjoint::build_plan` builds each value's reader list by scanning
`Program::gathers` / `segments` in **array order**, so a value read by both a split domain's gather
and an unrelated one accumulates its adjoint in that scan order — move the entries and you reorder a
floating-point sum. Same total, different bits. `expand_owned` therefore replaces each owned entry
*at its own array position*, and D52 flags this as "the finding future rewrites that add or split
Column/Gather/Segment entries should know about". **Under the amended §4 that whole constraint
evaporates**, and with it a standing tax on every future structural rewrite: `detail::insert_domain_after`
and `merge_producer_into_consumer` (`ir_edit.hpp`) both document "insertion never drops or reorders a
gather/segment" as an invariant they preserve. They can stop.

**2. The adjoint's accumulation order is fixed and documented as a contract.** `adjoint.hpp`: "every
operation is per (row, lane) with no cross-lane or cross-row reduction and no dependence on the tile
boundaries, so a lane of a batched run is **bitwise** the B = 1 run of that state and every tile /
lane_tile gives the same bits (the E0 tests assert both)". That is a real constraint on kernel
design — it forbids any reduction whose order depends on the lane width, which is the natural way to
vectorise a reduction. Under §4 it becomes a tolerance test between the batched and unbatched paths,
which is exactly the "slow and fast paths agree" shape. This is the single largest *performance*
constraint the retirement lifts, and it is unmeasured: nobody has costed what a lane-width-dependent
reduction order would buy, because it was not allowed.

**3. `bucket_split_edit::all_bit_identical` compares IEEE bit patterns rather than values**,
"STRICTER than `==` on purpose — `+0.0` and `-0.0` compare equal under `==` but are not the same
value to fold into one literal, since `1.0 * -0.0` and `1.0 * 0.0` are not the same bits". Under §4
the fold is admissible and its error is exactly zero except at `-0`, where §3.2's own analysis
applies. The strict comparison can stay as a cheap conservative test; it no longer has to.

**4. `r4b.shared_reciprocal` is held back by a bit argument, not an error argument.** Its header:
`a / b` is "not bit-identical to `a * (1/b)` in general — DESIGN.md §12's own residual, two IEEE
divisions per element". The M1 profile prices that residual at **~175 µs of 1,584 µs at B=64, about
11%** (D27, "forwards 11%, two IEEE divisions per element"). Under §4 this is an ordinary
speed/accuracy trade priced by the propagator, and — note the direction — the *reverse* rewrite
(`mul_recip_as_div`) is an accuracy *improvement*. The search should hold both and let the budget
choose. That is 11% of the M1 kill-path that was not previously on the table.

**5. `planner_rules.hpp` requires "the default plan must be bit-identical and time-identical to
M1's".** A pipeline-shape constraint on the planner, inherited from PROBLEM.md §7. Time-identical is
still worth wanting; bit-identical is not, and it is the clause that forces the planner rules into
the exact dependency order the old greedy pass ran in.

**6. `rewrite::verifier` derives a bitwise tolerance from an all-E0 history** (`verifier.hpp`: "a
bound derived from `history`: bitwise (E0) when none of…"). That is the admission criterion §4
retired, wired into the gate. It becomes the oracle comparison plus a path-agreement check.

**What the retirement does *not* buy, stated so the claim is not overread.** It does not relax
§3.4's refusal to AC-match `Op::Sum`. The objection there was never "it changes bits" — it is that
the error is *unbounded* (a sum of mixed magnitudes can lose everything to cancellation under one
order and nothing under another) and the family infinite (n! orderings). A contract based on
characterised error refuses that even more firmly than one based on bits, because there is no bound
to state. It also does not relax the structural tier: `lower(graph, initial) == input` stays exact,
and costs nothing, because it is a graph identity with no arithmetic in it.

---

## 4. The recurrence rule kind (`PRINCIPLES.md` §2a)

### 4.1 Where chain detection comes from — the main structural question in this design

At the tape a chain is a chain of **nodes**, and collapsing it replaces a sub-DAG with a smaller
sub-DAG. Ordinary. `ir::infer` then simply does not find a scan there, because there is no longer a
chain to find. (The IR-layer draft concluded the recurrence kind could not be a term rule at all,
because a scan domain has one row per *step* while a closed form has one value per *chain*; that was
correct for that layer and is moot at this one. §0.1.)

But the stage must know a chain is there, and the tape does not say. `Inference::detect_chains`
(`src/ir/signature.cpp:408`) does exactly that job over tape nodes — and **it is not a function of
the tape.** Audited: it runs as phase five of `Inference::run`, after `compute_uses`,
`initial_boundaries`, `compute_deep` and `promote_shared`, and it both *reads* `boundary_` (it walks
"stopping at boundaries", D41) and *writes* it (marking each chain step and the chain's initial
value as boundaries, clearing the inner nodes). It is a function of **(tape, boundary set)** that
**mutates the boundary set**. That coupling is the finding and everything else follows from it.

The same prefix also produces the second thing the stage needs: the deep-hash partition that §2.3's
class-uniform saturation rests on. Both wanted things come out of one place, which is why a clean
answer exists.

**Three options.**

| | what | verdict |
|---|---|---|
| (a) | **Factor the prefix out** and let both consume it | **recommended**, in the cheap form below |
| (b) | **Duplicate** a weaker detector using only use counts, not inference's promoted boundaries | rejected: it would find a *different* set of chains than `infer` will, with no test able to say which is right. D41's rules are subtle enough (a cut under an Add whose target is single-use is a reduction and never a scan; the carry searched at most four operands deep; three steps minimum) that a second implementation drifts |
| (c) | **Run `ir::infer` twice** — once as analysis, rewrite, then again for real | correct and wasteful: Stage A inference is ~1.0 s of a 7.8 s record, and `Program` carries no node-id backlink, so mapping scan domains back to tape nodes needs new plumbing anyway. If plumbing is needed regardless, (a) is the better place for it |

**The cheap form of (a), which `include/epykos/ir/analysis.hpp` proposes.** Do not refactor
`Inference` into pieces. Add **one** entry point, `ir::analyse(tape)`, that runs the existing phases
up to and including `detect_chains` and returns what they found, leaving `infer` byte-for-byte
unchanged and still computing everything from scratch. `analyse` and `infer` then share an
implementation by construction rather than by discipline.

**There is no staleness risk**, and it is worth saying why: the stage *rewrites* the tape, after
which `infer` recomputes uses, boundaries, hashes, chains and domains from the rewritten tape as it
always has. The analysis is **advisory input to the rewriter**, never a contract the rewritten tape
must honour. A chain the rewriter collapses is simply not there the second time.

**What it costs.** One extra pass of the analysis prefix per compilation, plus the risk that a
future change to `Inference`'s phase order silently changes what `analyse` returns. The second is
the real one, and the mitigation is that it is the *same code*: a change that breaks `analyse`
breaks `infer` too, and `infer` has M1/M3 round-trip gates on it.

### 4.2 How a match is PROVED rather than pattern-matched

`maths/swap/compounding.hpp` records, and says so in its own header:

```
acc_{k+1} = acc_k · (1 + f_k·w_k),     f_k = (DF(t_rate_k) / DF(t_next_k) − 1) / τ_k
```

*"with r_i = (DF(s_i)/DF(e_i) − 1)/τ_i it collapses to DF(s)/DF(e) on paper"*. On paper. Whether it
collapses in **this recording** is three facts, all decidable exactly from the tape:

- **(P1) Shape.** The step is `mul(carry, X)` and X reduces to `div(num, den)` modulo the affine
  rearrangement the recorder and `affine_collapse` left behind (§1.4 — this is the part most likely
  to be got wrong first). Ordinary matching, and the weakest of the three.

- **(P2) The index condition — the telescope itself.** Step *k*'s denominator must be step *k+1*'s
  numerator:

  ```
  denominator_node(step k) == numerator_node(step k + 1)
  ```

  **Identical node ids, not merely equal values.** `cse` has already merged every node with the same
  op, operands and constant bit pattern, so if the two discount factors are the same computation
  they *are* the same node, and the check is an integer comparison. This is cleaner than the
  IR-layer form, which compared two gather index arrays, and cleaner than any numerical test could
  be: it establishes that the ratio cancels exactly in ℝ **by identity rather than by tolerance**.

- **(P3) The coefficient condition.** `1 + f_k·w_k` equals `DF_k/DF_{k+1}` only when the compounding
  weight equals the forward's accrual, `w_k == τ_k`. These are two `Const` leaves per step
  (`ObsDay::weight`, `ObsDay::tau_rate`) and — again because of `cse` — equal constants are *one
  node*, so this too is an identity check.

- **(P4) Liveness** is a use count from `ir::Analysis::uses`: collapsing to endpoints is legal only
  when nothing outside reads an intermediate step.

That all four obligations reduce to integer comparisons on a hash-consed graph is the single
strongest argument for doing this at the tape. At the IR layer (P2) and (P3) were array scans; here
`cse` has already done the work.

### 4.3 (P3) is not a formality — the Stage A book is a mix

`tables.hpp` states the finance in its own comment: *"plain: n_i = r′ − r and the product telescopes;
lookback / observation shift / lockout: the span and the weight differ, and it does not."*

Reading `src/conventions/rfr.cpp` and `src/maths/instrument/builder.cpp` against
`blueprints/problems/stage_a.json` (**measured from the blueprints, not from a run**):

| blueprint | mix weight | (P2) | (P3) | outcome |
|---|---|---|---|---|
| `USD-SOFR-OIS` (plain) | 25 | ✓ | ✓ | telescopes |
| `EUR-ESTR-OIS` (plain) | 15 | ✓ | ✓ | telescopes |
| `USD-SOFR-OIS-SHIFT2` (observation shift) | 10 | ✓ | ✓ | telescopes — see below |
| `USD-SOFR-OIS-SHIFT2-LOCKOUT2` | 10 | ✓ on a prefix | ✓ | **partial**: the last 2 steps have a frozen rate date, so (P2) fails there |
| `USD-SOFR-AVG-SWAP` | 10 | n/a | n/a | arithmetic average, not a product: no telescope |
| EURIBOR 6M / 3M / 3s6s basis | 30 | n/a | n/a | term rate, no scan |

Three things fall out of that table that a pattern matcher would get wrong:

1. **The `tables.hpp` comment is over-broad about observation shift.** Under
   `ObservationMethod::ObservationShift` the *whole observation period* is shifted
   (`rfr.cpp` shifts `obs_start` and `obs_end` together) and the loop then takes both `weight_days`
   and the rate's own span from the *same* shifted business days — so `w_k == τ_k` still holds and
   the product still telescopes. It is **lookback** that breaks (P3), because there the rate date is
   moved back independently of the accrual day (`od.rate_date = add_business_days(od.rate_date, −k)`)
   while the weight stays on the accrual period. This is worth fixing in the comment; more
   importantly it is worth 10 points of mix weight that a reader of the comment would write off. *This
   is a code-reading claim, not a measurement — §8.3 is the check.*
2. **Lookback passes (P2) and fails (P3).** With a uniform 2-day lookback in one calendar the rate
   dates are still consecutive, so the index condition holds and only the coefficient condition
   refuses. A rule that checked only the index structure would emit a wrong answer here. This is the
   concrete reason (P3) is a separate obligation and not an afterthought. Note the lookback
   blueprint (`blueprints/instruments/stage_a.json`, added at M3-fix) is **not drawn by the Stage A
   trade mix** — so it is a *correctness* case the rule must get right, not a share of this book. It
   is also the single best negative test for the classifier, precisely because the cheap half of the
   proof succeeds on it.
3. **Lockout telescopes on a prefix.** The last `lockout_days` steps repeat one frozen rate date, so
   (P2) fails on the tail and holds on everything before it. Hence
   `RecurrenceProof::prefix_steps`: the rule must be able to close a prefix and leave a tail, or it
   declines on a tenth of the book for no good reason.

**Assertion, to be checked by §8.3**: by trade-mix weight, roughly 50 points of the Stage A book
telescope fully, 10 partially, and 40 have no compounding scan at all.

### 4.4 The initial library, and what each entry is actually worth over a frozen op set

| class | recurrence | closed form | needs | win |
|---|---|---|---|---|
| `ConsecutiveRatioProduct` | `x_{k+1} = x_k·(a_k/a_{k+1})` | `x_n = x_0·a_0/a_n` | `Div` only | **asymptotic**: *n* steps → 1 divide |
| `Arithmetic` | `x_{k+1} = x_k + d` | `x_n = x_0 + n·d` | `Fma` + a per-row *k* Column | asymptotic, but on `Add`, the cheapest op there is |
| `Geometric` | `x_{k+1} = r·x_k` | `x_n = x_0·rⁿ` | `exp(n log r)`, `provably_positive(r)` | **constant factor**: ~*n* multiplies → one `exp` + one `log`, both far more expensive than a multiply |
| `LinearConstantCoefficient` | `x_{k+1} = a·x_k + b` | `aⁿx_0 + b(aⁿ−1)/(a−1)` | same, plus `a ≠ 1` | same, plus a cancellation at `a ≈ 1` |

So, stated as plainly as the brief asks: **of §2a's four classes, only the telescope gets an
asymptotic win over a frozen op set.** Arithmetic collapses honestly but on the cheapest op. The
other two need `rⁿ` spelled as `exp(n·log r)`, which is inexact, needs a positivity proof, and trades *n*
multiplies for two transcendentals — on the measured libm cost (D27: `exp` at ~5 ns/call amortised,
845–866 µs for 164k calls) that breaks even around *n* ≈ 10 and is a small constant-factor win
beyond it, not a 250:1 one.

This is not an argument against §2's discover-only stance. It is the price of it, and it is the
answer to "say plainly what it costs": **the frozen op set costs three of the four recurrence classes
their asymptotic win, and keeps the one that matters.**

### 4.5 What the rule emits

Two closures, both offered whenever both are legal, because which one pays is extraction's question
and not the rule's:

- **`Endpoints`** — replace the chain's steps with the endpoint expression. Requires (P4). The
  250:1 form.
- **`PerStep`** — one node per step, each the closed form at that step (`x_k = a_0/a_k`). Same node
  count, a divide instead of a multiply, but **no sequential dependence**. That is worth more than
  it sounds, and it is a tape-layer-only option: `ir::infer` will no longer see a chain, so those
  rows stop being a scan domain, and **"a scan is never fused or inlined" (D41)**.

  The measurement that makes this interesting: on Stage A the compounding scan domain d10 is
  **255,687 rows, 1,102 chains, 262 waves — 69% of the B=64 run and 74% at B=1** (M3-gate-1 planner
  coverage). So the single largest block of interpreter time is the one structure the planner is
  forbidden to touch, and `PerStep` hands it to the planner even when (P4) fails.

Both are ordinary `Rewrite`s. The rule additionally exposes `classify()` returning a `Proof` with
every obligation as a separate boolean and a written `account`, so D53's requirement is satisfiable
with "it did not fire, and here is the obligation that failed" — the useful form.

## 5. Extraction with two objectives

### 5.1 Why a per-op ulp count does not compose

Two reasons, with different fixes, worth separating:

1. **An ulp is not a scale.** It is the quantum of a representable neighbourhood, so "1 ulp" at 1e−300
   and "1 ulp" at 1e+300 are incomparable. Adding them is meaningless.
2. **Even relative error does not compose through a chain.** Multiplicative steps compose benignly
   ((1+δ₁)(1+δ₂) ≈ 1+δ₁+δ₂), but **subtraction of nearby quantities** amplifies its operands'
   relative error by the condition number (|a|+|b|)/|a−b|, which is unbounded. Stage A contains
   exactly this shape and it is not incidental: the recorded forward rate is `(DF_s/DF_e − 1)/τ`,
   where `DF_s/DF_e` is within ~1e−4 of 1, so the subtraction discards roughly thirteen significant
   digits and multiplies whatever error reached it by ~1e4. A sum of per-op bounds is off by that
   factor.

### 5.2 What does compose: the adjoint the engine already has

Model each rewrite as an **absolute perturbation** `δt` injected at the term it replaces, and
propagate it by the first-order sensitivity:

```
Δ(output o)  =  Σ over rewritten terms t  of  |∂o/∂t| · |δt|
```

This composes because it is a linear functional, and it is nearly free **here specifically**:
`∂o/∂t` is exactly what the mechanical adjoint computes (M2, D31), on the program already recorded,
in one reverse pass per output seed. No interval arithmetic, no per-rule error algebra, no new
machinery. `PRINCIPLES.md` §4 makes the templated oracle available; this makes the *search* able to
price accuracy without running the oracle on every candidate.

Stated sharply, because it is the part most likely to be over-trusted:

> **The adjoint-weighted number is an estimate, not a bound.** It is a linearisation. It is the
> **search heuristic**. The **gate** is `PRINCIPLES.md` §4's oracle, run on the one extracted
> candidate.

Two places the linearisation is known to be wrong, both handled explicitly rather than hoped away:

- **Across a `Select`.** The derivative with respect to the predicate is zero, so a rewrite under a
  `Select` that could flip the branch has a first-order effect of zero and a real effect of the
  difference between the arms. `ErrorTerm::across_select` marks it and the propagator bounds it by
  the arm difference instead of differentiating.
- **Where the term's value can vanish.** Relative error is undefined at zero, so a term that is not
  `provably_nonzero` states `absolute_delta` and leaves `relative_delta` at zero.

A third approximation, named rather than hidden: the sensitivities are those of the **original**
program, not of each candidate, so that one reverse pass serves every candidate. To first order they
agree, which is the order the whole model is stated at.

### 5.3 How a candidate carries its error, and why this is a constraint not an objective

A candidate carries `algebra::CandidateError`: the **list** of per-rewrite `ErrorTerm`s with their sites, not a
running total. Two reasons. The composition needs the per-site adjoint weight, which is not known
when the rewrite is made. And a failed oracle check must be attributable to a specific rewrite rather
than to a number.

`PRINCIPLES.md` §4 sets tolerances **per output class**, and a tolerance is a constraint. So the
honest formulation is not a Pareto search over (speed, accuracy) but

```
minimise predicted cost   subject to   predicted error ≤ budget(output class)
```

— a constrained single objective, which the existing bottom-up DP already does, once per error tier.
`OutputClass` is `Valuation` (tight) / `Sensitivity` (looser) / `Diagnostic` (loosest), with the
numbers living beside the outputs they govern in `PROBLEM.md`, per §4.

**What that gives up**, since it is a real choice: a candidate that is much *more* accurate for
slightly more cost is never preferred, because nothing asks for it. That is the right default —
the owner sets budgets; the engine does not get to trade the owner's accuracy for speed on its own
authority — but it does mean the engine will not volunteer accuracy it was not asked for.

**There is deliberately no default budget**, and that is a design decision rather than an omission.
A library default becomes the de-facto contract — which is precisely how bit-identity became one
(`PRINCIPLES.md` §0: "the space the search may explore was never written down, so nobody noticed it
excluded the only thing worth finding"). The numbers belong in `PROBLEM.md` beside the outputs they
govern. `ErrorBudget::no_rewrites()` is the zero budget, named for what it does; it is useful for
bisecting a regression and as a test fixture, and it is **not** "the safe setting" — a zero budget
rejects `fma_contract`, `mul_recip_as_div` and the telescope, every one of which is strictly *more*
accurate than what it replaces.

### 5.4 The oracle is real, and these are its numbers

`epykos::Wide` (D72, landed today on `p0/oracle`) is an in-repo double-double at 106 significand
bits — 2^53 finer than a double ulp — and the templated maths instantiates at it unchanged, which is
D3's dividend banked. **The naive path's own error is now measured rather than assumed:**

| | Stage A valuation | Stage A sensitivity |
|---|---|---|
| leg PV | **1.616e-13** | **2.064e-10** |
| trade PV | 7.854e-14 | 5.154e-11 |
| aggregates | 1.465e-13 | 1.209e-13 |

Three consequences, and they are why §5 is now written against numbers:

1. **The headroom is known.** A rewrite whose composed error lands at or below the naive path's own
   does not move the answer by more than the answer was already moving. That is ~1.6e-13 for
   valuation. `ErrorBudget::naive_path_stage_a()` names it as a reference point — deliberately a
   function, not a default, so re-measuring updates one place, and deliberately not the default,
   because a library default becomes the contract (§0 of PRINCIPLES.md).
2. **Sensitivities have about three more decades of room** (2.1e-10 against 1.6e-13, a factor of
   656 on the leg PVs). §4's "valuation is held tight; sensitivities are allowed more" was a policy;
   it is now a measurement.
3. **That 656× is itself the cancellation this design exists to remove.** The recorded forward rate
   is `(DF_s/DF_e − 1)/τ` with the ratio within ~1e-4 of 1, so the subtraction discards roughly
   thirteen digits and amplifies by ~1e4 — which is the order of the 656×. **A successful telescope
   should move that number down.** If it does not, the error model is wrong, and that is a sharper
   falsification test than anything the design could assert about itself.

### 5.5 What extraction prices

Not the term. A rewrite's value is usually not local: collapsing a 90-step compounded coupon removes
89 multiplies and then makes ~88 of the 91 discount factors that fed it unreachable, and those are
`exp` nodes. So extraction lowers each candidate to a tape, runs cse and dce, and prices *that*.

**The metric is node count, not nanoseconds, and that is a real limitation.** At this layer there is
no plan and no interpreter, so `optimise::estimate_program` cannot be called without inferring
first. `CostMetric::WeightedNodes` weights reachable nodes by op (an `exp` is not a `neg`) and is
enough to rank candidates that differ by orders of magnitude — which, if the telescope works at all,
is the case that matters. `CostMetric::InferAndEstimate` runs `ir::infer` per candidate and prices
the Program properly; it is strictly better information at ~1.0 s per candidate on Stage A, worth it
when two candidates are close and wasteful when one is 250× smaller.

This is an improvement on the IR-layer draft's position, worth noting because it cuts against one of
that draft's risks: §8.5 worried that the cost model (58.6%/49.7% mean relative error, modelling
`exec::Interpreter` only) could not see a telescope's win. At the tape the first-pass metric does not
consult the cost model at all — it counts nodes — so the search's ability to *find* the win no longer
depends on the instrument that has been the programme's weakest link.

### 5.6 The slow/fast path agreement test

`PRINCIPLES.md` §4 replaces the execution tier with a test, in the owner's words: *"we need to test
if our slow and fast paths agree to within floating point tolerance."* Generic interpreter against
catalogued kernel, unfused against fused, batched against unbatched, reference against release.
`algebra::PathAgreement` (error.hpp) is the shape, and three things about it are deliberate:

- **It carries the number, not a boolean.** §4: the test "measures conditioning for free — agreement
  at 1e-16 says the expression is well conditioned, agreement at 1e-9 says one path is losing
  precision and nothing is broken". That number is worth tracking per pair over time; it is the
  cheapest conditioning monitor the engine will ever have, and it costs nothing extra to record.
- **Oracle-against-path is preferred to path-against-path wherever the oracle is affordable**, per
  §4, because it says *which* path is wrong rather than merely that they differ. Path-against-path
  is the cheap form and belongs where the oracle is not affordable — which, for a 1,000-lane Stage A
  scenario grid, is most places.
- **Exact agreement is recorded, never required.** Where two paths happen to agree exactly and
  asserting it costs nothing, assert it: it is what caught D61's GCC operand-order divergence. The
  moment a kernel wants to reorder for speed, the assertion is relaxed *there* and the error
  measured. `PathAgreement::exact` therefore defaults to false — exactness is observed, not assumed.

The owner's own objection is worth repeating because it is the right one: if the algebraic engine is
correct, the paths agree by construction. True in ℝ and false in floating point — and, more to the
point, **the test is not testing the algebra, it is testing that the code implements the algebra**,
which is a claim about the construction rather than a consequence of it. Both of this week's
relevant defects were in that gap and neither was an algebra failure (D61's catalogue fingerprint,
canonical by design and not in fact; D65's harness sizing a buffer from the wrong vector).

## 6. Saturation control

Four mechanisms replace D62's `RefirePolicy` at this tier. Each one's completeness cost is stated, in
D62's style.

### 6.1 No AC matching on `Sum`/`Affine`

The most important bound, argued in §3.4. **Given up:** any optimum requiring a Sum's members to be
reordered or re-associated. Concretely, that includes pairwise/Kahan summation of a leg sum, which is
a genuine accuracy improvement this design cannot reach. Noted as a real loss, not glossed: it is the
price of not letting the graph fill with permutations.

### 6.2 Bounded pattern depth

`max_pattern_depth = 4`, enforced at rule registration. This bounds the work *per candidate root*:
matching one rule at one node costs O(branching^depth) with both factors small and constant
(branching ≤ 3, depth ≤ 4), so the round is O(nodes × rules) up to that constant instead of growing
with the graph's own depth. It is a constant-factor bound, not an asymptotic one, and is stated that
way rather than as a complexity claim it does not earn. **Given up:** a rule needing a 5-deep pattern
must be written as two rules with an intermediate form, which means the intermediate must itself be a
legal term — occasionally awkward, never impossible.

### 6.3 Per-rule banning with exponential backoff

A rule adding more than `ban_threshold` nodes in one round is banned for `ban_rounds`, doubling each
re-offence (egg's mechanism; D12 — no external library, this is ours). **This is the mechanism D65's
R2 needed and did not have**: at 10,111 sites it would have been banned in round 1 with a logged
reason, instead of running 700 s and blowing the node bound. **Given up:** a rule that is starved in
round *k* may have been the one that mattered; saturation is no longer confluent in the presence of
banning. Mitigated by the log naming every ban, so "the search found nothing" and "the search was
banned from looking" are distinguishable — which, on M4's record, is the distinction that matters.

### 6.4 Class-uniform saturation

§2.3's mechanism, and the load-bearing one: saturate over one representative per shape class rather
than over 517,036 nodes. **Given up:** a rewrite that is valid for one member of a shape class and
not another. A rule that could be in that position says `applies_class_uniformly() == false` and is
matched node by node, which costs speed and nothing else — the conservative direction is the default
and the driver never guesses.

This *replaces* the IR-layer draft's fourth bound, "per-group scoping with a quarantined
cross-domain phase", which existed to stop `gather_push_unary` copying a sub-DAG into another group.
The tape has no gathers, so that bound and its completeness loss are both gone. It was the least
satisfying of the four and the draft flagged that it "may block the very shape the telescope needs";
that worry is now moot.

### 6.5 What completeness means here, honestly

With all four on, saturation is **not** complete with respect to the rule set. It reaches a fixpoint
over: derivations of any length and any mixture of rules, at pattern depth ≤ 4, with no Sum
reordering, over one representative per shape class, minus whatever banning cut.

Compared with D62's `NoFreshCrossRule` this is strictly *more* complete in the dimension that was
hurting — mixed rule chains, which are the normal case for algebra (factor, then cancel, then
contract) — and newly restricted only where a rule declines class uniformity.

---

## 7. What happens to the existing optimiser

**Nothing is rebuilt and nothing is ported.** That is the main practical consequence of the layer
correction, and it is worth stating as flatly as possible.

| | disposition |
|---|---|
| `rewrite::Rule`, `Proposal`, `MatchSite` | **kept, unchanged, no apology.** They are correct for a layer where `ir::infer` has already turned terms into arrays (§0.1) |
| the seven layout rules R1–R7 | **kept, unchanged in what they do.** Only the driver above them changes |
| `optimise::EGraph` and `src/optimise/egraph.cpp` | **not rebuilt.** D62's whole-program-e-node problem is **bypassed, not solved**: algebra no longer happens at that layer, and layout rewrites do not need a saturating search to find |
| the plan tier | **retired as a search; becomes a deterministic compilation pass** |

**Recommendation on the plan tier, which was an open question and is now easy.** The interpreter
needs a plan regardless, so planning is compilation, not search. D63 records that the joint search
*"passes BY IDENTITY — the extracted candidate is the default plan's own execution, not something
better than it"*. D68 prices a perfect plan-level cost model at **~0.08% of Stage A's wall clock**.
And D62 measured the plan tier as the binding constraint on saturation: a 2^k subset lattice over
~12 per-domain decisions, **105,977 plan nodes and 10.5 GiB on a sixty-trade fixture**. Retiring it
deletes `RefirePolicy::PipelineOrderedPlans` and the completeness that policy gave up, and costs
nothing any measurement can find.

**The condition, stated rather than buried:** this assumes program choice and plan choice are
separable. The failure shape is a rewrite that makes a domain *fusable* which was not — worth much
more with the fusion than without, so mediocre when priced under the default plan. At this layer the
mitigation is cheap and I would take it: when `CostMetric::InferAndEstimate` is used, price each
candidate under the plan its own program would get, not under a fixed one. §8.8 settles it in an
afternoon either way.

**What this package does NOT fix and does not claim to:** the cost model's 58.6%/49.7% error, its
blindness to the catalogue and the reverse ladder, and the fact that it models `exec::Interpreter`
only. §5.5 argues the stage's first-pass metric routes around that rather than depending on it.

## 8. Where I think this breaks, and the cheapest experiments

Ordered by how likely each is to kill the programme, cheapest test first within each.

### 8.1 The sequencing is still backwards, and this is cheaper than ever to fix

**Finding, not a risk.** §4 establishes that the recurrence kind needs exactly two things: chain
detection (`ir::analyse`, §4.1) and the ability to replace a sub-DAG. **It does not need the
e-graph.** A telescope is a local rewrite on a chain the analysis already found, applied to at most
a few hundred chains (Stage A has 1,102), and it can be written as a direct tape pass in the shape
`tape/passes.cpp` already uses — rebuild the node table, return a remap — with no saturation, no
extraction and no cost model.

So `PRINCIPLES.md` §8's order (term rewriting, then identities and recurrences, then telescoping end
to end) puts several thousand lines of e-graph ahead of the one deliverable with a 250:1 claim on
it, and the layer correction makes that gap *wider*, not narrower: at the IR layer the recurrence
kind at least needed the program tier's machinery, and here it needs almost nothing.

**Recommendation: do telescoping first, as a fifth tape pass.** It needs `ir::analyse` (§4.1, which
is the one piece of shared plumbing either route requires) and the classifier of §4.2. If it lands
and the number is real, the e-graph has a measured justification. If it lands and the number
evaporates, that is much better learned for the cost of one pass than for the cost of the
interface.

### 8.2 The win lives where no rule is applied — the largest finding in this package

**Measured, in this repository, this session.** `src/solver/residual.cpp:73–77`:

```
program_ = ir::infer(slice_.tape);
exec::Options io;  io.max_batch = 1;  io.lane_tile = 1;
interp_ = std::make_unique<exec::Interpreter>(program_, io);
```

`ResidualProgram` builds an `ir::Program` from the sliced tape and constructs an interpreter and an
adjoint on it **directly**. No rewrite pipeline, no e-graph, no cost model, no plan. D68 says the
same thing from the other end: *"the per-block residual solves inside it do build their own, from
hardcoded local options no caller can reach, on slice programs no rule is applied to."*

The calibration solve is **~48% of Stage A** (`PRINCIPLES.md` §6 item 6), its instruments are OIS
swaps whose residuals are exactly the compounded-coupon product loops, and `PRINCIPLES.md` §0 puts the
telescoping win "about 250:1 **on the calibration side**". So:

> The 250:1 cannot be realised by any amount of term-rewriting work, because the programs it would
> apply to are constructed and executed without ever passing through the optimiser.

The fix is now **one line**, and smaller than it was at the IR layer: `ResidualProgram` already
calls `standard_passes(slice_.tape)` immediately before `ir::infer(slice_.tape)`, so the algebraic
stage goes between them exactly as it does on the main path — `algebra::reduce(slice_.tape, rules)`.
No new plumbing, no Program-level hook, no optional-pass parameter threading. It is **in scope**
under `PRINCIPLES.md` §3, which
excludes "the solver itself — Jacobian policy, iteration strategy, convergence criteria" but
explicitly includes "the arithmetic the engine was handed". Rewriting the residual's *expression* is
arithmetic, not solver policy: same iterates, same convergence, cheaper residual.

**Cheapest experiment (hours, existing tooling):** call `ir::to_string` on one `ResidualProgram`'s
`program_` for the USD SOFR curve block and check whether it contains scan domains with the
compounding shape. If it does, the finding is confirmed and the hook should be built before anything
else in this design.

### 8.3 The telescope's preconditions may not hold on the actual tape — the single best experiment

§4.3's table is read off blueprints and builder code. It has **not** been checked against a recorded
tape, and three things could each falsify it: `affine_collapse` may have rearranged `1 + f·w` so
that (P1) no longer matches (§1.4 — the most likely of the three); `cse` may not in fact have shared
`DF(t_next_k)` with `DF(t_rate_{k+1})`, breaking (P2) even though the values are equal; or an
intermediate accumulator may be read somewhere, breaking (P4).

The experiment is *cheaper at this layer than it was at the IR layer*, because every check is now an
integer comparison on the node table rather than a scan of gather and column arrays.

**Cheapest experiment, and the one I would run first (one afternoon, no new machinery):** a throwaway
tool over the existing Stage A fixture that, for each chain `Inference::detect_chains` finds,

1. dumps the step's node shape,
2. checks `denominator_node(step k) == numerator_node(step k+1)` — an integer comparison — and
   reports the longest prefix on which it holds,
3. checks whether the two coefficient constants are the same node (they are, if equal, after `cse`),
4. reads `use_counts` for every intermediate step,
5. reports the owning domain's share of measured wall clock from the existing `EPYKOS_EXEC_PROFILE`
   table — for the compounding scan this is already known to be **69% at B=64**.

Four booleans and a number per chain. It answers, before any interface is written: *is the worked
example of `PRINCIPLES.md` §8 item 4 actually available, and is it worth anything here?* If (2) fails,
the recurrence library's first citizen is wrong and §8 item 4 needs re-picking. If (5) is small, §8.2
and `PRINCIPLES.md` §5's re-grading are confirmed and the arithmetic-dominated workload must come
first.

### 8.4 Two objectives may be one objective

§5 builds error budgets, an `ErrorPropagator` and a per-tier DP. But the two rewrites we can already
name that change accuracy (`fma_contract`, `mul_recip_as_div`) are both *faster and more accurate*,
and so is the telescope. If the Pareto frontier is almost always a single point, all of §5's machinery
beyond a single scalar tolerance is over-engineering.

**Cheapest experiment:** once the first three inexact axioms exist, extract at `no_rewrites()` and at a
loose budget and diff the results. If they differ in fewer than a handful of places, ship the budget
as a scalar and delete the propagator.

### 8.5 The cost model may not see the win — reduced by the layer change, not eliminated

At the IR layer this was a serious risk: the cost model's mean relative error is **58.6%/49.7%**
after D63 (target <25%, measured), it models `exec::Interpreter` only, and D68 measured the
whole-program interpreter at **2.513% of Stage A**. A search that could only rank candidates through
that instrument was repeating M4's failure mode.

The tape layer routes around it. `CostMetric::WeightedNodes` counts reachable nodes after cse+dce
and does not consult the cost model at all, so *finding* a 250:1 collapse no longer depends on the
programme's weakest instrument. What still depends on it is deciding between two candidates that are
close — and if the telescope works, nothing is close.

**What remains, and it is the sharper version of the old worry:** node count is not time. Removing
89 multiplies from a scan and 88 `exp` nodes is obviously good; removing 5% of nodes may be worth
nothing or may be worth a cache level, and node count cannot tell. The mitigation is that the shapes
this design targets are order-of-magnitude shapes.

**Cheapest experiment:** hand-construct the telescoped tape once — edit the node table directly, no
rule — then infer, run, and compare against the un-telescoped tape end to end. Two numbers: node
count ratio and wall-clock ratio. The gap between them is exactly how much node count is lying by,
and it is worth knowing before any ranking is built on it.

### 8.6 The frozen op set: what §2's stance costs, stated plainly

The brief asks me to say if part of §2's discover-only stance cannot work over a frozen op set. My
conclusion: **it works, and here is the bill.**

- **It works** for the thing that matters. The telescope needs only `Div`, its proof is exact index
  arithmetic, and it is expressible as a general fact about scans with no instrument knowledge
  anywhere. §2's distinction ("a fact about a mathematical form is discovery; a fact about this
  instrument is declaration") holds cleanly here, and (P2)/(P3) are exactly the form/instrument
  boundary made mechanical: the rule knows nothing about coupons, it knows that consecutive value
  ids cancel.
- **It costs three of §2a's four recurrence classes their asymptotic win** (§4.4). Geometric and
  linear-constant-coefficient need `rⁿ`, must spell it `exp(n log r)`, become inexact, need a positivity
  proof, and win a constant factor instead of an order.
- **It costs a handful of identities entirely** (§3.5): no `Abs`, so no `sqrt(x²)` normal form; no
  `Log1p`, so the `(DF_s/DF_e − 1)` cancellation has no expressible fix (tier 2 anyway).
- **It does not cost the identity set much otherwise.** Twenty-eight of the thirty-two axioms need no
  op the engine lacks.

If the owner later reconsiders, the single highest-value addition is **`Pow`** (or equivalently
`Exp2`/`Log2` with a fused path), which would move geometric and linear-constant-coefficient from
constant-factor to asymptotic. That is a note for a future decision, not a request.

### 8.7 Smaller risks, recorded

- **`affine_collapse` has already eaten the shape the classifier looks for** (§1.4). `1 + f·w` is an
  `Affine` node by the time the stage runs. This is the single likeliest way a first implementation
  finds nothing, and §8.3's experiment catches it on day one.
- **Side conditions are weaker here than at the IR layer** (§1.3). There is no Column array to scan,
  so `provably_positive` can discharge little beyond `exp(·)`, `x*x` and positive constants. The
  conditional axioms (`log_product`, `sqrt_product`, `exp_log`) will fire less often than the
  IR-layer draft assumed. Unmeasured, and not recoverable at this layer.
- **`ir::analyse` couples the stage to inference's phase order** (§4.1). Mitigated by it being the
  same code, which has M1/M3 round-trip gates on it.
- **First-order error propagation across `Select`** is invalid and handled by a fallback that may be
  so conservative no inexact rewrite under a `Select` is ever selectable. Unmeasured.
- **Node count is not time** (§8.5).
- **A rewrite may duplicate a multiply-used node.** At the tape, replacing a shared sub-DAG can turn
  one node into several. `ir::Analysis::uses` is in the interface for this reason, but nothing yet
  bounds a rule's node growth, and the e-graph's node budget is a blunt instrument for it.

### 8.8 The planning question, settled in an afternoon

§7.4 is open and does not need to stay open. On the existing 60-trade Stage A fixture — the one D62
and D54 both measured — extract twice: once with `PlanningMode::InSearch` under
`PipelineOrderedPlans`, once with `PostExtractionPass`. Compare the winning program, the winning
plan, and the measured wall clock of each.

Three outcomes and each is decisive. **Same program, same plan** (what every number in the record
predicts, since D63's rediscovery passes by identity): Option B is free, the plan tier and its 10.5
GiB go away, and a completeness loss goes with them. **Different plan, same wall clock**: the joint
search is finding a tie, which is noise — take Option B. **Option A genuinely wins**: the
separability assumption of §7.4 is false on this fixture, and the *reason* it is false is the most
valuable thing this whole design could learn, because it names the shape of cross-layer interaction
the term tier will have to model.

Cost: no new machinery. Both modes run through the same entry point.

---

### 8.9 Does the e-graph actually fit? — the experiment §2.3 owes

§2.3 claims the graph seeds at ~10³ nodes under class-uniform saturation and declines to claim
anything about what it saturates *to*. Both halves are checkable before any rule is written:

1. **Count the shapes.** Run the deep-hash partition over the Stage A tape and report the number of
   distinct shape classes and the size distribution. This is `ir::analyse`'s own output and needs no
   rules at all. If it is a few hundred, §2.3 holds; if it is tens of thousands, class-uniform
   saturation does not save us and the whole placement needs rethinking.
2. **Saturate with associativity and distributivity only**, on one representative, with the node
   budget set high and the round cap low, and plot nodes against rounds. Those two axioms are the
   fastest-growing pair in the set, so they bound the rest. If a ten-node term goes to 10⁵ in three
   rounds, `max_pattern_depth` and banning are not enough and the rule set needs a normal form
   rather than a search.

Cost: (1) is an afternoon and reuses existing machinery. (2) needs a minimal e-graph — a few hundred
lines, no rules beyond two, no extraction, no error model — and is the cheapest honest test of the
central tractability assumption. **I would run (1) before committing to this design and (2) before
committing to the e-graph specifically**; note that §8.1's recommendation (telescoping as a direct
tape pass) needs neither.

---

## 9. Summary of what is design, what is measured, and what was discarded

**Discarded, with the reasoning recorded in §0.1:** the IR-layer targeting; the term e-graph over
`ir::Program`; the anchor domain; row-universality as a constraint; index maps and the two gather
axioms; the quarantined cross-domain saturation phase; the three-tier migration and the wrapper
strategy; and — the one that has to be *retracted* rather than simplified — the tractability
argument from the 517,036 → 67-domain collapse, which does not apply at a layer that runs before
that collapse. `rewrite::Rule`, `Proposal`, `MatchSite` and the seven layout rules are **not**
replaced by this package: they are correct for their layer.

**Measured, cited:** the tape is a hash-consed term DAG already rewritten in place (`tape/tape.hpp`,
`tape/passes.hpp`); `affine_collapse` recovers SwapEngine's W-cache, 857 rows (`PRIOR_ART.md`);
Stage A is **517,036 nodes** after the E0 passes from 27,459,283 recorded, **67 domains, 5 scan
domains, 1,102 chains** (D44, M3-gate-1); the compounding scan d10 is **255,687 rows and 69% of the
B=64 run, 74% at B=1**, and a scan is never fused or inlined (M3-gate-1, D41);
`Inference::detect_chains` reads and writes `boundary_` (`src/ir/signature.cpp:408`);
`ResidualProgram` builds an interpreter with no rewrite pass between `standard_passes` and
`ir::infer` (`src/solver/residual.cpp:73–77`); the oracle's measured naive-path error, **1.616e-13
valuation and 2.064e-10 sensitivity** on Stage A leg PVs (D72); R2's 36–68-ulp adjoint failure and
the rest of §3.6's audit; the cost model's 58.6%/49.7% and the interpreter's 2.513% share (D63/D68);
D62's 105,977 plan nodes and 10.5 GiB; the Stage A trade mix and the observation-method definitions.

**Design:** the stage and its placement; `TermRef == node_id` and the `Expr`/`Rewrite` shape; the
thirty-axiom set and its three-way split; `fold_untainted` as a tape-only axiom; the recurrence
obligations (P1)–(P4) as integer comparisons on a hash-consed graph, and the two closures;
adjoint-weighted error composition; constrained single-objective extraction with node count as the
first-pass metric; the four saturation bounds; `ir::analyse` as the cheap form of factoring
inference's prefix; and the recommendation to retire the plan tier as a search.

**Assertion, not yet measured:** that the shape-class count is a few hundred (§2.3, §8.9 counts it);
that the graph saturates to something bounded (§8.9 measures it); that the Stage A mix telescopes
50/10/40 and that observation shift telescopes contra the `tables.hpp` comment (§8.3); that
`affine_collapse` has not eaten the classifier's shape (§8.3); and that node count ranks candidates
well enough to matter (§8.5). Five claims, five named experiments, none of them expensive.
