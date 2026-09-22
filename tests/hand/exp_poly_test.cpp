// M1/P5: accuracy of the vectorisable exp against libm, and its lane determinism.
#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

#include "epykos/hand/exp_poly.hpp"
#include "epykos/rng/philox.hpp"

namespace {

// |a − b| in units of ulp(b).
double ulp_diff(double a, double b) {
  if (a == b) return 0.0;
  const double u = std::nextafter(std::fabs(b), INFINITY) - std::fabs(b);
  return std::fabs(a - b) / u;
}

}  // namespace

TEST(ExpPoly, ExactAtZeroAndPowersOfTwoRange) {
  EXPECT_EQ(epykos::hand::exp_poly(0.0), 1.0);
  EXPECT_EQ(epykos::hand::exp_poly(-0.0), 1.0);
  // ln2·k lands on 2^k up to rounding of the argument itself.
  for (int k = -10; k <= 10; ++k) {
    const double x = static_cast<double>(k) * std::log(2.0);
    EXPECT_LE(ulp_diff(epykos::hand::exp_poly(x), std::exp(x)), 2.0) << k;
  }
}

// The M1 kernel's arguments are −z(t)·t in (−2, 1): a dense grid there, then random points across
// the whole valid range. Bound: 2 ulp of libm (which is itself within about 1 ulp of exact).
TEST(ExpPoly, WithinTwoUlpOfLibm) {
  double worst = 0.0, worst_x = 0.0;
  const int n = 200001;
  for (int i = 0; i < n; ++i) {
    const double x = -2.0 + 3.0 * static_cast<double>(i) / static_cast<double>(n - 1);
    const double d = ulp_diff(epykos::hand::exp_poly(x), std::exp(x));
    if (d > worst) {
      worst = d;
      worst_x = x;
    }
  }
  EXPECT_LE(worst, 2.0) << "at x = " << worst_x;
  std::cout << "exp_poly vs std::exp on [-2, 1]: max " << worst << " ulp at x = " << worst_x << "\n";

  epykos::rng::Philox g(20260922, 555000);
  double worst_wide = 0.0, worst_wide_x = 0.0;
  for (int i = 0; i < 200000; ++i) {
    const double x = g.uniform_range(-700.0, 700.0);
    const double d = ulp_diff(epykos::hand::exp_poly(x), std::exp(x));
    if (d > worst_wide) {
      worst_wide = d;
      worst_wide_x = x;
    }
  }
  EXPECT_LE(worst_wide, 2.0) << "at x = " << worst_wide_x;
  std::cout << "exp_poly vs std::exp on [-700, 700]: max " << worst_wide << " ulp at x = " << worst_wide_x << "\n";
}

// The array form (the loop the compiler vectorises) is bitwise the scalar form, lane by lane.
TEST(ExpPoly, ArrayIsScalarBitwise) {
  epykos::rng::Philox g(20260922, 555001);
  std::vector<double> x(4099), y(4099), s(4099);
  for (std::size_t i = 0; i < x.size(); ++i) x[i] = g.uniform_range(-3.0, 1.0);
  epykos::hand::exp_poly_array(x.data(), y.data(), static_cast<int>(x.size()));
  for (std::size_t i = 0; i < x.size(); ++i) s[i] = epykos::hand::exp_poly(x[i]);
  EXPECT_EQ(std::memcmp(y.data(), s.data(), x.size() * sizeof(double)), 0);
  // In place.
  std::vector<double> z = x;
  epykos::hand::exp_poly_array(z.data(), z.data(), static_cast<int>(z.size()));
  EXPECT_EQ(std::memcmp(z.data(), s.data(), x.size() * sizeof(double)), 0);
}
