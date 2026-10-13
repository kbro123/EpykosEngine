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

#include "epykos/conventions/rfr.hpp"
#include "epykos/fixtures/compare_ois.hpp"
#include "epykos/maths/instrument/coupon.hpp"
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


// ---- the fixing CALENDAR, which the par rate does not constrain --------------------------------
//
// The par rate on a smooth curve is nearly independent of WHICH days are business days, provided
// the weights sum to the period length: EUR-ESTR projects **257** observation days against USD
// SOFR's 249 on this same generating curve and the two par rates agree to 1e-15. So the agreement
// above constrains the business-day COUNT and not the day PLACEMENT, and a calendar wrong by a few
// days in compensating directions would pass every test before this one. These are QuantLib's own
// `OvernightIndexedCoupon::fixingDates()` for the one-year instrument, as day offsets from the
// valuation date, and they constrain the placement.
// QuantLib 249 fixing dates, as DAY OFFSETS from the valuation date
inline constexpr int kQlSofr1yFixingOffset[] = {
    2, 5, 6, 7, 8, 9, 12, 13, 14, 15, 16, 20, 21, 22, 23, 26,
    27, 28, 29, 30, 33, 34, 35, 36, 37, 40, 41, 42, 43, 44, 47, 48,
    50, 51, 54, 55, 56, 57, 58, 61, 62, 63, 65, 68, 69, 70, 71, 72,
    75, 76, 77, 78, 79, 82, 83, 84, 85, 86, 89, 90, 91, 92, 96, 97,
    98, 99, 103, 104, 105, 106, 107, 110, 111, 112, 113, 114, 118, 119, 120, 121,
    124, 125, 126, 127, 128, 131, 132, 133, 134, 135, 138, 139, 140, 141, 142, 146,
    147, 148, 149, 152, 153, 154, 155, 156, 159, 160, 161, 162, 163, 166, 167, 168,
    169, 170, 173, 174, 175, 176, 177, 180, 181, 182, 183, 187, 188, 189, 190, 191,
    194, 195, 196, 197, 198, 201, 202, 203, 204, 205, 208, 209, 210, 211, 212, 215,
    216, 217, 218, 219, 222, 223, 224, 225, 226, 229, 230, 231, 232, 233, 236, 237,
    238, 239, 240, 243, 244, 245, 246, 247, 251, 252, 253, 254, 257, 258, 259, 260,
    261, 264, 265, 266, 267, 271, 272, 273, 274, 275, 278, 279, 280, 281, 282, 286,
    287, 288, 289, 292, 293, 294, 295, 296, 299, 300, 301, 302, 303, 306, 307, 308,
    309, 310, 313, 314, 315, 316, 317, 320, 321, 322, 323, 324, 327, 328, 329, 330,
    331, 334, 335, 336, 337, 338, 341, 342, 343, 344, 345, 349, 350, 351, 352, 355,
    356, 357, 358, 359, 362, 363, 364, 365, 366,
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

// ---- §4: the convention that disagreed, and the ARRC's answer ---------------------------------

// OBSERVATION SHIFT disagreed with QuantLib by 2.12 bp at 1Y until 2026-10-13, decaying as 1/T,
// and the cause was localised to exactly one factor: ours was QuantLib's times d_accrual / d_obs
// (367/365 on the single-period 1Y instrument, to 1.3e-13). D116 recorded it as an open question
// between two defensible readings and declined to pick one.
//
// **The ARRC settles it, and this engine was on the wrong side.** "An Updated User's Guide to
// SOFR", Appendix 4 — titled "Methods for Calculating Daily Accrual in an Interest-Period
// Weighted Observation Shift" — asks over how many days the annualised shifted rate is applied and
// names two answers: (1) "the number of days in the OBSERVATION period", and (2) "the number of
// days in the INTEREST period … (this variant was labeled an interest-period weighted shift)".
// The main text then fixes which one the NAME means: "with an observation shift, interest is
// accrued according to the number of days in the observation period, which may differ from the
// number of days in the interest period."
//
// This engine computed (2) under the name `observation_shift`. Option (2) is a real convention —
// "widely used in FRNs" — but the ARRC records it as "not the principle recommendation", "discussed
// but not recommended" in its business-loan conventions, and notes it "can at times result in a
// negative daily accrual even if SOFR rates are positive". `maths/instrument/coupon.hpp`'s
// `rate_accrual` now returns the observation tau for the RFR kinds, which is option (1) and is
// bit-identical wherever there is no shift.
//
// The literals below were generated BEFORE the fix, so this test passing is the fix landing on
// numbers it could not have been fitted to.
TEST(OisParOracleE0, ObservationShiftMatchesQuantLib) {
  const std::vector<double> ours = quotes_of("USD-SOFR-OIS-SHIFT2");
  ASSERT_EQ(ours.size(), std::size(kQlObsShift2ParRate));
  const double worst = worst_gap_bp(ours, kQlObsShift2ParRate, ours.size());
  std::cout << "  [ql-oracle] 16 observation-shift OIS par rates vs QuantLib: worst " << worst << " bp\n";
  EXPECT_LT(worst, kTolBp) << "observation shift is " << worst
                           << " bp from QuantLib. If that is 2.12 bp at 1Y, the engine has gone back to the "
                              "interest-period weighted variant -- see this test's header on ARRC Appendix 4.";

  // And the convention still bites: it must differ from plain by far more than the tolerance, or
  // this test would pass on a shift that silently did nothing.
  const std::vector<double> plain = quotes_of("USD-SOFR-OIS");
  EXPECT_GT(std::abs(ours[0] - plain[0]) * 1.0e4, 1.0e3 * kTolBp) << "the observation shift changes nothing";
}

// The fix is a NO-OP wherever there is no shift, and that is what makes it checkable rather than
// merely plausible: the observation weights of a plain or lookback coupon sum to the calculation
// period's own length, so `obs_tau == accrual` and the payment is unchanged bit for bit. Asserted
// on the fixture's own tables rather than argued from the formula.
TEST(OisParOracleE0, TheObservationTauEqualsTheAccrualWithoutAShift) {
  struct Case { const char* blueprint; bool shifted; };
  const Case cases[] = {{"USD-SOFR-OIS", false},
                        {"USD-SOFR-OIS-LOOKBACK2", false},
                        {"USD-SOFR-AVG-SWAP", false},
                        {"USD-SOFR-OIS-SHIFT2", true}};
  for (const Case& c : cases) {
    fixtures::CompareOisOptions o;
    o.trades = 1;
    o.quote_noise_bp = 0.0;
    o.blueprint = c.blueprint;
    const fixtures::CompareOis s = fixtures::make_compare_ois(o);
    std::size_t floats = 0, differing = 0;
    for (const epykos::instrument::Leg& leg : s.set.instruments[0].instrument.legs) {
      if (leg.index.empty()) continue;
      for (const epykos::instrument::Coupon& cp : leg.coupons) {
        ++floats;
        if (std::abs(cp.obs_tau - cp.accrual) > 1.0e-15) ++differing;
      }
    }
    ASSERT_GT(floats, 0u) << c.blueprint;
    if (c.shifted) {
      EXPECT_EQ(differing, floats) << c.blueprint << ": a shifted coupon's observation tau equals its accrual, so "
                                      "the shift is not shifting the window";
    } else {
      EXPECT_EQ(differing, 0u) << c.blueprint << ": an UNSHIFTED coupon's observation tau differs from its accrual, "
                                  "so the coupon.hpp change is no longer a no-op here and every unshifted number in "
                                  "this repository has moved";
    }
    std::cout << "  [ql-oracle] " << c.blueprint << ": " << differing << " of " << floats
              << " float coupons have obs_tau != accrual\n";
  }
}

// The calendar, gated rather than inferred. `USD-SOFR-SWAP` is `join(USD-SIFMA, USD-FED)` and the
// SOFR fixing calendar is `USD-SOFR` (U.S. Government Securities Business Days, less Good Friday);
// QuantLib's `Sofr` index fixes on `UnitedStates(GovernmentBond)`. Those are independently built
// holiday sets, and over the 249 business days of a 367-day period they agree on every single day.
//
// This test exists because the agreement on the par rate does NOT establish it — see the note on
// the table above. D115 first claimed the calendars agreed "day for day" on the strength of the
// rate and a matching count, which was true and insufficiently supported; this is the support.
TEST(OisParOracleE0, EveryObservationDateMatchesQuantLibsFixingDates) {
  fixtures::CompareOisOptions o;
  o.trades = 1;
  o.quote_noise_bp = 0.0;
  const fixtures::CompareOis s = fixtures::make_compare_ois(o);
  const epykos::instrument::Instrument& in = s.set.instruments[0].instrument;
  std::vector<int> ours;
  for (const epykos::instrument::Leg& leg : in.legs) {
    if (leg.index.empty()) continue;
    for (const epykos::instrument::ObsDay& od : leg.obs) {
      ours.push_back(static_cast<int>(od.rate_date - s.valuation));
    }
    break;
  }
  ASSERT_EQ(ours.size(), std::size(kQlSofr1yFixingOffset))
      << "the engine projects " << ours.size() << " observation days where QuantLib fixes on "
      << std::size(kQlSofr1yFixingOffset) << ": the fixing calendars no longer agree on the business-day COUNT";
  std::size_t mismatches = 0;
  for (std::size_t i = 0; i < ours.size(); ++i) {
    if (ours[i] == kQlSofr1yFixingOffset[i]) continue;
    if (mismatches < 8) {
      ADD_FAILURE() << "observation " << i << ": the engine reads the rate for valuation + " << ours[i]
                    << " days, QuantLib for valuation + " << kQlSofr1yFixingOffset[i]
                    << ". The par rate would barely move, so this is the only test that sees it.";
    }
    ++mismatches;
  }
  EXPECT_EQ(mismatches, 0u);
  std::cout << "  [ql-oracle] " << ours.size() << " SOFR observation dates vs QuantLib's fixingDates(): "
            << mismatches << " mismatches\n";
}

// And the proof that the test above is needed: a different fixing calendar, same generating curve,
// gives a DIFFERENT number of observation days and the same par rate to 1e-15. Without this, the
// claim "the par rate does not constrain the calendar" would itself be unsupported.
TEST(OisParOracleE0, TheParRateDoesNotConstrainTheFixingCalendar) {
  fixtures::CompareOisOptions o;
  o.trades = 1;
  o.quote_noise_bp = 0.0;
  const fixtures::CompareOis usd = fixtures::make_compare_ois(o);
  o.blueprint = "EUR-ESTR-OIS";   // EUR-TARGET, not join(SIFMA, FED)
  const fixtures::CompareOis eur = fixtures::make_compare_ois(o);

  const auto obs_count = [](const fixtures::CompareOis& c) {
    for (const epykos::instrument::Leg& leg : c.set.instruments[0].instrument.legs) {
      if (!leg.index.empty()) return leg.obs.size();
    }
    return std::size_t{0};
  };
  const std::size_t nu = obs_count(usd), ne = obs_count(eur);
  const double gap_bp = std::abs(usd.quotes[0] - eur.quotes[0]) * 1.0e4;
  std::cout << "  [ql-oracle] USD SOFR " << nu << " observation days, EUR ESTR " << ne
            << "; 1Y par rates differ by " << gap_bp << " bp\n";
  EXPECT_NE(nu, ne) << "the two calendars now give the same business-day count, so this no longer demonstrates "
                       "anything and the note on the fixing-date table must be re-derived";
  EXPECT_LT(gap_bp, 1.0e-6) << "the par rates differ by " << gap_bp
                            << " bp across two calendars, so the rate DOES constrain the calendar after all -- then "
                               "the note above is wrong and should be corrected, not deleted";
}

// ---- D130: the shift accrual is a TERM, and this is the arithmetic it selects ------------------
//
// ISDA's own paper settles what D116 could not and what D123 decided from the ARRC alone.
// *Documenting RFR derivatives using different approaches to compounding/averaging under the 2006
// ISDA Definitions* (ISDA, 27 April 2021), section 1.2 "Observation Period Shift":
//
//   "while the weightings of the fixings used to calculate the compounded rate will be determined
//    on the basis of the Business Days in the observation period, which may differ from the
//    Business Days in the Calculation Period, THE RATE WILL BE ANNUALIZED AND APPLY BASED ON THE
//    NUMBER OF DAYS IN THE CALCULATION PERIOD, using the day count basis applicable to the
//    relevant RFR."
//
// Annualised by d_c and applied over d_c: the two cancel, so the Floating Amount is N·(Π − 1) —
// the SAME CASH FLOW as the ARRC's option (1), reached by a different route. The two authorities
// differ only on the quoted rate's basis (ARRC d_o, ISDA d_c), which no PV and no par rate can
// see. So D123's fix is right under BOTH, and the engine's default needs no argument from
// preference. (It also refutes D116's unsourced line that the interest-period variant "is ISDA's
// formal structure": that inference read the Floating Amount's day count fraction and ignored
// which day count the RATE is annualised by.)
//
// Option (2) remains real — the ARRC calls it "widely used in FRNs" and SwapEngine computes it —
// so `ObservationSpec::accrual` now carries it as a term a convention can state. These two gates
// pin the arithmetic the term selects and the fact that it cannot bite without a shift.
TEST(OisParOracleE0, TheShiftAccrualTermSelectsExactlyTheDcOverDoFactor) {
  fixtures::CompareOisOptions o;
  o.trades = 1;
  o.quote_noise_bp = 0.0;
  o.blueprint = "USD-SOFR-OIS-SHIFT2";
  const fixtures::CompareOis s = fixtures::make_compare_ois(o);
  const auto df = [&s](int slot, double tt) { return s.generating_df(slot, tt); };

  // Not every shifted coupon can tell the two apart: the shifted endpoints can land a window of
  // the SAME calendar length as the interest period, and then d_c/d_o is 1 and the conventions
  // coincide on that coupon. So the gate checks the ones that CAN distinguish and fails if the
  // fixture stops producing any — never by asserting a coupon must differ.
  std::size_t checked = 0, vacuous = 0;
  double widest = 1.0;
  for (const epykos::instrument::CalibrationInstrument& ci : s.set.instruments) {
    for (const epykos::instrument::Leg& leg : ci.instrument.legs) {
      if (leg.index.empty()) continue;
      for (const epykos::instrument::Coupon& cp : leg.coupons) {
        ASSERT_GT(cp.obs_tau, 0.0);
        epykos::instrument::Coupon ipw = cp;
        ipw.rate_over_interest_period = true;
        // The term is the ONLY difference between the two coupons, so the payments differ by
        // exactly d_c/d_o -- which is also D116's localisation, now pinned as an identity.
        EXPECT_EQ(epykos::instrument::rate_accrual(cp), cp.obs_tau);
        EXPECT_EQ(epykos::instrument::rate_accrual(ipw), cp.accrual);
        const double pv_obs = epykos::instrument::float_coupon_pv<double>(leg, cp, df);
        const double pv_ipw = epykos::instrument::float_coupon_pv<double>(leg, ipw, df);
        ASSERT_NE(pv_obs, 0.0);
        const double expected = cp.accrual / cp.obs_tau;
        if (expected == 1.0) {
          EXPECT_EQ(pv_obs, pv_ipw) << "d_c == d_o on this coupon, so the two terms must be bitwise equal";
          ++vacuous;
          continue;
        }
        EXPECT_NEAR(pv_ipw / pv_obs, expected, 1.0e-15 * std::abs(expected))
            << "the interest-period term must scale the payment by d_c/d_o and nothing else";
        if (std::abs(expected - 1.0) > std::abs(widest - 1.0)) widest = expected;
        ++checked;
      }
    }
  }
  ASSERT_GT(checked, 0u) << "no coupon in the shifted fixture has d_c != d_o, so this gate cannot see the "
                            "difference between the two conventions at all";
  EXPECT_GT(std::abs(widest - 1.0), 1.0e-4) << "the widest d_c/d_o in the fixture is " << widest
                                            << ", too close to 1 for this gate to mean anything";
  std::cout << "  [ql-oracle] " << checked << " shifted coupons scale by exactly d_c/d_o (widest " << widest
            << "), " << vacuous << " with d_c == d_o are bitwise equal\n";
}

// And it cannot bite where there is no shift: `obs_tau == accrual` there, so BOTH members of the
// term give bitwise the same payment. That is what makes the field safe to add to any convention.
TEST(OisParOracleE0, TheShiftAccrualTermIsBitwiseVacuousWithoutAShift) {
  for (const char* bp : {"USD-SOFR-OIS", "USD-SOFR-OIS-LOOKBACK2", "USD-SOFR-AVG-SWAP"}) {
    fixtures::CompareOisOptions o;
    o.trades = 1;
    o.quote_noise_bp = 0.0;
    o.blueprint = bp;
    const fixtures::CompareOis s = fixtures::make_compare_ois(o);
    const auto df = [&s](int slot, double tt) { return s.generating_df(slot, tt); };
    std::size_t checked = 0;
    for (const epykos::instrument::Leg& leg : s.set.instruments[0].instrument.legs) {
      if (leg.index.empty()) continue;
      for (const epykos::instrument::Coupon& cp : leg.coupons) {
        epykos::instrument::Coupon ipw = cp;
        ipw.rate_over_interest_period = true;
        EXPECT_EQ(epykos::instrument::float_coupon_pv<double>(leg, cp, df),
                  epykos::instrument::float_coupon_pv<double>(leg, ipw, df))
            << bp << ": the shift-accrual term changed an UNSHIFTED payment, so it is not vacuous here and every "
                     "unshifted number in this repository depends on which member a convention happens to carry";
        ++checked;
      }
    }
    ASSERT_GT(checked, 0u) << bp;
    std::cout << "  [ql-oracle] " << bp << ": " << checked << " unshifted coupons, both terms bitwise equal\n";
  }
}
