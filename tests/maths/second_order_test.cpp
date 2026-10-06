// PRINCIPLES.md §1b invariant I2, the agreement gate: the second derivatives one nested pass of
// `Dual<N, Dual<N>>` produces must agree with central differences of the EXACT first derivatives.
//
// This is §5's error contract, case 2 — a tolerance stated once measured, never guessed. What was
// measured first, 2026-10-06, by sweeping the step over ten decades: the deviation follows the
// textbook central-difference curve, falling a clean 100x per decade from h = 1e-3 down to a floor
// near h = 1e-6 and rising again below it. The floor is the FINITE DIFFERENCE's — truncation
// against cancellation, ~eps^(2/3) at best — and not the dual's. Then, at h = 1e-6, over the
// twenty-one cases this file runs (the [ MEASURED ] lines it prints are that measurement):
//
//     max |H_dual − H_fd| / max |H_dual|                    worst 2.290e-9   (the SOFR 3M future)
//     the same, entry by entry, over |H_ij| >= 0.1 max|H|   worst 3.562e-9   (the same case)
//     max |H_ij − H_ji| / max |H|   (the nested pass alone) worst 1.596e-15  (the same case)
//
// So the gates are 2e-8, 5e-8 and 2e-14 — about an order of magnitude above what was measured, per
// §5.2a case 2 — and every case PRINTS its measured value so drift is visible rather than
// absorbed. Two of the three are tolerances on the REFERENCE's accuracy, not the dual's. The
// symmetry figure is different in kind: it is a property of the nested pass alone (Clairaut), it
// needs no reference at all, and it is held to roundoff.
//
// Coverage is the pricing maths, not a toy: the M1 book (1,000 OIS swaps on a 12-knot
// linear-zero curve, seasoned coupons included), every interpolation scheme on every variable and
// a two-region composite, and the instrument sample (every Stage A blueprint: compounded RFR with
// lookback / shift / lockout, averaged RFR, term rate, deposits, futures, basis). The scalar-level
// algebra of the nesting is tests/scalar/dual_nested_test.cpp; that the nesting leaves the
// first-order answer bitwise alone is tests/maths/second_order_e0_test.cpp.
#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <string>
#include <type_traits>
#include <vector>

#include "epykos/fixtures/instrument_sample.hpp"
#include "epykos/fixtures/m1_book.hpp"
#include "epykos/fixtures/m1_price.hpp"
#include "epykos/fixtures/second_order.hpp"
#include "epykos/maths/curve/composite.hpp"
#include "epykos/maths/curve/curve.hpp"
#include "epykos/maths/curve/scheme.hpp"

namespace fx = epykos::fixtures;
namespace ec = epykos::curve;

namespace {

// The step the sweep picked: the minimum of truncation (O(h^2)) against cancellation (O(eps/h))
// for inputs of order 1e-2 (rates) and 1e-1 (log discount factors).
constexpr double kStep = 1e-6;
// Measured worst 2.290e-9 / 3.562e-9 / 1.596e-15; stated about an order of magnitude looser
// (§5.2a case 2). Never move one of these to make a change pass: a failure here says the second
// derivative is wrong, or the step is no longer at the floor, and both are findings.
constexpr double kTolRelNorm = 2e-8;
constexpr double kTolRelTop = 5e-8;
constexpr double kTolAsymmetry = 2e-14;

// Run one case: nested pass, central-difference reference, the three gates, the measured values.
template <int N, class F>
fx::SecondOrder<N> check(const std::string& name, F&& f, const double* z) {
  const fx::SecondOrder<N> s = fx::second_order<N>(f, z);
  const std::array<double, N * N> ref = fx::hessian_central<N>(f, z, kStep);
  const fx::Agreement a = fx::agreement<N>(s, ref);
  const double asym = s.asymmetry();
  std::printf("[ MEASURED ] %-44s max|H| %-11.4e rel(norm) %-10.3e rel(top %2d) %-10.3e asym %.3e\n", name.c_str(),
              a.norm, a.rel_norm, a.n_top, a.rel_top, asym);
  EXPECT_LE(a.rel_norm, kTolRelNorm) << name << ": second derivatives disagree with central differences of the first";
  EXPECT_LE(a.rel_top, kTolRelTop) << name << ": a dominant Hessian entry disagrees with its central difference";
  EXPECT_LE(asym, kTolAsymmetry) << name << ": the nested pass's own Hessian is not symmetric";
  return s;
}

const fx::Book& book() {
  static const fx::Book b = fx::make_m1_book();
  return b;
}
const fx::InstrumentSample& sample() {
  static const fx::InstrumentSample s = fx::make_instrument_sample();
  return s;
}

}  // namespace

// ---- the M1 book: 1,000 OIS swaps, 12 knots ----------------------------------------------------

TEST(SecondOrder, M1Book) {
  constexpr int N = fx::n_knots;
  std::vector<double> z(fx::record_state.begin(), fx::record_state.end());
  auto book_pv = [](const auto* x) {
    using Scalar = std::decay_t<decltype(*x)>;
    std::vector<Scalar> out(static_cast<std::size_t>(book().n_swaps));
    Scalar total;
    fx::price_book<Scalar>(book(), x, out.data(), &total);
    return total;
  };
  const fx::SecondOrder<N> s = check<N>("M1 book PV", book_pv, z.data());

  // The engine could not compute any of this before (PRINCIPLES.md §1b: "no gamma, cross-gamma,
  // vanna or volga anywhere"). Sanity, in the maths rather than against a reference: the book is
  // a sum of discount factors, so d2DF/dz2 = t^2 DF > 0 term by term, and the diagonal of a
  // receive-fixed-heavy book of positive-notional swaps cannot be identically zero.
  EXPECT_GT(s.norm(), 0.0);
  double diag = 0.0;
  for (int k = 0; k < N; ++k) diag = std::fmax(diag, std::fabs(s.at(k, k)));
  EXPECT_GT(diag, 0.0);
}

TEST(SecondOrder, M1SingleSwaps) {
  constexpr int N = fx::n_knots;
  std::vector<double> z(fx::record_state.begin(), fx::record_state.end());
  for (int i : {0, 199, 200, 999}) {  // 0 and 199 are seasoned (a realised first fixing)
    auto one = [i](const auto* x) {
      using Scalar = std::decay_t<decltype(*x)>;
      return fx::swap_pv<Scalar>(book(), i, x);
    };
    check<N>("M1 swap " + std::to_string(i), one, z.data());
  }
}

// ---- the interpolation schemes, every variable, and a composite --------------------------------

namespace {
constexpr int Nk = 6;
const double kKnotT[Nk] = {0.25, 1.0, 2.0, 5.0, 10.0, 30.0};
const double kZero[Nk] = {0.040, 0.0405, 0.0415, 0.0400, 0.0410, 0.0435};     // zero / forward variable
const double kLogDf[Nk] = {-0.010, -0.0405, -0.083, -0.20, -0.41, -1.30};     // logdf variable
}  // namespace

TEST(SecondOrder, CurveSchemesOnTheZeroVariable) {
  auto run = [](const std::string& name, auto scheme_tag, double at) {
    using Scheme = decltype(scheme_tag);
    auto f = [at](const auto* x) {
      using Scalar = std::decay_t<decltype(*x)>;
      const ec::Curve<Scheme, ec::Variable::zero> c(kKnotT, Nk);
      return c.template df<Scalar>(x, at);
    };
    check<Nk>(name, f, kZero);
  };
  run("Curve<Flat, zero>.df(3.3)", ec::Flat{}, 3.3);
  run("Curve<Linear, zero>.df(3.3)", ec::Linear{}, 3.3);
  run("Curve<Hermite, zero>.df(3.3)", ec::Hermite{}, 3.3);
  run("Curve<NaturalCubic, zero>.df(3.3)", ec::NaturalCubic{}, 3.3);
  run("Curve<MonotoneCubic, zero>.df(3.3)", ec::MonotoneCubic{}, 3.3);
  run("Curve<BSpline, zero>.df(3.3)", ec::BSpline{}, 3.3);
  // MonotoneCubic is the one scheme whose value depends on the ORDER of the knot values: its
  // Hyman limiter is a stack of selects. A second derivative through a select is the chosen
  // arm's, and this says the chosen arm is differentiated twice correctly.
  run("Curve<MonotoneCubic, zero>.df(15.0)", ec::MonotoneCubic{}, 15.0);
}

TEST(SecondOrder, CurveSchemesOnTheOtherVariables) {
  {
    auto f = [](const auto* x) {
      using Scalar = std::decay_t<decltype(*x)>;
      const ec::Curve<ec::Linear, ec::Variable::logdf> c(kKnotT, Nk);
      return c.template df<Scalar>(x, 3.3);
    };
    check<Nk>("Curve<Linear, logdf>.df(3.3)", f, kLogDf);
  }
  {
    auto f = [](const auto* x) {
      using Scalar = std::decay_t<decltype(*x)>;
      const ec::Curve<ec::NaturalCubic, ec::Variable::logdf> c(kKnotT, Nk);
      return c.template df<Scalar>(x, 7.5);
    };
    check<Nk>("Curve<NaturalCubic, logdf>.df(7.5)", f, kLogDf);
  }
  {
    auto f = [](const auto* x) {
      using Scalar = std::decay_t<decltype(*x)>;
      const ec::Curve<ec::Linear, ec::Variable::forward> c(kKnotT, Nk);
      return c.template df<Scalar>(x, 3.3);
    };
    check<Nk>("Curve<Linear, forward>.df(3.3)", f, kZero);
  }
  {
    auto f = [](const auto* x) {
      using Scalar = std::decay_t<decltype(*x)>;
      const ec::Curve<ec::Flat, ec::Variable::forward> c(kKnotT, Nk);
      return c.template df<Scalar>(x, 12.0);
    };
    check<Nk>("Curve<Flat, forward>.df(12.0)", f, kZero);
  }
}

TEST(SecondOrder, CompositeCurve) {
  static const double mix[Nk] = {0.040, 0.0405, -0.083, -0.20, -0.41, -1.30};
  auto f = [](const auto* x) {
    using Scalar = std::decay_t<decltype(*x)>;
    std::vector<ec::RegionSpec> regions(2);
    regions[0] = {0.0, 2.0, ec::SchemeKind::linear, ec::Variable::zero};
    regions[1] = {2.0, 1e30, ec::SchemeKind::monotone_cubic, ec::Variable::logdf};
    const ec::Composite comp(kKnotT, Nk, regions);
    return comp.template df<Scalar>(x, 7.5);
  };
  check<Nk>("Composite(linear/zero + monotone/logdf).df(7.5)", f, mix);
  // The region boundary carries an anchor whose value is converted between the two variables;
  // a second derivative must survive that conversion too.
  auto g = [](const auto* x) {
    using Scalar = std::decay_t<decltype(*x)>;
    std::vector<ec::RegionSpec> regions(2);
    regions[0] = {0.0, 2.0, ec::SchemeKind::linear, ec::Variable::zero};
    regions[1] = {2.0, 1e30, ec::SchemeKind::monotone_cubic, ec::Variable::logdf};
    const ec::Composite comp(kKnotT, Nk, regions);
    return comp.template df<Scalar>(x, 1.5);
  };
  check<Nk>("Composite(...).df(1.5), region 0 with its anchor", g, mix);
}

// ---- the instrument sample: the real coupon maths ----------------------------------------------

namespace {

// The sample's inputs are 4 curves x 11 knots; this varies the USD-SOFR curve's 11 knots and
// holds the other three at the record point, which keeps a nested pass at 12^2 doubles a scalar.
constexpr int Ks = fx::sample_knots;  // 11

template <class Scalar>
Scalar sample_output(const Scalar* x, int output) {
  const fx::InstrumentSample& s = sample();
  std::vector<Scalar> full(static_cast<std::size_t>(s.n_inputs()));
  for (int m = 0; m < s.n_inputs(); ++m) full[static_cast<std::size_t>(m)] = Scalar(s.z0[static_cast<std::size_t>(m)]);
  for (int k = 0; k < Ks; ++k) full[static_cast<std::size_t>(k)] = x[k];
  std::vector<Scalar> out(static_cast<std::size_t>(s.n_outputs()));
  fx::price_sample<Scalar>(s, full.data(), out.data());
  return out[static_cast<std::size_t>(output)];
}

}  // namespace

TEST(SecondOrder, InstrumentSampleBook) {
  auto f = [](const auto* x) {
    using Scalar = std::decay_t<decltype(*x)>;
    return sample_output<Scalar>(x, sample().n_trades());
  };
  const fx::SecondOrder<Ks> s = check<Ks>("sample book PV wrt USD-SOFR", f, sample().z0.data());
  EXPECT_GT(s.norm(), 0.0);
}

TEST(SecondOrder, InstrumentSampleTrades) {
  // One trade of each awkward kind, so that a small Hessian is not averaged into the book's.
  for (int i : {0, 17, 34}) {
    auto f = [i](const auto* x) {
      using Scalar = std::decay_t<decltype(*x)>;
      return sample_output<Scalar>(x, i);
    };
    const std::string name = "sample trade " + std::to_string(i) + " " +
                             sample().instruments[static_cast<std::size_t>(i)].blueprint;
    const fx::SecondOrder<Ks> s = check<Ks>(name, f, sample().z0.data());
    EXPECT_GT(s.norm(), 0.0) << name << ": a trade priced off the varied curve has a zero Hessian";
  }
}

// A trade that does not read the curve being varied must give EXACTLY zero, not noise: a derivative
// the maths does not have is a structural zero, and the engine reproduces it as one. (D94's lesson,
// in miniature: a plausible small number where there should be an unmistakable one is the failure
// mode this repository has shipped twice.)
TEST(SecondOrder, StructuralZeroIsExactlyZero) {
  const fx::InstrumentSample& s = sample();
  int found = 0;
  for (int i = 0; i < s.n_trades() && found < 3; ++i) {
    if (s.instruments[static_cast<std::size_t>(i)].currency != "EUR") continue;  // EUR trades do not read slot 0
    auto f = [i](const auto* x) {
      using Scalar = std::decay_t<decltype(*x)>;
      return sample_output<Scalar>(x, i);
    };
    const fx::SecondOrder<Ks> so = fx::second_order<Ks>(f, s.z0.data());
    for (int a = 0; a < Ks; ++a) {
      EXPECT_EQ(so.grad[static_cast<std::size_t>(a)], 0.0)
          << "trade " << i << " (" << s.instruments[static_cast<std::size_t>(i)].blueprint << ") d/dz_" << a;
      for (int b = 0; b < Ks; ++b) {
        EXPECT_EQ(so.at(a, b), 0.0) << "trade " << i << " d2/dz_" << a << "dz_" << b;
      }
    }
    ++found;
  }
  EXPECT_EQ(found, 3) << "the sample should hold EUR trades, which do not read the USD-SOFR curve";
}
