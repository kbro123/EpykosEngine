# `bench/compare/` — the MX head-to-head, and exactly what is and is not like-for-like

D21 requires this file: "the comparison lives under `bench/compare/` with a README stating exactly
what was and was not like-for-like". Its numbers are informational and never gate anything (D9).

## 0. What was read of the other engine, and under what licence

The second engine is **SwapEngine**. It began as a strict **black box** (D21): binaries executed,
stdout read. The licence has widened twice, and both widenings are owner decisions, not drift:

| when | what it permits | where it may be used |
|---|---|---|
| D21 (2026-09-22) | execute its binaries, read its `README.md`, its baseline prose, its public API descriptor and one committed example request | anywhere in `bench/compare/`, `scripts/` |
| **D78 (2026-09-27)** | **COMPILE against its public facade headers** so both engines can be timed in one process | **`tools/h2h/` alone**, behind `-DEPYKOS_H2H=ON` |
| D92 (2026-10-06) | read its `OPTIMIZATION.md` | reasoning only; it priced its "single biggest win" at ~0 for us |
| **2026-10-09, owner** | **READ ITS CODE FREELY** | reasoning and `tools/h2h/`; still **nothing copied, ported or adapted** |

**What has NOT changed, and is the point:** nothing under `include/epykos/` or `src/` may contain,
derive from or imitate anything in that checkout. The clean-room property is exactly what makes a
timing comparison between the two worth running at all — if either engine's maths had been copied
from the other there would be nothing to compare. `tools/h2h/` is the only target in this repository
that compiles against those headers; it is OFF by default and never configured in CI.

The 2026-10-09 widening paid for itself immediately and in one specific place. §3 used to say most
of the request schema "was not read at all but recovered by probing the CLI's own error messages",
and D90 §4 records a whole measurement run lost to handing a codec the wrong shape. That is over:
`include/swaps/api/codec.hpp` declares **`bundle_to_json`** as well as `bundle_from_json`, and
`api/codec.cpp` is the schema. `tools/h2h/h2h_bridge.cpp` now builds `cal::BundleProblem` and
`pf::MultiCurveBook` as **typed C++ objects** the compiler checks, and serialises the bundle through
**their own `bundle_to_json`**, so the exchange file is their schema by construction rather than by
this repository's belief about it. Probing is retired.

## 1. What is compared

Two named problems, and the shape is a **parameter** (`tools/h2h --problem`), not a constant:

| `--problem` | curves | currencies | calibration instruments | book |
|---|---|---|---|---|
| `compare_ois` | 1 (USD SOFR OIS) | 1 | N fixed-vs-compounded-SOFR OIS, par-rate quoted | N plain spot-starting OIS |
| **`stage_a_h2h`** | **4** (USD-SOFR, EUR-ESTR, EUR-EURIBOR-3M, EUR-EURIBOR-6M) | **2** | **70**: 4 deposits, 16 money-market futures, 41 OIS/IRS par rates, 9 3s6s tenor-basis par spreads | **2,000 trades over 8 blueprints in 5 families**, ~20% seasoned with realised fixings, 40 netting sets |

`compare_ois` is retained verbatim, with `--trades` / `--tenors` / `--seed` / `--start-years`
unchanged, so **D90's and D81 §7's numbers stay reproducible**. It was the only problem in this
comparison until 2026-10-09, and §4 item 7 of the version before this one listed what it left out:
*"multi-curve and tenor basis, EUR, ... seasoned trades with realised fixings"*. Those are not
things this engine cannot price — `blueprints/problems/stage_a.json` is a problem it has priced
since M3 — so **every number this project quoted against the other engine covered roughly a quarter
of what it can already do.** `stage_a_h2h` closes that, and needed no new maths on our side.

The outputs compared are `PROBLEM.md` §2's

| output | quantity |
|---|---|
| O1 | the calibrated curves — discount factors at 200 sample times **per curve, on that curve's own grid** (§5) |
| O2 | book NPV, per product family and in total |
| O3 | the bucketed delta ladder d(PV)/d(quote), through the adjoint and the IFT |

O3 is the target: it is where the design claim lives (mechanical adjoints against
bump-and-recalibrate), and it is the one ladder both engines compute analytically.

### `stage_a_h2h` is `stage_a` with four field values changed, and that is gated

`blueprints/problems/stage_a_h2h.json` is `stage_a.json` with each curve naming its **`-LOGDF`**
definition instead of its linear-zero one, and **nothing else** — same seed, same valuation date,
same quote noise, same fixings, same 2,000-trade mix, same tenor and notional distributions, same
seasoned fraction, same netting sets. It is the same desk problem, trade for trade.

That is not a tuning choice, it is §3's matched-ness requirement: **log DF piecewise linear on
`{0, knots...}` — piecewise-constant instantaneous forward — is the only interpolation family the
other engine's curve shares** (D71 measured it: the closed form reproduces its sampled discount
factors to 0.000e+00 relative). `stage_a.json`'s linear-zero parameterisation has no counterpart
there at all, because that engine's free variables are its curves' own **instantaneous forwards**,
so its `Scheme::Linear` interpolates the forward — a different function space from a
piecewise-linear zero rate. Comparing the two directly would measure the schemes, not the engines.

`USD-SOFR-LOGDF` already existed (it is one of `stage_a.json`'s own scheme variants);
`EUR-ESTR-LOGDF`, `EUR-EURIBOR-3M-LOGDF` and `EUR-EURIBOR-6M-LOGDF` were added to
`blueprints/curves/eur.json` for this, each its parent's instrument set verbatim.

**`tests/compare/h2h_problem_test.cpp` gates every clause of the two paragraphs above** — field by
field, no hashing, so a failure names the field that moved — and it needs no second engine, so it
runs in CI like any other test. A curve definition quietly edited to another scheme or another
instrument set would otherwise surface as what looks like an engine disagreement, which is the most
expensive kind of confusion.

**How narrow that is, as a number rather than a feeling: `COVERAGE.md` in this directory.** It is
generated by `tools/coverage/` and it exists because the paragraph above is a prose description of a
comparison surface, and prose does not go stale loudly. The matrix puts the other engine's
calibration instruments, problem families and surrounding capabilities against ours on three axes
and reports the fraction we can compare on *today* — separately from the fraction we support and
merely do not exercise, which is the gap this fixture is responsible for. Our side of it is
discovered from the shipped enums and `blueprints/` at run time and the head-to-head's own reach is
read off `fixtures::compare_ois` by building it, so adding a product moves the number without
anyone editing a list; the other engine's side is a dated snapshot that cites a file and a symbol
per row. Regenerate with `cmake --build --preset release --target coverage && ./build/release/tools/coverage/coverage`.

Two things that section says about *this* file, and which belong here too. First, §4's caveats are
not all of one kind: the `logdf` curve, the square Jacobian and the stub-free book are **deliberate
agreement controls** — widening them would damage the comparison rather than improve it — while one
curve, one currency, one instrument type and the empty fixings history are nothing but fixture
narrowness. Second, the restriction to OIS understates what is already built: Stage A's
*calibration set* exercises all five of `instrument::Kind`, and its book runs to three kinds over
four coupon mechanics, so a second instrument type here is a fixture change rather than a product
change.

## 2. The pieces

```
tools/compare/compare_ois        builds fixtures::compare_ois, records ONE tape, calibrates
                                 through the implicit node, prices the book, takes the ladder.
                                 Writes --exchange (the NEUTRAL problem) and --results (our answers).
scripts/compare_swapengine.py    feeds that neutral file to the other engine's public JSON CLI
                                 and diffs the two answer sets. Exit 1 = they disagree.
                                 BLACK-BOX route, compare_ois only.
tools/h2h/                       both engines in ONE process on one clock (D78, D90).
  h2h_problem.{hpp,cpp}          the named problems on OUR side: the shape as a parameter
  h2h_bridge.{hpp,cpp}           our row tables -> their BundleProblem / MultiCurveBook, typed,
                                 per coupon, with an audit of which form each coupon took
  h2h_main.cpp                   the CLI, the agreement gate, the interleaved measurement
bench/compare/ois_ladder_bench   our side's timing: BM_LadderChord / BM_LadderWarm /
                                 BM_LadderCold / BM_Evaluate / BM_Calibrate / BM_Build.
tests/compare/ois_test           our side alone: the fixture calibrates, the ladder agrees with
                                 bump-and-recalibrate. Runs in CI; needs no other engine.
tests/compare/h2h_problem_test   the head-to-head's problem DATA (§1). Also CI, also no other engine.
tools/coverage/coverage          the capability inventory: writes COVERAGE.md. Links `epykos`
                                 only, takes no timings, safe on a loaded box. Exits non-zero
                                 rather than emitting a matrix this repository has outgrown.
```

### `tools/h2h` — the recipe

**`--bundle` and `--book` are gone.** h2h builds the other engine's objects itself, from the same
`ProblemView` it builds ours from, so there is no second document to get wrong and no Python step.
The footgun D90 §4 wrote down — hand it the raw exchange form and its codec throws
`value is not an object` out of `boost::json` — cannot happen any more, because there is no file in
the path at all. `--exchange <file>` still writes one, for inspection, in their schema through
their own serialiser.

```
cmake --preset release -DEPYKOS_H2H=ON -DSWAPENGINE_ROOT=<path to that checkout>
cmake --build --preset release --target h2h

# the headline: the whole Stage A problem, every product family, 256-row ladder
./build/release/tools/h2h/h2h --problem stage_a_h2h --per-family --ladder-rows 256 --reps 20

# D90's shape, reproduced: 25 knots, 1,000 OIS trades, one-row ladder
T=1Y,2Y,3Y,4Y,5Y,6Y,7Y,8Y,9Y,10Y,11Y,12Y,13Y,14Y,15Y,16Y,17Y,18Y,19Y,20Y,25Y,30Y,35Y,40Y,50Y
./build/release/tools/h2h/h2h --problem compare_ois --trades 1000 --tenors "$T" --reps 20 \
    --ladder-rows 1
```

The binary **reads the 1-minute load itself and refuses above cores/2** (the rule `bench/run.sh`
enforces and `tools/ladder` adopted in D103); `--no-load-check` overrides it and says so in the
footer of every report it prints. It prints the load before and after, the fingerprint's flags,
`max_batch`, `lane_tile` and the ladder row count beside every number, and it **exits 1** if any
product disagreed.

## 3. The exchange form(s)

There are two, for two routes, and they are not the same object.

**The neutral form** (`fixtures::compare_ois_exchange_json`, §2's black-box route) is purely
time-based: year fractions from the valuation date on ACT/365F curve time, and nothing else. No
calendar, day-count name, index name or scheme name is needed to reprice it.

* `knots` — the curve's knot times, ascending.
* `instruments[]` — `key`, `tenor`, `market`, `effective`, `termination`, `t_maturity`, and
  `fixed[]` / `float[]` coupon lists.
* `book[]` — the same, plus `notional`, `side` (+1 receives fixed) and `fixed_rate`.
* a coupon — `t_start`, `t_end`, `t_pay`, `tau_accrual`, and for a compounded one `tau_obs` and
  `n_obs_days`; with `--obs-days`, also `obs_days[] = [t_rate, t_next, tau_rate, weight]` per
  projected observation day.

It carries one curve and one product and **cannot carry Stage A**: it has no vocabulary for four
curves, two currencies, a tenor-basis quote, an averaged coupon, a lockout or a realised fixing.
Inventing that vocabulary would have meant designing a second schema and writing a converter for
it, when the target schema already exists and can now be read.

**The typed form** (`tools/h2h/h2h_bridge.cpp`) is therefore the h2h route: our row tables become
their objects directly. Every mapping is an algebraic identity between
`maths/instrument/coupon.hpp` and their `pricing/cashflows.hpp`, chosen **per coupon** from the row
tables, and the bridge **throws, naming the instrument and the coupon, rather than approximate**:

| ours (`coupon.hpp`) | theirs (`cashflows.hpp`) |
|---|---|
| `RfrCompounded`, plain or observation-shifted, nothing realised | ONE arithmetic sub-period `(t_rate of the first day, t_next of the last)`, `tau_index = obs_tau` — their `standard()` shape |
| the same, partially realised | `compounded`, `realized_factor = realised_factor`, ONE sub-period (`rf·(1 + (DF(s)/DF(e) − 1)) − 1 ≡ rf·DF(s)/DF(e) − 1`) |
| `RfrCompounded` with a **lockout** | `compounded`, one sub-period per day, `weight = weight/tau_rate` |
| `RfrAveraged` | arithmetic, one sub-period per day, `weight = weight_days/tau_rate`, `tau_index = obs_days`, `realized = realised_sum` |
| `TermRate`, projected | ONE sub-period `(t_fix_start, t_fix_end)`, `tau_index = fix_tau` |
| `TermRate`, realised | no sub-period, `realized = rate·fix_tau` |
| `Fixed` | `FixedCoupon{pay, tau}`; the rate rides the position's `fixed_rate`, or `fixed_rates` when the leg steps |
| `Ois` / `Irs` / `Deposit`, `pv = side·(fixed − float)` | one `Position` with `notional = −side·N` (theirs is `notional·(float − K·annuity)`) |
| `Basis`, float vs float | **TWO** `Position`s with opposite notionals; their `Position` is one float leg plus an annuity |
| `Ois`/`Irs` calibration row | `QuoteKind::ParRate`, `fwd` = our float leg, `fixed` = our fixed leg's annuity rows |
| `Basis` calibration row | `QuoteKind::ParSpread`, `fwd` = our spread leg **with the spread zeroed**, `bench` = our flat leg, `fixed` = the spread leg's annuity |
| `Deposit` / `Future` calibration row | `QuoteKind::Rate`, `obs` = that leg's one coupon's observation, `convexity = 0` |

**The one collapse, and it is the reason D81 is in this comparison.** When every day of a compounded
coupon has `weight == tau_rate` and the days are contiguous, the product telescopes to a single
discount-factor ratio, and that ratio is handed over instead of the day list. `epykos::compile`
**derives exactly that collapse on our side** (D81), so the two engines are given the same algebra —
which is what makes the timing a comparison of two engines rather than of two coupon
representations. Plain and observation-shifted coupons telescope; a **lockout does not**, and is
handed over as the day product it is, which is also what our tape is left holding. The harness
prints the audit (`coupons telescoped / day-by-day / averaged`, and how many observation days the
telescope removed) with every run, so that sentence is a measurement and not a claim.

**The curve family is the load-bearing choice** — see §1 for why, and note that it is now four
curves rather than one. On our side it is `{scheme: linear, variable: logdf}` per curve; on theirs,
one region of `Scheme::Flat` over every knot, which is what `flat_hermite(knots, {})` produced in
the old Python route, so the bundle `compare_ois` is handed is unchanged.

## 4. Matched, and not matched

### Matched

| | how |
|---|---|
| valuation date | one date, used by both |
| curve count, currencies, slot order | 4 curves / 2 currencies on `stage_a_h2h`, in OUR slot order, and their `curve` indices are that order |
| instrument set | ours verbatim: every calibration instrument of every curve, in OUR quote order, which is also their residual order — so the two ladders index off the same thing |
| knots | at the instruments' own maturities; **every block square**, so neither side needs a pseudo-inverse or a regulariser, which the two would not share |
| curve family | log-linear DF / piecewise-constant forward on **every** curve, both sides, verified not assumed (§1, §3) |
| schedules, day-count fractions, observation days | generated ONCE, by this engine's conventions, handed over as explicit times and accruals and consumed verbatim. **No schedule is regenerated on their side** — the typed route hands over coupons, not trade dates |
| quotes | the same; a futures row's quote is a PRICE here and a RATE there, converted by the one stated Jacobian (below) |
| book | the same trades: notional, side, fixed rate, spread, effective, termination, and the realised part of every seasoned coupon |
| discounting | per leg, by curve slot, both sides |
| what is differentiated | d(PV)/d(quote) for every quote, on both sides |
| build flags | **re-verified 2026-10-09**: theirs is `-O3 -DNDEBUG -fno-math-errno` + `-march=x86-64-v3` (`CMAKE_CXX_FLAGS_RELEASE` plus `SWAPS_ARCH_FLAGS`, and its generated `simd_config.hpp` reports `AVX2+FMA`, `-march=x86-64-v3`, AppleClang 21.0.0); ours is `-O3 -march=x86-64-v3 -fno-math-errno` under the same compiler. Parity holds |

### Not matched — and these are the caveats on every number below

1. **The compounded coupon's representation. RESOLVED 2026-09-27 (D81).** It was the single largest
   asymmetry: this engine evaluated the OIS float coupon as the daily product over its projected
   observation days while the other engine could only be given one telescoped sub-period per
   accrual, a ratio of work around 250:1 (measured at 47x–169x in §7). `epykos::compile` now derives
   the collapse itself, and §3 hands the collapsed form over wherever it is exact. The agreement is
   **tighter** after the collapse, not looser.
   What survives of the old text and still matters: handing the other engine the *daily*
   decomposition instead was tried and **does not work** — splitting one observation period into
   equal sub-periods changes its calibrated curve (one sub-period over [0,1] gives
   DF(1) = 0.961538461538461, two equal give 0.961168781237985, four give 0.960980344482816), so its
   `obs` sub-period list is not a telescoping product. `scripts/compare_swapengine.py --daily` keeps
   that as evidence, reports the disagreement and exits 1.

2. **Book schedule generation. NO LONGER A CAVEAT on the h2h route.** The black-box route hands the
   book over as trades by index and dates and the other engine regenerates their schedules from its
   own conventions (measured to agree to 2.6e-12, which it could not if a roll, a lag or a day count
   differed). The typed route hands over the coupons themselves, so there is nothing to regenerate.

3. **Jacobian reuse.** Every one of our ladder calls rebuilds the calibration Jacobian
   (`jacobians=1` in the counters), because `ImplicitProgram::adjoint` solves the block and
   factorises before the IFT. The other engine's `risk_us` is taken on a curve that is already
   calibrated and reuses its calibration Jacobian. The gap is therefore **in the other engine's
   favour** and is left in rather than argued away. D92 priced what a free Jacobian would be worth
   to us: 0% of the warm path, 0.5% of a 256-row ladder, 15.8% of a one-row one.

4. **Timing method.** Both sides are wall-clocked from the outside with one `steady_clock`,
   interleaved round-robin, median over repetitions. Their own self-reported `solve_micros` /
   `price_us` / `risk_us` are printed beside ours because they are **not the same quantity**: their
   `risk_us` excludes forming the risk operator M, so it is a floor and the outer clock is the
   honest column. Our `price` excludes tape recording and `ImplicitProgram` construction; theirs
   excludes its own session build.

4a. **Which of THEIR entry points is timed, which D78 got wrong.** Their facade has two reprice
   routes and its own header separates them: `price_portfolio` is the **one-shot** path
   (*"building the W-cache does NOT pay off on a single cold reprice ... so the compiled twin is
   reserved for THIS cached path"*) and `bind_portfolio` + `reprice_bound` is the **amortised**
   one (*"the AMORTIZED path for a live book repriced every streaming tick"*). D78's harness called
   `price_portfolio` in a loop of 20 repetitions, so **D90's "price ~48x ours" measured their cold
   entry point against our warm one.** Both are now timed and both reported; `bound x` is the
   headline. Measured 2026-10-09: their bound path is **2.74x** faster than their one-shot path on
   `stage_a_h2h` at 200 trades, which takes that shape's price ratio from 32.41x to 11.84x.
   Their facade has **no bound risk path** — `price_portfolio_risk` is the only risk entry point —
   so the ladder comparison was already on their best route and does not move.

5. **Extrapolation beyond the last knot differs, and is kept out of reach.** Inside
   `[0, last knot]` the two curves are the same function of the knot values. Beyond it they are not:
   this engine holds the variable flat, so log DF is constant and the forward is zero, while the
   other engine continues the last forward. The comparison is sound only because **nothing is priced
   beyond the last knot** — and that is no longer prose: `h2h` walks every coupon, observation day
   and payment date of every calibration instrument and every book trade of every problem and every
   family, prints `max t read / last knot` per curve, and says `EXTRAPOLATION` loudly if it is ever
   exceeded. `tests/compare/ois_test.cpp` gates the same thing for `compare_ois` in CI.

6. **`compare_ois`'s book is stub-free, which is forced, and it therefore shares structure — a
   caveat that runs in THIS engine's favour.** Every `compare_ois` book trade spot-starts on whole
   annual periods. That is not a preference: give the trades a forward start that is not a whole
   number of years and they acquire a front stub, and the two engines **stop agreeing** on the
   black-box route (measured: book NPV and ladder off by 1e-3 to 1e-4 relative while the curve still
   matches to 5.8e-13, which localises it to the stub). Their `USD-SOFR-OIS` conventions entry
   states no stub rule, ours states `ShortFront`, and there is no trade-level override.
   The consequence: a stub-free book is necessarily on ONE annual grid, so its trades share coupon
   structure and this engine's E0 passes collapse them. Measured: a 256-trade book is 92,534 tape
   nodes against 80,581 for no book at all — about 47 nodes per trade, where the same book with
   distinct starts costs about 125. **On `compare_ois` this engine's book-side cost is understated
   relative to a real desk book.**
   **`stage_a_h2h` does not have this caveat.** Its book draws 12 tenors, 8 blueprints and ~20%
   seasoned trades with ages uniform in 30–1500 days, so its trades do not share one grid; and
   because the typed route hands over coupons rather than dates, a stub is not a disagreement there
   at all. The structural-sharing understatement that item 6 records for `compare_ois` is therefore
   the single biggest reason to quote Stage A numbers in preference to `compare_ois` ones.

7. **The futures quote's units, which is the one conversion in this comparison.** A money-market
   futures row is quoted as a PRICE here and as a RATE there (their market is `1 − price/100`;
   neither engine applies a convexity adjustment — ours is zero by D35, theirs is an input number
   set to zero). So `d(their market)/d(our quote) = −1/100` on those rows, and the harness divides
   OUR ladder column by it before comparing. Applied to our side, stated once, exact. The market
   perturbation each repetition applies is likewise 1 bp **in rate units on every row**, so both
   engines are moved to the same market.

8. **A futures POSITION is not expressible in their book schema.** Ours is
   `side·N·(price − traded)/100`, undiscounted variation margin; their `MultiCurveBook` has exactly
   two position kinds, `Swap` and `Xccy`, and no undiscounted flow. Stage A's book mix contains no
   futures (they appear only as calibration instruments, where `QuoteKind::Rate` carries them
   exactly), so this is a reported gap rather than a blocker — and the bridge throws if a future
   ever reaches the book, instead of pricing something else.

9. **A float-vs-float trade costs them two positions.** §3: their `Position` is one float leg plus
   an annuity, so each 3s6s basis trade becomes two positions with opposite notionals. The book NPV
   and the ladder are both linear in the positions, so the split is exact — but their `n` then
   counts more positions than we have trades, and the harness prints both numbers so the per-trade
   cost of that family is read against the right denominator.

10. **The calibration is sequential here and simultaneous there.** Our `CurveSet` solves one
    implicit block per curve in the dependency order it discovers from the maths (€STR → 6M → 3M;
    SOFR on its own); their `BundleProblem` solves every knot of every curve jointly over one
    stacked vector. Both land on the same curves — the system is square and exactly solvable, and
    §5's gate measures that they do — but the cold-calibration *iteration counts* are not
    comparable row for row, and neither are the starting points (ours is a flat 3% in each knot's
    variable, theirs is `flat_x0`, the mean outright quote).
    One further asymmetry existed in `calibrate_cold` and is **fixed**: D78's harness re-ran their
    `calibrate(x0)` at the **base** market while ours ran at the repetition's **moved** market, so
    the row compared two different inputs. `set_market` (which writes the quote RHS with no solve,
    outside the clock) now puts both at the same market. The difference is immaterial — 1 bp on one
    of 70 quotes — so D90's figure is not invalidated by it, but it was not the same question asked
    twice. `calibrate_hot` never had the problem.

11. **The two-currency book NPV compares only because Stage A has no FX.** Their `MultiCurveBook`
    sums each position's value "in the discount curve's currency" with no conversion, and its
    positions carry no currency tag at all. Our book total is Σ `pv_usd`, and `pv_usd` of a EUR
    trade is `pv ×` the blueprint's placeholder **1.0** (a recorded product Stage B replaces with an
    FX spot input). The two sums are therefore the same number — but only because the placeholder
    is 1.0. The moment Stage B gives FX a real input, this clause stops holding and the book NPV
    comparison needs a per-currency split or their `fx_spot` on each foreign position.

12. **Three of our outputs have no counterpart in their book schema and are not compared.**
    Per-netting-set aggregates (40 of them), per-currency aggregates, and per-leg PVs are all
    recorded outputs of our tape (`StageALayout`) and simply are not quantities their
    `MultiCurveBook` produces. They are not part of the agreement gate and no claim here covers
    them; the per-trade PVs they are folded from **are** gated, via the per-family NPVs.

13. **Not attempted at all.** Cross-currency and FX as live inputs, scenario grids (O4), the G10
    desk, any curve whose scheme is not piecewise-constant forward, and second-order risk (which
    this engine cannot compute by any route — D93). Those are the rest of `PROBLEM.md`; none of
    them is in this comparison and no claim here extends to them.

## 5. The gate

**Agreement gates the timing, before a single microsecond, in the same process, on the very objects
about to be timed.** `tools/h2h` checks three things in order and refuses rather than caveat:

1. **The calibrated curve.** Discount factors at 200 sample times spanning the problem, on **every**
   curve. Tolerance 1e-12 relative. A failure stops the whole run — if the curves differ, nothing
   downstream means anything.
2. **Book NPV**, per family and in total, relative to the book's own gross scale (Σ|trade PV|).
   Tolerance 1e-10.
3. **The risk ladder**, every bucket, relative to the ladder's own infinity norm. Tolerance 1e-9.

These are the tolerances `scripts/compare_swapengine.py` gates on (D71) and they are not relaxed.
**A product family that fails is reported and not timed** — the summary table prints `DISAGREE` in
its ratio columns — and the harness localises the failure by pricing each trade of that family on
its own and naming the worst one. On the black-box route the same rule holds:
`--risk-samples` refuses to take a timing unless the agreement check just passed, and refuses
outright under `--daily`.

## 6. The measurement, when the machine is reserved

`bench/run.sh` refuses above a 1-minute load of cores/2 = 8.0 on this box, and `h2h` now enforces
the same bar itself. Contention well under that bar still makes a 7 ms row noise, so this wants the
box to itself.

```
uptime; ps -Ao %cpu,comm -r | head            # record before: load AND what is running
scripts/fingerprint.sh

cmake --preset release -DEPYKOS_H2H=ON -DSWAPENGINE_ROOT=<that checkout>
cmake --build --preset release --target h2h
./build/release/tools/h2h/h2h --problem stage_a_h2h --per-family --ladder-rows 256 --reps 20
./build/release/tools/h2h/h2h --problem compare_ois --trades 1000 --tenors "$T" --reps 20

uptime                                         # record after
```

Record: the fingerprint; load before and after (the harness prints both); our medians with the
`solves` / `residual_evals` / `jacobians` counters that say what each row actually did; their own
reported times beside ours; `max_batch`, `lane_tile` and the ladder **row count**; and the flags
each side was built with — ours from `build/release/epykos_flags.txt`, theirs from its own build's
CMake cache and generated `simd_config.hpp`, which are the **same** toolchain here but must be
stated rather than assumed (D9, D13).

**Three parameters move results materially and are printed with every figure, because a figure
without them is not a measurement:**

* **the ladder's row count.** D103 §1 found this harness passed `ordinals = {book_output}`, so
  D90's flagship 531.5 us is a ONE-ROW ladder; D92 had separately measured the exit Jacobian's share
  decaying 31.3% at one row → 1.9% at 64 → 0.5% at 256. Both entries were right and neither noticed
  it was describing the other's object. `--ladder-rows` is now explicit and the count is in every
  line.
* **the book size.** D92's 31.3%/0.5% pair is a 16-trade book throughout; at R = 1 the reverse pass
  is 42% on a 256-trade book and 57–62% on a 1,000-trade book.
* **`max_batch` and `lane_tile`.** D104 measured `max_batch = 2` at −17.3% and a trailing group of 5
  at −20.3% once the lane-chunking holes were closed, and `lane_tile` defaults to **8** — not 64, as
  two briefs have now assumed. `--max-batch` and `--lane-tile` set both runtimes and both are
  printed.

**On curve build:** on the h2h route both engines' calibration is wall-clocked from the outside, so
`calibrate_cold` and `calibrate_hot` are like-for-like up to §4 item 10. On the black-box route it
is not available at all — the stateless CLI reports `risk_us` and `price_us` but no calibration time
— which is why `BM_Calibrate` is ours only there.

Threads: this engine is single-threaded throughout — there is no `std::thread` and no
`hardware_concurrency` call anywhere under `src/` or `include/epykos/`. The other engine's thread
use on this path is not established and should be checked in the window before any ratio is quoted.

Then the ratio — and §4 items 3, 5, 6 and 10 go in front of it, not after it. **Within-run ratios
are the comparable quantity; absolutes across sessions are not** (D90: the other engine moved 5–45%
between sessions, and so did we on phases that change did not touch).

## 7. Addendum: §4 item 1's "about 250:1" has been measured, and it is not 250:1 (D74)

Item 1 warned that the book's recorded days are "largely shared away by the E0 passes", and the
warning applies to the calibration side too. `spike/telescoping-prize` recorded the `compare_ois`
fixture TWICE — once as the engine writes the coupon, once hand-telescoped as test-only fixture code
— and measured the difference end to end. D74 has the full table; the summary, at sixteen
calibration instruments and sixteen book trades (D64), fingerprint `d448afd70180`:

| | naive | telescoped | ratio |
|---|---|---|---|
| tape nodes recorded | 1,020,583 | 6,713 | 152.03x |
| tape nodes after the E0 passes | 81,321 | 1,313 | **61.94x** |
| IR steps x rows, whole program | 80,938 | 1,182 | **68.48x** |
| IR steps x rows, the residual slice the solve iterates | 80,319 | 563 | **142.66x** |
| IR scan rows (the compounding itself) | 9,946 | 18 | 552.56x |

In wall clock, same fixture and fingerprint, `bench/run.sh` at 20 repetitions (load 4.90 before,
5.81 after; a confirmation run reproduces every ratio to within 0.8%–5.9%):

| row | naive | telescoped | ratio |
|---|---|---|---|
| evaluate (whole-program interpreter, one lane) | 145.4 us | 3.096 us | **46.97x** |
| calibrate (one full recalibration plus the forward pass) | 26,150 us | 155.0 us | **168.73x** |
| the O3 ladder, warm | 7,341 us | 49.78 us | **147.48x** |

The 250:1 loses about 9.5x before any optimiser is involved: the fixture's 94,942 recorded
observation days are already only 9,946 distinct IR scan rows, because sixteen instruments on one
annual grid out to forty years share their daily steps and `cse` merges them. What is left is still
large. **Quote the measured figures, not 250:1.**

One thing the comparison could not have shown, and this measurement did: against ground truth at 106
significand bits (`epykos::Wide`, D72), **the telescoped form is 39.7x to 53.5x MORE accurate than
the daily product loop** on this fixture, closer to truth on 16 of 16 calibration par rates and 16
of 16 trade PVs. The two engines agreeing to 2.7e-13 on the ladder is therefore not two equally good
answers: it is one path carrying about fifty times the rounding of the other.

**D90's second timing column is retired on this basis.** Its table had `ours naive` beside
`ours telescoped` — the engine's own product-loop recording against D74's hand-written spike. Since
D81 the engine derives the collapse, so **both recordings compile to the same program**: on the
16-instrument / 64-trade fixture, 1,679,435 raw nodes against 11,511, and **3,026 after the passes
either way.** The column was measuring run-to-run noise on one program. `h2h --verify-spike`
re-checks that node equality on demand rather than leaving it asserted; nothing is timed from it.

## 8. The measurement of 2026-10-09

`tools/h2h` as described in §6, both engines in one process on one `steady_clock`, interleaved
round-robin. Fingerprint **`d448afd70180`** (Intel Xeon W-3223, 8 physical / 16 logical cores,
Apple clang 21.0.0), `-O3 -march=x86-64-v3 -fno-math-errno` on **both** sides (§4, re-verified).
1-minute load **5.79 before, 4.99 after** across runs A–C, every run under the binary's own
cores/2 = 8.0 bar, which was enforced (no `--no-load-check` anywhere). **Informational under D9.**
Every ratio below is `theirs/ours`, so **> 1 is in our favour**; every ratio is a within-run ratio,
and the absolutes are not comparable across sessions (D90).

### 8.1 Agreement, which gated all of it

Every one of the eight product families agrees. **Nothing disagreed and nothing went untimed.**

| | worst relative |
|---|---|
| calibrated curves, 4 curves / 800 sample times | **6.717e-15** (tolerance 1e-12) |
| book NPV, whole 2,000-trade book, against gross 9.326e+08 | **7.829e-15** (tolerance 1e-10) |
| 70-bucket ladder, against its own inf-norm 9.745e+09 | **5.969e-15** (tolerance 1e-09) |
| book NPV, worst of the eight families | **4.023e-14** (`USD-SOFR-OIS-SHIFT2-LOCKOUT2`) |
| ladder, worst of the eight families | **6.012e-15** (`USD-SOFR-AVG-SWAP`) |

On `compare_ois` at D90's shape the figures are **1.860e-15** on the curve, **1.412e-15** on book
NPV and **2.896e-15** on the ladder — D90 reported 5.533e-15 and 3.818e-15, so the agreement is
**tighter**, which it should be: the typed route hands over the observation window the engine
actually reads instead of the accrual period the neutral form carried.

The 37 MB exchange document the same run writes was fed back to their **stateless CLI** — a separate
process, a separate code path from the linked session — and accepted: 4 curves, 70 instruments,
2,212 positions, converged in 6 iterations at rms 4.726e-15, book NPV
`6.88072233186896741e+07` against h2h's in-process `6.88072233186942041e+07`. The file route and
the in-process route are the same answer.

### 8.2 Calibration — family-independent, 70 quotes on 70 knots over 4 curves

| phase | ours | theirs | theirs/ours |
|---|---|---|---|
| `calibrate_cold` | 934.4 us | 2,253.7 us | **2.41x ours** |
| `calibrate_hot` | 83.3 us | 23.2 us | **0.28x — we are 3.6x slower** |
| `price_cal_only` (no book) | 8.8 us | — | ours only |

**The cold calibration reverses on Stage A.** On `compare_ois` in the same session it is 0.66x
(we are 1.5x slower, consistent with D90's 1.33x); on the four-curve problem it is 2.41x **in our
favour**. Our sequential block solve pays off as the problem stops being one curve; their
simultaneous 70-knot LM does not. Read it against §4 item 10 — the two solves are different
algorithms, which `PRINCIPLES.md` §4 names as the one admissible difference.
**The warm gap does not improve**: 0.28x here against 0.38x on `compare_ois`, i.e. 3.6x and 2.6x
slower. D90's 2.24x and this 2.6x are the same measurement at the same shape; the four-curve
problem makes it slightly worse, and four block solves instead of one is the obvious reason
(`solves=4 residual_evals=12 jacobians=0`).

### 8.3 The whole 2,000-trade book — and the number this project has been quoting

2,000 trades → **2,212** of their positions (212 basis trades split in two), tape 27,162,726 raw →
**145,090** after the passes, recorded in 8.3 s. `max_batch` 64, `lane_tile` 8.

| phase | ours | theirs | theirs/ours |
|---|---|---|---|
| `price`, their ONE-SHOT `price_portfolio` (D90's column) | 456.7 us | 94,313.5 us | 206.53x |
| `price_bound`, their AMORTISED `reprice_bound` | 456.7 us | 49,938.8 us | **109.36x** |
| `risk_ladder`, **R = 1** (book-level) | 2,022.6 us | 600,763.7 us | **297.02x** |
| `risk_ladder`, **R = 256** (per-trade) | 237,913.2 us | 183,872.9 us | **0.77x — we are 1.29x SLOWER** |

**That last pair is the most important number in this file.** D103 §1 found this harness only ever
asked for one row and warned that one-row ladders are unrepresentative. They are worse than
unrepresentative: between R = 1 and R = 256 **the ladder ratio moves by a factor of 385, and it
crosses 1.** The mechanism is §4's stated asymmetry and is not noise — our adjoint carries the whole
book's forward pass in every one of the 256 rows, because our tape prices the book in one program
and cannot be asked for a subset, while their 256 `price_portfolio_risk` calls on single-trade books
touch 256 trades and no more. Per row: ours 929.3 us against their 718.3 us.
So: **a book-level delta ladder is ~300x ours; a per-trade delta ladder is roughly parity.** Any
claim about "the risk ladder" that does not say which one it means is not a claim.

### 8.4 Per product family — does the advantage hold outside plain OIS?

`--per-family`, each family its own problem with its own tape and its own gate. R = 1, 20 reps,
`max_batch` 64, `lane_tile` 8. `bound x` is the honest price ratio (their amortised path);
`bound x*` is the same against our book-attributable pass (ours minus the 8.8 us calibration-only
twin — arithmetic on two measurements, not a measurement).

| family | trades | their posns | `price x` (one-shot) | **`bound x`** | `bound x*` | **`risk x`** (R=1) | agree NPV | agree ladder |
|---|---|---|---|---|---|---|---|---|
| ALL | 2000 | 2212 | 206.53x | **109.36x** | 111.50x | **297.02x** | 7.829e-15 | 5.969e-15 |
| `USD-SOFR-OIS-SHIFT2-LOCKOUT2` | 209 | 209 | 550.46x | **553.13x** | 619.69x | **506.62x** | 4.023e-14 | 5.463e-15 |
| `USD-SOFR-AVG-SWAP` | 199 | 199 | 206.06x | **13.30x** | 13.89x | **272.51x** | 6.461e-15 | 6.012e-15 |
| `EUR-3S6S-BASIS` | 212 | 424 | 18.32x | **1.27x** | 1.41x | **17.08x** | 2.526e-14 | 1.818e-15 |
| `EUR-EURIBOR-3M-IRS` | 167 | 167 | 22.92x | **1.59x** | 2.03x | **11.69x** | 2.630e-15 | 2.231e-15 |
| `USD-SOFR-OIS` | 481 | 481 | 17.36x | **4.17x** | 5.06x | **10.35x** | 5.726e-15 | 4.472e-15 |
| `EUR-EURIBOR-6M-IRS` | 237 | 237 | 18.84x | **1.47x** | 1.90x | **9.60x** | 1.769e-15 | 1.129e-15 |
| `EUR-ESTR-OIS` | 300 | 300 | 13.49x | **3.27x** | 4.25x | **7.26x** | 1.329e-15 | 1.446e-15 |
| `USD-SOFR-OIS-SHIFT2` | 195 | 195 | 11.31x | **1.91x** | 2.59x | **5.79x** | 2.198e-15 | 1.523e-15 |

**On the book-level ladder the advantage holds on every family, and plain OIS is not where it is
largest.** The range is 5.79x to 506.62x; plain `USD-SOFR-OIS` sits at 10.35x, below `EUR-3S6S-BASIS`
(17.08x), the averaging swap (272.51x) and the lockout (506.62x), and above only `EUR-ESTR-OIS` and
`USD-SOFR-OIS-SHIFT2`. The advantage does not merely survive contact with basis, averaging swaps and
seasoned trades: on the two products that do not telescope it is one to two orders of magnitude
larger.

**On pricing, their W-cache is excellent and the per-family split is what shows it.** At the whole
book their amortised reprice looks 109x slower than ours, but the family rows say that is almost
entirely two families: per position, `reprice_bound` costs **0.24 us** on `EUR-EURIBOR-6M-IRS`,
0.26 us on the basis, 0.43 us on plain OIS — against **14.0 us** on the averaging swap and
**216.8 us** on the lockout. 45.3 ms of their 49.9 ms whole-book figure is the 209-trade lockout
family. Their own header explains it: a compounded or seasoned position "rides the AAD block", so
**their W-cache compiles telescoping and term-rate coupons and does not compile lockout or averaged
ones.** On the products it compiles, their repeated reprice is at parity with ours (1.27x–4.17x);
on the products it does not, we are 13x–553x.

That pair of mechanisms — their W-cache covering the telescoping products, our scan layout covering
the day-by-day ones — is the cleanest statement of where each engine wins, and it is only visible
per family. An aggregate reports 109x and explains nothing.

### 8.5 `max_batch` and `lane_tile`, measured rather than assumed

500 trades, R = 64, 10 reps, load ~5.0, us per ladder row:

| `max_batch` | `lane_tile` | ours/row | theirs/row | theirs/ours |
|---|---|---|---|---|
| 64 | **8** (shipped) | **383.8** | 700.5 | 1.83x |
| 64 | 32 | 470.2 | 674.9 | 1.44x |
| 16 | **8** | 390.0 | 693.0 | 1.78x |
| 16 | 32 | 431.8 | 686.9 | 1.59x |

**`lane_tile = 32` costs 22.5% at `max_batch` 64** (383.8 → 470.2) and 10.7% at 16. The shipped
default of 8 is the right one and the cliff is real. `max_batch` 64 against 16 moves the figure by
1.6%, inside the ~1.5% within-condition spread D104 measured — consistent with D104 §4's finding
that there is no single best `max_batch` default.

Note also the book-size effect on the per-trade ladder: 1.83x in our favour at 500 trades / R = 64,
0.77x at 2,000 trades / R = 256. The per-trade ladder ratio **decays with book size**, for the same
reason §8.3 gives.

### 8.6 `compare_ois` at D90's shape, in this session — what reproduces and what does not

1,000 trades, 25 tenors, R = 1, 20 reps, load 4.99 → 5.70.

| phase | D90 (2026-10-04) | here | verdict |
|---|---|---|---|
| `calibrate_cold` | 1.33x slower | 1.52x slower (0.66x) | reproduced within session drift |
| `calibrate_hot` | 2.24x slower | 2.63x slower (0.38x) | reproduced within session drift |
| `price` | **47.9x ours** | 53.43x ours (their ONE-SHOT) | reproduced — **and both are the wrong entry point** |
| `price`, their amortised path | not measured | **2.92x ours** | the honest figure |
| `risk ladder` (R = 1) | 6.12x ours | **5.82x ours** | reproduced |

Everything D90 measured reproduces. The one row that does not survive is the one D90 did not know
it was measuring: `price_portfolio` is their cold entry point and `bind_portfolio` + `reprice_bound`
is the amortised one, so **"pricing ~48x" was our warm pass against their cold one, and the honest
number on that shape is 2.92x** (§4 item 4a). The risk ladder's ~6x, the headline this project has
quoted since D81 §7, is reproduced exactly — and §8.3 says what it is a ladder of.
