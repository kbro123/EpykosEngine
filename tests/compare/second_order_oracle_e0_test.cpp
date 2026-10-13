// THE FIRST EXTERNAL CHECK ON A SECOND DERIVATIVE in this repository.
//
// D93 §1b recorded that `Dual<N>` over a hardcoded `double` "forecloses every second derivative —
// the engine cannot compute a gamma by any route". D97 and D101 reopened it: `Dual<Scalar, N>`
// nests, so one pass of `Dual<N, Dual<N>>` over the templated maths yields a Hessian, and D101
// computed a book gamma with respect to par quotes. **Nothing external had ever checked one** —
// not QuantLib, not SwapEngine, not a finite difference (D125 §4 named it the largest remaining
// gap).
//
//   book PV              engine vs QuantLib                         1.52e-13 relative
//   Hessian, 16x16       nested Dual vs QuantLib 4-point mixed FD   1.62e-08 of |H|max
//
// ---- what is differentiated, and what this therefore covers -----------------------------------
//
// The variable is the CURVE KNOT, not the par quote. `compare_ois`'s region is linear in log DF,
// so a knot IS its log discount factor and a perturbation means the same thing to both engines
// exactly. **No solver is involved on either side**, so this isolates the second-order channel of
// the PRICING MATHS — which is the layer D93 said was foreclosed, and the foundation the IFT's
// second order is built on.
//
// It does NOT cover the IFT's second-order channel (gamma with respect to par QUOTES, D101), which
// adds `solver::implicit_dual`'s own nesting on top. That needs a four-point difference over
// re-solves and is the remaining piece; the distinction is the same one D120 §2 drew for the
// first-order ladder, and is stated here rather than left for a reader to assume.
//
// ---- why the residual is the finite difference's ----------------------------------------------
//
// Swept, worst deviation as a fraction of |H|max:
//
//   h = 1e-3    3.36e-07
//   h = 3e-4    2.95e-08    falling 11.4x for a 3.33x step -- h^2, FD TRUNCATION
//   h = 1e-4    1.62e-08    falling 1.8x, flattening
//   h = 3e-5    2.00e-07    RISING 12.3x -- round-off, amplified by 1/h^2
//
// The same V as D120 §3 one order up, and steeper on the small-h side because a second difference
// divides by h^2. The engine's analytic Hessian is the accurate side; 1.6e-08 of |H|max is the
// floor of what a four-point difference can resolve on a 3.6e6 book PV.
//
// ---- two internal properties the nested pass also satisfies -----------------------------------
//
// Reported by the same pass and gated below, because they constrain it in ways the FD cannot:
// the two gradient channels (outer tangent and inner tangent) agree **exactly, 0.0**, and
// Clairaut symmetry holds to **2.36e-15** of |H|max. A Hessian that matched an FD to 1e-8 while
// being asymmetric at 1e-6 would be wrong in a way this file would otherwise miss.
//
// ---- provenance (D108's rule, as D114-D125) ---------------------------------------------------
//
// 256 exact hex-float literals from QuantLib 1.35 at h = 1e-4. No dependency; D12 untouched.
// TO REGENERATE: dump the knot dates, the generating curve's knot log-DFs and the 8-trade book,
// then for each (i, j) price the book four times with knots i and j perturbed by +-h, as
// `OvernightIndexedSwap(..., Schedule(eff, UNADJUSTED term, Annual, join(SIFMA, FED),
// ModifiedFollowing, ModifiedFollowing, DateGeneration::Backward, eom), ..., Compound)` on an
// `InterpolatedDiscountCurve<LogLinear>` over `exp(knot + perturbation)` with DF(valuation) = 1.
// The traps of D115 §3 and D119 §2 apply unchanged.
//
// An E0 TU (-ffp-contract=off in every preset), which is also what makes the two gradient channels
// bitwise rather than merely close (D8, D25).
#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <iostream>
#include <iterator>
#include <type_traits>
#include <vector>

#include "epykos/fixtures/compare_ois.hpp"
#include "epykos/fixtures/second_order.hpp"
#include "epykos/maths/curve/composite.hpp"
#include "epykos/maths/instrument/coupon.hpp"

#ifndef EPYKOS_FP_CONTRACT_OFF
#error "an _e0_test.cpp TU must see EPYKOS_FP_CONTRACT_OFF"
#endif

namespace fixtures = epykos::fixtures;
namespace curve = epykos::curve;

namespace {

constexpr int kN = 16;   // compare_ois's knot count, and the nesting width

inline constexpr double kQlHessian[] = {
0x1.bebaa9fecc65p+23, -0x1.36290cc4e52p+21, -0x1.af171e0d3p+12, -0x1.7d784p-4,
    0x0p+0, -0x1.7d784p-7, -0x1.7d784p-3, -0x1.4dc938p-4,
    0x0p+0, -0x1.7d784p-3, 0x1.7d784p-7, 0x0p+0,
    -0x1.7d784p-4, 0x0p+0, 0x0p+0, 0x0p+0,
    -0x1.36290cc4e52p+21, 0x1.acd52782610dp+23, -0x1.29128aee8bfp+21, -0x1.9cb5b405cp+12,
    0x1.ad2748p-4, 0x1.7d784p-6, -0x1.7d784p-4, -0x1.7d784p-4,
    -0x1.7d784p-3, -0x1.954fc4p-3, -0x1.65a0bcp-3, 0x0p+0,
    0x0p+0, 0x0p+0, 0x0p+0, 0x0p+0,
    -0x1.af171e0d3p+12, -0x1.29128aee8bfp+21, 0x1.9c3102f67efp+23, -0x1.21ef1674f658p+21,
    -0x1.99ae608e2cp+13, -0x1.7d784p-4, 0x0p+0, -0x1.7d784p-4,
    0x1.7d784p-4, 0x1.7d784p-7, 0x1.4dc938p-4, -0x1.7d784p-4,
    0x1.7d784p-4, -0x1.7d784p-4, -0x1.7d784p-4, 0x0p+0,
    -0x1.7d784p-4, -0x1.9cb5b405cp+12, -0x1.21ef1674f658p+21, 0x1.a9a35b0f88bep+22,
    -0x1.009befe04476p+22, -0x1.4155dc414p+13, 0x0p+0, 0x0p+0,
    0x1.7d784p-3, 0x1.ad2748p-4, 0x1.4dc938p-4, -0x1.7d784p-4,
    0x1.7d784p-3, -0x1.7d784p-4, -0x1.7d784p-4, 0x0p+0,
    0x0p+0, 0x1.ad2748p-4, -0x1.99ae608e2cp+13, -0x1.009befe04476p+22,
    0x1.8b6aea45b39e4p+25, -0x1.20fdbfd235fp+20, -0x1.63282e19ep+10, 0x1.4dc938p-4,
    -0x1.7d784p-4, 0x0p+0, -0x1.ad2748p-4, 0x0p+0,
    0x0p+0, 0x0p+0, 0x0p+0, 0x0p+0,
    -0x1.7d784p-7, 0x1.7d784p-6, -0x1.7d784p-4, -0x1.4155dc414p+13,
    -0x1.20fdbfd235fp+20, 0x1.df3f8edc8c6aap+26, -0x1.ebd5c03d12ap+18, -0x1.564988892p+10,
    0x0p+0, -0x1.4dc938p-4, -0x1.ad2748p-4, 0x0p+0,
    -0x1.7d784p-4, 0x0p+0, 0x0p+0, 0x0p+0,
    -0x1.7d784p-3, -0x1.7d784p-4, 0x0p+0, 0x0p+0,
    -0x1.63282e19ep+10, -0x1.ebd5c03d12ap+18, 0x1.6436a78357c8p+21, -0x1.d76ad878a0cp+18,
    -0x1.47379f69ep+10, 0x1.7d784p-4, 0x1.7d784p-4, 0x0p+0,
    -0x1.7d784p-4, 0x0p+0, 0x0p+0, 0x0p+0,
    -0x1.4dc938p-4, -0x1.7d784p-4, -0x1.7d784p-4, 0x0p+0,
    0x1.4dc938p-4, -0x1.564988892p+10, -0x1.d76ad878a0cp+18, 0x1.553e09b0ca28p+21,
    -0x1.c147683ede2p+18, -0x1.3610fedd4p+11, -0x1.7d784p-7, 0x0p+0,
    0x1.1e1a3p-2, -0x1.7d784p-4, -0x1.7d784p-4, 0x0p+0,
    0x0p+0, -0x1.7d784p-3, 0x1.7d784p-4, 0x1.7d784p-3,
    -0x1.7d784p-4, 0x0p+0, -0x1.47379f69ep+10, -0x1.c147683ede2p+18,
    0x1.7e360fa985d4p+21, -0x1.a9b49f2a081p+19, -0x1.2907820b5p+11, 0x0p+0,
    -0x1.7d784p-2, 0x1.7d784p-4, 0x1.7d784p-4, 0x0p+0,
    -0x1.7d784p-3, -0x1.954fc4p-3, 0x1.7d784p-7, 0x1.ad2748p-4,
    0x0p+0, -0x1.4dc938p-4, 0x1.7d784p-4, -0x1.3610fedd4p+11,
    -0x1.a9b49f2a081p+19, 0x1.9243eae620c8p+21, 0x1.624f9cd73a8p+16, -0x1.6f5049ap+7,
    -0x1.7d784p-4, -0x1.7d784p-4, -0x1.7d784p-4, 0x0p+0,
    0x1.7d784p-7, -0x1.65a0bcp-3, 0x1.4dc938p-4, 0x1.4dc938p-4,
    -0x1.ad2748p-4, -0x1.ad2748p-4, 0x1.7d784p-4, -0x1.7d784p-7,
    -0x1.2907820b5p+11, 0x1.624f9cd73a8p+16, 0x1.8b1a9a3fe58p+21, 0x1.003b64ab9ebp+19,
    -0x1.023b82f3p+7, 0x0p+0, 0x0p+0, 0x0p+0,
    0x0p+0, 0x0p+0, -0x1.7d784p-4, -0x1.7d784p-4,
    0x0p+0, 0x0p+0, 0x0p+0, 0x0p+0,
    0x0p+0, -0x1.6f5049ap+7, 0x1.003b64ab9ebp+19, 0x1.dfcf05996938p+21,
    0x1.bd9a242cf51p+19, -0x1.896402p+3, -0x1.7d784p-4, 0x0p+0,
    -0x1.7d784p-4, 0x0p+0, 0x1.7d784p-4, 0x1.7d784p-3,
    0x0p+0, -0x1.7d784p-4, -0x1.7d784p-4, 0x1.1e1a3p-2,
    -0x1.7d784p-2, -0x1.7d784p-4, -0x1.023b82f3p+7, 0x1.bd9a242cf51p+19,
    0x1.1f26344db7bcp+24, 0x1.084a002901p+18, -0x1.3be795p+3, 0x0p+0,
    0x0p+0, 0x0p+0, -0x1.7d784p-4, -0x1.7d784p-4,
    0x0p+0, 0x0p+0, 0x0p+0, -0x1.7d784p-4,
    0x1.7d784p-4, -0x1.7d784p-4, 0x0p+0, -0x1.896402p+3,
    0x1.084a002901p+18, 0x1.1f09a8de054p+20, 0x1.a3fdfa69c6p+17, -0x1.004ccbp+2,
    0x0p+0, 0x0p+0, -0x1.7d784p-4, -0x1.7d784p-4,
    0x0p+0, 0x0p+0, 0x0p+0, -0x1.7d784p-4,
    0x1.7d784p-4, -0x1.7d784p-4, 0x0p+0, -0x1.7d784p-4,
    -0x1.3be795p+3, 0x1.a3fdfa69c6p+17, 0x1.412b19dca38p+20, 0x1.57570edf58p+18,
    0x0p+0, 0x0p+0, 0x0p+0, 0x0p+0,
    0x0p+0, 0x0p+0, 0x0p+0, 0x0p+0,
    0x0p+0, 0x0p+0, 0x0p+0, 0x0p+0,
    0x0p+0, -0x1.004ccbp+2, 0x1.57570edf58p+18, 0x1.34be9f6f1bp+22,
};

// Comfortably above the measured 1.62e-08, which is the FD's own floor (see the header).
constexpr double kHessRelTol = 1.0e-6;
// The ENGINE's own book PV, as a fixture-identity check rather than an oracle: QuantLib's agrees
// with it to 1.52e-13 relative, which is recorded in D126 §1 and not re-derived here.
inline constexpr double kEngineBookPv = -0x1.b8623794bd078p+21;

// The book PV as a function of the curve knots, templated on the Scalar so the nested pass can
// drive it. This is the engine's own pricing maths; nothing here is written for the test.
struct BookOfKnots {
  const fixtures::CompareOis* s;
  const curve::Composite* comp;
  template <class Scalar>
  Scalar operator()(const Scalar* z) const {
    const auto st = comp->prepare(z);
    auto df = [&](int, double t) { return comp->df(st, t); };
    Scalar pv = Scalar(0.0);
    for (const epykos::instrument::Instrument& in : s->book) {
      for (std::size_t L = 0; L < in.legs.size(); ++L) {
        const Scalar lpv = epykos::instrument::leg_pv<Scalar>(in.legs[L], df);
        pv = pv + ((L == 0 ? 1.0 : -1.0) * static_cast<double>(in.side)) * lpv;
      }
    }
    return pv;
  }
};

struct Pass {
  fixtures::CompareOis s;
  curve::Composite comp;
  fixtures::SecondOrder<kN> so;
};

const Pass& nested_pass() {
  static const Pass p = [] {
    fixtures::CompareOisOptions o;
    o.trades = 8;
    o.quote_noise_bp = 0.0;
    fixtures::CompareOis s = fixtures::make_compare_ois(o);
    curve::Composite comp = s.set.composite();
    std::vector<double> z(s.generating_z.begin(), s.generating_z.end());
    Pass r{std::move(s), std::move(comp), {}};
    r.so = fixtures::second_order<kN>(BookOfKnots{&r.s, &r.comp}, z.data());
    return r;
  }();
  return p;
}

}  // namespace

// The fixture the literals belong to.
TEST(SecondOrderOracleE0, TheFixtureTheLiteralsWereGeneratedFromHasNotMoved) {
  const Pass& p = nested_pass();
  ASSERT_EQ(p.s.n_quotes(), kN);
  EXPECT_EQ(p.s.n_trades(), 8);
  EXPECT_EQ(static_cast<std::size_t>(kN) * kN, std::size(kQlHessian));
  EXPECT_NEAR(p.so.value, kEngineBookPv, std::abs(kEngineBookPv) * 1.0e-11)
      << "the book PV moved, so the Hessian literals belong to a different book";
}

// The oracle.
TEST(SecondOrderOracleE0, TheHessianMatchesQuantLibsFourPointSecondDifference) {
  const Pass& p = nested_pass();
  const double scale = p.so.norm();
  ASSERT_GT(scale, 0.0) << "the Hessian is identically zero";
  double worst = 0.0;
  int wi = 0, wj = 0;
  for (int i = 0; i < kN; ++i) {
    for (int j = 0; j < kN; ++j) {
      const double dev = std::abs(p.so.at(i, j) - kQlHessian[static_cast<std::size_t>(i) * kN + j]) / scale;
      if (dev > worst) {
        worst = dev;
        wi = i;
        wj = j;
      }
    }
  }
  std::cout << "  [ql-oracle] 16x16 Hessian vs QuantLib 4-point FD: worst " << worst << " of |H|max = " << scale
            << " at (" << wi << ", " << wj << ")\n";
  EXPECT_LT(worst, kHessRelTol) << "worst at (" << wi << ", " << wj << "): engine " << p.so.at(wi, wj)
                                << ", QuantLib FD " << kQlHessian[static_cast<std::size_t>(wi) * kN + wj]
                                << ". Read this file's header on the h sweep before concluding the engine is wrong: "
                                   "the FD's own floor here is 1.6e-08 of |H|max.";
}

// Two properties the finite difference cannot constrain, and which a wrong Hessian could satisfy
// the FD check while violating.
TEST(SecondOrderOracleE0, TheTwoGradientChannelsAgreeBitwiseAndClairautHolds) {
  const Pass& p = nested_pass();
  double grad_gap = 0.0;
  for (int i = 0; i < kN; ++i) grad_gap = std::fmax(grad_gap, std::abs(p.so.grad[i] - p.so.grad_inner[i]));
  // BITWISE, not close: both channels are the same expressions in the same IEEE operations on the
  // same inputs, and this TU pins contraction off (§5.2a case 1).
  EXPECT_EQ(grad_gap, 0.0) << "the outer and inner tangent channels differ by " << grad_gap
                           << ", so one of them is not the Dual<N> pass it is supposed to be";
  const double asym = p.so.asymmetry();
  EXPECT_LT(asym, 1.0e-12) << "Clairaut symmetry is violated at " << asym
                           << " of |H|max; the two mixed partials are different operation orders and should agree "
                              "to roundoff, not to a tolerance";
  std::cout << "  [ql-oracle] gradient channels differ by " << grad_gap << " (bitwise), Clairaut asymmetry " << asym
            << '\n';
}

// The Hessian is not degenerate: a near-diagonal or near-zero one would make the comparison cheap.
TEST(SecondOrderOracleE0, TheHessianIsNotDegenerate) {
  const Pass& p = nested_pass();
  const double scale = p.so.norm();
  int offdiag = 0;
  double offdiag_max = 0.0;
  for (int i = 0; i < kN; ++i) {
    for (int j = 0; j < kN; ++j) {
      if (i == j) continue;
      const double a = std::abs(p.so.at(i, j));
      if (a > 1.0e-6 * scale) ++offdiag;
      offdiag_max = std::fmax(offdiag_max, a);
    }
  }
  std::cout << "  [ql-oracle] " << offdiag << " off-diagonal entries above 1e-6 of |H|max; largest off-diagonal is "
            << 100.0 * offdiag_max / scale << "% of |H|max\n";
  EXPECT_GT(offdiag, 20) << "the Hessian is essentially diagonal, so matching it says little about the mixed "
                            "partials -- which are the entries a nested pass can get wrong on their own";
}
