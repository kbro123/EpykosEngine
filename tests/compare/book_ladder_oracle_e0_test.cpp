// THE FIRST INDEPENDENT NUMBER ON O3. Until this file the risk ladder — the engine's strongest
// published result (D90: 6.12x the other engine; D103: the reverse pass is 62-80% of it) — had been
// compared only to the engine: to `Dual`, which shares the recording, and to the other engine,
// whose conventions ours were imported from (D117 §1). Here QuantLib prices the book and a central
// difference supplies the derivative, so both the pricer and the differentiation method are
// outside this repository.
//
//   book NPV       engine vs QuantLib on the engine's calibrated curve   9.80e-15 relative
//   ladder         16 buckets, d(book NPV)/d(quote), IFT vs central FD   4.46e-10 relative worst
//
// The book NPV figure is itself a first: D109 compared it only to SwapEngine (7.829e-15).
//
// ---- WHAT THIS DOES AND DOES NOT ESTABLISH ----------------------------------------------------
//
// The chain is quote -> (solve) -> knots -> (price) -> NPV and the ladder is
// (dNPV/dknots)·(dknots/dq). The PRICING and the DIFFERENTIATION here are independent; the
// CALIBRATION MAP is the engine's in both arms, because QuantLib's natural OIS pillar is the swap's
// maturity while ours is its last cash flow, so a QuantLib bootstrap would put the two curves on
// node sets two business days apart and the comparison would measure node placement (D115 §5).
//
// So: an independent check of the ADJOINT and of the IFT's composition, and NOT of the solve. The
// honest version of "our risk is verified" is that sentence, not the headline.
//
// ---- why the residual is the FD's and not the engine's ----------------------------------------
//
// Swept, worst relative difference over the 16 buckets:
//
//   h = 0.1 bp     3.54e-09
//   h = 0.05 bp    1.07e-09      falling 3.3x per halving — h^2, i.e. FD TRUNCATION
//   h = 0.025 bp   4.46e-10      falling 2.4x
//   h = 0.0125 bp  7.23e-10      RISING — FD round-off has taken over
//
// That V, falling as ~h^2 and then rising, is a central difference's own error curve: the engine's
// analytic ladder is the accurate side of the comparison, and 4.5e-10 is the floor of what a
// finite difference can resolve here rather than a defect. It is also the method their own
// `risk_ladder_oracle_test.cpp` uses to make the same argument ("fell exactly h^2 at 0.25 bp:
// truncation, so the bump shrank"), arrived at here independently and recorded as such.
//
// ---- provenance (D108's rule, as D114-D119) ---------------------------------------------------
//
// The expectations are QuantLib 1.35's own central differences, exact hex floats, generated once.
// No dependency; D12 untouched. `quote_noise_bp = 0` makes the quotes the generating curve's exact
// par rates. TO REGENERATE: dump the engine's calibrated knot log-DFs at the base quotes and at
// every quote bumped +-h, hold each as `InterpolatedDiscountCurve<LogLinear>` with
// `Actual365Fixed` on the knots' own dates (DF(valuation) = 1 prepended), price the 64 trades as
// `OvernightIndexedSwap(side, notional, Schedule(eff, UNADJUSTED term, Annual, join(SIFMA, FED),
// ModifiedFollowing, ModifiedFollowing, DateGeneration::Backward, eom), rate, Actual360(),
// Sofr(h), 0.0, 2, ModifiedFollowing, cal, false, Compound)`, sum, and difference. The two harness
// traps of D115 §3 and D119 §2 apply unchanged.
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

// QuantLib 1.35, (NPV(q_k + h) - NPV(q_k - h)) / 2h with h = 0.025 bp, on the engine's calibrated
// curves, one per calibration quote in fixture order.
inline constexpr double kQlBookLadder[] = {
    0x1.fe3e2f0bd30b7p+27,  -0x1.2830c732f6948p+27,  // 1Y, 2Y
    -0x1.3049844e21fb4p+28, 0x1.82f7337338ac7p+28,   // 3Y, 4Y
    -0x1.1c3ada77a7e64p+30, -0x1.0f6ccc4b4f3c8p+27,  // 5Y, 6Y
    0x1.823287946c1bfp+26,  0x1.f8b40c64bbb0fp+28,   // 7Y, 8Y
    0x1.c13fa242ed89fp+27,  -0x1.f872dec2053cfp+28,  // 9Y, 10Y
    -0x1.1c46e10cdfb64p+30, -0x1.1a6436c590878p+31,  // 12Y, 15Y
    -0x1.f942d7553dd9fp+28, 0x1.7a0a0819544ffp+30,   // 20Y, 25Y
    -0x1.189ce45365f11p+31, -0x1.ad448ecba674fp+28,  // 30Y, 40Y
};

// QuantLib's NPV of the same book on the same curve agrees with the engine's to 9.80e-15 relative;
// this is the engine's, as a literal, so a change to the book or the curve fails here.
inline constexpr double kEngineBookNpv = -0x1.61c8d180d9bbbp+24;

// Two orders above the measured 4.46e-10 worst, which is the FD's own floor (see the header). A
// real defect in the adjoint or the IFT moves a bucket by far more than this; the last digits of a
// central difference are not a maintenance burden.
constexpr double kLadderRelTol = 1.0e-8;

}  // namespace

TEST(BookLadderOracleE0, TheBookNpvAndTheLadderMatchQuantLib) {
  fixtures::CompareOisOptions o;
  o.trades = 64;
  o.quote_noise_bp = 0.0;  // the generating curve's exact par rates
  const fixtures::CompareOis s = fixtures::make_compare_ois(o);
  ASSERT_EQ(static_cast<std::size_t>(s.n_quotes()), std::size(kQlBookLadder))
      << "the calibration set changed; regenerate the QuantLib table (see this file's header)";
  ASSERT_EQ(s.n_trades(), 64);

  fixtures::CompareOisTape t = fixtures::record_compare_ois(s);
  solver::ImplicitProgram program(t.tape, t.registry, fixtures::compare_ois_program_options(s));
  const std::vector<int> ordinals = {t.book_output};
  std::vector<double> outputs;
  const std::vector<double> ladder = fixtures::compare_ois_ladder(program, s.quotes, ordinals, &outputs);

  const double npv = outputs[static_cast<std::size_t>(t.book_output)];
  EXPECT_NEAR(npv, kEngineBookNpv, std::abs(kEngineBookNpv) * 1.0e-12)
      << "the book NPV moved, so the ladder literals belong to a different book";

  double worst = 0.0;
  std::size_t worst_k = 0;
  for (std::size_t k = 0; k < std::size(kQlBookLadder); ++k) {
    const double rel = std::abs(ladder[k] - kQlBookLadder[k]) / std::max(std::abs(kQlBookLadder[k]), 1.0);
    if (rel > worst) {
      worst = rel;
      worst_k = k;
    }
    EXPECT_LT(rel, kLadderRelTol) << "bucket " << k << " (" << s.set.instruments[k].tenor << "): engine "
                                  << ladder[k] << ", QuantLib central FD " << kQlBookLadder[k] << ", relative "
                                  << rel;
  }
  std::cout << "  [ql-oracle] book NPV " << npv << "; 16-bucket ladder vs QuantLib central FD: worst " << worst
            << " relative at bucket " << worst_k << " (" << s.set.instruments[worst_k].tenor << ")\n";
}

// The ladder is not trivially zero, nor trivially one-signed, nor concentrated in one bucket: the
// comparison above would be weak if it were. The book is 64 trades drawn across the tenor set with
// both sides, so the ladder must straddle zero and spread across buckets.
TEST(BookLadderOracleE0, TheLadderIsNotDegenerate) {
  fixtures::CompareOisOptions o;
  o.trades = 64;
  o.quote_noise_bp = 0.0;
  const fixtures::CompareOis s = fixtures::make_compare_ois(o);
  fixtures::CompareOisTape t = fixtures::record_compare_ois(s);
  solver::ImplicitProgram program(t.tape, t.registry, fixtures::compare_ois_program_options(s));
  const std::vector<int> ordinals = {t.book_output};
  const std::vector<double> ladder = fixtures::compare_ois_ladder(program, s.quotes, ordinals, nullptr);

  int positive = 0, negative = 0;
  double total = 0.0, largest = 0.0;
  for (const double v : ladder) {
    if (v > 0.0) ++positive;
    if (v < 0.0) ++negative;
    total += std::abs(v);
    largest = std::max(largest, std::abs(v));
  }
  EXPECT_GE(positive, 3) << "the ladder has almost no positive buckets";
  EXPECT_GE(negative, 3) << "the ladder has almost no negative buckets";
  EXPECT_LT(largest / total, 0.5) << "one bucket carries over half the ladder, so agreeing on it would be most of "
                                     "the comparison and the other fifteen would be nearly free";
  std::cout << "  [ql-oracle] ladder shape: " << positive << " positive, " << negative
            << " negative buckets; largest bucket is " << 100.0 * largest / total << "% of the total\n";
}
