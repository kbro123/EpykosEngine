// M3/G1: the interpolation variables and the composite curve on double — DF(0) = 1 on every
// variable, logdf's origin knot and piecewise-constant forwards, the forward variable's closed
// form against a trapezoid quadrature, the M4 composite (Linear zero / MonotoneCubic zero /
// Linear logdf on the M1 knots) with DF value-continuous at both region boundaries (bitwise where
// the variables agree, 1e-15 where the anchor is converted), left anchors, a forward last region,
// region lookup and the definition checks.
#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

#include "epykos/fixtures/m1_book.hpp"
#include "epykos/maths/curve/composite.hpp"
#include "epykos/maths/curve/curve.hpp"
#include "epykos/maths/curve/linear.hpp"
#include "curve/curve_test_helpers.hpp"

namespace curve = epykos::curve;
using epykos::fixtures::knot_times;
using epykos::fixtures::n_knots;
using epykos::fixtures::record_state;

namespace {

constexpr double inf = std::numeric_limits<double>::infinity();
const double* kt() { return knot_times.data(); }
const double* z0() { return record_state.data(); }

double rel(double a, double b) { return std::fabs(a - b) / std::fmax(std::fabs(a), std::fabs(b)); }

}  // namespace

TEST(CurveVariables, ZeroIsTheM1Curve) {
  const curve::Curve<curve::Linear, curve::Variable::zero> c(kt(), n_knots);
  const auto st = c.prepare(z0());
  EXPECT_EQ(c.df(st, 0.0), 1.0);
  for (double t : {0.01, 0.5, 0.75, 2.6, 12.0, 30.0, 31.0}) {
    EXPECT_DOUBLE_EQ(c.df(st, t), curve::linear::df(kt(), z0(), n_knots, t)) << t;
    EXPECT_EQ(c.value(st, t), curve::linear::zero_rate(kt(), z0(), n_knots, t)) << t;
  }
}

TEST(CurveVariables, LogdfHasTheOriginKnotAndPiecewiseConstantForwards) {
  const curve::Curve<curve::Linear, curve::Variable::logdf> c(kt(), n_knots);
  EXPECT_TRUE(c.has_origin_knot());
  EXPECT_EQ(c.scheme().n(), n_knots + 1);
  // The state is log DF at the knots: y_k = -z_k t_k.
  std::vector<double> y(static_cast<std::size_t>(n_knots));
  for (int k = 0; k < n_knots; ++k) y[static_cast<std::size_t>(k)] = -record_state[static_cast<std::size_t>(k)] * knot_times[static_cast<std::size_t>(k)];
  const auto st = c.prepare(y.data());
  EXPECT_EQ(c.df(st, 0.0), 1.0);
  for (int k = 0; k < n_knots; ++k) EXPECT_EQ(c.log_df(st, knot_times[static_cast<std::size_t>(k)]), y[static_cast<std::size_t>(k)]) << k;
  // Linear on log DF: between knots DF(t) = DF(t_k)^(1−w) DF(t_{k+1})^w, and before the first
  // knot DF(t) = DF(t_0)^(t/t_0) (the origin knot).
  for (int k = 0; k + 1 < n_knots; ++k) {
    const double t = 0.5 * (knot_times[static_cast<std::size_t>(k)] + knot_times[static_cast<std::size_t>(k) + 1]);
    const double w = (t - knot_times[static_cast<std::size_t>(k)]) / (knot_times[static_cast<std::size_t>(k) + 1] - knot_times[static_cast<std::size_t>(k)]);
    const double expect = std::exp((1.0 - w) * y[static_cast<std::size_t>(k)] + w * y[static_cast<std::size_t>(k) + 1]);
    EXPECT_LT(rel(c.df(st, t), expect), 1e-15) << k;
  }
  const double t = 0.5 * knot_times[0];
  EXPECT_LT(rel(c.df(st, t), std::exp(y[0] * (t / knot_times[0]))), 1e-15);
  // Flat beyond the last knot: DF constant (the stated simplification).
  EXPECT_EQ(c.df(st, 40.0), c.df(st, 30.0));
  // A logdf curve whose first knot is at 0 has no origin knot.
  const double t0[3] = {0.0, 1.0, 2.0};
  const curve::Curve<curve::Linear, curve::Variable::logdf> c0(t0, 3);
  EXPECT_FALSE(c0.has_origin_knot());
}

TEST(CurveVariables, ForwardIsExpOfMinusTheIntegralInClosedForm) {
  std::vector<double> f(static_cast<std::size_t>(n_knots));
  for (int k = 0; k < n_knots; ++k) f[static_cast<std::size_t>(k)] = 0.03 + 0.0005 * k;
  const curve::Curve<curve::Flat, curve::Variable::forward> cf(kt(), n_knots);
  const curve::Curve<curve::Linear, curve::Variable::forward> cl(kt(), n_knots);
  const auto sf = cf.prepare(f.data());
  const auto sl = cl.prepare(f.data());
  EXPECT_EQ(cf.df(sf, 0.0), 1.0);
  EXPECT_EQ(cl.df(sl, 0.0), 1.0);
  // Flat: ∫_0^t f = Σ f_k · |cell_k ∩ [0, t]| (cell 0 reaches back to 0); Linear: the trapezoid
  // rule on the breakpoints is exact for a piecewise-linear integrand.
  for (double t : {0.01, 0.4, 1.5, 6.0, 12.5, 30.0, 35.0}) {
    double If = 0.0, Il = 0.0;
    double prev = 0.0;
    double fprev = f[0];  // f(0) = f_0 (flat)
    for (int k = 0; k < n_knots; ++k) {
      const double tk = knot_times[static_cast<std::size_t>(k)];
      const double hi = std::fmin(t, k + 1 < n_knots ? knot_times[static_cast<std::size_t>(k) + 1] : inf);
      // Flat cell of knot k: [t_k, t_{k+1}) (from 0 for k = 0).
      const double lo = k == 0 ? 0.0 : tk;
      if (hi > lo) If += f[static_cast<std::size_t>(k)] * (hi - lo);
      // Linear: trapezoid from prev to min(t, t_k).
      const double x = std::fmin(t, tk);
      if (x > prev) {
        const double fx = cl.value(sl, x);
        Il += 0.5 * (fprev + fx) * (x - prev);
        prev = x;
        fprev = fx;
      }
    }
    if (t > prev) Il += f[static_cast<std::size_t>(n_knots) - 1] * (t - prev);  // flat beyond the last knot
    EXPECT_LT(rel(cf.df(sf, t), std::exp(-If)), 1e-14) << t;
    EXPECT_LT(rel(cl.df(sl, t), std::exp(-Il)), 1e-14) << t;
  }
  // The forward curve's log DF is −∫f: negative forwards are fine (no sign trick).
  std::vector<double> fn(static_cast<std::size_t>(n_knots), -0.01);
  EXPECT_LT(rel(cf.df(cf.prepare(fn.data()), 2.0), std::exp(0.02)), 1e-15);
}

TEST(CurveComposite, M4CompositeIsValueContinuousAtBothBoundaries) {
  const curve::Composite c = epykos::test::m4_composite();
  std::cout << "[ regions  ]\n" << c.describe();
  ASSERT_EQ(c.n_regions(), 3);
  EXPECT_EQ(c.region(0).count, 4);
  EXPECT_EQ(c.region(1).count, 5);
  EXPECT_EQ(c.region(2).count, 3);
  EXPECT_TRUE(c.region(0).right_anchor);
  EXPECT_TRUE(c.region(1).right_anchor);
  EXPECT_FALSE(c.region(2).right_anchor);
  EXPECT_FALSE(c.region(1).left_anchor);
  EXPECT_FALSE(c.region(2).left_anchor);
  EXPECT_FALSE(c.region(0).origin);
  EXPECT_EQ(c.region(1).grid.size(), 6u);  // 1, 2, 3, 5, 7 and the 10Y anchor
  EXPECT_EQ(c.region(1).grid.back(), 10.0);

  // The state: zero rates in regions 0 and 1, log DF in region 2.
  std::vector<double> v(record_state.begin(), record_state.end());
  for (int k = 9; k < n_knots; ++k) v[static_cast<std::size_t>(k)] = -record_state[static_cast<std::size_t>(k)] * knot_times[static_cast<std::size_t>(k)];
  const auto st = c.prepare(v.data());
  EXPECT_EQ(c.df(st, 0.0), 1.0);
  EXPECT_EQ(c.region_of(0.0), 0);
  EXPECT_EQ(c.region_of(0.999), 0);
  EXPECT_EQ(c.region_of(1.0), 1);
  EXPECT_EQ(c.region_of(9.999), 1);
  EXPECT_EQ(c.region_of(10.0), 2);
  EXPECT_EQ(c.region_of(100.0), 2);
  EXPECT_EQ(c.region_of(-1.0), 0);

  // 1Y: both regions on zero, the anchor is the 1Y knot itself: bitwise.
  EXPECT_EQ(c.log_df_in_region(st, 0, 1.0), c.log_df_in_region(st, 1, 1.0));
  EXPECT_EQ(std::exp(c.log_df_in_region(st, 0, 1.0)), c.df(st, 1.0));
  // 10Y: zero on the left, logdf on the right: the anchor is y_10Y·(−1/10), two roundings away.
  const double left10 = std::exp(c.log_df_in_region(st, 1, 10.0));
  const double right10 = c.df(st, 10.0);
  EXPECT_LT(rel(left10, right10), 1e-15);
  EXPECT_EQ(right10, std::exp(v[9]));
  // The one-sided limits through the composite itself.
  for (double tb : {1.0, 10.0}) {
    const double before = c.df(st, std::nextafter(tb, 0.0));
    const double at = c.df(st, tb);
    EXPECT_LT(rel(before, at), 1e-15) << tb;
  }
  // The knots are interpolated in their own variable.
  for (int k = 0; k < 9; ++k) EXPECT_EQ(c.value_in_region(st, c.region_of(knot_times[static_cast<std::size_t>(k)]), knot_times[static_cast<std::size_t>(k)]), v[static_cast<std::size_t>(k)]) << k;
  for (int k = 9; k < n_knots; ++k) EXPECT_EQ(c.log_df(st, knot_times[static_cast<std::size_t>(k)]), v[static_cast<std::size_t>(k)]) << k;
  // Region 1 is monotone between its knots when its data is (the M1 state falls 1Y → 5Y and
  // rises 5Y → 10Y anchor; the anchor value is z_10Y).
  double prev = c.value_in_region(st, 1, 1.0);
  for (double t = 1.0; t <= 5.0; t += 0.05) {
    const double cur = c.value_in_region(st, 1, t);
    EXPECT_LE(cur, prev + 1e-15) << t;
    prev = cur;
  }
}

TEST(CurveComposite, LeftAnchorWhenARegionStartsOffAKnot) {
  // [0, 0.75) Linear zero (knots 7d..6M), [0.75, 10) NaturalCubic logdf (knots 1Y..7Y: the first
  // knot is past t_a, so the region takes a left anchor from region 0's log DF at 0.75),
  // [10, ∞) Linear zero (its first knot is the 10Y knot at t_b: region 1 takes a right anchor).
  const curve::Composite c(std::vector<double>(knot_times.begin(), knot_times.end()),
                           {{0.0, 0.75, curve::SchemeKind::linear, curve::Variable::zero},
                            {0.75, 10.0, curve::SchemeKind::natural_cubic, curve::Variable::logdf},
                            {10.0, inf, curve::SchemeKind::linear, curve::Variable::zero}});
  ASSERT_EQ(c.n_regions(), 3);
  EXPECT_FALSE(c.region(0).right_anchor);
  EXPECT_TRUE(c.region(1).left_anchor);
  EXPECT_TRUE(c.region(1).right_anchor);
  EXPECT_EQ(c.region(1).grid.size(), 7u);  // 0.75, 1, 2, 3, 5, 7, 10
  std::vector<double> v(record_state.begin(), record_state.end());
  for (int k = 4; k < 9; ++k) v[static_cast<std::size_t>(k)] = -record_state[static_cast<std::size_t>(k)] * knot_times[static_cast<std::size_t>(k)];
  const auto st = c.prepare(v.data());
  // 0.75: region 0 flat beyond its 6M knot; region 1 starts from that log DF (zero → logdf, an
  // affine conversion): 1e-15.
  EXPECT_LT(rel(std::exp(c.log_df_in_region(st, 0, 0.75)), c.df(st, 0.75)), 1e-15);
  EXPECT_LT(rel(c.df(st, std::nextafter(0.75, 0.0)), c.df(st, 0.75)), 1e-15);
  EXPECT_EQ(std::exp(c.log_df_in_region(st, 0, 0.75)), std::exp(-record_state[3] * 0.75));
  // 10: logdf → zero anchor.
  EXPECT_LT(rel(std::exp(c.log_df_in_region(st, 1, 10.0)), c.df(st, 10.0)), 1e-15);
  EXPECT_LT(rel(c.df(st, std::nextafter(10.0, 0.0)), c.df(st, 10.0)), 1e-15);
  // The region's own knots are interpolated.
  for (int k = 4; k < 9; ++k) EXPECT_EQ(c.log_df(st, knot_times[static_cast<std::size_t>(k)]), v[static_cast<std::size_t>(k)]) << k;
}

TEST(CurveComposite, ForwardLastRegionInheritsItsLevel) {
  // [0, 5) Linear zero on 7d..3Y, [5, ∞) Flat forward on 5Y..30Y.
  const curve::Composite c(std::vector<double>(knot_times.begin(), knot_times.end()),
                           {{0.0, 5.0, curve::SchemeKind::linear, curve::Variable::zero},
                            {5.0, inf, curve::SchemeKind::flat, curve::Variable::forward}});
  ASSERT_EQ(c.n_regions(), 2);
  EXPECT_FALSE(c.region(0).right_anchor);
  EXPECT_FALSE(c.region(1).left_anchor);
  std::vector<double> v(record_state.begin(), record_state.end());
  for (int k = 7; k < n_knots; ++k) v[static_cast<std::size_t>(k)] = 0.03 + 0.001 * k;  // forwards
  const auto st = c.prepare(v.data());
  // At 5: the forward region starts at region 0's log DF (flat beyond 3Y): bitwise.
  EXPECT_EQ(c.log_df(st, 5.0), c.log_df_in_region(st, 0, 5.0));
  EXPECT_EQ(c.df(st, 5.0), std::exp(-record_state[6] * 5.0));
  // Beyond: DF(t) = DF(5)·exp(−Σ f Δ).
  const double df5 = c.df(st, 5.0);
  EXPECT_LT(rel(c.df(st, 6.0), df5 * std::exp(-v[7] * 1.0)), 1e-15);
  EXPECT_LT(rel(c.df(st, 8.5), df5 * std::exp(-(v[7] * 2.0 + v[8] * 1.5))), 1e-15);
  EXPECT_LT(rel(c.df(st, 35.0), df5 * std::exp(-(v[7] * 2.0 + v[8] * 3.0 + v[9] * 10.0 + v[10] * 10.0 + v[11] * 5.0))), 1e-14);
}

TEST(CurveComposite, SingleRegionEqualsTheCurveTemplateOnDouble) {
  for (const epykos::test::SchemeVariable& sv : epykos::test::all_scheme_variables()) {
    const curve::Composite c = epykos::test::single_composite(sv.scheme, sv.variable);
    ASSERT_EQ(c.n_regions(), 1) << sv.name();
    EXPECT_EQ(c.region(0).origin, sv.variable == curve::Variable::logdf) << sv.name();
    const auto st = c.prepare(z0());
    EXPECT_EQ(c.df(st, 0.0), 1.0) << sv.name();
    for (double t : {0.01, 0.3, 2.6, 12.0, 30.0, 31.0}) {
      EXPECT_TRUE(std::isfinite(c.df(st, t))) << sv.name() << " " << t;
      EXPECT_GT(c.df(st, t), 0.0) << sv.name() << " " << t;
    }
  }
  // The template with the same scheme and variable gives the same double values.
  {
    const curve::Curve<curve::NaturalCubic, curve::Variable::logdf> tc(kt(), n_knots);
    const curve::Composite cc = epykos::test::single_composite(curve::SchemeKind::natural_cubic, curve::Variable::logdf);
    for (double t : {0.01, 0.3, 2.6, 12.0, 30.0, 31.0}) EXPECT_EQ(tc.df(tc.prepare(z0()), t), cc.df(cc.prepare(z0()), t)) << t;
  }
  {
    const curve::Curve<curve::MonotoneCubic, curve::Variable::zero> tc(kt(), n_knots);
    const curve::Composite cc = epykos::test::single_composite(curve::SchemeKind::monotone_cubic, curve::Variable::zero);
    for (double t : {0.01, 0.3, 2.6, 12.0, 30.0, 31.0}) EXPECT_EQ(tc.df(tc.prepare(z0()), t), cc.df(cc.prepare(z0()), t)) << t;
  }
}

TEST(CurveComposite, DefinitionChecks) {
  const std::vector<double> t(knot_times.begin(), knot_times.end());
  using curve::SchemeKind;
  using curve::Variable;
  // A forward region in the middle.
  EXPECT_THROW(curve::Composite(t, {{0.0, 5.0, SchemeKind::flat, Variable::forward}, {5.0, inf, SchemeKind::linear, Variable::zero}}), std::invalid_argument);
  // A forward region on a cubic scheme.
  EXPECT_THROW(curve::Composite(t, {{0.0, inf, SchemeKind::hermite, Variable::forward}}), std::invalid_argument);
  // Non-contiguous regions; a first region not at 0; t_a >= t_b.
  EXPECT_THROW(curve::Composite(t, {{0.0, 5.0, SchemeKind::linear, Variable::zero}, {6.0, inf, SchemeKind::linear, Variable::zero}}), std::invalid_argument);
  EXPECT_THROW(curve::Composite(t, {{1.0, inf, SchemeKind::linear, Variable::zero}}), std::invalid_argument);
  EXPECT_THROW(curve::Composite(t, {{0.0, 0.0, SchemeKind::linear, Variable::zero}, {0.0, inf, SchemeKind::linear, Variable::zero}}), std::invalid_argument);
  // A region owning no knot ([0.01, 0.015) holds none of the M1 knots).
  EXPECT_THROW(curve::Composite(t, {{0.0, 0.01, SchemeKind::linear, Variable::zero}, {0.01, 0.015, SchemeKind::linear, Variable::zero}, {0.015, inf, SchemeKind::linear, Variable::zero}}), std::invalid_argument);
  // Bad knots.
  EXPECT_THROW(curve::Composite(std::vector<double>{1.0, 1.0}, {{0.0, inf, SchemeKind::linear, Variable::zero}}), std::invalid_argument);
  EXPECT_THROW(curve::Composite(std::vector<double>{-1.0, 1.0}, {{0.0, inf, SchemeKind::linear, Variable::zero}}), std::invalid_argument);
  EXPECT_THROW(curve::Composite(std::vector<double>{}, {{0.0, inf, SchemeKind::linear, Variable::zero}}), std::invalid_argument);
  EXPECT_THROW(curve::Composite(t, {}), std::invalid_argument);
  // Names round-trip.
  for (int k = 0; k <= static_cast<int>(SchemeKind::bspline); ++k) EXPECT_EQ(curve::parse_scheme(curve::to_string(static_cast<SchemeKind>(k))), static_cast<SchemeKind>(k));
  for (int v = 0; v <= static_cast<int>(Variable::forward); ++v) EXPECT_EQ(curve::parse_variable(curve::to_string(static_cast<Variable>(v))), static_cast<Variable>(v));
  EXPECT_THROW(curve::parse_scheme("spline"), std::invalid_argument);
  // A finite last t_b is read as +inf.
  const curve::Composite c(t, {{0.0, 10.0, SchemeKind::linear, Variable::zero}, {10.0, 30.0, SchemeKind::linear, Variable::logdf}});
  EXPECT_EQ(c.region(1).spec.t_b, inf);
  EXPECT_EQ(c.region_of(31.0), 1);
}
