// M1/P1: Philox-4x32-10 known-answer vectors, stream semantics and the AS241 inverse normal CDF.
#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include "epykos/rng/philox.hpp"

using epykos::rng::inverse_normal_cdf;
using epykos::rng::Philox;
using epykos::rng::philox4x32_10;

namespace {

using Words = std::array<std::uint32_t, 4>;

// Standard normal CDF through erfc, used as an independent check of the inverse.
double normal_cdf(double x) { return 0.5 * std::erfc(-x / std::sqrt(2.0)); }

}  // namespace

// Published Philox4x32-10 test vectors (Random123 known-answer tests): counter, key -> output.
TEST(Philox, KnownAnswerVectors) {
  EXPECT_EQ(philox4x32_10({0u, 0u, 0u, 0u}, {0u, 0u}),
            (Words{0x6627e8d5u, 0xe169c58du, 0xbc57ac4cu, 0x9b00dbd8u}));
  EXPECT_EQ(philox4x32_10({0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu},
                          {0xffffffffu, 0xffffffffu}),
            (Words{0x408f276du, 0x41c83b0eu, 0xa20bc7c6u, 0x6d5451fdu}));
  EXPECT_EQ(philox4x32_10({0x243f6a88u, 0x85a308d3u, 0x13198a2eu, 0x03707344u},
                          {0xa4093822u, 0x299f31d0u}),
            (Words{0xd16cfe09u, 0x94fdccebu, 0x5001e420u, 0x24126ea1u}));
}

TEST(Philox, UniformIsInHalfOpenUnitInterval) {
  Philox g(20260922u, 7u);
  double lo = 1.0;
  double hi = 0.0;
  for (int n = 0; n < 100000; ++n) {
    const double u = g.uniform();
    ASSERT_GE(u, 0.0);
    ASSERT_LT(u, 1.0);
    lo = std::fmin(lo, u);
    hi = std::fmax(hi, u);
  }
  EXPECT_LT(lo, 1e-3);
  EXPECT_GT(hi, 1.0 - 1e-3);
  EXPECT_EQ(g.index(), 100000u);
}

TEST(Philox, UniformOpenNeverReturnsZeroOrOne) {
  Philox g(1u, 2u);
  for (int n = 0; n < 1000; ++n) {
    const double u = g.uniform_open();
    ASSERT_GT(u, 0.0);
    ASSERT_LT(u, 1.0);
  }
}

TEST(Philox, DrawsArePureFunctionsOfIndex) {
  const Philox pure(20260922u, 3u);
  Philox seq(20260922u, 3u);
  for (std::uint64_t n = 0; n < 64; ++n) {
    EXPECT_EQ(seq.uniform(), pure.uniform_at(n)) << n;
  }
  // Seeking reproduces bits regardless of what was drawn before.
  seq.seek(5);
  EXPECT_EQ(seq.uniform(), pure.uniform_at(5));
  EXPECT_EQ(seq.uniform(), pure.uniform_at(6));
  // Two draws share one block (words 0,1 and 2,3) but are different numbers.
  EXPECT_NE(pure.bits53_at(0), pure.bits53_at(1));
  EXPECT_LT(pure.bits53_at(0), std::uint64_t{1} << 53);
}

TEST(Philox, SubStreamsAndSeedsDiffer) {
  const Philox a(20260922u, 0u);
  const Philox b(20260922u, 1u);
  const Philox c(20260923u, 0u);
  for (std::uint64_t n = 0; n < 16; ++n) {
    EXPECT_NE(a.bits53_at(n), b.bits53_at(n));
    EXPECT_NE(a.bits53_at(n), c.bits53_at(n));
  }
  // A sub-stream's draws do not depend on other sub-streams having been drawn from.
  Philox x(20260922u, 100000u);
  Philox y(20260922u, 100000u);
  Philox other(20260922u, 100001u);
  for (int n = 0; n < 10; ++n) (void)other.uniform();
  for (int n = 0; n < 10; ++n) EXPECT_EQ(x.uniform(), y.uniform());
}

TEST(Philox, UniformIntCoversRangeInclusive) {
  Philox g(20260922u, 11u);
  std::vector<int> count(31, 0);
  for (int n = 0; n < 30000; ++n) {
    const std::int64_t k = g.uniform_int(1, 30);
    ASSERT_GE(k, 1);
    ASSERT_LE(k, 30);
    ++count[static_cast<std::size_t>(k)];
  }
  for (int k = 1; k <= 30; ++k) {
    EXPECT_GT(count[static_cast<std::size_t>(k)], 700) << k;  // expected 1000 each
    EXPECT_LT(count[static_cast<std::size_t>(k)], 1300) << k;
  }
  // Negative ranges and single-point ranges.
  Philox h(1u, 1u);
  for (int n = 0; n < 100; ++n) {
    const std::int64_t k = h.uniform_int(-300, -30);
    ASSERT_GE(k, -300);
    ASSERT_LE(k, -30);
    EXPECT_EQ(h.uniform_int(4, 4), 4);
  }
}

TEST(Philox, LogUniformAndRange) {
  Philox g(20260922u, 5u);
  double lo = 1e9;
  double hi = 0.0;
  for (int n = 0; n < 20000; ++n) {
    const double v = g.log_uniform(1e6, 1e8);
    ASSERT_GE(v, 1e6);
    ASSERT_LT(v, 1e8);
    lo = std::fmin(lo, v);
    hi = std::fmax(hi, v);
  }
  EXPECT_LT(lo, 1.1e6);
  EXPECT_GT(hi, 9e7);
  for (int n = 0; n < 1000; ++n) {
    const double e = g.uniform_range(-0.05, 0.05);
    ASSERT_GE(e, -0.05);
    ASSERT_LT(e, 0.05);
  }
}

TEST(AS241, KnownQuantiles) {
  EXPECT_NEAR(inverse_normal_cdf(0.975), 1.959963984540054, 1e-15);
  EXPECT_NEAR(inverse_normal_cdf(0.025), -1.959963984540054, 1e-15);
  EXPECT_NEAR(inverse_normal_cdf(0.95), 1.6448536269514722, 1e-15);
  EXPECT_NEAR(inverse_normal_cdf(0.05), -1.6448536269514722, 1e-15);
  EXPECT_NEAR(inverse_normal_cdf(0.9), 1.2815515655446004, 1e-15);
  EXPECT_NEAR(inverse_normal_cdf(0.75), 0.6744897501960817, 1e-15);
  EXPECT_NEAR(inverse_normal_cdf(0.99), 2.3263478740408408, 1e-15);
  EXPECT_NEAR(inverse_normal_cdf(0.995), 2.5758293035489004, 1e-15);
  EXPECT_NEAR(inverse_normal_cdf(0.999), 3.090232306167813, 1e-15);
  EXPECT_NEAR(inverse_normal_cdf(0.9999), 3.719016485455709, 1e-15);
  EXPECT_EQ(inverse_normal_cdf(0.5), 0.0);
}

TEST(AS241, Symmetry) {
  for (double p : {1e-300, 1e-100, 1e-20, 1e-10, 1e-3, 0.01, 0.1, 0.3, 0.45, 0.4999}) {
    const double a = inverse_normal_cdf(p);
    const double b = inverse_normal_cdf(1.0 - p);
    if (p >= 1e-3) {
      // 1 − p is exact enough here for the two branches to agree to rounding.
      EXPECT_NEAR(a, -b, 4e-16 * std::fabs(a) + 1e-15) << p;
    }
    EXPECT_LT(a, 0.0);
  }
}

// Round trip Φ(Φ⁻¹(p)) = p through erfc over the whole usable range, all three branches.
TEST(AS241, RoundTripThroughErfc) {
  std::vector<double> ps;
  for (int e = -300; e <= -1; e += 3) ps.push_back(std::pow(10.0, e));
  for (double p = 0.001; p < 0.5; p += 0.0137) ps.push_back(p);
  ps.push_back(0.5 - 1e-9);
  for (double p : ps) {
    const double x = inverse_normal_cdf(p);
    ASSERT_TRUE(std::isfinite(x)) << p;
    const double back = normal_cdf(x);
    // d log Φ(x)/dx ≈ |x| in the tail, so a relative error δ in x becomes ≈ x²·δ relative in p;
    // AS241 is good to ~1e-16 in x and erfc to a few ulp.
    const double tol = 2e-15 * (1.0 + x * x);
    EXPECT_NEAR(back / p, 1.0, tol) << "p=" << p << " x=" << x;
  }
}

TEST(AS241, EndpointsAndDomain) {
  EXPECT_EQ(inverse_normal_cdf(0.0), -std::numeric_limits<double>::infinity());
  EXPECT_EQ(inverse_normal_cdf(1.0), std::numeric_limits<double>::infinity());
  EXPECT_TRUE(std::isnan(inverse_normal_cdf(-0.1)));
  EXPECT_TRUE(std::isnan(inverse_normal_cdf(1.1)));
  EXPECT_TRUE(std::isnan(inverse_normal_cdf(std::numeric_limits<double>::quiet_NaN())));
}

TEST(Philox, GaussianMomentsAreSane) {
  Philox g(20260922u, 100001u);
  const int n = 200000;
  double s1 = 0.0;
  double s2 = 0.0;
  for (int k = 0; k < n; ++k) {
    const double x = g.gaussian();
    ASSERT_TRUE(std::isfinite(x));
    s1 += x;
    s2 += x * x;
  }
  const double mean = s1 / n;
  const double var = s2 / n - mean * mean;
  EXPECT_NEAR(mean, 0.0, 0.01);  // se ≈ 0.0022
  EXPECT_NEAR(var, 1.0, 0.02);   // se ≈ 0.0032
}
