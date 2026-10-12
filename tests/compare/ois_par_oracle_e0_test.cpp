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
#include <vector>

#include "epykos/fixtures/compare_ois.hpp"

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
