# The convention surface, audited (D131, 2026-10-13)

Every convention defect found in this project — **six harness defects and two engine defects** over
D114–D130 — had one shape: *a convention that was a hard-coded choice where a document could
reasonably decide otherwise, stated nowhere.* Researching them one at a time, as each one blows up
in an oracle, is the slow path. This file enumerates the surface instead, so the unexamined ones are
visible before they cost anything.

**What a row's status means.**

| status | meaning |
|---|---|
| **TERM** | a convention states it; the engine resolves it once and records it. Two implementations given the same trade cannot disagree about it. |
| **FIXED, sourced** | one arithmetic, hard-coded, with a primary source that says it is the only one. |
| **FIXED, one of two sourced** | hard-coded, and the other reading is *equally* documented. A latent divergence: harmless only while nothing consumes it. |
| **OWN RESEARCH** | no primary source adjudicates it. Our choice is recorded; nobody external has agreed. |

**Gated** means a test fails if the behaviour changes, not merely that a number was once checked.

## The surface

| # | convention | where | status | source / evidence | gated |
|---|---|---|---|---|---|
| 1 | roll convention (EOM / day-of-month) | `conventions::ScheduleSpec::eom`, `eom_auto`; resolved onto `Schedule`/`Leg` | **TERM** (D129) | FpML `RollConventionEnum`; ARRC/QuantLib comparison over 2,000 schedules | yes — 4,000 legs, reported term == the term its dates obey |
| 2 | EOM: force vs ISDA inference | same | **TERM** (D129) | ISDA 2006's EOM phrasing is about the *roll date*, which D128 recorded as not decided; the inference is the shipped default | yes — force/infer separated on a non-month-end anchor |
| 3 | shift accrual basis (what the compounded amount is applied over) | `ObservationSpec::accrual`; `Coupon::rate_over_interest_period` | **TERM** (D130) | ISDA, *Documenting RFR derivatives…* (27 Apr 2021) §1.2; ARRC *Updated User's Guide* App. 4 | yes — exactly `d_c/d_o`, vacuous without a shift, refused where it cannot bite |
| 4 | **quoted rate basis for a shifted coupon** | `coupon_rate` returns `(Π−1)/obs_tau` | **FIXED, one of two sourced** | ARRC annualises on `d_o`, ISDA on `d_c`; **both authorities, same cash flow, different quoted rate** | no — and nothing in the engine reports a shifted coupon's rate, so it is worth exactly zero today. The first blotter that prints one prints the ARRC basis under an ISDA-documented trade. Named in D130 §5 |
| 5 | averaged RFR under a shift | `coupon_rate`'s `RfrAveraged` ÷ `obs_days`, times `rate_accrual` | **FIXED, sourced** | the same ISDA §1.2 sentence — the paper is about "compounding/**averaging**" — and our arithmetic already satisfies it: `(Σ r_i n_i / d_o) · (d_o/basis)` cancels to `Σ r_i n_i / basis`, the interest itself | **no fixture shifts an averaged leg**, so the agreement is by construction and unexercised. The cheapest real gap on this list |
| 6 | shift ∘ lockout composed | `observation_days`: shift the window, then freeze the last *m* days **of the shifted window** | **OWN RESEARCH** | ARRC and ISDA document each separately; neither composes them. `USD-SOFR-OIS-SHIFT2-LOCKOUT2` is `cross_check: own_research`. SwapEngine's model carries ONE `RfrStyle` and cannot express the pair | the mechanism is gated (lockout freezes, weights unchanged); the *composition order* is not adjudicated by anything |
| 7 | lockout: weights unchanged | `observation_days` | **FIXED, sourced** | ARRC: "the SOFR rate applied for the last k days … is frozen at the rate observed k days before the period ends" — about rates, not weights | yes (D116: lockout live on both sides, 8.29e-9 vs QuantLib's 8.33e-9 at 40Y) |
| 8 | stub rule | `ScheduleSpec::stub` | **TERM** | per-blueprint; `ShortFront` is own research for the OIS families | yes — but the *harness* translation `ShortFront → DateGeneration::Backward` holds only because our generator rolls backward for a front stub. Documented in the gate (D129 §5), not relied on silently |
| 9 | long-stub merge | `schedule.cpp`: `if (longer) dates.pop_back()` | **FIXED, sourced by definition** | "long" *means* the irregular period absorbs the adjacent regular one | yes (`Schedule.LongFrontStub`) |
| 10 | day counts, incl. 30/360 vs 30E/360 | `DayCount` per leg | **TERM** | D117 records `disagree` with SwapEngine on the EURIBOR IRS fixed leg | yes — and **priced at exactly 0** on all 54 Stage A quotes (D118), with the exposing shape (a stub ending on a 31st) shown absent |
| 11 | payment lag | `InstrumentConvention::payment_lag` | **TERM** | D117 records `partial`: ours 2 BD against TP ICAP / LCH 2019's 1 BD | **priced at ≤0.019 bp** (1Y), under 0.007 beyond 2Y (D117 §2) |
| 12 | spot lag | `spot_lag` | **TERM, sourced** | ISDA MPN 2022-04-08; CFTC MAT; Strata | yes |
| 13 | ModifiedFollowing's month test | `calendar.cpp:221` | **FIXED, sourced** | ISDA 2006; ours, QuantLib's and SwapEngine's agree character for character (D128) | yes |
| 14 | futures convexity = 0 | `instrument::build_future` | **FIXED by ruling** | the owner's D124 ruling: quotes are inclusive of convexity, so zero is the *convention*, not an approximation | yes — SR3 6.25e-11 bp, FEU3 and the deposits exactly 0 |
| 15 | fixing calendar per index | `IndexDef::fixing_calendar` | **TERM, sourced** | gated directly against QuantLib's `fixingDates()`, 249 dates 0 mismatches — *not* inferred from the par rate, which D117 showed is nearly blind to it | yes |
| 16 | realised / projected handover | `realised_observations` | **FIXED, sourced** | only pre-valuation fixings load; verified 1.31e-11 bp on a straddling coupon (D125) | yes |
| 17 | curve interpolation (`monotone_cubic`) | `curve::` schemes | **model choice, not a market convention** | D114: it matches *nothing* in QuantLib; the contract difference is real and documented | yes (4 schemes to ≤1.4e-17) |

## What the audit says to do

**Three rows are not finished, and they rank in this order.**

1. **Row 5 — shift an averaged leg in a fixture.** Our arithmetic is right by the source, and nothing
   exercises it. A convention that is correct by construction and untested is how row 3 got wrong in
   the first place: D116's shift gap sat inside a family that *was* tested, and was found only because
   an external engine disagreed. Cheapest item here.
2. **Row 6 — shift ∘ lockout is unadjudicated**, and the other engine cannot express the pair at all.
   Either find a primary source for the composition or state in the blueprint that the order is ours.
3. **Row 4 — the quoted-rate basis** needs a term the day anything reports a shifted coupon's rate.
   Not before: building it now would be a term with no consumer, which D129 §2 shows is its own
   defect class.

**Two rows are closed by measurement, not by resolution, and that is the right outcome** (D68's
method: price it before fitting it). Rows 10 and 11 are D117's open `disagree` and `partial`, worth
exactly 0 and ≤0.019 bp. Neither needs a primary source to trust any number this repository quotes.

## The rule this file exists to enforce

A convention belongs in the TERM column when **two correct implementations given the same trade could
still disagree about what the trade means.** That is not a question about arithmetic difficulty; it is
a question about whether the trade's terms determine the answer. Every time this project got it
wrong, the terms did not — and no amount of care in either implementation could have closed the gap,
because each was faithfully computing a different contract.
