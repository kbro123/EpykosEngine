// M1/P1: the templates compile for a class Scalar that offers only what the contract promises
// (+ − * / between Scalars, unary minus, exp, mixed arithmetic with double on either side; no
// comparison, no conversion to double) and produce bit-identical values to the double instantiation.
// E0 TU (-ffp-contract=off) so "bit-identical" is not at the mercy of contraction decisions.
#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <vector>

#include "epykos/maths/m1/book.hpp"
#include "epykos/maths/m1/curve.hpp"
#include "epykos/maths/m1/price.hpp"

#ifndef EPYKOS_FP_CONTRACT_OFF
#error "an _e0_test.cpp TU must see EPYKOS_FP_CONTRACT_OFF"
#endif

namespace {

// A minimal Scalar: a boxed double with an op counter. Deliberately no operator double(), no
// comparisons, no construction from int, no std::max/min/abs.
struct Boxed {
  double v;
  static inline std::uint64_t ops = 0;
  static inline std::uint64_t mixed = 0;  // ops with a double on one side
  static inline std::uint64_t exps = 0;

  friend Boxed operator+(Boxed a, Boxed b) { ++ops; return {a.v + b.v}; }
  friend Boxed operator-(Boxed a, Boxed b) { ++ops; return {a.v - b.v}; }
  friend Boxed operator*(Boxed a, Boxed b) { ++ops; return {a.v * b.v}; }
  friend Boxed operator/(Boxed a, Boxed b) { ++ops; return {a.v / b.v}; }
  friend Boxed operator-(Boxed a) { ++ops; return {-a.v}; }
  friend Boxed operator+(double a, Boxed b) { ++ops; ++mixed; return {a + b.v}; }
  friend Boxed operator-(double a, Boxed b) { ++ops; ++mixed; return {a - b.v}; }
  friend Boxed operator*(double a, Boxed b) { ++ops; ++mixed; return {a * b.v}; }
  friend Boxed operator/(double a, Boxed b) { ++ops; ++mixed; return {a / b.v}; }
  friend Boxed operator+(Boxed a, double b) { ++ops; ++mixed; return {a.v + b}; }
  friend Boxed operator-(Boxed a, double b) { ++ops; ++mixed; return {a.v - b}; }
  friend Boxed operator*(Boxed a, double b) { ++ops; ++mixed; return {a.v * b}; }
  friend Boxed operator/(Boxed a, double b) { ++ops; ++mixed; return {a.v / b}; }
  friend Boxed exp(Boxed a) { ++exps; return {std::exp(a.v)}; }
};

}  // namespace

// Every operator of the contract is exercised once, mixed forms on both sides.
TEST(M1ScalarE0, ContractOperators) {
  const Boxed a{3.0};
  const Boxed b{0.5};
  EXPECT_EQ((a + b).v, 3.5);
  EXPECT_EQ((a - b).v, 2.5);
  EXPECT_EQ((a * b).v, 1.5);
  EXPECT_EQ((a / b).v, 6.0);
  EXPECT_EQ((-a).v, -3.0);
  EXPECT_EQ((2.0 + a).v, 5.0);
  EXPECT_EQ((2.0 - a).v, -1.0);
  EXPECT_EQ((2.0 * a).v, 6.0);
  EXPECT_EQ((2.0 / a).v, 2.0 / 3.0);
  EXPECT_EQ((a + 2.0).v, 5.0);
  EXPECT_EQ((a - 2.0).v, 1.0);
  EXPECT_EQ((a * 2.0).v, 6.0);
  EXPECT_EQ((a / 2.0).v, 1.5);
  EXPECT_EQ(exp(b).v, std::exp(0.5));
}

TEST(M1ScalarE0, RestrictedScalarMatchesDoubleBitwise) {
  const epykos::m1::Book b = epykos::m1::make_m1_book();
  std::vector<Boxed> zb(epykos::m1::n_knots);
  for (int k = 0; k < epykos::m1::n_knots; ++k) zb[static_cast<std::size_t>(k)] = {b.z0[static_cast<std::size_t>(k)]};

  std::vector<double> pv_d(static_cast<std::size_t>(b.n_swaps + 1));
  std::vector<Boxed> pv_b(static_cast<std::size_t>(b.n_swaps + 1));
  epykos::m1::price_book<double>(b, b.z0.data(), pv_d.data(), pv_d.data() + b.n_swaps);
  Boxed::ops = Boxed::mixed = Boxed::exps = 0;
  epykos::m1::price_book<Boxed>(b, zb.data(), pv_b.data(), pv_b.data() + b.n_swaps);

  for (int i = 0; i <= b.n_swaps; ++i) {
    const auto s = static_cast<std::size_t>(i);
    EXPECT_EQ(pv_b[s].v, pv_d[s]) << i;
  }
  EXPECT_GT(Boxed::ops, 0u);
  EXPECT_GT(Boxed::mixed, 0u);
  // One exp per DF evaluation: fixed rows read DF(e); float rows read DF(e) and, unless realised,
  // DF(s). (CSE across rows is the recorder's job, not the author's.)
  int expected_exps = 0;
  for (int r = 0; r < b.n_rows; ++r) {
    const auto s = static_cast<std::size_t>(r);
    expected_exps += 1 + (b.row_leg[s] == epykos::m1::float_leg && !b.row_is_realised_first[s]);
  }
  EXPECT_EQ(Boxed::exps, static_cast<std::uint64_t>(expected_exps));
}

TEST(M1ScalarE0, CurveTemplatesOnRestrictedScalar) {
  std::vector<Boxed> zb(epykos::m1::n_knots);
  for (int k = 0; k < epykos::m1::n_knots; ++k) zb[static_cast<std::size_t>(k)] = {epykos::m1::record_state[static_cast<std::size_t>(k)]};
  for (double t : {-0.5, 0.0, 7.0 / 365.0, 0.1, 1.0, 2.6, 731.0 / 365.0, 29.9, 30.0, 45.0}) {
    const double zd = epykos::m1::zero_rate(epykos::m1::knot_times.data(), epykos::m1::record_state.data(), epykos::m1::n_knots, t);
    const Boxed zbx = epykos::m1::zero_rate(epykos::m1::knot_times.data(), zb.data(), epykos::m1::n_knots, t);
    EXPECT_EQ(zbx.v, zd) << t;
    const double dd = epykos::m1::df(epykos::m1::knot_times.data(), epykos::m1::record_state.data(), epykos::m1::n_knots, t);
    const Boxed db = epykos::m1::df(epykos::m1::knot_times.data(), zb.data(), epykos::m1::n_knots, t);
    EXPECT_EQ(db.v, dd) << t;
  }
}
