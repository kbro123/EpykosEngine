// P0/oracle: the error-against-truth harness of verify/oracle.hpp (PRINCIPLES.md §4; D72).
//
// The harness's own gate, on synthetic programs whose truth is known analytically rather than on a
// fixture. It is deliberately cheap: the ctest name `verify_oracle_truth_test` matches D33's gate
// regex, so scripts/mutation_test.sh runs this file once per registered mutant.
//
// The load-bearing property, and the one most easily got wrong, is the second test: the error must
// be formed IN THE ORACLE'S ARITHMETIC. If the harness computed `approx - truth.hi` in double it
// would report 0 for a value whose whole error lives in the oracle's low word — that is, it would
// discard exactly the information the oracle exists to supply and report an uncharacterised
// approximation as exact. That is the failure PRINCIPLES.md §4 exists to end, reappearing inside
// the instrument meant to end it.
#include <gtest/gtest.h>

#include <cmath>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <vector>

#include "epykos/scalar/wide.hpp"
#include "epykos/verify/differential.hpp"
#include "epykos/verify/oracle.hpp"

namespace verify = epykos::verify;
using epykos::Wide;

namespace {

// A one-input, two-output toy: out[0] = (1 + z)^64 as the naive product loop of the definition
// (which is what PRINCIPLES.md §2 says the pricing layer hands the engine), out[1] = exp(z).
// Templated on Scalar for the same reason the real maths is: one source, two instantiations.
template <class S>
void toy(const S* z, S* out) {
  S acc = S(1.0);
  for (int i = 0; i < 64; ++i) acc = acc * (S(1.0) + z[0]);
  out[0] = acc;
  out[1] = exp(z[0]);
}

verify::StateBall toy_ball(int draws) {
  const double centre = 0.03;
  verify::BallOptions o;
  o.draws = draws;
  o.rho = 0.01;
  return verify::make_state_ball(&centre, 1, o);
}

}  // namespace

TEST(OracleTruth, MeasuresTheNaivePathAgainstAnInstantiationOfTheSameSource) {
  const verify::StateBall ball = toy_ball(64);
  const verify::BatchFn approx = verify::batch_of([](const double* z, double* out) { toy<double>(z, out); }, 1, 2);
  const verify::OracleFn truth = [](const Wide* z, Wide* out) { toy<Wide>(z, out); };

  verify::TruthOptions opt;
  opt.classes = {verify::OutputClass{"product loop", 0, 1}, verify::OutputClass{"exp", 1, 1}};
  const verify::TruthReport rep = verify::error_against_truth(ball, 2, approx, truth, opt);

  EXPECT_EQ(rep.n_outputs, 2);
  EXPECT_EQ(rep.n_states, 64);
  EXPECT_EQ(rep.oracle_mantissa_bits, 106);
  EXPECT_EQ(rep.headroom_bits, 53);
  EXPECT_EQ(rep.degraded, 0u);
  ASSERT_EQ(rep.classes.size(), 2u);

  // A 64-step product loop in double accumulates a handful of ulps; the oracle resolves it.
  EXPECT_GT(rep.classes[0].max_rel, 0.0);
  EXPECT_LT(rep.classes[0].max_rel, 1e-14);
  // libm's exp is good to about an ulp, so the second class must be much tighter than the first.
  EXPECT_LT(rep.classes[1].max_rel, 1e-15);
  EXPECT_GT(rep.classes[0].max_rel, rep.classes[1].max_rel);

  std::cout << "[ harness  ] " << rep.summary() << "\n" << rep.per_class_table();
}

// The property the whole instrument rests on.
TEST(OracleTruth, TheErrorIsFormedAtTheOraclesPrecisionNotInDouble) {
  const verify::StateBall ball = toy_ball(4);
  // Truth carries a tail that a double cannot see; the path under test returns truth's HIGH word
  // exactly. In double the difference is 0. At the oracle's precision it is the tail.
  const double head = 1.0;
  const double tail = 0x1p-80;  // far below ulp(1.0) = 2^-52
  const verify::OracleFn truth = [head, tail](const Wide*, Wide* out) { out[0] = Wide(head, tail); };
  const verify::BatchFn approx = [head](const double*, int B, double* out) {
    for (int b = 0; b < B; ++b) out[b] = head;
  };

  const verify::TruthReport rep = verify::error_against_truth(ball, 1, approx, truth);
  ASSERT_EQ(rep.outputs.size(), 1u);
  EXPECT_DOUBLE_EQ(rep.outputs[0].max_abs, tail);
  EXPECT_DOUBLE_EQ(rep.outputs[0].max_rel, tail);
  EXPECT_FALSE(rep.outputs[0].exact());
  // Sanity: a double-arithmetic harness would have reported exactly this, and it is wrong.
  EXPECT_EQ(head - Wide(head, tail).hi, 0.0);
  std::cout << "[ harness  ] a tail of " << std::scientific << std::setprecision(3) << tail
            << " below a head of 1.0 is reported as error " << rep.outputs[0].max_abs
            << "; computed in double it would have been " << (head - Wide(head, tail).hi) << "\n";
}

TEST(OracleTruth, StatesPromoteExactlySoTheTwoSidesSeeTheSameInput) {
  const verify::StateBall ball = toy_ball(8);
  std::vector<double> seen_by_truth;
  std::vector<double> seen_by_approx;
  const verify::OracleFn truth = [&](const Wide* z, Wide* out) {
    EXPECT_EQ(z[0].tail(), 0.0) << "a promoted state must have no tail";
    seen_by_truth.push_back(z[0].value());
    out[0] = Wide(0.0);
  };
  const verify::BatchFn approx = [&](const double* state, int B, double* out) {
    for (int b = 0; b < B; ++b) {
      seen_by_approx.push_back(state[b]);
      out[b] = 0.0;
    }
  };
  verify::TruthOptions opt;
  opt.batch = 3;  // deliberately not a divisor of 8, so the last call is short
  const verify::TruthReport rep = verify::error_against_truth(ball, 1, approx, truth, opt);
  ASSERT_EQ(seen_by_truth.size(), 8u);
  ASSERT_EQ(seen_by_approx.size(), 8u);
  for (std::size_t i = 0; i < 8; ++i) EXPECT_EQ(seen_by_truth[i], seen_by_approx[i]) << "state " << i;
  EXPECT_EQ(rep.max_rel, 0.0);
}

TEST(OracleTruth, OutputClassesArePartitionedAndValidated) {
  const verify::StateBall ball = toy_ball(2);
  const verify::BatchFn approx = verify::batch_of([](const double* z, double* out) { toy<double>(z, out); }, 1, 2);
  const verify::OracleFn truth = [](const Wide* z, Wide* out) { toy<Wide>(z, out); };

  verify::TruthOptions overlap;
  overlap.classes = {verify::OutputClass{"a", 0, 2}, verify::OutputClass{"b", 1, 1}};
  EXPECT_THROW(verify::error_against_truth(ball, 2, approx, truth, overlap), std::invalid_argument);

  verify::TruthOptions past;
  past.classes = {verify::OutputClass{"a", 1, 5}};
  EXPECT_THROW(verify::error_against_truth(ball, 2, approx, truth, past), std::invalid_argument);

  EXPECT_THROW(verify::error_against_truth(ball, 0, approx, truth), std::invalid_argument);
  EXPECT_THROW(verify::error_against_truth(ball, 2, nullptr, truth), std::invalid_argument);
  EXPECT_THROW(verify::error_against_truth(ball, 2, approx, nullptr), std::invalid_argument);

  // No classes given: one class over everything.
  const verify::TruthReport rep = verify::error_against_truth(ball, 2, approx, truth);
  ASSERT_EQ(rep.classes.size(), 1u);
  EXPECT_EQ(rep.classes[0].count, 2);
  EXPECT_TRUE(rep.classes[0].name.empty());
}

TEST(OracleTruth, TheScaleSeparatesArithmeticQualityFromConditioning) {
  // out[0] = (1+z)^8 − (1+z)^8 computed as two independent chains: the answer is 0 to within
  // rounding, so an error relative to the VALUE is meaningless while an error relative to the
  // scale of the terms (D26) is the real number. The harness must report both.
  const verify::StateBall ball = toy_ball(16);
  const verify::BatchFn approx = verify::batch_of(
      [](const double* z, double* out) {
        double a = 1.0, b = 1.0;
        for (int i = 0; i < 8; ++i) a = a * (1.0 + z[0]);
        for (int i = 0; i < 8; ++i) b = b * (1.0 + z[0] * 1.0000000000000002);
        out[0] = a - b;
      },
      1, 1);
  const verify::OracleFn truth = [](const Wide* z, Wide* out) {
    Wide a(1.0), b(1.0);
    for (int i = 0; i < 8; ++i) a = a * (Wide(1.0) + z[0]);
    for (int i = 0; i < 8; ++i) b = b * (Wide(1.0) + z[0] * 1.0000000000000002);
    out[0] = a - b;
  };

  verify::TruthOptions opt;
  opt.scale = [](const double* z, double* scale) {
    double a = 1.0;
    for (int i = 0; i < 8; ++i) a = a * (1.0 + z[0]);
    scale[0] = 2.0 * std::fabs(a);
  };
  const verify::TruthReport rep = verify::error_against_truth(ball, 1, approx, truth, opt);
  EXPECT_TRUE(rep.scaled);
  // Against the terms' scale the arithmetic is near-perfect; against the cancelled value it is not.
  EXPECT_LT(rep.outputs[0].max_rel, 1e-16);
  EXPECT_GT(rep.outputs[0].max_rel_self, rep.outputs[0].max_rel);
  std::cout << "[ harness  ] cancelling difference: error vs the terms' scale "
            << std::scientific << std::setprecision(3) << rep.outputs[0].max_rel
            << ", vs the value itself " << rep.outputs[0].max_rel_self << "\n";
}

// ------------------------------------------------------------------------------------------------
// The sensitivity channel
// ------------------------------------------------------------------------------------------------

TEST(OracleTruth, OracleJacobianMatchesAnAnalyticDerivativeFarBelowDoublesReach) {
  // d/dz (1+z)^64 = 64(1+z)^63 and d/dz exp(z) = exp(z), both computable at the oracle's precision,
  // so the finite difference is checked against the real thing rather than against itself. The
  // polynomial is the hard case on purpose: f'''/f' = 3,684 there, which is what the plain central
  // difference's truncation term is proportional to and what Richardson exists to cancel.
  const double z = 0.03;
  const verify::OracleFn truth = [](const Wide* zz, Wide* out) { toy<Wide>(zz, out); };

  Wide analytic0(64.0);
  for (int i = 0; i < 63; ++i) analytic0 = analytic0 * (Wide(1.0) + Wide(z));
  const Wide analytic1 = exp(Wide(z));

  auto err = [](const Wide& got, const Wide& want) { return std::fabs(((got - want) / want).hi); };

  verify::JacobianOptions plain;
  plain.richardson = false;
  const std::vector<Wide> jp = verify::oracle_jacobian(truth, &z, 1, 2, plain);
  ASSERT_EQ(jp.size(), 2u);
  const double p0 = err(jp[0], analytic0), p1 = err(jp[1], analytic1);

  const std::vector<Wide> jr = verify::oracle_jacobian(truth, &z, 1, 2);  // Richardson, the default
  ASSERT_EQ(jr.size(), 2u);
  const double r0 = err(jr[0], analytic0), r1 = err(jr[1], analytic1);

  // The same difference taken in double, which is the alternative this instrument replaces.
  const double h_d = std::cbrt(3.0 * 0x1p-53);
  double zp = z + h_d, zm = z - h_d;
  double op[2], om[2];
  toy<double>(&zp, op);
  toy<double>(&zm, om);
  const double d0 = err(Wide((op[0] - om[0]) / (2.0 * h_d)), analytic0);

  // Richardson must beat the plain form by orders of magnitude on the polynomial, where the h^2
  // truncation term dominates. On exp(z) it does NOT: f'''/f' = 1 there, so the plain difference is
  // already at its roundoff floor (u/h ~ 4e-22) and Richardson's 1.67x roundoff amplification makes
  // it marginally worse. That is the trade, stated rather than averaged away, and it is the right
  // default because a truncation error scales with a function the instrument cannot see while the
  // roundoff floor is fixed and known.
  EXPECT_LT(r0, 1e-3 * p0);
  // Every form must be decades below the ~1e-15 a double derivative path sits at, which is the
  // margin the instrument needs in order not to be the thing being measured.
  EXPECT_LT(p0, 1e-17);
  EXPECT_LT(p1, 1e-19);
  EXPECT_LT(r0, 1e-21);
  EXPECT_LT(r1, 1e-19);
  EXPECT_GT(d0, 1e5 * r0);

  std::cout << std::scientific << std::setprecision(3)
            << "[ jacobian ] vs the analytic derivative, (1+z)^64 and exp(z) at z = 0.03, step "
            << verify::oracle_jacobian_step(z) << ":\n"
            << "[ jacobian ]   plain central difference at 106 bits: " << p0 << " and " << p1 << "\n"
            << "[ jacobian ]   Richardson-extrapolated (default)   : " << r0 << " and " << r1 << "\n"
            << "[ jacobian ]   the same difference taken in double : " << d0 << " (" << (d0 / r0)
            << "x the Richardson error)\n";
}
TEST(OracleTruth, OracleJacobianTakesASubsetOfInputsAndValidatesThem) {
  const double z[3] = {0.01, 0.02, 0.03};
  const verify::OracleFn truth = [](const Wide* zz, Wide* out) {
    out[0] = zz[0] * zz[1] * zz[2];
  };
  verify::JacobianOptions opt;
  opt.inputs = {0, 2};
  const std::vector<Wide> jac = verify::oracle_jacobian(truth, z, 3, 1, opt);
  ASSERT_EQ(jac.size(), 2u);
  EXPECT_NEAR(jac[0].value(), z[1] * z[2], 1e-20);  // d/dz0
  EXPECT_NEAR(jac[1].value(), z[0] * z[1], 1e-20);  // d/dz2

  verify::JacobianOptions bad;
  bad.inputs = {5};
  EXPECT_THROW(verify::oracle_jacobian(truth, z, 3, 1, bad), std::invalid_argument);
  EXPECT_THROW(verify::oracle_jacobian(nullptr, z, 3, 1), std::invalid_argument);
}

TEST(OracleTruth, DegradedOracleValuesAreCountedNotHidden) {
  // A value inside the last decades before the exponent floor: the oracle's own low word goes
  // subnormal there, so the harness must say so rather than quote a figure it cannot stand behind.
  const verify::StateBall ball = toy_ball(2);
  const verify::OracleFn truth = [](const Wide*, Wide* out) { out[0] = exp(Wide(-740.0)); };
  const verify::BatchFn approx = [](const double*, int B, double* out) {
    for (int b = 0; b < B; ++b) out[b] = std::exp(-740.0);
  };
  const verify::TruthReport rep = verify::error_against_truth(ball, 1, approx, truth);
  EXPECT_EQ(rep.degraded, 2u);
  EXPECT_EQ(rep.outputs[0].degraded, 2);
  EXPECT_NE(rep.summary().find("WARNING"), std::string::npos);
  std::cout << "[ harness  ] " << rep.summary() << "\n";
}
