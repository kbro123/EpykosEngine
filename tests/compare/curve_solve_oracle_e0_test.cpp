// THE SOLVE, checked independently — the last link in the chain, and it was recorded as BLOCKED on
// a belief that turned out to be false.
//
// D115 §5 and D120 §2 both state that a direct bootstrap comparison is not available because
// "QuantLib's natural OIS pillar is the swap's maturity while ours is its last cash flow (D64), so
// two piecewise-log-linear curves on node sets two business days apart are different curves even
// when both reprice every quote". **That is wrong.** QuantLib's DEFAULT is
// `Pillar::LastRelevantDate`, and `oisratehelper.cpp` computes it as
//
//     lastPaymentDate     = max(overnightLeg().back()->date(), fixedLeg().back()->date())
//     latestRelevantDate_ = max(maturityDate_, lastPaymentDate)
//
// — it INCLUDES the payment lag. `Pillar::MaturityDate` is the opt-in, not the default. So
// QuantLib's natural pillar coincides with our knot and nothing was ever misaligned; the obstacle
// came from reading the enumerator's NAME in the header instead of its definition in the source.
// This test asserts the coincidence rather than assuming it: **0 of 16 pillar dates may differ.**
//
//   worst relative knot DF difference   8.09e-13
//   worst zero-rate difference          4.16e-10 bp
//
// An earlier run of this comparison reported 0.0009–0.12 bp and attributed part of it to node
// placement. The whole of it was the `MakeOIS` stub bug of D115 §3; there was no node-placement
// term at all.
//
// WHAT IT ESTABLISHES, and it is the piece every other oracle here was missing. D120 checked the
// adjoint and the IFT's composition with the calibration map held fixed and ours in both arms.
// This checks the calibration map itself: QuantLib takes the same 16 par quotes, bootstraps its own
// curve with its own solver, and lands on our knot discount factors. The chain
// quote -> solve -> knots -> price -> NPV -> ladder now has an independent number at every link.
//
// The residual is at QuantLib's own bootstrap accuracy (its default is 1e-12 on the
// `IterativeBootstrap` accuracy), so 8e-13 relative is the floor of the comparison rather than a
// measure of our solver's error — the same reading as D120 §3's finite-difference floor.
//
// ---- the harness, for whoever extends this -----------------------------------------------------
//
// `DatedOISRateHelper`, not `OISRateHelper`, and the reason is both traps at once: it takes the
// start and end dates EXPLICITLY so the tenor is never advanced to the adjusted termination
// (D115 §3's spurious weekend stub), and it generates the schedule with MakeOIS's default Backward
// rule, which is what our `stub: ShortFront` means (D119 §2). Curve:
// `PiecewiseYieldCurve<Discount, LogLinear>` with `Actual365Fixed` and the reference date at the
// VALUATION date — not at spot, or every DF would be rescaled by a constant, since a par condition
// is scale-invariant. That family is piecewise-constant instantaneous forward, which is exactly
// `compare_ois`'s linear-in-logdf region, so the interpolation scheme is not part of the comparison.
//
// An E0 TU (-ffp-contract=off in every preset), and in the mutation gate set by name.
#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <iostream>
#include <iterator>
#include <vector>

#include "epykos/fixtures/compare_ois.hpp"
#include "epykos/solver/implicit_program.hpp"

#ifndef EPYKOS_FP_CONTRACT_OFF
#error "an _e0_test.cpp TU must see EPYKOS_FP_CONTRACT_OFF"
#endif

namespace fixtures = epykos::fixtures;
namespace solver = epykos::solver;

namespace {

// QuantLib 1.35's own bootstrap of the same 16 par quotes: the discount factor at each knot date.
inline constexpr double kQlBootstrapDf[] = {
    0x1.ec38ab2d7249bp-1, 0x1.d8fcdacb6344ep-1,  // 1Y, 2Y
    0x1.c610e47d1bff2p-1, 0x1.b3a1fddb85e48p-1,  // 3Y, 4Y
    0x1.a1aacc155a999p-1, 0x1.905c7181718f3p-1,  // 5Y, 6Y
    0x1.7fc63978b0934p-1, 0x1.6fd0bef4e955cp-1,  // 7Y, 8Y
    0x1.60702865e00dbp-1, 0x1.518e86a8a332p-1,   // 9Y, 10Y
    0x1.35d1f344def1ep-1, 0x1.10671d6940c71p-1,  // 12Y, 15Y
    0x1.b7646cb137f4bp-2, 0x1.625a0e4da19b6p-2,  // 20Y, 25Y
    0x1.1dbb9edde6341p-2, 0x1.73919c990c913p-3,  // 30Y, 40Y
};

// Three orders above the measured 8.09e-13, which is QuantLib's own bootstrap accuracy floor.
constexpr double kDfRelTol = 1.0e-9;

}  // namespace

TEST(CurveSolveOracleE0, TheCalibratedKnotsMatchQuantLibsOwnBootstrap) {
  fixtures::CompareOisOptions o;
  o.trades = 1;            // the book plays no part; the calibration does
  o.quote_noise_bp = 0.0;  // the generating curve's exact par rates
  const fixtures::CompareOis s = fixtures::make_compare_ois(o);
  ASSERT_EQ(static_cast<std::size_t>(s.n_quotes()), std::size(kQlBootstrapDf))
      << "the calibration set changed; regenerate the QuantLib bootstrap (see this file's header)";

  fixtures::CompareOisTape t = fixtures::record_compare_ois(s);
  ASSERT_TRUE(t.record_report.converged) << "the engine's own solve did not converge, so there is nothing to compare";

  double worst_rel = 0.0, worst_zero_bp = 0.0;
  std::size_t worst_k = 0;
  for (std::size_t k = 0; k < std::size(kQlBootstrapDf); ++k) {
    // The region's variable is logdf, so the knot output IS log(DF).
    const double ours = std::exp(t.record_knots[k]);
    const double rel = std::abs(ours - kQlBootstrapDf[k]) / kQlBootstrapDf[k];
    const double tk = s.set.knot_t[k];
    const double zero_bp = std::abs(-std::log(ours) / tk + std::log(kQlBootstrapDf[k]) / tk) * 1.0e4;
    if (rel > worst_rel) {
      worst_rel = rel;
      worst_k = k;
    }
    worst_zero_bp = std::max(worst_zero_bp, zero_bp);
    EXPECT_LT(rel, kDfRelTol) << "knot " << k << " (" << s.set.instruments[k].tenor << "): engine DF " << ours
                              << ", QuantLib bootstrap " << kQlBootstrapDf[k] << ", relative " << rel << " ("
                              << zero_bp << " bp of zero rate)";
  }
  std::cout << "  [ql-oracle] 16 calibrated knot DFs vs QuantLib's own bootstrap: worst " << worst_rel
            << " relative at " << s.set.instruments[worst_k].tenor << ", " << worst_zero_bp << " bp of zero rate\n";
}

// The premise the test above rests on, and the one that was got wrong: QuantLib's DEFAULT OIS
// pillar is the LAST PAYMENT date, which is where our knot is. Asserted from the engine's own
// dates, so that if either side ever moves its node the comparison fails here with the reason
// rather than silently measuring node placement.
//
// Our knot sits at the instrument's last cash flow (D64), which the payment lag puts two business
// days after the termination. If a future QuantLib changed its default to `Pillar::MaturityDate`,
// or if our knot moved to the termination, the two node sets would separate and every DF above
// would shift by the forward's curvature over those two days — small, systematic, and invisible to
// any other test in the suite.
TEST(CurveSolveOracleE0, OurKnotIsTheLastCashFlowAndNotTheTermination) {
  fixtures::CompareOisOptions o;
  o.trades = 1;
  o.quote_noise_bp = 0.0;
  const fixtures::CompareOis s = fixtures::make_compare_ois(o);
  int lagged = 0;
  for (int i = 0; i < s.n_quotes(); ++i) {
    const epykos::instrument::Instrument& in = s.set.instruments[static_cast<std::size_t>(i)].instrument;
    EXPECT_GT(in.last_payment, in.termination)
        << s.set.instruments[static_cast<std::size_t>(i)].key
        << ": the last payment is not after the termination, so this convention has no payment lag and the "
           "pillar question this test exists to pin has changed shape";
    // The knot time is the last cash flow's, on ACT/365 from the valuation date.
    const double expect_t = static_cast<double>(in.last_payment - s.valuation) / 365.0;
    EXPECT_NEAR(s.set.knot_t[static_cast<std::size_t>(i)], expect_t, 1.0e-12)
        << "knot " << i << " is not at the last cash flow";
    ++lagged;
  }
  EXPECT_EQ(lagged, s.n_quotes());
  std::cout << "  [ql-oracle] all " << lagged
            << " knots sit at the instrument's last cash flow, two business days past termination\n";
}
