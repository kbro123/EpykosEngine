// M1/P1: the linear-in-t zero curve with flat extrapolation, DF = exp(−z·t).
#include <gtest/gtest.h>

#include <cmath>

#include "epykos/fixtures/m1_book.hpp"
#include "epykos/maths/curve/linear.hpp"

using epykos::curve::linear::df;
using epykos::fixtures::knot_times;
using epykos::fixtures::n_knots;
using epykos::fixtures::record_state;
using epykos::curve::linear::zero_rate;

TEST(M1Curve, ZeroRateAtKnotsIsTheKnotValue) {
  for (int k = 0; k < n_knots; ++k) {
    EXPECT_EQ(zero_rate(knot_times.data(), record_state.data(), n_knots, knot_times[static_cast<std::size_t>(k)]),
              record_state[static_cast<std::size_t>(k)]);
  }
}

TEST(M1Curve, DiscountFactorAtKnotsIsExactlyExpOfMinusZT) {
  for (int k = 0; k < n_knots; ++k) {
    const double t = knot_times[static_cast<std::size_t>(k)];
    const double z = record_state[static_cast<std::size_t>(k)];
    EXPECT_EQ(df(knot_times.data(), record_state.data(), n_knots, t), std::exp(-z * t)) << k;
  }
}

TEST(M1Curve, InterpolationAtMidpointsIsTheAverage) {
  for (int k = 0; k + 1 < n_knots; ++k) {
    const double t0 = knot_times[static_cast<std::size_t>(k)];
    const double t1 = knot_times[static_cast<std::size_t>(k + 1)];
    const double z0 = record_state[static_cast<std::size_t>(k)];
    const double z1 = record_state[static_cast<std::size_t>(k + 1)];
    const double t = 0.5 * (t0 + t1);
    EXPECT_NEAR(zero_rate(knot_times.data(), record_state.data(), n_knots, t), 0.5 * (z0 + z1), 1e-16) << k;
  }
  // A general interior point, against the closed form.
  const double t = 2.6;  // between 2 and 3
  const double w = (t - 2.0) / (3.0 - 2.0);
  EXPECT_DOUBLE_EQ(zero_rate(knot_times.data(), record_state.data(), n_knots, t), (1.0 - w) * 0.0410 + w * 0.0400);
  // Linear in t: the second difference on a uniform triple inside one segment vanishes.
  const double a = zero_rate(knot_times.data(), record_state.data(), n_knots, 12.0);
  const double b = zero_rate(knot_times.data(), record_state.data(), n_knots, 14.0);
  const double c = zero_rate(knot_times.data(), record_state.data(), n_knots, 16.0);
  EXPECT_NEAR(a - 2.0 * b + c, 0.0, 1e-16);
}

TEST(M1Curve, FlatExtrapolation) {
  const double* kt = knot_times.data();
  const double* z = record_state.data();
  EXPECT_EQ(zero_rate(kt, z, n_knots, 0.0), record_state[0]);
  EXPECT_EQ(zero_rate(kt, z, n_knots, -1.0), record_state[0]);
  EXPECT_EQ(zero_rate(kt, z, n_knots, 0.5 * knot_times[0]), record_state[0]);
  EXPECT_EQ(zero_rate(kt, z, n_knots, 30.0), record_state[11]);
  EXPECT_EQ(zero_rate(kt, z, n_knots, 31.0), record_state[11]);
  EXPECT_EQ(zero_rate(kt, z, n_knots, 1000.0), record_state[11]);
  EXPECT_EQ(df(kt, z, n_knots, 0.0), 1.0);
  EXPECT_EQ(df(kt, z, n_knots, 40.0), std::exp(-record_state[11] * 40.0));
  EXPECT_EQ(df(kt, z, n_knots, -0.5), std::exp(record_state[0] * 0.5));
}

TEST(M1Curve, WorksOnAnyKnotCount) {
  const double kt[3] = {1.0, 2.0, 4.0};
  const double z[3] = {0.01, 0.03, 0.02};
  EXPECT_EQ(zero_rate(kt, z, 3, 0.5), 0.01);
  EXPECT_EQ(zero_rate(kt, z, 3, 1.0), 0.01);
  EXPECT_DOUBLE_EQ(zero_rate(kt, z, 3, 1.5), 0.02);
  EXPECT_EQ(zero_rate(kt, z, 3, 2.0), 0.03);
  EXPECT_DOUBLE_EQ(zero_rate(kt, z, 3, 3.0), 0.025);
  EXPECT_EQ(zero_rate(kt, z, 3, 4.0), 0.02);
  EXPECT_EQ(zero_rate(kt, z, 3, 9.0), 0.02);
}
