# CLAUDE.md — EpykosEngine

**Read `docs/PRINCIPLES.md` first** — it is the contract and it outranks everything else here. Then `docs/DESIGN.md`,
`docs/ROADMAP.md` and `docs/WORKLOADS.md`. `docs/DECISIONS.md` is the append-only ledger: cite it, do not read it end to
end. `docs/PRIOR_ART.md` holds the measured evidence the design rests on; `docs/RESUME.md` is the handoff brief.

## What this is
A financial computation engine that records pricing maths written once (templated on `Scalar`) and compiles it into a
domain-typed array program executed by pre-compiled fused kernels, with mechanically derived adjoints. **No JIT
(D1) — whose revisit clause FIRED on 2026-10-09 (D106): on a path-dependent payoff the back half measures 0.45–0.78x
the engine's own naive templated `double`. The decision is the owner's and is not taken.**

## Status
**The front half of the compiler exists and the engine derives telescoping.** `docs/PRINCIPLES.md`
is the contract and is authoritative; this paragraph summarises it and is rewritten, never extended
(§9).

M1–M3 passed 2026-09-23. **M4 failed its exit gate** 2026-09-24 (D59) and the post-mortem found the
cause was structural: the engine had a good back half and no front half, and the tape passes were
contracted to be bit-identical, which forbids algebra outright. **D79 replaced that contract with
the pin** — above it the engine chooses WHICH expression to evaluate and is judged against the
recorded expression in exact real arithmetic; below it, HOW, bit-identical to the pinned tape under
a declared contraction and transcendental policy.

**D81, 2026-09-27: `epykos::compile` now collapses the compounded OIS coupon by itself.** Five
local peepholes in one forward sweep — the tau cancellation, the +1/-1 cancellation, the
multiplicative identity, the telescope `mul(div(p,q),div(q,s)) -> div(p,s)` and one reassociation
for seasoned coupons. No recurrence solver, no e-graph, no new types. compare_ois goes from
1,020,583 recorded nodes to **1,183**, where the four data-movement passes alone left 81,321 and
D74's hand-written spike produced 1,313; Stage A goes 517,036 → **174,388**. It costs 1.03–1.08x
the old pass time. Verified against an independent engine and TIGHTER than before the collapse
(discount factors 7.355e-16, was 5.329e-15); the mechanical adjoint, the IFT ladder and every
error-against-truth gate pass unedited.

**Against a specialist, both engines in one process on one clock** (D81 §7, `tools/h2h`, 25 knots
and 1,000 trades, idle box): 161x faster cold calibration than the engine managed a day earlier,
150x warm, 36.5x on the risk ladder, each better than the hand-written spike. The **risk ladder is
5.70x in our favour**. Calibration is still **1.7x slower cold and 7.1x warm** and the collapse did
not move that ratio at all, which says the remaining gap is solver algorithm, not arithmetic —
`PRINCIPLES.md` §4's named exception.

Two defects found on the way and fixed: `exec::Interpreter` silently left duplicated outputs
unwritten (pre-existing, exposed by the collapse — silent zeros for 19 of 300 trade PVs on a Stage
A variant), and the generated catalogue registry was stale so coverage had dropped a quarter.

**D85, 2026-09-29: the solve cache is built, measured at 0.99x, and reverted.** D82 put ~65 us of a
74.9 us warm solve in one Jacobian build paid every call, so a seed cache was aimed at the small
half — measured, it lands exactly on a fixed record-point seed. What replaces it is a **lazy final
Jacobian**: build `J_z` at the solution when the adjoint or the IFT asks, not at the end of every
`run`. A re-quote falls **69.0 us → 11.4 us** with the ladder untouched, and because it changes no
iterate path it costs the contract nothing — D83's §5.2 amendment is withdrawn, and §4a's gate
restates as a *bitwise* equality. **Built 2026-10-03 (D89)**: paired before/after at load 1.95–1.97,
**69.0–70.7 us → 15.1–15.5 us (4.6x)** on the h2h hot configuration and 241 → 188 on the cold
default. It also exposed a latent hazard — `Factors` had no `valid()` guard, so an unbuilt Eigen
decomposition returned a silently all-zero risk ladder; it throws now. **D90, the head-to-head that
settles it:** both engines one process one clock, 25 knots / 1,000 trades, the warm calibration gap
goes **7.36x → 2.24x slower** (ours 31.1 us against 13.9) and cold 1.73x → 1.33x; the ladder stays
~6x in our favour and pricing ~48x. D89's "~1.6x" was optimistic — it divided a minimum by a
self-reported figure across two harnesses. Agreement with the independent engine survives at
5.533e-15 on book NPV. **D91 decomposes what is left**: the warm solve fits
`2.22·evals + 0.34·iters + 8.39 us` (worst error 8.45%), so it is **53% residual evaluation, 43%
fixed, 6% linear algebra** — and the ‖JᵀF‖∞ diagnostic is now the largest single fixed item at
~5.5 us. Bigger finding: the probe measures 19.4 us for the call h2h reports at 31.1 on an
identical workload, and forced eviction reproduces the difference, so **~40% of the head-to-head
number is cache refill, not arithmetic**. `max_batch` was tested as the cause and largely refuted.
The levers, measured: residual evaluation (§10 step 9's vector transcendentals now have a
denominator — d3 is `exp` over 101 of 402 values), the diagnostic, and residency, which nothing on
the roadmap attacks. **D94 takes the second of those**: `SolveOptions::optimality_diagnostic`,
default true and bit-for-bit today's behaviour, and when false the `jt_product` lane is skipped and
both `SolveReport::jtr_inf` and the block's diagnostic output are **NaN**, never zero — zero is the
signature of a converged solve, so it would announce success, and this repository has twice shipped
a plausible number where there should have been an unmistakable one (D81 §6(a), D89 §3). Paired
interleaved arms on an idle box (load 1.90, committed binary, D94 §6): the record point
**9.94 → 4.50 us (5.44 saved, 54.7%)** and a +1bp re-quote **19.14 → 14.19 us (4.95, 25.9%)**.
Four runs spanning load 1.90–4.22 put the saving at 5.30–5.55 us at +0bp and 4.82–5.49 at +1bp, so
**run-to-run spread exceeds the load effect** — and D91's independently-derived 5.46 us sits inside
it. When nothing iterates, over half the call was the diagnostic. The engine
could not find this itself — a derivative is not a tape node (D87 §1), so the one optimisation every
compiler does to an unused value, deleting it, is unavailable here. That is **invariant I3 failing
in its smallest instance**, and the switch is a mitigation, not a discharge of its gate.

**D92, 2026-10-06: the owner widened D11 to read the other engine's `OPTIMIZATION.md`; its "single
biggest win" turns out to be our smallest.** Their W-cache (`DF = exp(−W·x)`, precomputed W,
analytic Jacobian) removes "no curve rebuild, no autodiff sweep" — and Epykos never rebuilds a
curve, because the curve is inlined into the tape as d2/d3 at recording time, with the adjoint
batched over a collapsed 402-value slice. **We already hold that benefit by construction.** Measured
ceiling for a closed-form residual Jacobian: 87% of cold calibration, **0% of the warm path** (chord
builds none), and **0.5% of a 256-row risk ladder** — the earlier 63.7% was a one-row artefact,
since `Factors` is built once per solve and reused across every seed. A *free* Jacobian would save
229 us once per session — 2.0% of one 256-row ladder, 1.4% at the achievable 2x-4x, and decaying as
1/N in the ladders a session runs. **Not built; `AdMode::ClosedFormAffine` stays
unconsumed.** D68's method in a new subsystem: measure the share before fitting. The guard that
matters now: having read their list, no search that "rediscovers" an item on it is evidence of
anything (D88).
Dropping the final Jacobian outright is NOT the same thing and is wrong: the O3 ladder goes 6.5%
wrong at +25bp and 24% at +100bp.

**D86, 2026-09-29: M4's exit gate is changed, not passed.** The owner took all three of D68 §5's
recommendations. The cross-stage-optimisation clause is retired on Stage A and the cost model's
<25% error target with it — a model with ZERO error is worth 0.166% of an O4 lane batch — and the
optimisation gate is re-pointed at the block solves (~46% of the problem), the reverse ladder
(24.66%, uncosted) and record-plus-build (26.97%). M4 is not retroactively passed: its gate is
replaced because the gate was wrong, and D59's account stands unedited. The framework itself never
failed — saturation fixpoints a 517,036-node tape in 3.9 s, every extracted program verifies — it
was aimed at 2.513% of the problem.

**D87, 2026-09-29: the thesis has only been applied to half the engine.** Asked why D85's win was
found by a person rather than by the engine, the answer is structural: ‖JᵀF‖∞ is written in Eigen in
`src/solver/residual.cpp` because **a derivative cannot be expressed as a tape node** — `Op::Linmap`
is reserved with no producer, and `adjoint::Adjoint` is a compiled artifact, so differentiation is a
pipeline STAGE, not an OPERATION. Every quantity defined by a derivative — the diagnostic, the IFT's
own products, any Hessian — is forced out of the recorded world by construction. Pricing maths is
recorded and collapses 2,565x; the engine's own numerics are hand-written and get none of it, and
D68 measures that half at ~95% of an O4 lane. Third independent route to the same place. §1 states
the limit; D87 §7 gives three options and **decides none** — the owner's call. **D88** follows it
with the vocabulary for the next question — what earns the name *operator* (the catalogue already
discovers kernels and measured 1.11–1.16x; an operator is a kernel that also carries an algebra, a
derivative rule and a structural property), which operators are discoverable at all, and the finding
that a rewrite which is inexact-but-within-budget is licensed **nowhere** in this contract: it fails
the pin's exact-real test above and §5.3 forbids the search from choosing it below. Also undecided.

The M4 search — the e-graph, the cost model and layout rules R1–R7 — stays **quarantined behind
`EPYKOS_LEGACY_SEARCH`, default OFF, and deliberately not deleted** (§7.1). §10 step 11's
term-level e-graph is a *different* object: that one's e-nodes are whole `ir::Program`s and it sits
below the pin. R1, R2 and R5 now find **zero sites** on both fixtures, because the algebra removed
the near-duplicate structure they matched.

**D127, 2026-10-13: Stage A's 2,000-trade book — 1,997 agree to 1e-10 of notional, and the 3 that do
not are one convention question about EOM DETECTION.** The problem D109's h2h measured against
SwapEngine, whose conventions ours were imported from, so that comparison is blind to this layer.
Every trade rebuilt in QuantLib from its terms, so **2,000 schedules** are under test: five families
agree to **1.9e-14 of gross or better**, book NPV differs by 236.34 = **2.53e-07 of the book's gross
PV** of 9.33e8, and the whole of it is three trades. All three have a termination that is the **last
business day of its month without being the last calendar day** — ours `is_end_of_month` tests the
literal CALENDAR end, QuantLib's `Calendar::isEndOfMonth` tests whether the next BUSINESS day is in
the next month. Trade 129 terminates Saturday 2029-09-29: we keep the 29th, QuantLib snaps to the
30th. **Neither blanket setting matches** — `endOfMonth = false` fixes two and breaks a third whose
effective IS a month end — which proves it is two tests for one flag, not a missing adjustment.
**Not decided here**: ISDA's EOM is phrased about the last CALENDAR day, which reads our way, but
that is the inference that was wrong about the observation shift (D123), so it needs the primary
source. Third convention question the programme has surfaced, and the first with a measured cost on
the book. Also: the gate's tolerance is against each family's GROSS PV, because **holding a
near-cancelling net to a relative tolerance measures the cancellation, not the engine** — the first
version did that and failed EUR-ESTR-OIS, whose net is 695,702 out of millions in notional.

**D126, 2026-10-13: the first external check on a SECOND derivative — 1.6e-08 against a QuantLib
four-point difference.** D93 §1b said `Dual<N>` over hardcoded `double` "forecloses every second
derivative — the engine cannot compute a gamma by any route"; D97/D101 reopened it with
`Dual<Scalar,N>` and computed a book gamma, and **nothing external had ever checked one**. One pass
of `Dual<16, Dual<16>>` over the templated maths against QuantLib's four-point mixed second
difference, 8 trades and 16 knots: **book PV 1.52e-13**, all **256 Hessian entries to 1.62e-08 of
|H|max** (1.256e8), the **two gradient channels bitwise identical (0.0)**, and **Clairaut symmetry
2.36e-15**. The residual is the FD's own: swept, the error falls 11.4x for a 3.33x step (h²,
truncation) to 1.62e-08 at h = 1e-4 and then RISES 12.3x — the D120 V one order up, steeper because
a second difference divides by h². **What it covers**: the variable is the CURVE KNOT, no solver on
either side, so this is the second-order channel of the PRICING MATHS — the layer D93 called
foreclosed. **What it does not**: the IFT's second-order channel, gamma w.r.t. par QUOTES, which is
what a desk asks for and needs a difference over re-solves. Gated alongside are two properties an FD
cannot constrain (a Hessian matching an FD at 1e-8 while asymmetric at 1e-6 would pass otherwise)
and a degeneracy control: 52 off-diagonal entries exceed 1e-6 of |H|max, largest 3.35%.

**D125, 2026-10-13: the realised prefix matches QuantLib to 1.3e-11 bp — the seasoned gap is
closed.** D123's convention error lived in the BACK half of the compounded-SOFR formula (which day
count the annualised rate is applied over); the realised prefix is the FRONT half, where published
fixings hand over to forwards, and no oracle had reached it — `compare_ois` is unseasoned by
construction. Stage A's trade 39, a 10Y SOFR OIS seasoned 52 days, has a straddling coupon with **51
realised calendar days** (realised_factor 1.005437477536187) ahead of 212 projected observations;
QuantLib counts 248 business days in the period, the same decomposition read the other way.
**Coupon rate 1.31e-11 bp, trade PV 5.01e-14 relative.** The fixings are the engine's own synthetic
series handed to QuantLib verbatim — the right direction of travel, since a published history is an
INPUT to both engines like the curve and the trade, so sharing it shares a fixture and not an
implementation; what is compared is what each does WITH it. Only pre-valuation fixings are loaded,
which is `realised_observations`' own rule — the fourth instance of the harness trap anticipated
rather than walked into. The control is what makes it mean something: **neutralising the realised
prefix moves the rate 55.6 bp**, so the comparison constrains that contribution to ~5e12x its
tolerance. **Largest remaining gap: SECOND DERIVATIVES** — I2 is reachable, D101 computes a book
gamma, and nothing external has ever checked one.

**D124, 2026-10-13: Stage A's calibration surface is COMPLETE — all 70 quotes independently
verified.** The owner's ruling that futures **quotes are inclusive of any convexity** settles what a
futures oracle compares: `convexity zero` is the CONVENTION, not an approximation being excused, so
no adjustment on either side. SR3 against QuantLib's `OvernightIndexFuture(..., 0, Compound)`
**6.25e-11 bp of rate**; FEU3 as 100·(1 − 3M forward) **exactly 0**; the four deposits **exactly 0**.
With D119/D121's 50 par-rate rows that is **70 of 70** — two currencies, three calendars,
one/two/three-curve pricing, par rates, par spreads, prices and deposits. Two gates keep it honest
about units: a **price point is 100 bp of rate**, so the measured figures are quoted in rate and the
file says its printed column for a price family is price×1e4 (reporting the larger unit would have
flattered it for free); and the quoting convention itself is pinned — exactly the Future rows may
carry `quote_is_price`, counts fixed at 16 and 54 — because a family that silently changed
convention would be compared against the wrong kind of number. **Largest uncovered area now:
SEASONED trades** — `compare_ois` is unseasoned by construction and Stage A's book is 20% seasoned,
but no oracle yet reaches a coupon with a realised fixing, which is exactly where a compounding
convention error would hide.

**D123, 2026-10-13: the ARRC settles the observation shift and THE ENGINE WAS WRONG.** At the
owner's direction the authority is the ARRC's *Updated User's Guide to SOFR*, whose **Appendix 4** is
titled "Methods for Calculating Daily Accrual in an Interest-Period Weighted Observation Shift" and
names both readings: apply the annualised shifted rate over **(1)** the observation period's days,
or **(2)** the interest period's. The main text fixes which one the NAME means — *"with an
observation shift, interest is accrued according to the number of days in the observation period"* —
and records (2) as *"not the principle recommendation"*, *"discussed but not recommended"*, and able
to *"result in a negative daily accrual even if SOFR rates are positive"*. **This engine computed
(2)**: `float_coupon_pv` formed `N·d_c/360·[(prod−1)·360/d_o]·DF`. Fixed via `rate_accrual`, and
**verified against literals generated before the fix existed** — the shift families go 2.12 bp →
**4.3e-11 bp** while plain, lookback and averaged stay bit-identical. **And a VALUE-neutral change
is not a STRUCTURE-neutral one**: the first version altered the recorded graph everywhere and five
gates caught it (catalogue x2, R1, R2, the telescoping spike), because D81's collapse, the
catalogue's coverage and the rewrite site counts are properties of the graph, not the numbers. The
shipped form changes only a τ constant and keeps the ops in order.

**D122, 2026-10-13: the solve is verified, and the obstacle that blocked it for two days DID NOT
EXIST.** The owner asked one question about D120 §2 and it sent me to QuantLib's source instead of
its header. D115 §5 and D120 §2 both claimed "QuantLib's natural OIS pillar is the swap's maturity
while ours is its last cash flow" — **false**: `Pillar::LastRelevantDate` is the DEFAULT and
`oisratehelper.cpp` computes it as `max(maturityDate_, lastPaymentDate)`, so it **includes the
payment lag** and coincides with our knot. I read the enumerator's name and assumed its meaning —
*citing a thing is not checking it*, now applied to a library rather than one of our own gates. The
gate: QuantLib bootstraps its own curve from the same 16 quotes, **0 of 16 pillar dates differ**,
worst relative knot DF **8.09e-13**, worst zero rate **4.16e-10 bp** — at QuantLib's own bootstrap
accuracy floor. **The chain quote → solve → knots → price → NPV → ladder now has an independent
number at every link.** (Also: we are NOT solving knot forwards — Stage A is linear in zero,
`compare_ois` linear in logdf; it is SwapEngine whose free variables are instantaneous forwards.)

**D121, 2026-10-12: the 3s6s basis closes the par-rate surface — 9 rows at 8.5e-14 bp, THREE curves,
float-vs-float.** 3M EURIBOR + spread (quarterly) against 6M flat (semiannual), ESTR-discounted,
forecasting off two distinct slots. First attempt, no harness correction, and the tightest agreement
in the suite for a stateable reason: the basis is quoted as an ~8 bp spread rather than a 2.7% rate,
so the same absolute tolerance is four orders tighter in relative terms. The par spread is solved in
CLOSED FORM, s = (PV6 − PV3)/A3, because importing a solver into an oracle would be a second thing
to be wrong. Gated with the control the shape needs — both legs must forecast off DIFFERENT slots,
exactly one carries the spread — since a basis whose forecast slots had collapsed would give a
spread near zero and still match a QuantLib swap built the same wrong way. **The par-rate surface is
now complete: 50 instruments, every par-rate calibration family Stage A has** (ESTR OIS 17, SOFR OIS
13, EURIBOR-6M IRS 11, basis 9). What remains on the calibration side is the 16 futures rows, and
they are blocked on a DECISION not effort: quoted as prices with convexity stated zero, so what a
futures oracle compares depends on whether that zero is the convention under test or an
approximation being excused — the owner's call.

**D120, 2026-10-12: the first independent number on O3 — the IFT ladder matches a QuantLib central
difference to 4.5e-10, and the residual is the FD's.** The risk ladder is this project's strongest
published result (D90's 6.12x) and had been compared only to itself: to `Dual`, which shares the
recording, and to SwapEngine, whose conventions ours were imported from. Now, on `compare_ois` with
64 trades: **book NPV 9.80e-15 relative** (a first in itself — D109 only had SwapEngine's 7.829e-15)
and the **16-bucket ladder 4.46e-10 relative, worst**. The residual is the finite difference's own:
swept over the bump, the worst error falls 3.3x per halving from 0.1 to 0.05 bp — h², i.e.
truncation — reaches 4.46e-10 at 0.025 bp and then **rises** at 0.0125 bp as round-off takes over.
That V is a central difference's error curve, so **the engine's analytic ladder is the accurate
side**. Gated with a degeneracy control (6 positive / 10 negative buckets, largest 19.2% of the
total), because a one-signed or concentrated ladder would make the agreement cheap. **What it does
NOT establish**: the pricing and the differentiation are independent, the calibration map is ours in
both arms, so this checks the adjoint and the IFT's composition and NOT the solve — the pillar
mismatch of D115 §5 is the one remaining structural obstacle to checking that too.

**D119, 2026-10-12: the TWO-CURVE oracle — 41 Stage A par rates to 5.3e-11 bp, and one row of 41
carried the stub convention.** D117 listed a two-curve fixture as the enabling work; it was not
needed, because **Stage A already has the shape** — its `EUR-EURIBOR-6M-IRS` rows forecast on slot 3
and discount on slot 1, and at `quote_noise_bp = 0` its quotes are the generating curves' exact par
rates. Measured against QuantLib 1.35: `EUR-ESTR-OIS` 17 rows **5.25e-11 bp**, `USD-SOFR-OIS` 13
rows **5.15e-11 bp**, `EUR-EURIBOR-6M-IRS` 11 rows **7.63e-13 bp**. Two currencies, three calendars,
a 30/360 annual fixed leg against an ACT/360 semiannual float, forecast ≠ discount, and the
sub-annual single-period clause. Dates and accruals matched before the rates did. **The 18M ESTR row
— the only Stage A instrument that is neither sub-annual nor a whole number of years — disagreed by
0.0265 bp until the stub rule was corrected from Forward to Backward generation**, because the two
agree on every other tenor: a convention applying to every stubbed trade a desk books was visible in
ONE calibration row. Coverage of a convention is not coverage of the instruments that use it. Tally
across four oracle builds: **3 harness defects to 1 genuine disagreement** — localise, then conclude.

**D118, 2026-10-12: the EURIBOR day-count `disagree` is worth EXACTLY ZERO, and can only bite on a
stub.** Stage A's 54 par-rate quotes computed twice, the second time with both EURIBOR IRS fixed legs
flipped to SwapEngine's **30E/360** via `$EPYKOS_BLUEPRINTS` (no code change; the override was
verified to reach the loaded registry before the result was believed): **not one quote moved by more
than 1e-9 bp**. The reason is structural and the opposite of what one would guess — 30/360 (US) maps
a day-31 END to the 30th only when the START is the 30th or 31st, so **a swap rolling on the 31st
every period is UNAFFECTED** and `eom: true` does not expose the question. Only a **stub** beginning
mid-month and ending on a 31st can, and Stage A's EURIBOR swaps roll annually from spot and are
stub-free. Gated in two halves: the bases really are distinct here (five dated cases, or the zero
could be two names for one function), and no Stage A 30/360 fixed accrual has the exposing shape
(109 scanned, 0 exposed, and the scan fails if it finds none). **Neither of D117's two EUR questions
needs resolving to trust any number this repository quotes** — ≤0.019 bp and exactly 0 — which
settles the priority without settling the convention.

**D117, 2026-10-12: it is not one convention, it is ALL of them — and an oracle constrains only
what it is sensitive to.** The audit: **17 of 17 instrument conventions, 7 of 7 calendars and 4 of 4
indices in `blueprints/conventions/` were imported from SwapEngine.** So the head-to-head cannot
check any of them, and the oracles are the only thing that can. The `cross_check` field records what
the import found: **two `disagree`** (`EUR-EURIBOR-3M/6M-IRS` fixed-leg day count — SwapEngine
30E/360 against our 30/360) and **one `partial`** (`EUR-ESTR-OIS` payment lag 2 against TP ICAP /
LCH 2019's 1 BD, "recorded, not resolved") — three named convention questions adjudicated by
nobody. The payment-lag one is now PRICED rather than argued: **worth at most 0.019 bp** (1Y), under
0.007 beyond 2Y, so it is real and far below bid-offer. `compare_ois` derives its index and currency
from the blueprint's convention and rejects a non-OIS one rather than giving an IRS a one-curve
world; the USD numbers are bit-identical after the change. **And D115's claim that the fixing
calendars "agree day for day" did not follow from the par rate**: EUR-ESTR projects 257 observation
days against SOFR's 249 and the 1Y par rates differ by 4.37e-12 bp, so the rate is nearly blind to
day PLACEMENT. The claim is true and is now gated directly against QuantLib's `fixingDates()` — 249
dates, 0 mismatches — with the 249-vs-257 insensitivity gated too, so the note cannot rot. The
general lesson: **an oracle constrains only the quantities its comparison is sensitive to**, so each
clause of an agreement claim needs its own sensitivity argument or its own test.

**D116, 2026-10-12: the RFR observation conventions — and the first real disagreement.** Over the four other
single-curve SOFR families (`compare_ois` gains a `blueprint` option whose default leaves every recorded number
bit-identical): `lookback` **9.0e-13 bp**, arithmetic `averaged` **1.1e-12 bp** (the family that does not telescope,
so D81's collapse never touches it — agreeing at that level means the 249-entry observation table is right day for
day), the lockout confirmed live on both sides (8.29e-9 against QuantLib's 8.33e-9 at 40Y). But
**`observation_shift` differs by 2.12 bp at 1Y**, decaying as 1/T, and the cause is exactly one division: ours is
QuantLib's times `d_accrual / d_obs`, which on the single-period 1Y is 367/365 to **1.3e-13**. Our window and its
weights are right — they sum to the observation period's own 365 days against an accrual of 367, which is what
distinguishes shift from lookback. What differs is whether the compounded amount is divided by the observation
period's length (ours, which is ISDA's Rate x Calculation-Period-DCF structure) or is the interest directly
(QuantLib's). **Both readings are defensible; the resolution is the owner's and needs the 2021 ISDA Definitions, not
a preference.** The gate pins the mechanism and not the verdict, in two halves: the gap must exceed 1 bp, and it must
equal `d_accrual/d_obs` with both day counts read off the fixture.

**D115, 2026-10-12: the second oracle, on the INSTRUMENT — and our conventions were imported from the engine we
compare against.** `blueprints/conventions/instruments.json` records USD-SOFR-OIS as
`"imported_from": "SwapEngine/conventions/conventions.json"`, so a convention error on their side is in this tree
too and **D109's head-to-head is structurally incapable of seeing it** — that is the honest answer to "have we done
the full h2h". `tests/compare/ois_par_oracle_e0_test.cpp` compares all sixteen `compare_ois` SOFR OIS par rates to
QuantLib 1.35: worst **4.14e-11 bp**, and **5.58e-11 bp** on the engine's own CALIBRATED curve, so QuantLib finds
every instrument at par on the curve we solved — the calibration is verified, not only the pricer. It covers the T+2
spot on the joint SIFMA-and-Fed calendar, the forward annual roll on an unadjusted grid, EOM, ACT/360 both legs, SOFR
compounded over 249 daily observations, the two-business-day payment lag and the discounting. A negative control
ships with it (a 1 bp curve bump moves the rates 0.87 bp, ~1e8x the tolerance). **The first run reported the ENGINE
as wrong by 0.126 bp and the harness was at fault**: `MakeOIS` and `OISRateHelper` generate the schedule backward
from the ADJUSTED termination, leaving a spurious three-day front stub on exactly the five tenors whose roll date is
a weekend — a disagreement localises to the harness before it localises to an engine. Both oracles are ungated and
run in CI, where `tools/h2h/` never does.

**D114, 2026-10-12: the first INDEPENDENT oracle, and `monotone_cubic` is not QuantLib's.** D108's grant to lift
their test CASES sat unused for three days; this takes it, from two of their 33 T1 files, and the expectations are
QuantLib 1.35 hex-float literals so **D12 acquires nothing**. Four schemes match number for number —
`Flat`/`ForwardFlat` **exactly 0**, `Linear` 6.9e-18 value / 5.6e-17 integral, `NaturalCubic`/`CubicNaturalSpline`
1.4e-17, `Hermite`/`Parabolic` 1.4e-17 — and `NaturalCubic` is the strongest of them, because it solves the
tridiagonal system by Thomas over `Scalar` and QuantLib solves the same system its own way. `monotone_cubic` matches
**nothing** in QuantLib: its tangents are QuantLib's `Parabolic` exactly, its limiter is one QuantLib does not
implement, and QuantLib's own Hyman filter is a structural **no-op** on parabolic tangents (`tmp[i] == pm` makes its
sign test and its min vacuous, verified bit-identically). That settles D110's open question. The contract difference
is real and in our favour on one axis — our overshoot on a wiggly dataset is 0 against
`MonotonicCubicNaturalSpline`'s 2.7e-3 — but **not a QuantLib defect**: its guarantee is documented only "in the
regions of local monotoniticity of the input", which that segment is not. Every other gate here compares one of our
evaluations with another; **agreement with SwapEngine is not validation, because a shared convention error produces
perfect agreement and two wrong engines.** D92 §5's laundering guard bit once in the doing: an identification with
QuantLib's `Harmonic` scheme held on three sample points and one dataset and was refuted by two knot sets.

**D113, 2026-10-12: D104's shipped regression was chunk COUNT, reached +25%, and the fix is mostly a revert.** The
ROADMAP's own account of it — "B = 3, 7, 11, 15 by 4-13%", fixed by one condition on a tail of 3 — was wrong in three
ways: it measured `lane_tile` 8 only, where the reference is already chunked, and at `lane_tile` 64 the adjoint loses
**24-25%** at B = 15 and 63 (five and seven chunks against one). The tail-of-3 rule was built and measured first and
fixed the interpreter everywhere while leaving the adjoint untouched at +4%. `decomposed_widths` = {2, 5, 6} now names
the three widths whose decomposition is measured faster than the generic kernel in **both** runtimes; all five
non-variant widths below 8 were measured at two lane tiles, and 3 and 7 lose. No regression remains above the ±2.5%
noise floor, the three wins keep full size, and the absolute default-path gains at `lane_tile` 64 are large (adjoint
µs/lane: B = 15 11.06 → 8.57, B = 63 9.41 → 7.41). D104's gate asserted that the decomposition used specialised
kernels, which stayed true while the decomposition got slower — *a gate binds only what it can observe*, for the
sixth time in this repository and the first on a performance quantity.

`ctest`: **156/156** on this box's release preset (with `EPYKOS_LEGACY_SEARCH=ON` and `EPYKOS_H2H=ON`, which add
tests; the default configuration's count is lower). **CI green on all four jobs** (ubuntu
release / reference / mutation, macos release) at `6d68b09`. **`integrate/invariants` was merged into
`main` on 2026-10-12 (`046427d`): 82 commits, D95–D116** — §1b's three invariants, the exotics kill
test, the head-to-head lifted to Stage A, and the first independent oracles. The merged tree is
byte-identical to the CI-green tip. **`main` is the line of development**; `origin/integrate/rebuild`
(`fb5f0ed`) and `origin/integrate/invariants` are retained as history, not branches to push to.
`v1.0-m4` tags the pre-rebuild engine.

**D106, 2026-10-09: the exotics kill test — D1's revisit clause has FIRED, and D81's collapse is
rates-only.** Commissioned by an external review whose summary was *"you have solved the hard,
generic problems on the easiest asset class."* Criterion committed **before the fixture existed**
(verified from git: `6e3a68a` touches only the criterion doc). On a scripted path-dependent payoff
the engine measures **0.45–0.78x its own naive templated `double`** — FAIL at all nine swept points,
degrading in both path and step count, with agreement 0.00e+00–4.03e-16: the right answer, computed
slowly. The mechanism is not the one D1 names: a fixed **2.6–6.0 ns per path-step**, because a path
grid has one row per path-step and almost no sharing, where the rates book has 1,000 trades sharing
one curve. **D81's 2,565x collapse measures 1.22–1.25x here** — it is an interest-rate phenomenon,
and that pair of numbers is the cleanest statement of where this engine wins. Catalogue coverage is
**anti-correlated** with the verdict (99.6% row coverage scores 0.568x), so the pre-registered
diagnostic fails but does not predict. Structural finding: **a `Select` inside a recurrence destroys
the scan layout** — zero scan rows, coverage 1–4%, domain count linear in step count — which every
barrier, lookback and cliquet hits, and which needs no op-set change. The engine's strengths are
untouched and remain real; what is now measured is that their benefit is confined to workloads with
high structural sharing. **The decision — JIT below the pin, a different execution strategy for
low-sharing programs, or narrowing the claimed domain — is the owner's.**

**D93, 2026-10-06: the architecture is now specified as INVARIANTS WITH GATES (§1b), because goals
do not bind.** Asked whether these capabilities should have driven the design from day one: partly —
two representation decisions were load-bearing and unexamined (`SlotKind::Gather` as an addressing
mode, whose reverse is a scatter-accumulate the IR cannot express; `Dual<N>` over hardcoded `double`
rather than `Dual<Scalar,N>`, which forecloses every second derivative — **the engine cannot compute
a gamma by any route**). But the expensive foundations are right, which is why D81's collapse was
possible at all, and fixing either is an extension rather than a demolition. The cause was stating
goals where closure properties were needed, and `DESIGN.md` §6's named-but-ungated W-cache is the
proof that a goal does not survive a milestone. **§1b's three invariants (I1 closed under
differentiation, I2 second order reachable, I3 the engine's own maths recorded) all currently FAIL,
each with a stated gate**, and §10 now derives from them. Not a rewrite; and argued as capability,
not performance, since D92 priced the optimisation case at roughly nothing.

## Rules
- **`docs/PRINCIPLES.md` is the contract** and outranks every other document, this one included. It is **rewritten in
  place**, never appended to; so is the Status block above (PRINCIPLES.md §9).
- **`docs/DECISIONS.md` is an append-only ledger**: the evidence and the reasoning as they stood on the day. Entries are
  cited, not read end to end, and are never edited afterwards except to add a one-line pointer to what superseded them.
  A change to a principle lands as a ledger entry in the same commit as the rewrite of `PRINCIPLES.md`.
- **No code shared with SwapEngine (D11).** Never copy, port or vendor anything from that checkout; the engine is
  written from these docs. **One exception, owner-granted (D78): `tools/h2h/` alone** may compile against its public
  facade so both engines can be timed in one process. It is behind `-DEPYKOS_H2H=ON`, OFF by default, never in CI, and
  `bench/compare/README.md` §0 lists every file read. Nothing under `include/epykos/` or `src/` may follow it.
- **Maths is written once**, templated on `Scalar`. No hand-written derivatives, no per-product hot-path kernels.
- **Recording discipline:** constants are leaves; no implicit `Rec → double`; value branches use `select`;
  `structural_if` only for branches that provably do not depend on inputs; no `std::max/min/abs` on a `Scalar`.
- **A rewrite is licensed by which side of THE PIN it is on** (PRINCIPLES.md §1), not by a declared exactness class.
  Above the pin — canonicalise, simplify, solve recurrences — the engine chooses WHICH expression to evaluate, is judged
  against the recorded expression in exact real arithmetic, and may re-round. Below the pin — inference, planning,
  execution, the adjoint — it chooses HOW, and is bit-identical to the pinned tape under the declared contraction and
  transcendental policy (§5.3). Every rewrite still ships with its differential test and a mutation test.
- **Verification before features:** round-trip identity and differential tests land with the pass that needs them.
- **Performance claims are measured**, per machine+toolchain fingerprint, never compared across fingerprints; state load
  and flags alongside any number. Estimates are labelled as estimates.
- **Fixtures are generated from the seed in `docs/WORKLOADS.md`.** No data files in the repo, except definitions:
  conventions, curve definitions and instrument blueprints are JSON under `blueprints/` (D36).
- **Dependencies are fixed by D12.** Adding one needs a decision entry.
- **Keep the docs current:** a change to the op set, the pipeline or a milestone updates `docs/DESIGN.md` /
  `docs/ROADMAP.md` in the same commit.

## Build
```
scripts/bootstrap.sh            # fetch pinned third_party (Eigen 3.4.0, GoogleTest 1.18.0, Google Benchmark 1.9.5)
cmake --preset release && cmake --build --preset release
ctest --preset release
cmake --preset reference        # -ffp-contract=off build for E0 gates (then build/ctest --preset reference)
cmake --preset debug            # -O0 -g
scripts/mutation_test.sh        # mutation gate: builds the `mutation` preset (reference + EPYKOS_MUTATIONS=ON) and
                                # runs the gate tests once per registered mutant; every mutant must be caught (D32)
cmake --preset profile           # release + EPYKOS_EXEC_PROFILE=ON (D48): tools/costmodel/'s calibration only,
                                # never a gated or perf-gated build (adds instrumentation to every Interpreter::run())
```
Presets build into `build/<preset>/`. Flags per D13; the `-march=x86-64-v3` flag is dropped on non-x86-64 hosts (the
arm64 CI runner) and `build/<preset>/epykos_flags.txt` / `epykos::build_flags()` state the flags actually used.
Fingerprint: `scripts/fingerprint.sh`.
Definitions are data (D36, D42, D43, D44): `blueprints/conventions/*.json` is the conventions registry, `blueprints/instruments/*.json`
the instrument blueprints, `blueprints/curves/*.json` the curve definitions and `blueprints/problems/*.json` the problem
definitions (the Stage A desk problem: `fixtures/stage_a.hpp` fills one from the seed), located at build time (`EPYKOS_BLUEPRINTS_DIR`
= the source tree's `blueprints/`) or by `$EPYKOS_BLUEPRINTS`; the citations are in `docs/G4_BUNDLE.md`.
Benchmarks and the perf gate (D29): `bench/run.sh build/release/bench/<name>_bench [gbench args]` runs one binary (refuses
at 1-minute load > cores/2) and writes `bench/results/<fingerprint-id>/<name>.json` (fingerprint, loads before/after, commit,
preset, parameters, min/median/p90); `scripts/perf_gate.py <that file>` compares it with `baseline.json` of the same fingerprint
only (> 1.25× a median fails, exit 1; load, cross-fingerprint or no baseline: refused, exit 2) and with `bench/targets.json`;
`--accept` moves the baseline — perf commits only, before/after in the message. The baseline is keyed by that `<name>`, the one
`run.sh` derives from the binary, so the default invocation is what it gates; `--name` runs are exploratory, not gated (D34).

Adding code needs no CMake edits (all globs are `CONFIGURE_DEPENDS`): headers under `include/epykos/**` and sources
under `src/**/*.cpp` compile into the library `epykos` (`include/epykos/fixtures/` and `src/fixtures/` are test-only
fixtures, not engine API; engine headers never include them; D28); `tests/**/<name>_test.cpp` becomes the gtest
executable and ctest entry `<subdir>_<name>_test`; `bench/**/<name>_bench.cpp` becomes a Google Benchmark executable
(built, never run by ctest or CI); `bench/hand/` is the hand-fused reference kernel, the static library `epykos_hand`
that `tests/hand/` and the hand benches link (D9, D28). **Tests named `*_e0_test.cpp` are compiled with
`-ffp-contract=off` in every preset** (label `e0`, `ctest -L e0`): that is how E0 gates get the reference flags.
**Library sources named `src/**/*_e0.cpp` are pinned the same way in every preset** (fixture generators, the
interpreter kernels; D25), and so is `bench/hand/*_e0.cpp` (the hand-fused reference; D28) — a clang pragma is not
enough, GCC contracts in C++ even in ISO mode. An E0 gate that crosses into any other code compiled in `src/` runs
under the reference preset.


## Commits
`type(scope): summary`, type ∈ `feat|perf|fix|test|bench|refactor|build|docs|chore`. `perf` commits include measured
before/after. Do not put model identifiers in commits or code.
