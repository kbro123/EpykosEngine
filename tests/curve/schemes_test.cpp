// M3/G1: the interpolation schemes on double (the oracle) and Dual — the interpolation property
// per scheme, Flat's step, Linear = the M1 statements, Hermite's quadratic precision and C1,
// NaturalCubic's second derivatives against an independent solve, C2 across the knots and the
// natural end conditions, MonotoneCubic's monotonicity on monotone data, BSpline's partition of
// unity, end interpolation, linear precision at the Greville abscissae and local support.
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

#include "epykos/fixtures/m1_book.hpp"
#include "epykos/maths/curve/linear.hpp"
#include "epykos/maths/curve/scheme.hpp"
#include "epykos/scalar/dual.hpp"
#include "tape/tape_test_helpers.hpp"

namespace curve = epykos::curve;
using epykos::Dual;
using epykos::fixtures::knot_times;
using epykos::fixtures::n_knots;
using epykos::fixtures::record_state;

namespace {

const double* kt() { return knot_times.data(); }
const double* z0() { return record_state.data(); }

template <class S>
void expect_interpolates_knots(const S& s, const double* v) {
  const auto coef = s.prepare(v);
  for (int k = 0; k < s.n(); ++k) {
    EXPECT_EQ(s.value(v, coef, s.grid()[k]), v[k]) << S::name() << " knot " << k;
  }
}

// Sample points inside the grid, none on a knot.
std::vector<double> samples(const curve::Grid& g, int per_segment = 7) {
  std::vector<double> ts;
  for (int k = 0; k + 1 < g.n(); ++k) {
    for (int j = 1; j <= per_segment; ++j) ts.push_back(g[k] + g.h(k) * j / (per_segment + 1.0));
  }
  return ts;
}

}  // namespace

TEST(CurveSchemes, EverySchemeInterpolatesItsKnots) {
  expect_interpolates_knots(curve::Flat(kt(), n_knots), z0());
  expect_interpolates_knots(curve::Linear(kt(), n_knots), z0());
  expect_interpolates_knots(curve::Hermite(kt(), n_knots), z0());
  expect_interpolates_knots(curve::NaturalCubic(kt(), n_knots), z0());
  expect_interpolates_knots(curve::MonotoneCubic(kt(), n_knots), z0());
  // BSpline: the end control points only (the state is the de Boor points).
  const curve::BSpline b(kt(), n_knots);
  EXPECT_EQ(b.value(z0(), knot_times.front()), record_state.front());
  EXPECT_EQ(b.value(z0(), knot_times.back()), record_state.back());
}

TEST(CurveSchemes, FlatIsRightContinuousStepFlatOutside) {
  const curve::Flat s(kt(), n_knots);
  for (int k = 0; k + 1 < n_knots; ++k) {
    const double t = 0.5 * (knot_times[static_cast<std::size_t>(k)] + knot_times[static_cast<std::size_t>(k) + 1]);
    EXPECT_EQ(s.value(z0(), t), record_state[static_cast<std::size_t>(k)]) << k;
    EXPECT_EQ(s.value(z0(), std::nextafter(knot_times[static_cast<std::size_t>(k) + 1], 0.0)), record_state[static_cast<std::size_t>(k)]) << k;
  }
  EXPECT_EQ(s.value(z0(), 0.0), record_state.front());
  EXPECT_EQ(s.value(z0(), -1.0), record_state.front());
  EXPECT_EQ(s.value(z0(), 31.0), record_state.back());
  // ∫_0^t of the step: v_k on [t_k, t_{k+1}), v_0 before t_0 as well.
  const double I = s.integral(z0(), 0.0, 0.3);  // [0, 1/12) v0, [1/12, 0.25) v1, [0.25, 0.3) v2
  const double expect = record_state[0] * (1.0 / 12.0) + record_state[1] * (0.25 - 1.0 / 12.0) + record_state[2] * (0.3 - 0.25);
  EXPECT_NEAR(I, expect, 1e-16);
  EXPECT_EQ(s.integral(z0(), 0.3, 0.3), 0.0);
  EXPECT_EQ(s.integral(z0(), 0.3, 0.0), -I);
}

TEST(CurveSchemes, LinearIsTheM1Curve) {
  const curve::Linear s(kt(), n_knots);
  epykos::test::SplitMix64 rng(7);
  for (int i = 0; i < 2000; ++i) {
    const double t = rng.uniform(-0.5, 32.0);
    EXPECT_EQ(s.value(z0(), t), curve::linear::zero_rate(kt(), z0(), n_knots, t)) << t;
  }
  // The integral of the piecewise-linear function: a trapezium per cell.
  const double I = s.integral(z0(), 0.0, 2.5);
  double expect = record_state[0] * (7.0 / 365.0);
  for (int k = 0; k + 1 < n_knots && knot_times[static_cast<std::size_t>(k) + 1] <= 2.5; ++k) {
    expect += 0.5 * (record_state[static_cast<std::size_t>(k)] + record_state[static_cast<std::size_t>(k) + 1]) *
              (knot_times[static_cast<std::size_t>(k) + 1] - knot_times[static_cast<std::size_t>(k)]);
  }
  // [2, 2.5]: half the cell [2, 3], the linear function from z(2) to z(2.5).
  const double z25 = s.value(z0(), 2.5);
  expect += 0.5 * (record_state[5] + z25) * 0.5;
  EXPECT_NEAR(I, expect, 1e-15);
  // Linear data: the integral is exact for a linear function whatever the grid.
  std::vector<double> lin(static_cast<std::size_t>(n_knots));
  for (int k = 0; k < n_knots; ++k) lin[static_cast<std::size_t>(k)] = 0.01 + 0.002 * knot_times[static_cast<std::size_t>(k)];
  const double a = 0.1, b = 25.0;
  EXPECT_NEAR(s.integral(lin.data(), a, b), 0.01 * (b - a) + 0.001 * (b * b - a * a), 1e-14);
}

TEST(CurveSchemes, HermiteReproducesQuadraticsAndIsC1) {
  const curve::Hermite s(kt(), n_knots);
  std::vector<double> q(static_cast<std::size_t>(n_knots));
  for (int k = 0; k < n_knots; ++k) {
    const double t = knot_times[static_cast<std::size_t>(k)];
    q[static_cast<std::size_t>(k)] = 0.03 + 0.001 * t - 0.00002 * t * t;
  }
  for (double t : samples(s.grid())) {
    EXPECT_NEAR(s.value(q.data(), t), 0.03 + 0.001 * t - 0.00002 * t * t, 1e-14) << t;
  }
  // C1: one-sided derivatives at the interior knots agree (the tangent m_k is shared).
  const auto m = s.prepare(z0());
  for (int k = 1; k + 1 < n_knots; ++k) {
    const double t = knot_times[static_cast<std::size_t>(k)];
    const double h = 1e-6 * std::min(s.grid().h(k - 1), s.grid().h(k));
    const double left = (s.value(z0(), m, t) - s.value(z0(), m, t - h)) / h;
    const double right = (s.value(z0(), m, t + h) - s.value(z0(), m, t)) / h;
    EXPECT_NEAR(left, right, 1e-5 * std::max(1.0, std::fabs(left))) << k;
    EXPECT_NEAR(left, m[static_cast<std::size_t>(k)], 1e-5 * std::max(1.0, std::fabs(left))) << k;
  }
  // Two knots: the line; one knot: the constant.
  const double t2[2] = {1.0, 3.0}, v2[2] = {0.02, 0.04};
  EXPECT_DOUBLE_EQ(curve::Hermite(t2, 2).value(v2, 2.0), 0.03);
  const double t1[1] = {1.0}, v1[1] = {0.02};
  EXPECT_EQ(curve::Hermite(t1, 1).value(v1, 5.0), 0.02);
}

namespace {

// Dense Gaussian elimination for the natural spline's interior second derivatives.
std::vector<double> natural_second_derivatives(const double* t, const double* v, int n) {
  const int m = n - 2;
  std::vector<double> A(static_cast<std::size_t>(m * m), 0.0), r(static_cast<std::size_t>(m), 0.0);
  auto h = [&](int k) { return t[k + 1] - t[k]; };
  auto d = [&](int k) { return (v[k + 1] - v[k]) / h(k); };
  for (int i = 1; i <= m; ++i) {
    const int row = i - 1;
    if (i > 1) A[static_cast<std::size_t>(row * m + (row - 1))] = h(i - 1) / 6.0;
    A[static_cast<std::size_t>(row * m + row)] = (h(i - 1) + h(i)) / 3.0;
    if (i < m) A[static_cast<std::size_t>(row * m + (row + 1))] = h(i) / 6.0;
    r[static_cast<std::size_t>(row)] = d(i) - d(i - 1);
  }
  for (int c = 0; c < m; ++c) {
    for (int rr = c + 1; rr < m; ++rr) {
      const double f = A[static_cast<std::size_t>(rr * m + c)] / A[static_cast<std::size_t>(c * m + c)];
      for (int cc = c; cc < m; ++cc) A[static_cast<std::size_t>(rr * m + cc)] -= f * A[static_cast<std::size_t>(c * m + cc)];
      r[static_cast<std::size_t>(rr)] -= f * r[static_cast<std::size_t>(c)];
    }
  }
  std::vector<double> M(static_cast<std::size_t>(m), 0.0);
  for (int rr = m - 1; rr >= 0; --rr) {
    double s = r[static_cast<std::size_t>(rr)];
    for (int cc = rr + 1; cc < m; ++cc) s -= A[static_cast<std::size_t>(rr * m + cc)] * M[static_cast<std::size_t>(cc)];
    M[static_cast<std::size_t>(rr)] = s / A[static_cast<std::size_t>(rr * m + rr)];
  }
  return M;
}

// The cubic through four samples of f on [lo, hi] in the local coordinate x = (t − lo)/(hi − lo):
// coefficients c0..c3; the second derivative in t at x is (2 c2 + 6 c3 x)/(hi − lo)².
struct Cubic {
  double c[4];
  double second(double x, double width) const { return (2.0 * c[2] + 6.0 * c[3] * x) / (width * width); }
};
template <class F>
Cubic fit_cubic(F&& f, double lo, double hi) {
  const double xs[4] = {0.1, 0.4, 0.6, 0.9};
  double A[4][5];
  for (int i = 0; i < 4; ++i) {
    double p = 1.0;
    for (int j = 0; j < 4; ++j) {
      A[i][j] = p;
      p *= xs[i];
    }
    A[i][4] = f(lo + xs[i] * (hi - lo));
  }
  for (int c = 0; c < 4; ++c) {
    for (int r = c + 1; r < 4; ++r) {
      const double fct = A[r][c] / A[c][c];
      for (int cc = c; cc < 5; ++cc) A[r][cc] -= fct * A[c][cc];
    }
  }
  Cubic q;
  for (int r = 3; r >= 0; --r) {
    double s = A[r][4];
    for (int cc = r + 1; cc < 4; ++cc) s -= A[r][cc] * q.c[cc];
    q.c[r] = s / A[r][r];
  }
  return q;
}

}  // namespace

TEST(CurveSchemes, NaturalCubicSolvesTheTridiagonalSystemIsC2WithNaturalEnds) {
  const curve::NaturalCubic s(kt(), n_knots);
  const std::vector<double> M = s.prepare(z0());
  const std::vector<double> Mref = natural_second_derivatives(kt(), z0(), n_knots);
  ASSERT_EQ(M.size(), static_cast<std::size_t>(n_knots - 2));
  double mscale = 0.0;
  for (double x : Mref) mscale = std::max(mscale, std::fabs(x));
  for (std::size_t i = 0; i < M.size(); ++i) EXPECT_NEAR(M[i], Mref[i], 1e-12 * mscale) << i;

  // C2: the cubic pieces on neighbouring segments have the same second derivative at the knot
  // (the second derivatives at a knot from both sides agree) and it is M_k there.
  auto f = [&](double t) { return s.value(z0(), M, t); };
  for (int k = 1; k + 1 < n_knots; ++k) {
    const double lo = knot_times[static_cast<std::size_t>(k) - 1], mid = knot_times[static_cast<std::size_t>(k)],
                 hi = knot_times[static_cast<std::size_t>(k) + 1];
    const Cubic left = fit_cubic(f, lo, mid), right = fit_cubic(f, mid, hi);
    const double sl = left.second(1.0, mid - lo), sr = right.second(0.0, hi - mid);
    EXPECT_NEAR(sl, sr, 1e-8 * mscale) << "second derivative jumps at knot " << k;
    EXPECT_NEAR(sl, M[static_cast<std::size_t>(k) - 1], 1e-8 * mscale) << k;
  }
  // Natural ends: v'' = 0 at t_0 and t_{n−1}.
  {
    const Cubic first = fit_cubic(f, knot_times[0], knot_times[1]);
    const Cubic last = fit_cubic(f, knot_times[static_cast<std::size_t>(n_knots) - 2], knot_times[static_cast<std::size_t>(n_knots) - 1]);
    EXPECT_NEAR(first.second(0.0, knot_times[1] - knot_times[0]), 0.0, 1e-8 * mscale);
    EXPECT_NEAR(last.second(1.0, knot_times[static_cast<std::size_t>(n_knots) - 1] - knot_times[static_cast<std::size_t>(n_knots) - 2]), 0.0, 1e-8 * mscale);
  }
  // Linear data: M = 0 and the spline is the line.
  std::vector<double> lin(static_cast<std::size_t>(n_knots));
  for (int k = 0; k < n_knots; ++k) lin[static_cast<std::size_t>(k)] = 0.01 + 0.002 * knot_times[static_cast<std::size_t>(k)];
  for (double x : s.prepare(lin.data())) EXPECT_NEAR(x, 0.0, 1e-14);
  for (double t : samples(s.grid())) EXPECT_NEAR(s.value(lin.data(), t), 0.01 + 0.002 * t, 1e-15) << t;
  // Small grids: three knots (one unknown), two (the line), one (the constant).
  const double t3[3] = {0.0, 1.0, 3.0}, v3[3] = {0.0, 1.0, 0.0};
  const curve::NaturalCubic s3(t3, 3);
  EXPECT_NEAR(s3.prepare(v3)[0], natural_second_derivatives(t3, v3, 3)[0], 1e-14);
  const double t2[2] = {1.0, 3.0}, v2[2] = {0.02, 0.04};
  EXPECT_DOUBLE_EQ(curve::NaturalCubic(t2, 2).value(v2, 2.0), 0.03);
}

TEST(CurveSchemes, MonotoneCubicPreservesMonotonicity) {
  const curve::MonotoneCubic s(kt(), n_knots);
  epykos::test::SplitMix64 rng(11);
  const std::vector<double> ts = samples(s.grid(), 40);
  for (int trial = 0; trial < 200; ++trial) {
    std::vector<double> v(static_cast<std::size_t>(n_knots));
    double acc = rng.uniform(0.0, 0.05);
    const bool increasing = (trial % 2) == 0;
    for (int k = 0; k < n_knots; ++k) {
      // Steps of random size, some zero (a flat stretch).
      const double step = (rng.next() % 4 == 0) ? 0.0 : rng.uniform(0.0, 0.01);
      acc += increasing ? step : -step;
      v[static_cast<std::size_t>(k)] = acc;
    }
    const std::vector<double> m = s.prepare(v.data());
    double prev = s.value(v.data(), m, ts.front());
    for (std::size_t i = 1; i < ts.size(); ++i) {
      const double cur = s.value(v.data(), m, ts[i]);
      if (increasing) {
        EXPECT_GE(cur, prev - 1e-15) << "trial " << trial << " at t = " << ts[i];
      } else {
        EXPECT_LE(cur, prev + 1e-15) << "trial " << trial << " at t = " << ts[i];
      }
      prev = cur;
    }
    // Between two equal knots the interpolant is constant (both tangents are 0).
    for (int k = 0; k + 1 < n_knots; ++k) {
      if (v[static_cast<std::size_t>(k)] != v[static_cast<std::size_t>(k) + 1]) continue;
      for (int j = 1; j < 8; ++j) {
        const double t = knot_times[static_cast<std::size_t>(k)] + s.grid().h(k) * j / 8.0;
        EXPECT_NEAR(s.value(v.data(), m, t), v[static_cast<std::size_t>(k)], 1e-15) << trial << " " << k;
      }
    }
  }
  // Non-monotone data: the tangent at a local extremum is exactly 0.
  const double v[12] = {0.01, 0.02, 0.03, 0.02, 0.01, 0.02, 0.03, 0.04, 0.03, 0.02, 0.01, 0.0};
  const std::vector<double> m = s.prepare(v);
  EXPECT_EQ(m[2], 0.0);
  EXPECT_EQ(m[4], 0.0);
  EXPECT_EQ(m[7], 0.0);
  // The Hyman bound holds at every interior knot: |m_k| <= 3 min(|d_{k-1}|, |d_k|).
  for (int k = 1; k + 1 < n_knots; ++k) {
    const double dl = (v[k] - v[k - 1]) / s.grid().h(k - 1), dr = (v[k + 1] - v[k]) / s.grid().h(k);
    EXPECT_LE(std::fabs(m[static_cast<std::size_t>(k)]), 3.0 * std::min(std::fabs(dl), std::fabs(dr)) + 1e-18) << k;
  }
}

TEST(CurveSchemes, BSplinePartitionOfUnityEndsGrevilleLinearPrecisionLocalSupport) {
  const curve::BSpline s(kt(), n_knots);
  EXPECT_EQ(s.degree(), 3);
  EXPECT_EQ(s.knot_vector().size(), static_cast<std::size_t>(n_knots + 4));
  std::vector<double> ones(static_cast<std::size_t>(n_knots), 1.0);
  for (double t : samples(s.grid(), 15)) EXPECT_NEAR(s.value(ones.data(), t), 1.0, 1e-15) << t;
  EXPECT_EQ(s.value(z0(), knot_times.front()), record_state.front());
  EXPECT_EQ(s.value(z0(), knot_times.back()), record_state.back());
  EXPECT_EQ(s.value(z0(), 0.0), record_state.front());
  EXPECT_EQ(s.value(z0(), 40.0), record_state.back());
  // Linear precision: control points a + b·ξ_j reproduce a + b·t.
  std::vector<double> lin(static_cast<std::size_t>(n_knots));
  for (int j = 0; j < n_knots; ++j) lin[static_cast<std::size_t>(j)] = 0.01 + 0.002 * s.greville(j);
  for (double t : samples(s.grid(), 15)) EXPECT_NEAR(s.value(lin.data(), t), 0.01 + 0.002 * t, 1e-15) << t;
  // Local support: at most four control points carry a tangent (Dual<12>).
  using D = Dual<n_knots>;
  std::vector<D> vd(static_cast<std::size_t>(n_knots));
  for (int k = 0; k < n_knots; ++k) vd[static_cast<std::size_t>(k)] = D::variable(record_state[static_cast<std::size_t>(k)], k);
  for (double t : samples(s.grid(), 5)) {
    const D y = s.value(vd.data(), t);
    int nonzero = 0;
    double sum = 0.0;
    for (int k = 0; k < n_knots; ++k) {
      nonzero += (y.d[static_cast<std::size_t>(k)] != 0.0);
      sum += y.d[static_cast<std::size_t>(k)];
      EXPECT_GE(y.d[static_cast<std::size_t>(k)], 0.0) << "basis functions are non-negative";
    }
    EXPECT_LE(nonzero, 4) << t;
    EXPECT_NEAR(sum, 1.0, 1e-15) << t;
  }
  // Small grids: degree n − 1.
  const double t2[2] = {1.0, 3.0}, v2[2] = {0.02, 0.04};
  const curve::BSpline s2(t2, 2);
  EXPECT_EQ(s2.degree(), 1);
  EXPECT_DOUBLE_EQ(s2.value(v2, 2.0), 0.03);
  const double t3[3] = {0.0, 1.0, 3.0}, v3[3] = {0.0, 1.0, 0.0};
  const curve::BSpline s3(t3, 3);
  EXPECT_EQ(s3.degree(), 2);
  EXPECT_NEAR(s3.value(v3, 1.5), 0.5, 1e-15);  // the quadratic Bézier at its middle: (0 + 2·1 + 0)/4
}

TEST(CurveSchemes, DualValueChannelIsTheDoubleValue) {
  using D = Dual<1>;
  std::vector<D> vd(static_cast<std::size_t>(n_knots));
  for (int k = 0; k < n_knots; ++k) vd[static_cast<std::size_t>(k)] = D::variable(record_state[static_cast<std::size_t>(k)], 0, 0.0);
  vd[5] = D::variable(record_state[5], 0, 1.0);
  const curve::Flat f(kt(), n_knots);
  const curve::Linear l(kt(), n_knots);
  const curve::Hermite h(kt(), n_knots);
  const curve::NaturalCubic c(kt(), n_knots);
  const curve::MonotoneCubic mc(kt(), n_knots);
  const curve::BSpline b(kt(), n_knots);
  for (double t : samples(l.grid(), 3)) {
    EXPECT_DOUBLE_EQ(f.value(vd.data(), t).v, f.value(z0(), t));
    EXPECT_DOUBLE_EQ(l.value(vd.data(), t).v, l.value(z0(), t));
    EXPECT_DOUBLE_EQ(h.value(vd.data(), t).v, h.value(z0(), t));
    EXPECT_DOUBLE_EQ(c.value(vd.data(), t).v, c.value(z0(), t));
    EXPECT_DOUBLE_EQ(mc.value(vd.data(), t).v, mc.value(z0(), t));
    EXPECT_DOUBLE_EQ(b.value(vd.data(), t).v, b.value(z0(), t));
  }
  // The tangent along knot 5 equals a central difference — at a generic state: the record point
  // sits on a Hyman tie (|d_4| == |d_5| exactly), a genuine kink of the monotone limiter where a
  // central difference is not a derivative.
  std::vector<double> zg(record_state.begin(), record_state.end());
  for (int k = 0; k < n_knots; ++k) zg[static_cast<std::size_t>(k)] += 1e-4 * std::sin(1.7 * k + 0.3);
  for (int k = 0; k < n_knots; ++k) vd[static_cast<std::size_t>(k)] = D::variable(zg[static_cast<std::size_t>(k)], 0, k == 5 ? 1.0 : 0.0);
  for (double t : samples(l.grid(), 3)) {
    const double hh = 1e-6;
    std::vector<double> up = zg, dn = zg;
    up[5] += hh;
    dn[5] -= hh;
    EXPECT_NEAR(c.value(vd.data(), t).d[0], (c.value(up.data(), t) - c.value(dn.data(), t)) / (2 * hh), 1e-6) << t;
    EXPECT_NEAR(mc.value(vd.data(), t).d[0], (mc.value(up.data(), t) - mc.value(dn.data(), t)) / (2 * hh), 1e-6) << t;
    EXPECT_NEAR(h.value(vd.data(), t).d[0], (h.value(up.data(), t) - h.value(dn.data(), t)) / (2 * hh), 1e-6) << t;
  }
}

TEST(CurveSchemes, GridValidation) {
  const double bad[3] = {1.0, 1.0, 2.0};
  EXPECT_THROW(curve::Linear(bad, 3), std::invalid_argument);
  const double dec[2] = {2.0, 1.0};
  EXPECT_THROW(curve::Flat(dec, 2), std::invalid_argument);
  EXPECT_THROW(curve::Hermite(dec, 0), std::invalid_argument);
}
