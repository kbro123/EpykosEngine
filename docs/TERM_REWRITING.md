# TERM_REWRITING — replacing a term instead of a program

Design package P1, 2026-09-25. Implements `docs/PRINCIPLES.md` §8 item 2 ("Term-level rewriting: a
proposal that replaces a sub-term, e-classes over terms") and §8 item 3 (the identity set and the
recurrence rule kind of §2a). Decision entry: **D73**.

> **Written against `PRINCIPLES.md` §4 as amended at `f8849b4` (2026-09-25), which retires
> bit-identity as a contract anywhere.** Two tiers and a test, not three: structural is exact
> because it is a graph identity; mathematical is characterised error against the oracle and now
> covers every execution path too; execution is not a tier. Nothing in this design admits a rewrite
> *because* it preserves bits. §3.6 is what that retirement buys, measured. §7.4 is the open
> question the owner has not answered — whether the layout rules stay in the search or become a
> post-extraction pass — designed both ways, with a recommendation.

**This is a DESIGN. Nothing here is implemented.** The deliverable is this document plus interface
headers — `include/epykos/rewrite/{term,term_rule,error_model,recurrence}.hpp` and
`include/epykos/optimise/{term_egraph,term_extract}.hpp` — so that the shape can be reviewed
before roughly six thousand lines are written against it. One `.cpp` ships (`src/rewrite/term_names.cpp`,
three `to_string` tables) so the enumerations are linkable and testable; there is no other code.

Throughout, **design** means a proposed mechanism, **assertion** means a claim I have not measured,
and **measured** means a number taken from this repository's own records with its citation. §8
separates them again at the end, and names the experiments.

---

## 0. The one-paragraph summary

A rewrite stops returning an `ir::Program` and starts returning a replacement for one **term** — a
sub-DAG of one domain's `Group`, denoting one scalar per row of that domain. E-classes are over
terms, anchored to a domain, with gathers as an explicit index-map term-former that rules (not
congruence) commute across. The identity set is thirty-two named axioms over the existing twenty
ops, classified three ways by exactness, with `Op::Sum` and `Op::Affine` deliberately excluded from
associative-commutative matching. The recurrence kind of §2a turns out **not** to be a term rule at
all — it restructures domains, so it fits the existing `Rule` interface, has five sites on the whole
Stage A tape, and can be attempted *before* the term layer rather than after it. Extraction
minimises cost subject to a per-output-class error budget, composing error by the adjoint the engine
already has, because ulp counts do not compose and adjoint-weighted absolute perturbations do.
Saturation is bounded by four mechanisms that between them replace D62's `RefirePolicy`, whose
completeness loss this design recovers. The seven layout rules are wrapped, not ported, and where
they *run* — inside the search or after it — is designed both ways in §7.4.

And one finding that outranks all of it: **the ~48% of Stage A where the 250:1 telescoping win
lives is code no rule is applied to.** §8.2.

---

## 1. What replaces `Proposal`

### 1.1 What a term is, in this IR

The domain IR is not a term DAG, and pretending otherwise is the fastest way to get this wrong.
`ir::Program` is a list of `Domain`s, each with a `Group`: a short list of `Step`s evaluated once per
**row**. A `Step`'s operands are `Slot`s — an earlier Step of the same group, a `Literal` (uniform
across rows), a `Column` (one double per row), a `Gather` (one value id per row), a `Segment` (a
per-row variadic member list), or an `Input`.

So a group's steps already form a small DAG, and the definition falls out:

> A **term** is the sub-DAG rooted at one `Step` of one `Group`. It denotes, for each row *r* of that
> group's domain, one scalar. The domain is the term's **anchor**: a term is a row-indexed *vector*,
> not a scalar.

Addressing is therefore complete at `TermRef{domain, step}` — steps are topological, so a step index
names exactly one sub-DAG. There is no row index anywhere in the interface, and §1.3 is why.

### 1.2 The replacement

`rewrite::Proposal` holds an `std::optional<ir::Program>`. Every match clones, validates and
serialises an entire program. `rewrite::TermRewrite` holds an e-class id and a `TermExpr`: a small
node arena whose operands may be

- other nodes of the same arena,
- **e-classes the match bound** (`ExprRefKind::Bound`) — the pattern holes, which is how a rewrite
  *reuses* structure instead of copying it: `mul(a, add(b,c)) → add(mul(a,b), mul(a,c))` names `a`,
  `b`, `c` three times and materialises nothing,
- an existing `ir::Slot`, verbatim,
- **new data** — `new_literals`, `new_columns`, `new_gathers`, `new_segments`.

That last bullet is not decoration. A closed form usually needs data that does not exist yet: the
arithmetic-series collapse `Σ(a + kd) = na + d·n(n−1)/2` needs a per-row *k*, which is a Column. A
replacement language that could only rearrange existing nodes could express exactly one of
`PRINCIPLES.md` §2a's four recurrence classes. This is a requirement discovered by working §2a
through, not a generality added for its own sake.

`site_class` and `replacement` are an **equality claim**, not an instruction. The driver unions
them and keeps both forms — D49's intent and `rule.hpp` point 2's own words, now at the tier where
they are affordable.

### 1.3 What a rewrite of one step means for the rows, and whether a subset is possible

**A term rewrite applies to every row of its anchor domain.** That is not a restriction the design
adds; it is what a domain *is* — `ir::infer`'s isomorphism class, "`rows` instances of one op
sequence". Rewriting step 3 of group 7 rewrites it for all 16,103 rows at once, and that is precisely
why a term rewrite is cheap where a whole-program `Proposal` is not: one edit, one e-node, 16,103
rows of effect.

**A rule can never rewrite a subset of rows.** There is no row predicate in the interface, deliberately.
A per-row conditional rewrite would turn one group into a branchy one and destroy the property the
executor is built on (one kernel per (op, operand kinds, lane width); D27's whole kill-path result
rests on it). A rewrite valid for only some rows must **first split the domain** so those rows become
their own domain — which is `r2.bucket_rows`' job, a program-tier rule. Row-subset rewriting is
expressible as *(program-tier split) then (term-tier rewrite)*, never as one term rewrite.

This has a consequence worth stating because it will look like a limitation and is closer to a
safety property: the term layer cannot fragment the domain partition. D65 measured what happens when
something can — R2 without its structural floor turned the M1 book's 10 domains into 19,624 and the
e-graph died in two rounds. The term layer structurally cannot do that.

### 1.4 Side conditions are row-universal, and the IR can usually discharge them exactly

Because a rewrite applies to every row, every side condition is universally quantified over rows.
That sounds worse than it is. When *x* is a Column, "x > 0 for every row" is a scan of a
`std::vector<double>` sitting in the `ir::Program` — exact, cheap, no sampling. When *x* is a
Gather, it is a scan of the producing domain's Column. `TermView` exposes four such predicates
(`provably_positive`, `provably_nonzero`, `provably_finite`, `provably_not_negative_zero`), each
returning false for "not shown", never for "false".

The price is that the proof is about **this recording**. A rewrite licensed by "every entry of
column 3 is positive" is not valid for a different book. That is exactly what a recording compiler
is entitled to assume — D47 already frames a `Program` as one recording — but it must be said out
loud, because it means a cached compiled program is keyed to its recording and not to its blueprint.

---

## 2. E-classes over terms

### 2.1 Nodes, classes, congruence

An e-node is `(op, child e-classes, leaf payload, anchor domain)`. An e-class is a set of e-nodes
asserted equal. Hash-consing makes two nodes with equal content literally one node. Congruence is
the usual closure: if `a ≡ a'` and `b ≡ b'` then `op(a,b) ≡ op(a',b')`, maintained by deferred
canonicalisation — a whole round's matches are queued, then one `rebuild()` hash-conses and unions
them all. That is the pattern `optimise::EGraph` already uses and D49 already documented (egg,
Willsey et al. 2021); nothing about it is new here, only the thing it ranges over.

### 2.2 The anchor is part of a node's identity

Two structurally identical terms in two different domains denote **different row vectors** and must
not be unioned. `mul(col0, gather0)` in domain 3 and `mul(col0, gather0)` in domain 5 are the same
*shape* — that is what `ir::infer`'s signature pass is for, and what the catalogue keys on (D55/D61)
— but not the same *value*. Shape equality and value equality are different relations and this tier
is about value equality.

`TermEGraph::anchors_consistent()` is the invariant: every class has exactly one anchor. It is the
one most likely to be broken by a careless cross-domain rule, so it is a gate, not a comment.

### 2.3 How this interacts with the domain structure — and why it is tractable

The instinct is that a term e-graph over the Stage A tape means 517,036 e-nodes. It does not.

**Measured** (D35, D44, D65): `ir::infer` turns those 517,036 tape nodes into **67 domains** (80
after R2 and R1 fire) of a few `Step`s each. `ir::shape_string` switches to the `"<op>[<steps>]"`
abbreviation only past 24 steps, and the Stage A group shapes on record are short —
`exp(mul(neg(@0),$0))`, `div(sub(div(@0,@1),#0),$0)`, `mul(mul($0,@0),@1)`.

**Assertion, following from that**: a term e-graph over the groups is on the order of a **thousand**
e-nodes *before saturation* — roughly 80 groups × ~10 steps of root terms, plus their sub-terms — not
half a million. The 517,036 : 67 collapse *is* the sharing, and it has already happened, in a pass
that predates the optimiser.

**That is the seed size, not the saturated size, and the difference is where this could still go
wrong.** Equality saturation grows each class by the alternative forms it learns, and associativity
plus distributivity over a ten-node group is exactly the shape that grows fastest. If a saturated
class averages 50 nodes the graph is ~35k; at 500 it is ~350k and past
`TermSaturationLimits::max_nodes`. Neither number is measured and I am not going to pretend
otherwise. What *is* structural, and is the claim worth relying on, is the **shape** of the growth:
it is (number of groups) × (per-group saturation), where the second factor depends on the rule set
and the depth bound and **not** on the tape's 517,036 nodes. That decoupling is the property the
whole-program e-graph did not have. §6 bounds the second factor; §8.3's experiment is what would put
a number on it.

This is the single fact that separates this design from the whole-program e-graph it replaces, and
it is worth being precise about what it dissolves. D62's cause (B) was: *"k independent local edits
on one node still clone the whole Program k times and then 2^k times over the rounds, because this
is a whole-program-valued e-graph."* At the term tier, k independent edits are k choice points in
one graph: **O(k) nodes, not O(2^k)**. The problem is not mitigated, it is gone.

### 2.4 Gathers: the index map is part of the term, and commuting it is a rule

A `Gather` slot names one value id per row, and a value id is globally `(domain, row)`. A row's value
is its group's *last* step, so a gather into domain *p* always denotes "the root term of group *p*,
re-indexed by this gather's index vector":

```
Gath(g, t)      the term t of the producing domain, observed at the rows g names
```

Congruence handles `Gath(g,t) ≡ Gath(g,t')` when `t ≡ t'`. It does **not** handle

```
Gath(g, f(t)) = f(Gath(g, t))      f elementwise
```

which is a genuine mathematical fact about an index map and an elementwise function. That is a
**rule** — `r4a.push_unary_through_gathers`, generalised — and it is the bridge every cross-domain
identity goes over. Getting this boundary right is load-bearing in both directions: fold it into
congruence and the graph is unsound (it would equate terms with different index maps); leave it out
entirely and no algebra ever crosses a domain boundary, which is where the discount factors live
relative to the coupons that use them. §6.4 is why it is also the main explosion risk.

### 2.5 D62's three mechanisms: which survive

| D62 mechanism | fate at the term tier |
|---|---|
| **(1) application memo** | **Survives, re-keyed.** The key becomes `(e-NODE, rule, binding)`, not `(program node, rule, site)`. E-nodes are immutable under hash-consing; *classes* merge, so a class-keyed memo would go unsound the instant two classes union. Bindings are canonicalised to class representatives at lookup, so a merge makes the memo do redundant work at worst, never wrong work. Sound for exactly D62's reason: `TermRule::search`/`build` are pure. |
| **(2) match only a class representative** | **Unnecessary — true by construction.** Hash-consing means there is no congruent duplicate node to skip. D49's flagged "Known limitation" (plan tiers keyed per node rather than per class) disappears with it at this tier. |
| **(3) `RefirePolicy::NoFreshCrossRule`** | **Unnecessary, and should be deleted here.** It existed solely to bound cause (B). D62 booked its cost honestly: *"any genuine optimum that needs two different structural rewrites composed after both have already fired once is no longer reachable at all."* That completeness is **recovered**. Long mixed chains are the normal case for algebra — factor, then cancel, then contract — and are what equality saturation is for. |
| **(4) `RefirePolicy::PipelineOrderedPlans`** | **Survives unchanged, in the plan tier, which this design does not touch.** §7.3. |

The honest way to read that table: this package recovers one of the two completeness losses D62
recorded and leaves the other exactly where it was.

---

## 3. The identity set

Thirty-two axioms, enumerated in `rewrite::Axiom` (term_rule.hpp) so that the list is readable in one
place and a test can assert the enum and this table have not drifted apart
(`tests/rewrite/term_interface_test.cpp`). Three buckets.

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
| `commute_add_mul` | operand order of `Add`, `Mul`, `CmpEq` | | `op_is_commutative` already asserts this bitwise, and D61 already canonicalises it in `catalogue::Signature` |
| `gather_push_unary` | `gath(g, f(t)) = f(gath(g, t))` | Gather + any unary | exact: a gather *re-indexes*, it does not compute. `r4a` already declares this exact and is the precedent. What differs between the two sides is only how many rows `f` runs on — a cost question, which is exactly why it belongs in an e-graph rather than a greedy pass |
| `gather_push_binary` | `gath(g, f(s,t)) = f(gath(g,s), gath(g,t))` | Gather + binary, **same g both sides** | same argument; the same-gather condition is structural, checked on the table, not a value condition |

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

### 4.1 It is not a term rule, and that is the finding

§2a asks for "a rule KIND it does not have: one that analyses a detected scan's step and recognises
it as a member of a known recurrence class, rather than pattern-matching a term". Working the shape
through, the kind it needs is **not term-level**:

> A scan domain's rows are the **steps** of its chains (D41: chain-major, `chain_offsets`). The closed
> form of a chain has one value per **chain**, not one per step. So collapsing a recurrence deletes
> rows, changes a domain's row count, and re-points every gather that read it. That is a
> **domain-level** edit — the same kind `r5.group_formation` and `r7.block_linmap` make — and it is
> exactly what a term rewrite is defined not to do (§1.3).

So the recurrence kind slots into the **existing `rewrite::Rule` interface**, whole-Program `Proposal`
and all. And that is fine, because `PRINCIPLES.md` §6 item 1's combinatorial objection does not apply
to it: it has exactly **one site per scan domain**, and the full Stage A tape has **five** scan
domains (**measured**, D35/D44). Five programs, not 10,111.

**Consequence for sequencing, and the owner should see it plainly:** `PRINCIPLES.md` §8 orders
term-level rewriting (item 2) before the recurrence kind and telescoping (items 3 and 4). The
recurrence kind does not depend on item 2 at all. Telescoping — the 250:1 worked example — can be
attempted **first**, on the interface that already exists, and would then be the evidence that
decides whether the term layer is worth its six thousand lines. §8.1.

### 4.2 How a match is PROVED rather than pattern-matched

Shape matching is necessary and nowhere near sufficient. Take the class this engine was built for.
`maths/swap/compounding.hpp` records

```
acc_{k+1} = acc_k · (1 + f_k·w_k),     f_k = (DF(t_rate_k) / DF(t_next_k) − 1) / τ_k
```

and says in its own header that "with r_i = (DF(s_i)/DF(e_i) − 1)/τ_i it collapses to DF(s)/DF(e) on
paper". *On paper.* Whether it collapses in **this recording** is three separate facts, and all three
are decidable **exactly**, from integer and double tables the `ir::Program` already holds, with no
sampling and no numerical experiment:

- **(P1) Shape.** The scan group's step is `mul(carry, X)` and *X* reduces to `div(g_num, g_den)`
  modulo the affine rearrangement the recorder left behind. Ordinary matching, and the weakest of
  the three.

- **(P2) The index condition — the telescope itself.** The factor must be a ratio of *consecutive*
  terms of one indexed family: row *k*'s denominator must be row *k+1*'s numerator.

  ```
  gathers[g_den].index[r] == gathers[g_num].index[r + 1]      for every row r inside a chain
  ```

  This is an equality of `std::int32_t` **value ids** in a table the program already holds. It is a
  *proof*, not a pattern: it establishes that the two discount factors are literally the **same
  recorded value**, so their ratio cancels exactly in ℝ. No numerics are involved and none can be
  wrong.

- **(P3) The coefficient condition.** `1 + f_k·w_k` equals `DF_k/DF_{k+1}` only when the compounding
  weight equals the forward's accrual, `w_k == τ_k`, for every row. These are two different Columns
  (`ObsDay::weight` and `ObsDay::tau_rate`, `maths/instrument/tables.hpp`) and the check is a bitwise
  comparison of two double vectors.

And one about the surroundings rather than the recurrence:

- **(P4) Liveness.** Replacing 90 rows by 1 is legal only if nothing outside the domain reads an
  intermediate row. Decidable by scanning every other domain's gather index arrays and the output
  list.

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

Two closures, both offered whenever both are legal, because which one pays is the cost model's
question and not the rule's:

- **`ChainFinal`** — one row per chain replaces the chain's steps. Requires (P4). The 250:1 form.
- **`PerRow`** — one row per step, each the closed form at that step (`x_k = a_0/a_k`). Same row
  count, worse arithmetic per row (a divide instead of a multiply), but **no sequential dependency**:
  a parallel domain instead of D41's wave-by-wave scan. Always available when the proof holds, and
  the interesting one when (P4) fails.

Both are whole-`ir::Program` `Proposal`s. The rule additionally exposes `classify()` returning a
`RecurrenceProof` with every obligation as a separate boolean and a written `account` — so that D53's
requirement ("a package's gate must state whether its deliverable actually fires") is satisfiable
with "it did not fire, and here is the obligation that failed", which is the useful form.

---

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

A candidate carries `CandidateError`: the **list** of per-rewrite `ErrorTerm`s with their sites, not a
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

### 5.5 The slow/fast path agreement test

`PRINCIPLES.md` §4 replaces the execution tier with a test, in the owner's words: *"we need to test
if our slow and fast paths agree to within floating point tolerance."* Generic interpreter against
catalogued kernel, unfused against fused, batched against unbatched, reference against release.
`rewrite::PathAgreement` (error_model.hpp) is the shape, and three things about it are deliberate:

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

### 5.4 Extraction prices the whole program, not the term

A term rewrite's value is usually not local. Telescoping a 90-step compounded coupon removes 89
multiplies from the scan domain — and then kills about 88 of the 91 discount factors that fed it,
which are rows of the `exp(mul(neg(@0),$0))` domain (16,917 rows on Stage A), and `exp` was **54% of
the M1 book's B=64 time** (D27, measured). That win is a **dead-code consequence** downstream of the
rewrite, in a different domain, and a local cost delta cannot see any of it.

So extraction lowers each candidate to a whole `ir::Program`, runs DCE, and prices *that* with
`optimise::estimate_program`. That is affordable precisely because the term tier emits a handful of
candidates (one per error tier), not one per match — the exact inversion of the property that killed
the whole-program e-graph.

**A note on the cost model, since `PRINCIPLES.md` §6 flags it.** §6's last paragraph is right that the
instrument is better suited to algebra than to layout: "its fit puts essentially all predicted time
into the per-op rates, which is exactly why it is useless for layout … and roughly right for algebra.
Removing 249 multiplies of 250 would register strongly." But note what §5.4 has just established:
the *large* part of a telescope's win is not the 249 multiplies, it is the 88 dead `exp`s, and those
show up as removed **rows of another domain** — which is a rate × rows term the model does price.
So the mechanism is favourable. Whether the *magnitude* comes out right is §8.5.

---

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

### 6.4 Per-group scoping and a quarantined cross-domain phase

Within-group rules saturate first, per group, independently. Cross-domain rules
(`TermRule::crosses_domains()`) then run in their own phase for at most `max_cross_domain_rounds`,
and only through a gather whose producing domain has **one reader** (`r5.group_formation`'s own
condition).

This is the main explosion risk and the bound is the least satisfying of the four. Pushing a term
through a gather **copies a sub-DAG into another group**; a producer read by five gathers can be
inlined five ways, and that is the inlining lattice again, now in the term tier. The single-reader
restriction bounds it hard. **Given up:** an identity spanning three domains through two shared
gathers is unreachable. Since the discount factors are shared by construction (CSE gives one DF node
per date, read by both the coupon that starts on it and the one that ends on it), **this restriction
may block the very shape the telescope needs** — and if it does, the telescope must come from the
recurrence rule at the program tier, where no such restriction applies. That is a second, independent
reason §4.1's finding matters.

### 6.5 What completeness means here, honestly

With all four on, saturation is **not** complete with respect to the rule set. It reaches a fixpoint
over: within-group derivations of any length and any mixture of rules, at pattern depth ≤ 4, with no
Sum reordering, plus at most two rounds of single-reader cross-domain propagation, minus whatever
banning cut. Compared with D62's `NoFreshCrossRule` this is strictly *more* complete in the dimension
that was hurting (mixed rule chains) and newly restricted in a dimension D62 did not have
(cross-domain depth), because D62's tier had no cross-domain notion at all.

---

## 7. Migration

### 7.1 Three tiers, not two

| tier | holds | rules | status |
|---|---|---|---|
| **TERM** (new) | e-classes of terms within one program | algebraic identities (§3) | this design |
| **PROGRAM** (kept) | whole `ir::Program` alternatives | domain-restructuring rules | unchanged |
| **PLAN** (kept) | `PlanAnnotations` per program node | `r6`, the five `planner.*` rules | unchanged |

The relation is the important part, and it is what keeps the program tier from re-inflating: **the
term tier is a sub-solver, not a producer of program nodes per match.** It saturates, then extracts
*once per error tier*, contributing O(number of tiers) program nodes — not O(matches).

### 7.2 The seven layout rules: wrapped, not ported

| rule | kind | disposition |
|---|---|---|
| `r1.fold_uniform_columns` | changes the domain partition | **stays** on `rewrite::Rule`, program tier |
| `r2.bucket_rows` | splits domains | **stays**. Also becomes the *enabler* for row-subset algebra (§1.3) |
| `r3.elide_trivial_maps` | *is* a term rewrite | **should move**, later |
| `r4a.push_unary_through_gathers` | *is* the cross-domain bridge (§2.4) | **should move**, later |
| `r4b.shared_reciprocal` | inserts a domain | **stays** |
| `r5.group_formation` | merges domains | **stays** |
| `r6.materialise_boundaries` | annotation | **stays**, plan tier, untouched |
| `r7.block_linmap` | splits a domain | **stays** |

Recommendation: **wrap, do not port.** A `ProgramRuleAdapter` presents the old `Rule` at the program
tier while the term tier runs beneath each program node. Nothing in `rewrite/` or
`src/optimise/egraph.cpp` changes.

**The cost of wrapping, stated:**

- R3 and R4a will exist twice — once as program-tier rules, once as term-tier axioms
  (`select_same_arms`/`mul_one` overlap R3; `gather_push_unary` is R4a). Two rules producing the same
  content is not a correctness problem (the program tier hash-conses on `ir::serialize`, exactly as
  it does today), but it is wasted work and a confusing log. Mitigation: the driver disables the
  program-tier copy when the term tier is enabled, which is a one-line policy, not a rewrite.
- The program tier keeps `RefirePolicy::NoFreshCrossRule` and its completeness loss for the layout
  rules. Since those rules are **demoted** (`PRINCIPLES.md` §7) and worth 1.73×–1.88× only on
  workloads with that structure, paying a search-completeness cost on them is the right trade.

### 7.3 What this design does NOT fix

**The plan tier is still a whole-`PlanAnnotations`-valued e-graph with a 2^k lattice**, *if it
survives at all*. D62 measured it: 105,977 plan nodes and 10.5 GiB on 60 trades to saturate, which is
why `PipelineOrderedPlans` exists. Term-level rewriting does nothing for that, because a plan is not
a term. Fixing it *within* the search needs the same treatment applied to `PlanAnnotations` —
per-domain choice points instead of whole-plan clones — and that is a separate package.

§7.4 proposes not fixing it but **deleting it**, which is the open question. Given D68's measurement
(the whole plan stage is worth 1.027×–1.042× on Stage A, and a *perfect* plan-level cost model is
worth ~0.08% of Stage A's wall clock), spending a package on a better plan-tier data structure is
the worst of the three options. Recorded here so the decision is deliberate either way.

---

### 7.4 OPEN: does planning stay in the search, or become a post-extraction pass?

**The owner has not answered this, so it is designed both ways and the interface does not assume
one.** `optimise::PlanningMode` selects; `TermExtractResult::plan` is populated either way, so
nothing downstream needs to know which ran. That symmetry is the point — the question can stay open
without stalling the packages behind it.

**Option A — `InSearch` (D49/D62's shape, unchanged).** Plans are e-graph candidates. Extraction
scores (program, plan) pairs jointly, so a plan can pay for a program that is worse on its own.

**Option B — `PostExtractionPass`.** Extraction ranks *programs* under the default plan; the winner
is planned once, deterministically, by `plan_after_extraction(program, model, params)`. The plan
tier disappears. The pass may hill-climb on the cost model internally — a local search over one
program, not a dimension of the global one — and that flag is **off by default**.

#### What the record says

| evidence | bearing |
|---|---|
| D62: the plan tier is "a pure 2^k subset lattice over the k ≈ 12 independent per-domain deltas", **105,977 plan nodes and 10.5 GiB on 60 trades** to saturate | Option A's cost, measured. It is why `PipelineOrderedPlans` exists, and that policy gives up "every plan that needs one rule applied twice, or two rules out of the caller's declared order" |
| D62: term-level rewriting dissolves the 2^k problem **in the term tier only** — a plan is not a term, so Option A keeps the lattice | This design does not fix Option A's cost. §7.3 |
| D68: the whole plan stage is worth **1.027×–1.042×** on Stage A, and a *perfect* plan-level cost model is worth **~0.08% of Stage A's wall clock** | The prize Option A is searching for is very small on the current fixture |
| D63: rediscovery's 1.02× clause "passes **BY IDENTITY** — the extracted candidate is the default plan's own execution, not something better than it" | The joint search has never actually found a plan better than the default one |
| D63: the cost model captures **31% of reduction fusion and 11% of inlining** | The instrument Option A searches with cannot rank the decisions it is searching over |
| `PRINCIPLES.md` §7: the layout rules are worth **1.73×–1.88× on a workload that has that structure** | The prize is not small *in general*, only on Stage A. §5's arithmetic-dominated workload may change this |

#### Recommendation: Option B, with one condition

**Take the post-extraction pass.** The reasoning, in order of weight:

1. **The interpreter needs a plan regardless.** Planning is not optional work the search might skip;
   it is compilation. Modelling a mandatory, deterministic function of the program as a *search
   dimension* is a category error, and it is the one that produced a 2^k lattice over twelve
   independent boolean decisions.
2. **The joint search has never paid.** D63 is explicit that the rediscovery clause passes by
   identity. Every measurement in the record is consistent with "the default plan is the answer",
   and none shows a plan paying for a worse program.
3. **It removes a completeness loss instead of adding one.** `PipelineOrderedPlans` exists only to
   bound the plan tier. Delete the tier and the policy goes with it — and a local hill-climb over
   one program can apply a rule twice or out of order, which is exactly what that policy forbade.
4. **It makes the cost model's weakness matter less.** A 58.6%/49.7% model ranking twelve interacting
   decisions is a bad instrument doing a hard job. The same model choosing between a handful of
   local moves on one fixed program is the same instrument doing an easy one, and if it gets a move
   wrong the cost is bounded by that move.

**The condition, and it is the real risk:** Option B assumes **the best plan for the best program is
also the best plan reachable independently of it** — that program choice and plan choice are
separable. That is not a theorem, and there is one concrete shape where it plausibly fails: a term
rewrite that makes a domain *fusable* which was not (say, by removing the gather that forced
materialisation) is worth much more *with* the fusion than without it. Priced under the default
plan, such a program looks mediocre and loses the extraction. Option A would find it; Option B would
not.

Two mitigations, both cheap, and I would take the first:

- **Price each candidate under `plan_after_extraction`, not under the default plan** — i.e. run the
  deterministic pass inside the scoring loop rather than only on the winner. That restores
  separability by construction (every program is scored under *its own* best plan) at a cost of one
  planning pass per candidate, and the term tier emits a handful of candidates, not thousands. This
  is the version I recommend; `TermExtractOptions` already permits it, since `planning` is read by
  extraction and not only after it.
- Failing that, keep `InSearch` available behind the enum for the arithmetic-dominated workload of
  §5, where the layout rules' 1.73×–1.88× may make the joint search worth its cost again. Deleting
  the code would be premature; defaulting away from it is not.

**How to settle it cheaply** (§8.8): on the existing 60-trade fixture, extract twice — once under
`InSearch` with `PipelineOrderedPlans`, once under `PostExtractionPass` — and compare the winners
and their measured wall clock. If they are the same program and the same plan, which every number in
the record predicts, Option B is free and the 10.5 GiB goes away.

---

## 8. Where I think this breaks, and the cheapest experiments

Ordered by how likely each is to kill the programme, cheapest test first within each.

### 8.1 The sequencing is backwards, and this is the cheapest thing to fix

**Finding, not a risk.** §4.1 establishes that the recurrence kind is a program-tier rule with five
sites on the whole Stage A tape. It does not need the term layer. So `PRINCIPLES.md` §8's order —
term rewriting (item 2), then identities and recurrences (item 3), then telescoping end to end (item
4) — puts roughly six thousand lines of interface ahead of the one deliverable with a 250:1 claim
attached to it.

**Recommendation:** do item 4 first, on the existing `Rule` interface. It is a single rule with a
classifier, a whole-Program `Proposal`, and five sites. If it lands and the number is real, the term
layer has a measured justification. If it lands and the number evaporates, the term layer's business
case has to be made some other way — and that is much better learned for the cost of one rule than
for the cost of the interface.

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

The fix is small — `ResidualProgram`'s constructor takes an optional optimisation pass and applies it
to `program_` before building the interpreter — and it is **in scope** under `PRINCIPLES.md` §3, which
excludes "the solver itself — Jacobian policy, iteration strategy, convergence criteria" but
explicitly includes "the arithmetic the engine was handed". Rewriting the residual's *expression* is
arithmetic, not solver policy: same iterates, same convergence, cheaper residual.

**Cheapest experiment (hours, existing tooling):** call `ir::to_string` on one `ResidualProgram`'s
`program_` for the USD SOFR curve block and check whether it contains scan domains with the
compounding shape. If it does, the finding is confirmed and the hook should be built before anything
else in this design.

### 8.3 The telescope's preconditions may not hold on the actual tape — the single best experiment

§4.3's table is read off blueprints and builder code. It has **not** been checked against a recorded
tape, and three things could each falsify it: the tape passes (`fold_sum`, `affine_collapse`, D39)
may have rearranged `1 + f·w` so that (P1) no longer matches; CSE may not have shared `DF(t_next_k)`
with `DF(t_rate_{k+1})`, breaking (P2) even though the values are equal; or the intermediate
accumulator rows may be read somewhere, breaking (P4).

**Cheapest experiment, and the one I would run first (one afternoon, no new machinery):** a throwaway
tool over the existing Stage A fixture that, for each of the five scan domains,

1. prints `ir::shape_string`,
2. checks `gathers[den].index[r] == gathers[num].index[r+1]` inside every chain, and reports the
   longest prefix on which it holds,
3. bitwise-compares the two candidate coefficient columns,
4. scans every other domain's gathers and the output list for reads of non-final rows,
5. reports the domain's share of measured wall clock from the existing `EPYKOS_EXEC_PROFILE` table
   (D63/D68 already built it).

Four booleans and a number per domain. It answers, before any interface is written: *is the worked
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

### 8.5 The cost model may not see the win (this is M4's failure mode, repeating)

`PRINCIPLES.md` §6's last paragraph argues the cost model is "roughly right for algebra". Its mean
relative error is **58.6%/49.7%** after D63's repricing (target <25%, **measured**), it models
`exec::Interpreter` only, and D68 measured the whole-program interpreter at **2.513% of Stage A**. So
even a perfect algebraic rewrite inside the interpreter is bounded by that share on the current
fixture. §5.4 argues the *mechanism* is favourable (dead rows in a big domain are priced); the
*magnitude* is unverified.

**Cheapest experiment:** hand-construct the telescoped program once (by editing the tape, not by
writing a rule), price it with `estimate_program`, then run it. Two numbers: predicted speedup and
measured speedup. If the model predicts a large win and the wall clock does not move, the problem is
§8.2's plumbing, not the model. If the model predicts nothing and the wall clock moves, the model is
the problem and `PRINCIPLES.md` §6's optimism about it is misplaced.

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

- **Cross-domain quarantine may block the telescope's own shape** (§6.4). Independent reason to do
  §4.1 first.
- **First-order error propagation across `Select`** is invalid and handled by a fallback that is
  conservative but possibly so conservative that no inexact rewrite under a `Select` is ever selectable.
  Unmeasured.
- **Side conditions are recording-specific** (§1.4), so a compiled program is keyed to its recording.
  Fine today; a trap if anything ever caches compiled programs across books.
- **`TermExpr` can introduce new Columns**, which means a rewrite can grow the program's *data*, not
  just its structure. A per-row `k` Column on a 16,917-row domain is 135 KB. The cost model prices
  bytes; nothing currently bounds a rule's data growth. Worth a limit before the first such rule.

---

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

## 9. Summary of what is design and what is measured

**Measured, cited:** the domain collapse (517,036 → 67/80, D35/D44/D65); five scan domains on Stage A;
R2's 36–68-ulp adjoint failure from reordering gather-table entries, and the rest of §3.6's audit of
what bit-identity was costing (D52 finding 3; `adjoint.hpp`, `bucket_split_edit.hpp`,
`r4b_shared_reciprocal.hpp`, `planner_rules.hpp`, `verifier.hpp`); §4a's 14 sources / 37 tests /
~9,700 lines; the plan tier's 105,977 nodes and 10.5 GiB, and D63's rediscovery passing by identity;
R2's 10,111 sites / 700 s / blown bound (D65); D62's memo, representative-matching and refire policy
with their costs; `exp` at 54% of M1's B=64 (D27); the cost model's 58.6%/49.7% error and the
interpreter's 2.513% share (D63/D68); `ResidualProgram` constructing an interpreter with no rewrite
pass (`src/solver/residual.cpp:73–77`); the Stage A trade mix and the observation-method definitions
(blueprints, `src/conventions/rfr.cpp`, `src/maths/instrument/builder.cpp`).

**Design:** the term definition and `TermRef` addressing; `TermExpr` and `TermRewrite`; the anchor as
part of node identity; gather-commutation as a rule; the thirty-two-axiom set and its three-way
exactness split; the recurrence proof obligations (P1)–(P4) and the two closures; adjoint-weighted
error composition; the constrained-single-objective extraction; the four saturation bounds; the
three-tier migration; the two planning modes and the recommendation in §7.4.

**Assertion, not yet measured:** that the term graph SEEDS at ~10³ nodes (and, more importantly, no
number at all for what it SATURATES to — §2.3 says so plainly); that the Stage A mix telescopes
50/10/40; that observation shift telescopes (contra the `tables.hpp` comment); that the cost model
would register a telescope's magnitude correctly; and — the one §7.4 turns on — that program choice
and plan choice are **separable**, which is a condition of the post-extraction recommendation and
not a result. §8.3, §8.5 and §8.8 test all five.
