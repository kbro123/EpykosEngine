// THE SECOND INDEPENDENT ORACLE, and the first on an INSTRUMENT rather than on a function.
// D114 compared the interpolation schemes to QuantLib 1.35; this compares the thing a desk
// actually quotes — the par rate of a USD SOFR OIS, compounded daily in arrears over a real
// schedule — on all sixteen calibration tenors of the `compare_ois` fixture.
//
// ---- why this one matters more than the scheme oracle ----------------------------------------
//
// `blueprints/conventions/instruments.json` records, against USD-SOFR-OIS:
//
//     "provenance": { "imported_from": "SwapEngine/conventions/conventions.json
//                      products.USD-SOFR-OIS", "cross_check": "agree" }
//
// **Our SOFR conventions were imported from the other engine's.** So a convention error on their
// side is in this tree too, and the head-to-head of D109 — curves agreeing to 6.717e-15, book NPV
// to 7.829e-15 — would report perfect agreement with both engines wrong. That is not a
// hypothetical objection to the h2h; it is written in our own conventions file. QuantLib's SOFR
// conventions come from somewhere else, so it can see what the h2h cannot.
//
// What the comparison covers, clause by clause, because this is the layer where a shared error
// would live: the T+2 spot on the joint SIFMA-and-Fed calendar; the annual roll on an unadjusted
// grid with each date ModifiedFollowing-adjusted; EOM; ACT/360 on both legs; SOFR compounded in
// arrears with plain observation (no lookback, no shift, no lockout) over every business day of
// the period — 249 of them in the one-year instrument; the two-business-day payment lag; and
// discounting on the same curve.
//
// ---- provenance of the numbers (D108; same rule as D114) -------------------------------------
//
// The expectations below are QuantLib 1.35's own `OvernightIndexedSwap::fairRate()`, printed once
// as exact hex floats. **No dependency is added and D12 is untouched.** Nothing is lifted from
// SwapEngine here at all — not even a case — because the fixture is ours; what the lift bought was
// the knowledge that an oracle of this shape is worth building (D108 §1).
//
// TO REGENERATE: build the engine's generating curve on a daily grid (`CompareOis::generating_df`),
// hold it in QuantLib as `InterpolatedDiscountCurve<LogLinear>` with `Actual365Fixed`, and for each
// tenor price
//
//     Schedule(spot, UNADJUSTED termination, Annual, JointCalendar(UnitedStates(GovernmentBond),
//              UnitedStates(FederalReserve), JoinHolidays), ModifiedFollowing, ModifiedFollowing,
//              DateGeneration::Forward, endOfMonth = true)
//     OvernightIndexedSwap(Receiver, N, sched, quote, Actual360(), Sofr(h), 0.0, paymentLag = 2,
//                          ModifiedFollowing, cal, telescopicValueDates = false, Compound)
//
// **Do NOT use `MakeOIS` or `OISRateHelper` for this.** Both advance the tenor to the ADJUSTED
// termination and then generate the schedule BACKWARD, so whenever the roll date falls on a
// weekend the first generated date lands past the effective date and the swap acquires a spurious
// three-day front stub. The first run of this comparison did exactly that and reported the engine
// as disagreeing by 0.126 bp at 1Y, 0.024 at 6Y, 0.022 at 7Y, 0.012 at 12Y and 0.005 at 40Y —
// precisely the five tenors of this set whose termination is a Saturday or Sunday, and the pattern
// is what exposed it. QuantLib's own 2Y first coupon, which spans the same period as the 1Y swap,
// agreed with us to 1.3e-15 throughout. **A disagreement localises to a harness before it
// localises to an engine**, and this file says so because the next person to extend it will hit
// the same trap.
//
// An E0 TU (-ffp-contract=off in every preset), which also puts this oracle in the mutation gate
// set by name.
#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include "epykos/fixtures/compare_ois.hpp"
#include "epykos/maths/instrument/tables.hpp"

#ifndef EPYKOS_FP_CONTRACT_OFF
#error "an _e0_test.cpp TU must see EPYKOS_FP_CONTRACT_OFF"
#endif

namespace fixtures = epykos::fixtures;

namespace {

// QuantLib 1.35 `OvernightIndexedSwap::fairRate()`, one per tenor of
// `fixtures::compare_ois_default_tenors()`, on the generating curve of the fixture options
// asserted below. Exact hex floats: the only error in the comparison is ours.
inline constexpr double kQlOisParRate[] = {
    0x1.3f589a4de0706p-5, 0x1.44411c0c3c6dcp-5,  // 1Y, 2Y
    0x1.4838135c54abfp-5, 0x1.4b6b600bb93b8p-5,  // 3Y, 4Y
    0x1.4e05b569142bbp-5, 0x1.502d4b507c5c4p-5,  // 5Y, 6Y
    0x1.51ece94ef27c8p-5, 0x1.536180fc865adp-5,  // 7Y, 8Y
    0x1.549b46a284471p-5, 0x1.55a55c89eda74p-5,  // 9Y, 10Y
    0x1.5749a6472e638p-5, 0x1.58fee96637951p-5,  // 12Y, 15Y
    0x1.5abcd3fbe1c9bp-5, 0x1.5bc3c1c6d561ap-5,  // 20Y, 25Y
    0x1.5c6d1c7339ea3p-5, 0x1.5d3313194876dp-5,  // 30Y, 40Y
};

// The agreement actually measured is 4.14e-11 bp worst over the sixteen. The gate is set three
// orders above that, which is still far inside anything a desk could see (1e-8 bp is 1e-12 of a
// rate), so a real convention change fails loudly while the last bits of a 249-day compounding
// are not a maintenance burden.
constexpr double kTolBp = 1.0e-8;


// The other three single-curve SOFR families of `blueprints/instruments/stage_a.json`, same
// generating curve, same sixteen tenors, same QuantLib build. Each isolates one RFR OBSERVATION
// convention, mapped onto QuantLib's `OvernightIndexedSwap` parameters:
//
//   USD-SOFR-OIS-LOOKBACK2   lookbackDays = 2, applyObservationShift = false
//   USD-SOFR-AVG-SWAP        RateAveraging::Simple
//   USD-SOFR-OIS-SHIFT2      lookbackDays = 2, applyObservationShift = true   <-- DISAGREES, §4
inline constexpr double kQlLookback2ParRate[] = {
    0x1.3f5571b6e9eadp-5, 0x1.4436eceef4dfp-5,   // 1Y, 2Y
    0x1.482cd80e4f6a3p-5, 0x1.4b5e933f7ea24p-5,  // 3Y, 4Y
    0x1.4df8b408ce136p-5, 0x1.502121e52a851p-5,  // 5Y, 6Y
    0x1.51e1781994ccep-5, 0x1.5356abda09f07p-5,  // 7Y, 8Y
    0x1.5490fc39b43a8p-5, 0x1.559b735e55a93p-5,  // 9Y, 10Y
    0x1.57407cdbb4a08p-5, 0x1.58f687cb2dd88p-5,  // 12Y, 15Y
    0x1.5ab5385f9bd75p-5, 0x1.5bbc9dc421ed1p-5,  // 20Y, 25Y
    0x1.5c66478c48426p-5, 0x1.5d2c9a748cc1ap-5,  // 30Y, 40Y
};
inline constexpr double kQlAveragedParRate[] = {
    0x1.3932a22b72d64p-5, 0x1.3df11cfea04cap-5,  // 1Y, 2Y
    0x1.41c12ea344dap-5,  0x1.44d4874ca387ep-5,  // 3Y, 4Y
    0x1.47549b254a53p-5,  0x1.49640cae84b82p-5,  // 5Y, 6Y
    0x1.4b12b90a47dbbp-5, 0x1.4c79179aa7764p-5,  // 7Y, 8Y
    0x1.4da651bf2c24cp-5, 0x1.4ea55270aea0cp-5,  // 9Y, 10Y
    0x1.50382063d5267p-5, 0x1.51dc393a1fa4dp-5,  // 12Y, 15Y
    0x1.538817cae5c6bp-5, 0x1.54845c226951fp-5,  // 20Y, 25Y
    0x1.5526c183c1008p-5, 0x1.55e49cc77fcc2p-5,  // 30Y, 40Y
};
inline constexpr double kQlObsShift2ParRate[] = {
    0x1.3d926525cdc7bp-5, 0x1.434a95ef48c6dp-5,  // 1Y, 2Y
    0x1.4789dded539a3p-5, 0x1.4b5589fd88a2fp-5,  // 3Y, 4Y
    0x1.4df2ec4624fd6p-5, 0x1.4fd2297d65e62p-5,  // 5Y, 6Y
    0x1.519c8167f9666p-5, 0x1.53194b24bae17p-5,  // 7Y, 8Y
    0x1.54598b04b6b42p-5, 0x1.55925eae92dap-5,   // 9Y, 10Y
    0x1.571810d77cf7bp-5, 0x1.58ed78e897e4dp-5,  // 12Y, 15Y
    0x1.5a9c8dbd2a13p-5,  0x1.5ba8a1ad3b952p-5,  // 20Y, 25Y
    0x1.5c55af71fc0bdp-5, 0x1.5d1f67a80a9e3p-5,  // 30Y, 40Y
};

// The quotes of one blueprint on the fixture's generating curve.
std::vector<double> quotes_of(const std::string& blueprint) {
  fixtures::CompareOisOptions o;
  o.trades = 1;
  o.quote_noise_bp = 0.0;
  o.blueprint = blueprint;
  return fixtures::make_compare_ois(o).quotes;
}

// Worst |ours - QuantLib| in basis points over the sixteen tenors.
double worst_gap_bp(const std::vector<double>& ours, const double* ql, std::size_t n) {
  double w = 0.0;
  for (std::size_t i = 0; i < n; ++i) w = std::max(w, std::abs(ours[i] - ql[i]) * 1.0e4);
  return w;
}

}  // namespace

// The fixture options the literals belong to. Asserted rather than assumed: the quotes are a
// function of the generating curve and the tenor set, so a change to either must fail HERE with a
// message saying to regenerate, not silently somewhere downstream.
TEST(OisParOracleE0, TheFixtureTheLiteralsWereGeneratedFromHasNotMoved) {
  const fixtures::CompareOisOptions o;
  EXPECT_EQ(o.valuation, "2026-09-23");
  EXPECT_EQ(o.generating_short, 0.0380);
  EXPECT_EQ(o.generating_long, 0.0430);
  EXPECT_EQ(o.generating_reversion_years, 3.0);
  EXPECT_EQ(o.max_start_offset_days, 0);
  EXPECT_EQ(o.max_start_offset_years, 0);
  ASSERT_EQ(fixtures::compare_ois_default_tenors().size(), std::size(kQlOisParRate))
      << "the tenor set changed; regenerate the QuantLib table (see this file's header)";
  const std::vector<std::string> want = {"1Y", "2Y", "3Y",  "4Y",  "5Y",  "6Y",  "7Y",  "8Y",
                                         "9Y", "10Y", "12Y", "15Y", "20Y", "25Y", "30Y", "40Y"};
  EXPECT_EQ(fixtures::compare_ois_default_tenors(), want);
}

// The oracle. `quote_noise_bp = 0` makes the fixture's quotes the generating curve's EXACT par
// rates as this engine computes them, so comparing them with QuantLib's fair rates on the same
// curve compares two independent implementations of the same instrument and nothing else — no
// calibration, no interpolation scheme, no solver.
TEST(OisParOracleE0, EveryOisParRateMatchesQuantLib) {
  fixtures::CompareOisOptions o;
  o.trades = 1;             // the book plays no part; the instruments do
  o.quote_noise_bp = 0.0;   // the generating curve's exact par rates
  const fixtures::CompareOis s = fixtures::make_compare_ois(o);
  ASSERT_EQ(static_cast<std::size_t>(s.n_quotes()), std::size(kQlOisParRate));

  double worst_bp = 0.0;
  std::string worst_tenor;
  for (int i = 0; i < s.n_quotes(); ++i) {
    const std::size_t k = static_cast<std::size_t>(i);
    const double bp = (s.quotes[k] - kQlOisParRate[k]) * 1.0e4;
    if (std::abs(bp) > std::abs(worst_bp)) {
      worst_bp = bp;
      worst_tenor = s.set.instruments[k].tenor;
    }
    EXPECT_NEAR(s.quotes[k], kQlOisParRate[k], kTolBp * 1.0e-4)
        << s.set.instruments[k].tenor << ": " << bp << " bp from QuantLib. Before concluding the engine is wrong, "
        << "read this file's header on MakeOIS and the weekend-termination stub.";
  }
  std::cout << "  [ql-oracle] 16 USD SOFR OIS par rates vs QuantLib 1.35: worst " << worst_bp << " bp at "
            << worst_tenor << '\n';
}

// A negative control, because an oracle that cannot fail proves nothing: a one-day shift of the
// payment lag must move the rate by far more than the tolerance. This is the cheapest available
// proof that the comparison is sensitive to a convention at all — without it, a test that passed
// because both sides were reading the same wrong field would look identical to this one.
TEST(OisParOracleE0, TheComparisonIsSensitiveToAConventionChange) {
  fixtures::CompareOisOptions o;
  o.trades = 1;
  o.quote_noise_bp = 0.0;
  const fixtures::CompareOis base = fixtures::make_compare_ois(o);
  // The generating curve is smooth and the par rate of a one-year OIS moves ~1 bp per 25 bp of
  // curve, so a perturbation of the generating curve by one basis point must be visible here at
  // orders of magnitude above kTolBp, or the quotes are not a function of the curve at all.
  o.generating_short = 0.0380 + 1.0e-4;
  const fixtures::CompareOis bumped = fixtures::make_compare_ois(o);
  double worst = 0.0;
  for (int i = 0; i < base.n_quotes(); ++i) {
    const std::size_t k = static_cast<std::size_t>(i);
    worst = std::max(worst, std::abs(bumped.quotes[k] - base.quotes[k]) * 1.0e4);
  }
  std::cout << "  [ql-oracle] a 1 bp bump of the generating short rate moves the par rates by up to " << worst
            << " bp\n";
  EXPECT_GT(worst, 1.0e3 * kTolBp) << "the quotes barely move when the generating curve does, so the oracle above "
                                      "is not actually constraining the instrument";
}

// ---- the other two conventions that agree ----------------------------------------------------

// A two-business-day LOOKBACK: the rate applied on each business day is the one published two
// business days earlier, and the WEIGHTS stay those of the accrual period. Our observation table
// shows exactly that — the first entry's rate date moves from t = 0.00548 to t = 0, while its
// weight stays 3/360 — and the agreement is the tightest in this file.
TEST(OisParOracleE0, LookbackTwoDaysMatchesQuantLib) {
  const std::vector<double> ours = quotes_of("USD-SOFR-OIS-LOOKBACK2");
  ASSERT_EQ(ours.size(), std::size(kQlLookback2ParRate));
  const double worst = worst_gap_bp(ours, kQlLookback2ParRate, ours.size());
  std::cout << "  [ql-oracle] 16 lookback-2 OIS par rates vs QuantLib: worst " << worst << " bp\n";
  EXPECT_LT(worst, kTolBp);
  // And the convention is not a no-op: it must differ from plain by far more than the tolerance,
  // or this test would pass on a lookback that silently did nothing.
  const std::vector<double> plain = quotes_of("USD-SOFR-OIS");
  EXPECT_GT(std::abs(ours[0] - plain[0]) * 1.0e4, 1.0e3 * kTolBp) << "the lookback changes nothing";
}

// The ARITHMETIC AVERAGE of daily SOFR rather than the compound — the family that does NOT
// telescope (D81's collapse does not reach it), and the one whose par rate is furthest from
// plain, so it is the strongest check that the observation table itself is right.
TEST(OisParOracleE0, ArithmeticAveragingMatchesQuantLib) {
  const std::vector<double> ours = quotes_of("USD-SOFR-AVG-SWAP");
  ASSERT_EQ(ours.size(), std::size(kQlAveragedParRate));
  const double worst = worst_gap_bp(ours, kQlAveragedParRate, ours.size());
  std::cout << "  [ql-oracle] 16 averaged OIS par rates vs QuantLib: worst " << worst << " bp\n";
  EXPECT_LT(worst, kTolBp);
  const std::vector<double> plain = quotes_of("USD-SOFR-OIS");
  EXPECT_GT(std::abs(ours[0] - plain[0]) * 1.0e4, 1.0) << "averaging must differ from compounding by ~1 bp or more";
}

// ---- §4: the one convention that DISAGREES, and the exact factor ------------------------------

// OBSERVATION SHIFT disagrees with QuantLib by 2.12 bp at 1Y, decaying as 1/T, and the cause is
// localised exactly: **ours is QuantLib's times d_accrual / d_obs**, verified below to 1.3e-13 on
// the single-period instrument where that factor is unambiguous (367 accrual days against 365
// observation days).
//
// WHICH IS RIGHT IS AN OPEN QUESTION AND IS THE OWNER'S, because both readings are defensible and
// the engines differ on a definition rather than on arithmetic:
//
//   ours      Floating Rate = (prod_obs - 1) * 360 / d_obs, then Floating Amount =
//             N * Rate * d_accrual / 360. That is ISDA's formal structure — a Floating Rate
//             multiplied by the CALCULATION period's day count fraction — and under observation
//             shift the two periods have different lengths, so the factor survives.
//   QuantLib  Floating Amount = N * (prod_obs - 1) directly: the compounded amount IS the
//             interest, and the accrual period's length does not enter.
//
// Everything else about our shift is right and is checked above: the observation window is
// [start - 2bd, end - 2bd], its weights sum to its OWN length (365, not the accrual's 367 — the
// mismatch is present in the structure, which is what distinguishes shift from lookback), and the
// two engines agree exactly on lookback and on averaging. So this is one division, not a broken
// convention.
//
// This test pins the MECHANISM rather than the verdict. If someone changes the division, this
// fails and points at the decision that has to be recorded; it does not assert that today's
// behaviour is correct.
TEST(OisParOracleE0, ObservationShiftDiffersFromQuantLibByExactlyTheAccrualOverObservationRatio) {
  const std::vector<double> ours = quotes_of("USD-SOFR-OIS-SHIFT2");
  ASSERT_EQ(ours.size(), std::size(kQlObsShift2ParRate));
  const double worst = worst_gap_bp(ours, kQlObsShift2ParRate, ours.size());
  std::cout << "  [ql-oracle] 16 observation-shift OIS par rates vs QuantLib: worst " << worst << " bp\n";

  // Half one: the disagreement is real and far outside the tolerance the other four families meet.
  EXPECT_GT(worst, 1.0) << "observation shift now agrees with QuantLib to " << worst
                        << " bp. If that is intended, the division changed -- record the decision and replace this "
                           "expectation. Do not delete it.";

  // Half two: and it is EXACTLY one factor. The 1Y instrument is a single period, so d_accrual and
  // d_obs are read straight off the fixture rather than assumed.
  fixtures::CompareOisOptions o;
  o.trades = 1;
  o.quote_noise_bp = 0.0;
  o.blueprint = "USD-SOFR-OIS-SHIFT2";
  const fixtures::CompareOis s = fixtures::make_compare_ois(o);
  const epykos::instrument::Instrument& in = s.set.instruments[0].instrument;
  int accrual_days = 0, obs_weight_days = 0;
  for (const epykos::instrument::Leg& leg : in.legs) {
    if (leg.index.empty()) continue;
    ASSERT_EQ(leg.coupons.size(), 1u) << "the 1Y instrument must be one period for this factor to be unambiguous";
    accrual_days = static_cast<int>(leg.coupons[0].end - leg.coupons[0].start);
    for (const epykos::instrument::ObsDay& od : leg.obs) obs_weight_days += od.weight_days;
  }
  ASSERT_GT(accrual_days, 0);
  ASSERT_GT(obs_weight_days, 0);
  EXPECT_NE(accrual_days, obs_weight_days)
      << "the observation window and the accrual period are the same length, so there is no shift to disagree about";
  const double factor = static_cast<double>(accrual_days) / static_cast<double>(obs_weight_days);
  const double measured = ours[0] / kQlObsShift2ParRate[0];
  std::cout << "  [ql-oracle] observation shift, 1Y: accrual " << accrual_days << " days, observation window "
            << obs_weight_days << " days; ours/QuantLib = " << measured << " against d_accrual/d_obs = " << factor
            << '\n';
  EXPECT_NEAR(measured, factor, 1.0e-11)
      << "the disagreement is no longer the single factor d_accrual/d_obs, so it is not the division any more and "
         "the cause must be re-localised before anything is concluded";
}
