# `bench/compare/` — the MX head-to-head, and exactly what is and is not like-for-like

D21 requires this file: "the comparison lives under `bench/compare/` with a README stating exactly
what was and was not like-for-like". Its numbers are informational and never gate anything (D9).

The second engine is **SwapEngine**, run as a **black box** (D21): its binaries are executed and
their stdout is read. Nothing derived from it enters this repository, and its checkout is not
modified. What crosses the boundary is the *exchange file*, which this repository writes from its
own `blueprints/`, and the numbers that come back.

Precisely what was read of it, since "black box" is a claim and not a mood (D71):

| read | why it is allowed |
|---|---|
| `README.md` | documentation: how to invoke it |
| `baselines/baselines.json` | documentation: the prose descriptions of what its own metrics measure |
| `api/api_surface.py` | the API descriptor that generates its JSON dispatch and bindings — the public interface D21 permits reading "as far as needed to feed it the same bundle" |
| `bench/fixtures/sofr_bundle.json` | one committed example of that interface's *input*, read to learn the request shape; nothing from it is used as data here |
| `conventions/conventions.json` | D37, and it was already imported into `blueprints/` |
| directory listings | to find the above |

**Not read: any `.cpp` or `.hpp`, anywhere in that checkout** — including `bench/risk_bench.cpp`,
which is where its own risk fixture lives and is exactly why this comparison runs in the direction
it does (§4 and D71 §1). Most of the request schema was not read at all but recovered by probing
the CLI's own error messages.

## 1. What is compared

One USD SOFR OIS curve and a book of OIS trades, producing `PROBLEM.md` §2's

| output | quantity |
|---|---|
| O1 | the calibrated curve |
| O2 | PV per trade and the book total |
| O3 | the bucketed delta ladder d(book PV)/d(quote), through the adjoint and the IFT |

O3 is the target: it is where the design claim lives (mechanical adjoints against bump-and-
recalibrate), and it is the one ladder both engines compute analytically.

## 2. The pieces

```
tools/compare/compare_ois        builds fixtures::compare_ois, records ONE tape, calibrates
                                 through the implicit node, prices the book, takes the ladder.
                                 Writes --exchange (the problem) and --results (our answers).
scripts/compare_swapengine.py    feeds the exchange file to the other engine's public JSON CLI
                                 and diffs the two answer sets. Exit 1 = they disagree.
bench/compare/ois_ladder_bench   our side's timing: BM_LadderChord / BM_LadderWarm /
                                 BM_LadderCold / BM_Evaluate / BM_Calibrate / BM_Build.
tests/compare/ois_test           our side alone: the fixture calibrates, the ladder agrees with
                                 bump-and-recalibrate. Runs in CI; needs no other engine.
```

Run it:

```
cmake --build --preset release --target compare_ois compare_ois_ladder_bench
./build/release/tools/compare/compare_ois --exchange /tmp/x.json --results /tmp/r.json --trades 64
scripts/compare_swapengine.py --exchange /tmp/x.json --results /tmp/r.json \
    --cli <SwapEngine>/build/api/swaps_api_cli --per-trade
bench/run.sh build/release/bench/compare_ois_ladder_bench
```

## 3. The exchange form

Purely time-based: year fractions from the valuation date on ACT/365F curve time, and nothing
else. No calendar, day-count name, index name or scheme name is needed to reprice it. Every
accrual, observation and payment time a price depends on is written explicitly, at 17 significant
digits.

* `knots` — the curve's knot times, ascending.
* `instruments[]` — `key`, `tenor`, `market` (the par-rate quote), `effective`, `termination`,
  `t_maturity`, and `fixed[]` / `float[]` coupon lists.
* `book[]` — the same, plus `notional`, `side` (+1 receives fixed) and `fixed_rate`.
* a coupon — `t_start`, `t_end`, `t_pay`, `tau_accrual`, and for a compounded one `tau_obs` and
  `n_obs_days`; with `--obs-days`, also `obs_days[] = [t_rate, t_next, tau_rate, weight]` per
  projected observation day.

**The curve family is the load-bearing choice.** Both sides are log DF piecewise linear on
`{0, knots...}`, i.e. the instantaneous forward is piecewise constant, so the discount factor at
*every* time — between knots, before the first and beyond the last — is pinned by the knot values
alone. That is what removes the interpolation scheme from the comparison instead of assuming the
two engines share one. On our side it is `{scheme: linear, variable: logdf}`; on the other side it
is every knot in the bundle's `meeting` region with `back` empty, which was **measured** to be that
same family (D71: the closed form reproduces its sampled discount factors to 0.000e+00 relative).

## 4. Matched, and not matched

### Matched

| | how |
|---|---|
| valuation date | one date in the exchange file, used by both |
| currency / curve count | one, USD; a single SOFR curve that both discounts and projects |
| instrument set | sixteen fixed-vs-compounded-SOFR OIS, par-rate quoted, at 1Y–10Y annually then 12Y, 15Y, 20Y, 25Y, 30Y and 40Y |
| knots | sixteen, at the instruments' own maturities; **square**, so neither side needs a pseudo-inverse or a regulariser |
| curve family | log-linear discount factors / piecewise-constant instantaneous forward, on both sides, verified not assumed (§3) |
| schedules | generated ONCE, by this engine's conventions, and handed over as explicit times |
| day-count fractions | ditto: `tau_accrual` and `tau_obs` are ours, consumed verbatim |
| quotes | the same sixteen par rates |
| book | the same trades: notional, side, fixed rate, effective and termination |
| discounting | the same single curve on both sides |
| what is differentiated | the ladder is d(book PV)/d(par-rate quote) for all sixteen quotes, on both sides |
| output set | discount factors at 200 sample times, sixteen model par rates, book NPV, per-trade NPV, the sixteen-bucket ladder |

### Not matched — and these are the caveats on every number below

1. **The compounded coupon's representation.** This engine evaluates the OIS float coupon as the
   *daily product* over its projected observation days — 49,091 of them in the calibration
   instruments and 45,851 more in a sixteen-trade book, each an ACT/360 overnight forward. The
   other engine is given, and **can only be given**, one telescoped sub-period per accrual: 197
   coupon-periods on the calibration side. The two are algebraically equal for the plain
   observation method (the daily forwards are discount-factor ratios whose product telescopes),
   which is why the answers agree — but they are not the same arithmetic, and the ratio of work
   is about 250:1 on the calibration side.
   Handing the other engine the daily decomposition instead was **tried and does not work**:
   splitting one observation period into equal sub-periods changes its calibrated curve
   (one sub-period over [0,1] gives DF(1) = 0.961538461538461, two equal give 0.961168781237985,
   four give 0.960980344482816), so its `obs` sub-period list is not a telescoping product and
   cannot carry our daily ACT/360 semantics. Finding out what it *is* would mean reading its
   source, which D11 forbids. `scripts/compare_swapengine.py --daily` keeps that experiment as
   the evidence: it reports the disagreement and exits 1, and **no timing is quoted from it**.
   This is the single largest asymmetry and it is not in our favour.
2. **Book schedule generation.** We hand over explicit times for the calibration instruments, but
   the other engine's book takes trades by index and dates and regenerates their schedules from
   its own conventions. That the two agree is **measured, not assumed**: per-trade NPVs match to
   2.6e-12 relative, which they could not if a roll, a lag or a day count differed.
3. **Jacobian reuse.** Every one of our ladder calls rebuilds the 16×16 calibration Jacobian
   (`jacobians=1` in the counters), because `ImplicitProgram::adjoint` solves the block and
   factorises before the IFT. The other engine's `risk_us` is taken on a curve that is already
   calibrated and reuses its calibration Jacobian. `BM_LadderChord` asks for the chord policy to
   close this and does not: the factorisation is still built once per call. The gap is therefore
   **in the other engine's favour** and is quantified below rather than argued away.
4. **Timing method.** Ours is Google Benchmark, warm, in-process, median over repetitions, under
   `bench/run.sh`'s load rule. Theirs is the `risk_us` its own JSON response reports — one cold
   sample per process, so it is sampled many times and the median taken. Neither number includes
   JSON parsing; ours excludes the tape recording and `ImplicitProgram` construction, which
   `BM_Build` reports separately, and theirs excludes its own session build.
5. **Extrapolation beyond the last knot differs, and is kept out of reach.** Inside
   `[0, last knot]` the two curves are the same function of the knot values. Beyond it they are
   not: this engine holds the variable flat, so log DF is constant and the forward is zero, while
   the other engine continues the last forward. The comparison is sound only because **nothing in
   this fixture is ever priced beyond the last knot** — the longest book trade matures on the
   longest calibration instrument's own maturity, which is that knot.
   `tests/compare/ois_test.cpp` gates both halves of that sentence: it pins each engine-side
   behaviour and it walks every coupon, observation day and payment date of every calibration
   instrument and every book trade to assert none exceeds the last knot. Extend the tenor set or
   the book without re-checking that test and the agreement below stops being valid.
6. **Not attempted at all.** Futures and deposits as calibration instruments, multi-curve and
   tenor basis, EUR, cross-currency, seasoned trades with realised fixings, scenario grids, the
   G10 desk, and any curve whose scheme is not piecewise-constant forward. Those are the rest of
   `PROBLEM.md`; none of them is in this comparison and no claim here extends to them.

## 5. The gate

**Agreement gates the timing.** `scripts/compare_swapengine.py` exits non-zero when any compared
quantity is outside its tolerance, and a non-zero exit means no timing from this fixture means
anything. The tolerances are in that script; the measured agreement is in `docs/DECISIONS.md` D71
and `docs/RESUME.md` §5. The script enforces this itself: `--risk-samples` refuses to take a
timing at all unless the agreement check just passed, and refuses outright under `--daily`.

## 6. The measurement, when the machine is reserved

Not yet taken (D71). It needs a quiet machine — `bench/run.sh` refuses above a 1-minute load of
cores/2 = 8.0, but contention well under that bar still makes a 7 ms row noise, so this wants the
box to itself. About fifteen minutes end to end.

```
uptime; ps -Ao %cpu,comm -r | head            # record before: load AND what is running
scripts/fingerprint.sh                         # must be d448afd70180 for the agreement above

# ours: chord / warm / cold ladder, evaluate, build, at 16, 64 and 256 trades
bench/run.sh build/release/bench/compare_ois_ladder_bench
scripts/perf_gate.py bench/results/d448afd70180/compare_ois_ladder.json   # no baseline yet:
                                                                          # expect exit 2, then
                                                                          # --accept to seed it

# theirs: the SAME exchange file, its own self-reported risk_us, 100 cold samples per book size
for T in 16 64 256; do
  ./build/release/tools/compare/compare_ois --exchange /tmp/x$T.json --results /tmp/r$T.json \
      --trades $T
  scripts/compare_swapengine.py --exchange /tmp/x$T.json --results /tmp/r$T.json \
      --cli <SwapEngine>/build/api/swaps_api_cli --risk-samples 100 --json /tmp/c$T.json
done

uptime                                         # record after
```

Record: the fingerprint; load and the top processes before and after each side; our medians from
`bench/results/d448afd70180/compare_ois_ladder.json` with the `solves` / `residual_evals` /
`jacobians` counters that say what each row actually did; their min / median / p90 `risk_us` per
book size; and the flags each side was built with — ours from `build/release/epykos_flags.txt`,
theirs from its own build's CMake cache, which are **not** the same toolchain and must be stated
as such rather than presented as one fingerprint (D9, D13).

**On curve build, which was the brief's second target: the ladder is the only quantity for which
both engines self-report a comparable in-process time.** Their stateless JSON response carries
`risk_us` and `price_us` but no calibration time (the session verbs have a `last_solve_us`; the
stateless CLI does not expose it), so a matched curve-build timing cannot be obtained through the
public interface. `BM_Calibrate` is therefore ours only, reported as context. The curve-build
*agreement* is already established and needs nothing further: the calibrated discount factors
match to 5.832e-13 and the model par rates to 3.963e-14 (§4).

Informational rows worth taking in the same window, clearly labelled as **their fixture, not
ours**, so the ratio below has some context: one run of their `build/bench/risk_bench`
(`BM_Risk_QuantLib_Bump` and `BM_Risk_Ours_Analytic` on their own 23-knot, 9-swap problem) and of
their `build/bench/curve_build_bench`.

Threads: this engine is single-threaded throughout — there is no `std::thread` and no
`hardware_concurrency` call anywhere under `src/` or `include/epykos/`. The other engine's thread
use on this path is not established and should be checked in the window before any ratio is
quoted.

Then the ratio — and §4 items 1, 3 and 5 go in front of it, not after it.
