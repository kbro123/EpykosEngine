// M3/G1: affine_collapse's single-term scaling — a tainted product Const × atom that a reader
// scales again (Mul(Const, this)) becomes a one-term Affine (−0.0 + c·x, E0), so that a chain which
// scales a scaled affine (a Thomas sweep with structural reciprocal pivots, a Bessel tangent of
// scaled secants, a back-substitution) collapses all the way to Affine nodes; every other product
// stays as it was — one read only by chains is taken by them as a term (M1's weight·knot products,
// one Affine per interpolated time, unchanged), one read by an Exp or an output stays a Mul.
// An E0 TU (-ffp-contract=off): the replay after the passes is compared bitwise with the replay
// before and with the double instantiation of the same maths.
#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <vector>

#include "epykos/scalar/rec.hpp"
#include "epykos/tape/passes.hpp"
#include "epykos/tape/replay.hpp"
#include "epykos/tape/tape.hpp"
#include "tape/tape_test_helpers.hpp"

using epykos::Op;
using epykos::Rec;
using epykos::Tape;
using epykos::test::count_op;

namespace {

// A Thomas-like sweep over three inputs with structural reciprocals: every step scales an affine.
template <class Scalar>
std::vector<Scalar> sweep(const Scalar* x) {
  const Scalar d0 = (x[1] - x[0]) * 0.5;
  const Scalar d1 = (x[2] - x[1]) * 0.25;
  const Scalar r1 = d1 - d0;
  const Scalar dp1 = r1 * 1.5;
  const Scalar dp2 = (d0 - 0.3 * dp1) * 0.8;
  const Scalar m1 = dp2;
  const Scalar m0 = dp1 - 0.7 * m1;
  const Scalar value = 0.2 * x[0] + 0.3 * x[1] + 0.05 * m0 + 0.06 * m1;
  return {value, m0, m1};
}

}  // namespace

TEST(AffineSingleTerm, AChainOfScaledAffinesCollapsesToAffines) {
  Tape t;
  const double x0[3] = {0.04, 0.05, 0.045};
  std::vector<Rec> out;
  {
    Tape::Scope scope(t);
    Rec x[3];
    for (int k = 0; k < 3; ++k) x[k] = epykos::make_input(t, x0[k]);
    out = sweep<Rec>(x);
    for (const Rec& r : out) epykos::register_output(t, r);
  }
  const std::vector<double> before = epykos::replay(t, {x0[0], x0[1], x0[2]});
  epykos::standard_passes(t);
  t.validate();
  const std::vector<double> after = epykos::replay(t, {x0[0], x0[1], x0[2]});
  ASSERT_EQ(after.size(), 3u);
  for (std::size_t i = 0; i < 3; ++i) EXPECT_EQ(epykos::test::bits(after[i]), epykos::test::bits(before[i])) << i;
  const std::vector<double> oracle = sweep<double>(x0);
  for (std::size_t i = 0; i < 3; ++i) EXPECT_EQ(epykos::test::bits(after[i]), epykos::test::bits(oracle[i])) << i;
  // Every tainted node is an Input or an Affine: no Mul, Sub, Add or Sum survives.
  for (const epykos::Node& n : t.nodes()) {
    if (!n.tainted) continue;
    EXPECT_TRUE(n.op == Op::Input || n.op == Op::Affine) << epykos::to_string(n.op);
  }
  EXPECT_EQ(count_op(t, Op::Mul), 0u);
  EXPECT_EQ(count_op(t, Op::Sub), 0u);
  EXPECT_EQ(count_op(t, Op::Sum), 0u);
  // Another state replays bitwise the double instantiation too.
  const double x1[3] = {0.02, 0.07, 0.01};
  const std::vector<double> o1 = sweep<double>(x1);
  const std::vector<double> a1 = epykos::replay(t, {x1[0], x1[1], x1[2]});
  for (std::size_t i = 0; i < 3; ++i) EXPECT_EQ(epykos::test::bits(a1[i]), epykos::test::bits(o1[i])) << i;
}

TEST(AffineSingleTerm, AProductReadOnlyByChainsStaysATerm) {
  // M1's shape: two interpolations sharing w·z[k+1]; one Affine each, the product absorbed.
  Tape t;
  {
    Tape::Scope scope(t);
    Rec z0 = epykos::make_input(t, 0.04), z1 = epykos::make_input(t, 0.05), z2 = epykos::make_input(t, 0.045);
    const double w = 0.3;
    Rec a = (1.0 - w) * z0 + w * z1;
    Rec b = (1.0 - 0.6) * z1 + 0.6 * z2;
    Rec c = 0.2 * z2 + w * z1;  // shares w·z1 with a
    epykos::register_output(t, a);
    epykos::register_output(t, b);
    epykos::register_output(t, c);
  }
  epykos::cse(t);
  epykos::dce(t);
  epykos::fold_sum(t);
  epykos::affine_collapse(t);
  EXPECT_EQ(count_op(t, Op::Affine), 3u) << "one Affine per chain, no one-term Affine for the shared product";
  epykos::dce(t);
  EXPECT_EQ(count_op(t, Op::Mul), 0u);
  EXPECT_EQ(count_op(t, Op::Affine), 3u);
}

TEST(AffineSingleTerm, OnlyAProductScaledAgainBecomesAOneTermAffine) {
  Tape t;
  const double x0 = 0.04;
  {
    Tape::Scope scope(t);
    Rec x = epykos::make_input(t, x0);
    Rec s = 0.3 * x;              // an output: stays a Mul
    Rec e = exp(2.5 * x);         // read by exp: stays a Mul
    Rec n = 0.7 * (1.5 * x);      // the inner product is scaled again: a one-term Affine; the outer
                                  // product is read by an output: a Mul over that Affine
    epykos::register_output(t, s);
    epykos::register_output(t, e);
    epykos::register_output(t, n);
  }
  const std::vector<double> before = epykos::replay(t, {x0});
  epykos::standard_passes(t);
  EXPECT_EQ(count_op(t, Op::Mul), 3u);
  EXPECT_EQ(count_op(t, Op::Affine), 1u);
  for (const epykos::Node& n : t.nodes()) {
    if (n.op != Op::Affine) continue;
    EXPECT_EQ(n.nargs, 1);
    EXPECT_EQ(epykos::test::bits(n.konst), epykos::test::bits(-0.0));
    EXPECT_EQ(epykos::test::bits(t.coefs(n)[0]), epykos::test::bits(1.5));
    EXPECT_EQ(t[t.args(n)[0]].op, Op::Input);
  }
  const std::vector<double> after = epykos::replay(t, {x0});
  for (std::size_t i = 0; i < 3; ++i) EXPECT_EQ(epykos::test::bits(after[i]), epykos::test::bits(before[i])) << i;
  EXPECT_EQ(epykos::test::bits(after[0]), epykos::test::bits(0.3 * x0));
  EXPECT_EQ(epykos::test::bits(after[1]), epykos::test::bits(std::exp(2.5 * x0)));
  EXPECT_EQ(epykos::test::bits(after[2]), epykos::test::bits(0.7 * (1.5 * x0)));
  // −0.0 + c·x is exact for a zero product as well: x = ±0 gives the product's own zero.
  EXPECT_EQ(epykos::test::bits(epykos::replay(t, {0.0})[2]), epykos::test::bits(0.7 * (1.5 * 0.0)));
  EXPECT_EQ(epykos::test::bits(epykos::replay(t, {-0.0})[2]), epykos::test::bits(0.7 * (1.5 * -0.0)));
}

TEST(AffineSingleTerm, UntaintedAndNonAtomProductsAreLeftAlone) {
  Tape t;
  {
    Tape::Scope scope(t);
    Rec x = epykos::make_input(t, 0.04);
    Rec e = exp(x);
    Rec y = 2.0 * e;   // Exp is not an atom: stays a Mul
    Rec c = 3.0 * Rec(4.0);  // untainted
    epykos::register_output(t, y + c);
  }
  epykos::standard_passes(t);
  EXPECT_EQ(count_op(t, Op::Affine), 0u);
  EXPECT_GE(count_op(t, Op::Mul), 1u);
}
