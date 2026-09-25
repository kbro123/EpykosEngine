// M2/Q1: unit tests of the differential tester itself (verify/differential.hpp) on small
// synthetic functions: the ulp arithmetic, the state ball's draws, the batched calling
// convention, and that a harness meant to catch a wrong bit does catch one (a 1-ulp change is a
// mismatch and E0 failure, within E1 at 4 ulps; a 5-ulp change fails E1; NaN is a violation) and
// names the output, the draw and the state it happened at.
#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <set>
#include <string>
#include <vector>

#include "epykos/rng/philox.hpp"
#include "epykos/verify/differential.hpp"

namespace verify = epykos::verify;
using verify::BallOptions;
using verify::BatchFn;
using verify::DifferentialOptions;
using verify::Exactness;
using verify::Report;
using verify::ScalarFn;
using verify::StateBall;
using verify::Tolerance;

namespace {

constexpr double kInf = std::numeric_limits<double>::infinity();
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kEps = std::numeric_limits<double>::epsilon();  // 2^-52 = ulp(1)

std::uint64_t bits(double v) {
  std::uint64_t u;
  std::memcpy(&u, &v, sizeof u);
  return u;
}

double step(double x, int n) {  // x moved n ulps up (n < 0: down)
  for (int i = 0; i < n; ++i) x = std::nextafter(x, kInf);
  for (int i = 0; i > n; --i) x = std::nextafter(x, -kInf);
  return x;
}

// The synthetic program: 3 inputs, 2 outputs: y0 = z0 + z1 + z2, y1 = z0 * z1 − z2.
constexpr int kIn = 3;
constexpr int kOut = 2;
void synthetic(const double* z, double* out) {
  out[0] = (z[0] + z[1]) + z[2];
  out[1] = z[0] * z[1] - z[2];
}

StateBall ball(int draws = 10) {
  const double centre[kIn] = {1.0, -2.5, 0.25};
  BallOptions o;
  o.rho = 0.1;
  o.draws = draws;
  return verify::make_state_ball(centre, kIn, o);
}

const ScalarFn kReference = synthetic;

}  // namespace

// ---- ulp arithmetic

TEST(Ulp, SpacingOfDoubles) {
  EXPECT_EQ(verify::ulp(1.0), kEps);
  EXPECT_EQ(verify::ulp(-1.0), kEps);
  EXPECT_EQ(verify::ulp(2.0), 2 * kEps);
  EXPECT_EQ(verify::ulp(0.5), 0.5 * kEps);
  EXPECT_EQ(verify::ulp(1.5), kEps);  // the spacing in [1, 2) is ulp(1)
  EXPECT_EQ(verify::ulp(0.0), std::numeric_limits<double>::denorm_min());
  EXPECT_EQ(verify::ulp(kInf), kInf);
  EXPECT_EQ(verify::ulp(kNaN), kInf);
  const double mx = std::numeric_limits<double>::max();
  EXPECT_EQ(verify::ulp(mx), mx - std::nextafter(mx, 0.0));
}

TEST(Ulp, DistanceCountsSteps) {
  EXPECT_EQ(verify::ulp_distance(1.0, 1.0), 0.0);
  EXPECT_EQ(verify::ulp_distance(1.0, step(1.0, 1)), 1.0);
  EXPECT_EQ(verify::ulp_distance(1.0, step(1.0, 4)), 4.0);
  EXPECT_EQ(verify::ulp_distance(step(1.0, 4), 1.0), 4.0);
  EXPECT_EQ(verify::ulp_distance(1.0, step(1.0, -1)), 0.5);  // the spacing below 1 is half: measured at max(|a|, |b|) = 1
  EXPECT_EQ(verify::ulp_distance(0.0, -0.0), 0.0);
  EXPECT_EQ(verify::ulp_distance(kInf, kInf), 0.0);
  EXPECT_EQ(verify::ulp_distance(kInf, -kInf), kInf);
  EXPECT_EQ(verify::ulp_distance(1.0, kNaN), kInf);
  EXPECT_EQ(verify::ulp_distance(kNaN, kNaN), 0.0);   // the same bits: bitwise equal, whatever they are
  EXPECT_EQ(verify::ulp_distance(kNaN, -kNaN), kInf);  // a different NaN is a mismatch of infinite size
  EXPECT_TRUE(verify::within_ulps(1.0, step(1.0, 4), 4.0));
  EXPECT_FALSE(verify::within_ulps(1.0, step(1.0, 5), 4.0));
}

TEST(Ulp, ScaleMovesTheUnit) {
  // Two values that differ by ~1e-16: thousands of ulps of 1e-4 (ulp(1e-4) ~ 1.4e-20), a
  // fraction of an ulp of 1.
  const double a = 1e-4;
  const double b = 1e-4 + 1e-16;
  EXPECT_GT(verify::ulp_distance(a, b), 1e3);
  EXPECT_LT(verify::ulp_distance(a, b, 1.0), 1.0);
  EXPECT_EQ(verify::ulp_distance(a, b, 1.0), std::fabs(a - b) / kEps);
  // A scale below the values changes nothing.
  EXPECT_EQ(verify::ulp_distance(a, b, 1e-9), verify::ulp_distance(a, b));
  // The scale's sign is ignored.
  EXPECT_EQ(verify::ulp_distance(a, b, -1.0), verify::ulp_distance(a, b, 1.0));
}

// ---- the state ball

TEST(StateBall, DrawsLieInTheBallAndAreDistinct) {
  const StateBall bl = ball(50);
  ASSERT_EQ(bl.n_inputs, kIn);
  ASSERT_EQ(bl.n_draws, 50);
  ASSERT_EQ(bl.states.size(), 150u);
  ASSERT_EQ(bl.centre.size(), 3u);
  std::set<std::vector<double>> seen;
  for (int r = 0; r < bl.n_draws; ++r) {
    const double* z = bl.state(r);
    for (int k = 0; k < kIn; ++k) {
      EXPECT_LE(std::fabs(z[k] - bl.centre[static_cast<std::size_t>(k)]), 0.1) << "draw " << r << " knot " << k;
    }
    seen.insert(std::vector<double>(z, z + kIn));
  }
  EXPECT_EQ(seen.size(), 50u);
}

TEST(StateBall, DrawIsAFunctionOfSeedSubstreamAndCoordinate) {
  // Draw r does not depend on how many draws were asked for (counter-based streams, D17), and
  // is the documented formula: centre + rho·(−1 + 2·U), U = Philox(seed, base + r) draw k.
  const StateBall a = ball(10);
  const StateBall b = ball(40);
  for (int r = 0; r < 10; ++r) {
    for (int k = 0; k < kIn; ++k) EXPECT_EQ(bits(a.state(r)[k]), bits(b.state(r)[k])) << r << ' ' << k;
  }
  for (int r = 0; r < 10; ++r) {
    epykos::rng::Philox g(a.options.seed, a.options.substream_base + static_cast<std::uint64_t>(r));
    for (int k = 0; k < kIn; ++k) {
      const double u = -1.0 + 2.0 * g.uniform();
      const double expect = a.centre[static_cast<std::size_t>(k)] + a.options.rho * u;
      // Same formula, but this TU may contract the multiply-add: compare to the ulp.
      EXPECT_LE(verify::ulp_distance(a.state(r)[k], expect), 1.0) << r << ' ' << k;
    }
  }
  // Another seed or base gives other draws.
  BallOptions o;
  o.rho = 0.1;
  o.draws = 10;
  o.seed = 7;
  const double centre[kIn] = {1.0, -2.5, 0.25};
  const StateBall c = verify::make_state_ball(centre, kIn, o);
  EXPECT_NE(bits(c.state(0)[0]), bits(a.state(0)[0]));
  o.seed = a.options.seed;
  o.substream_base = 1;
  const StateBall d = verify::make_state_ball(centre, kIn, o);
  EXPECT_NE(bits(d.state(0)[0]), bits(a.state(0)[0]));
}

TEST(StateBall, SoaTransposesAChunk) {
  const StateBall bl = ball(10);
  std::vector<double> soa(3 * 4);
  bl.soa(6, 4, soa.data());
  for (int k = 0; k < kIn; ++k) {
    for (int b = 0; b < 4; ++b) EXPECT_EQ(bits(soa[static_cast<std::size_t>(k * 4 + b)]), bits(bl.state(6 + b)[k]));
  }
}

TEST(StateBall, RejectsBadArguments) {
  const double centre[kIn] = {0.0, 0.0, 0.0};
  BallOptions o;
  o.draws = 0;
  EXPECT_THROW(verify::make_state_ball(centre, kIn, o), std::invalid_argument);
  o.draws = 1;
  EXPECT_THROW(verify::make_state_ball(centre, 0, o), std::invalid_argument);
  EXPECT_THROW(verify::make_state_ball(nullptr, kIn, o), std::invalid_argument);
}

// ---- the comparison

TEST(Differential, IdenticalSidesAreBitwiseEqualAndPassBothClasses) {
  const StateBall bl = ball(10);
  const BatchFn compiled = verify::batch_of(kReference, kIn, kOut);
  for (Tolerance tol : {Tolerance::e0(), Tolerance::e1(4.0), Tolerance::e1(0.0)}) {
    DifferentialOptions o;
    o.tolerance = tol;
    const Report rep = verify::differential(bl, kOut, kReference, compiled, o);
    EXPECT_TRUE(rep.passed) << rep.summary();
    EXPECT_TRUE(rep.bitwise_equal);
    EXPECT_EQ(rep.mismatches, 0u);
    EXPECT_EQ(rep.violations, 0u);
    EXPECT_EQ(rep.worst_output, -1);
    EXPECT_EQ(rep.worst_draw, -1);
    EXPECT_TRUE(rep.worst_state.empty());
    EXPECT_EQ(rep.n_draws, 10);
    EXPECT_EQ(rep.n_outputs, kOut);
    EXPECT_EQ(rep.n_inputs, kIn);
    ASSERT_EQ(rep.outputs.size(), 2u);
    EXPECT_TRUE(rep.outputs[0].bitwise());
    EXPECT_TRUE(rep.outputs[1].bitwise());
    EXPECT_NE(rep.summary().find("bitwise equal"), std::string::npos);
    EXPECT_NE(rep.summary().find("PASS"), std::string::npos);
  }
}

TEST(Differential, OneUlpEverywhereFailsE0AndPassesE1) {
  const StateBall bl = ball(10);
  // Output 1 one ulp up at every draw.
  const BatchFn compiled = verify::batch_of(
      [](const double* z, double* out) {
        synthetic(z, out);
        out[1] = std::nextafter(out[1], kInf);
      },
      kIn, kOut);
  DifferentialOptions o;
  o.tolerance = Tolerance::e0();
  Report rep = verify::differential(bl, kOut, kReference, compiled, o);
  EXPECT_FALSE(rep.passed) << rep.summary();
  EXPECT_FALSE(rep.bitwise_equal);
  EXPECT_EQ(rep.mismatches, 10u);
  EXPECT_EQ(rep.violations, 10u);
  EXPECT_TRUE(rep.outputs[0].bitwise());
  EXPECT_EQ(rep.outputs[1].mismatches, 10);
  EXPECT_EQ(rep.outputs[1].violations, 10);
  EXPECT_EQ(rep.worst_output, 1);
  EXPECT_LE(rep.max_ulps, 1.0);
  EXPECT_GT(rep.max_ulps, 0.0);
  EXPECT_GT(rep.max_rel, 0.0);
  EXPECT_LT(rep.max_rel, 4 * kEps);
  EXPECT_NE(rep.summary().find("FAIL"), std::string::npos);
  ASSERT_EQ(rep.worst_state.size(), 3u);
  for (int k = 0; k < kIn; ++k) EXPECT_EQ(bits(rep.worst_state[static_cast<std::size_t>(k)]), bits(bl.state(rep.worst_draw)[k]));

  o.tolerance = Tolerance::e1(4.0);
  rep = verify::differential(bl, kOut, kReference, compiled, o);
  EXPECT_TRUE(rep.passed) << rep.summary();
  EXPECT_FALSE(rep.bitwise_equal);
  EXPECT_EQ(rep.mismatches, 10u);
  EXPECT_EQ(rep.violations, 0u);
  EXPECT_NE(rep.summary().find("PASS"), std::string::npos);

  o.tolerance = Tolerance::e1(0.5);
  rep = verify::differential(bl, kOut, kReference, compiled, o);
  EXPECT_FALSE(rep.passed) << rep.summary();
}

TEST(Differential, AFiveUlpErrorAtOneDrawIsLocated) {
  const StateBall bl = ball(10);
  const double* target = bl.state(3);
  const BatchFn compiled = verify::batch_of(
      [target](const double* z, double* out) {
        synthetic(z, out);
        if (bits(z[0]) == bits(target[0]) && bits(z[1]) == bits(target[1]) && bits(z[2]) == bits(target[2])) {
          out[0] = step(out[0], 5);
        }
      },
      kIn, kOut);
  DifferentialOptions o;
  o.tolerance = Tolerance::e1(4.0);
  const Report rep = verify::differential(bl, kOut, kReference, compiled, o);
  EXPECT_FALSE(rep.passed) << rep.summary();
  EXPECT_EQ(rep.mismatches, 1u);
  EXPECT_EQ(rep.violations, 1u);
  EXPECT_EQ(rep.worst_output, 0);
  EXPECT_EQ(rep.worst_draw, 3);
  EXPECT_EQ(rep.max_rel_output, 0);
  EXPECT_EQ(rep.max_rel_draw, 3);
  EXPECT_EQ(rep.outputs[0].ulps_draw, 3);
  EXPECT_EQ(rep.outputs[0].rel_draw, 3);
  EXPECT_EQ(rep.outputs[0].mismatches, 1);
  EXPECT_GE(rep.max_ulps, 4.0);  // 5 steps up is 5 ulps of the upper value, at least 2.5 of the lower
  EXPECT_LE(rep.max_ulps, 5.0);
  ASSERT_EQ(rep.worst_state.size(), 3u);
  for (int k = 0; k < kIn; ++k) EXPECT_EQ(bits(rep.worst_state[static_cast<std::size_t>(k)]), bits(target[k]));
  double expect[kOut];
  synthetic(target, expect);
  EXPECT_EQ(bits(rep.outputs[0].ref_at_worst), bits(expect[0]));
  EXPECT_EQ(bits(rep.outputs[0].cmp_at_worst), bits(step(expect[0], 5)));
  // At 5 ulps it passes, at the one-sided tolerance the scale can widen it.
  o.tolerance = Tolerance::e1(5.0);
  EXPECT_TRUE(verify::differential(bl, kOut, kReference, compiled, o).passed);
  o.tolerance = Tolerance::e1(4.0);
  o.scale = [](const double*, double* scale) {
    scale[0] = 1e6;  // 5 ulps of a value near 1 is a tiny fraction of an ulp of 1e6
    scale[1] = 0.0;
  };
  const Report scaled = verify::differential(bl, kOut, kReference, compiled, o);
  EXPECT_TRUE(scaled.passed) << scaled.summary();
  EXPECT_TRUE(scaled.scaled);
  EXPECT_LT(scaled.max_ulps, 1e-3);
  EXPECT_NE(scaled.summary().find("scaled"), std::string::npos);
}

TEST(Differential, NaNAndInfinityAreViolations) {
  const StateBall bl = ball(4);
  const BatchFn compiled = verify::batch_of(
      [](const double* z, double* out) {
        synthetic(z, out);
        out[0] = kNaN;
      },
      kIn, kOut);
  DifferentialOptions o;
  o.tolerance = Tolerance::e1(1e300);
  const Report rep = verify::differential(bl, kOut, kReference, compiled, o);
  EXPECT_FALSE(rep.passed) << rep.summary();
  EXPECT_EQ(rep.violations, 4u);
  EXPECT_EQ(rep.max_ulps, kInf);
  EXPECT_EQ(rep.max_rel, kInf);
  EXPECT_EQ(rep.max_abs, kInf);
  EXPECT_EQ(rep.worst_output, 0);
}

TEST(Differential, BatchedCallsReceiveSoaChunksOfTheRequestedWidth) {
  const StateBall bl = ball(10);
  std::vector<int> widths;
  const BatchFn compiled = [&widths, &bl](const double* state, int B, double* out) {
    widths.push_back(B);
    // The chunk is consecutive draws, transposed: find which by matching the first lane.
    int first = -1;
    for (int r = 0; r < bl.n_draws; ++r) {
      if (bits(state[0]) == bits(bl.state(r)[0]) && bits(state[static_cast<std::size_t>(B)]) == bits(bl.state(r)[1])) first = r;
    }
    ASSERT_GE(first, 0);
    for (int b = 0; b < B; ++b) {
      double z[kIn];
      for (int k = 0; k < kIn; ++k) {
        z[k] = state[static_cast<std::size_t>(k * B + b)];
        EXPECT_EQ(bits(z[k]), bits(bl.state(first + b)[k]));
      }
      double o[kOut];
      synthetic(z, o);
      for (int i = 0; i < kOut; ++i) out[static_cast<std::size_t>(i * B + b)] = o[i];
    }
  };
  DifferentialOptions o;
  o.batch = 4;
  const Report rep = verify::differential(bl, kOut, kReference, compiled, o);
  EXPECT_TRUE(rep.passed) << rep.summary();
  EXPECT_EQ(rep.batch, 4);
  ASSERT_EQ(widths.size(), 3u);
  EXPECT_EQ(widths[0], 4);
  EXPECT_EQ(widths[1], 4);
  EXPECT_EQ(widths[2], 2);
  // A batch wider than the ball is one call of the ball's width.
  widths.clear();
  o.batch = 64;
  EXPECT_TRUE(verify::differential(bl, kOut, kReference, compiled, o).passed);
  ASSERT_EQ(widths.size(), 1u);
  EXPECT_EQ(widths[0], 10);
}

TEST(Differential, ALaneErrorInABatchIsAttributedToItsDraw) {
  const StateBall bl = ball(10);
  const BatchFn compiled = [](const double* state, int B, double* out) {
    for (int b = 0; b < B; ++b) {
      double z[kIn];
      for (int k = 0; k < kIn; ++k) z[k] = state[static_cast<std::size_t>(k * B + b)];
      double o[kOut];
      synthetic(z, o);
      if (B == 4 && b == 3) o[1] = step(o[1], -7);  // the last lane of a full chunk: draws 3 and 7
      for (int i = 0; i < kOut; ++i) out[static_cast<std::size_t>(i * B + b)] = o[i];
    }
  };
  DifferentialOptions o;
  o.batch = 4;
  o.tolerance = Tolerance::e1(4.0);
  const Report rep = verify::differential(bl, kOut, kReference, compiled, o);
  EXPECT_FALSE(rep.passed) << rep.summary();
  EXPECT_EQ(rep.mismatches, 2u);
  EXPECT_EQ(rep.violations, 2u);
  EXPECT_EQ(rep.worst_output, 1);
  EXPECT_TRUE(rep.worst_draw == 3 || rep.worst_draw == 7) << rep.worst_draw;
  EXPECT_TRUE(rep.outputs[0].bitwise());
  EXPECT_EQ(rep.outputs[1].mismatches, 2);
}

TEST(Ulp, RelativeErrorAndTheRelativeTolerance) {
  EXPECT_EQ(verify::relative_error(1.0, 1.0), 0.0);
  EXPECT_EQ(verify::relative_error(2.0, 1.0), 0.5);
  EXPECT_EQ(verify::relative_error(1.0, 2.0), 0.5);
  EXPECT_EQ(verify::relative_error(1e-3, 2e-3, 1.0), 1e-3);  // against the scale
  EXPECT_EQ(verify::relative_error(0.0, -0.0), 0.0);
  EXPECT_EQ(verify::relative_error(1.0, kNaN), kInf);
  EXPECT_EQ(verify::relative_error(kInf, -kInf), kInf);
  // An infinite scale exempts the value: no error, whatever the difference.
  EXPECT_EQ(verify::relative_error(1.0, 2.0, kInf), 0.0);
  EXPECT_EQ(verify::ulp_distance(1.0, 2.0, kInf), 0.0);
  EXPECT_EQ(verify::relative_error(1.0, kNaN, kInf), 0.0);
  // within(): bitwise always; E0 nothing else; E1 by ulps or by rel, whichever is looser.
  const Tolerance e0 = Tolerance::e0();
  EXPECT_TRUE(verify::within(1.0, 1.0, e0));
  EXPECT_FALSE(verify::within(1.0, step(1.0, 1), e0));
  EXPECT_FALSE(verify::within(1.0, step(1.0, 1), e0, kInf));  // E0 knows no exemption
  const Tolerance e1 = Tolerance::e1(4.0);
  EXPECT_TRUE(verify::within(1.0, step(1.0, 4), e1));
  EXPECT_FALSE(verify::within(1.0, step(1.0, 5), e1));
  EXPECT_TRUE(verify::within(1.0, step(1.0, 5), e1, kInf));
  const Tolerance rel = Tolerance::e1_relative(1e-12);
  EXPECT_EQ(rel.cls, Exactness::E1);
  EXPECT_EQ(rel.ulps, 0.0);
  EXPECT_EQ(rel.rel, 1e-12);
  EXPECT_TRUE(verify::within(1.0, 1.0 + 0.9e-12, rel));
  EXPECT_FALSE(verify::within(1.0, 1.0 + 1.1e-12, rel));
  EXPECT_TRUE(verify::within(1.0, 1.0 + 1.1e-12, rel, 2.0));  // 0.55e-12 of the scale 2
  EXPECT_TRUE(verify::within(1.0, step(1.0, 1), rel));         // an ulp is far below 1e-12
  const Tolerance both = Tolerance::e1_relative(1e-15, 2.0);   // 2 ulps or 1e-15, the looser
  EXPECT_TRUE(verify::within(1.0, step(1.0, 2), both));        // 2 ulps (4.4e-16 > 1e-15? no: 2 ulps passes on ulps)
  EXPECT_TRUE(verify::within(1.0, step(1.0, 4), both));        // 4 ulps = 8.9e-16 <= 1e-15 passes on rel
  EXPECT_FALSE(verify::within(1.0, step(1.0, 5), both));       // 5 ulps = 1.1e-15 fails both
}

TEST(Differential, TheRelativeToleranceIsD26sForm) {
  // Output 1 scaled by (1 + 3e-13) at every draw: far beyond 4 ulps, within 1e-12 relative.
  const StateBall bl = ball(10);
  const BatchFn compiled = verify::batch_of(
      [](const double* z, double* out) {
        synthetic(z, out);
        out[1] *= 1.0 + 3e-13;
      },
      kIn, kOut);
  DifferentialOptions o;
  o.tolerance = Tolerance::e1(4.0);
  Report rep = verify::differential(bl, kOut, kReference, compiled, o);
  EXPECT_FALSE(rep.passed) << rep.summary();
  EXPECT_EQ(rep.violations, 10u);
  EXPECT_GT(rep.max_ulps, 1000.0);
  o.tolerance = Tolerance::e1_relative(1e-12);
  rep = verify::differential(bl, kOut, kReference, compiled, o);
  EXPECT_TRUE(rep.passed) << rep.summary();
  EXPECT_EQ(rep.violations, 0u);
  EXPECT_EQ(rep.mismatches, 10u);
  EXPECT_LE(rep.max_rel, 3.1e-13);
  EXPECT_GE(rep.max_rel, 2.9e-13);
  EXPECT_NE(rep.summary().find("1e-12 relative"), std::string::npos);
  o.tolerance = Tolerance::e1_relative(1e-13);
  rep = verify::differential(bl, kOut, kReference, compiled, o);
  EXPECT_FALSE(rep.passed) << rep.summary();
  // The exemption: a scale of +inf on output 1 makes the same comparison pass at 4 ulps, and
  // max_rel / max_ulps report 0 for it (the mismatch is still counted).
  o.tolerance = Tolerance::e1(4.0);
  o.scale = [](const double*, double* scale) {
    scale[0] = 0.0;
    scale[1] = kInf;
  };
  rep = verify::differential(bl, kOut, kReference, compiled, o);
  EXPECT_TRUE(rep.passed) << rep.summary();
  EXPECT_EQ(rep.mismatches, 10u);
  EXPECT_EQ(rep.outputs[1].max_ulps, 0.0);
  EXPECT_EQ(rep.outputs[1].max_rel, 0.0);
}

TEST(Differential, RejectsBadArguments) {
  const StateBall bl = ball(2);
  const BatchFn compiled = verify::batch_of(kReference, kIn, kOut);
  DifferentialOptions o;
  EXPECT_THROW(verify::differential(bl, 0, kReference, compiled, o), std::invalid_argument);
  o.batch = 0;
  EXPECT_THROW(verify::differential(bl, kOut, kReference, compiled, o), std::invalid_argument);
  o.batch = 1;
  EXPECT_THROW(verify::differential(bl, kOut, ScalarFn{}, compiled, o), std::invalid_argument);
  EXPECT_THROW(verify::differential(bl, kOut, kReference, BatchFn{}, o), std::invalid_argument);
  EXPECT_THROW(verify::differential(StateBall{}, kOut, kReference, compiled, o), std::invalid_argument);
  EXPECT_THROW(verify::batch_of(ScalarFn{}, kIn, kOut), std::invalid_argument);
  EXPECT_THROW(verify::batch_of(kReference, 0, kOut), std::invalid_argument);
}
