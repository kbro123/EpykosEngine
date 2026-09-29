// M2/Q2 E0 gates: (1) the value channel of price_book<Dual<N>> is bitwise price_book<double> at
// the record point and at every one of the 64 batch states, for N = 1 and N = 12 and through the
// tangent / Jacobian helpers; (2) one pass of Dual<12> is bitwise the 12 passes of Dual<1> along
// the coordinate directions; (3) a general N-slot pass is bitwise the N single-direction passes.
//
// This TU is compiled with -ffp-contract=off in every preset (the _e0_test.cpp convention): the
// double oracle, the Dual value channel and every tangent slot are then the same sequence of
// IEEE operations, so equality is a theorem rather than a compiler decision.
#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "epykos/fixtures/m1_book.hpp"
#include "epykos/fixtures/m1_price.hpp"
#include "epykos/fixtures/m1_reference.hpp"
#include "epykos/fixtures/m1_tangent.hpp"
#include "epykos/scalar/dual.hpp"

#ifndef EPYKOS_FP_CONTRACT_OFF
#error "an _e0_test.cpp TU must see EPYKOS_FP_CONTRACT_OFF"
#endif

namespace fixtures = epykos::fixtures;
using epykos::Dual;

namespace {

struct Fixture {
  fixtures::Book book;
  fixtures::Batch batch;
  fixtures::ReferenceTable oracle;  // record + 64 states, computed in this (contraction-free) TU
};

const Fixture& fixture() {
  static const Fixture f = [] {
    Fixture x;
    x.book = fixtures::make_m1_book();
    x.batch = fixtures::make_m1_batch();
    x.oracle = fixtures::m1_reference_values(x.book, x.batch);
    return x;
  }();
  return f;
}

std::uint64_t bits(double x) {
  std::uint64_t b;
  std::memcpy(&b, &x, sizeof b);
  return b;
}

// Number of (index) mismatches between two equally long arrays, bit for bit; the first few reported.
std::size_t mismatches(const double* got, const double* expect, std::size_t n, const std::string& what) {
  std::size_t m = 0;
  for (std::size_t k = 0; k < n; ++k) {
    if (bits(got[k]) != bits(expect[k])) {
      if (m < 5) ADD_FAILURE() << what << ", index " << k << ": " << got[k] << " vs " << expect[k];
      ++m;
    }
  }
  return m;
}

// The state to evaluate: b = -1 is the record point, otherwise batch state b.
const double* state_of(const Fixture& f, int b, double* buf) {
  if (b < 0) return f.book.z0.data();
  f.batch.state(b, buf);
  return buf;
}
const double* oracle_of(const Fixture& f, int b) { return b < 0 ? f.oracle.record.data() : f.oracle.state(b); }

}  // namespace

TEST(DualM1E0, ValueChannelIsTheDoubleOracleBitwise) {
  const Fixture& f = fixture();
  const fixtures::Book& b = f.book;
  const auto n_out = static_cast<std::size_t>(b.n_swaps + 1);
  ASSERT_EQ(f.oracle.stride(), b.n_swaps + 1);
  std::vector<Dual<1>> z1(static_cast<std::size_t>(fixtures::n_knots));
  std::vector<Dual<12>> z12(static_cast<std::size_t>(fixtures::n_knots));
  std::vector<Dual<1>> out1(n_out);
  std::vector<Dual<12>> out12(n_out);
  std::vector<double> value(n_out);
  double zbuf[fixtures::n_knots];
  std::size_t total = 0;
  for (int s = -1; s < f.batch.n_states; ++s) {
    const double* z = state_of(f, s, zbuf);
    const double* expect = oracle_of(f, s);
    const std::string where = s < 0 ? "record point" : "state " + std::to_string(s);
    // Dual<1> along a direction that touches every knot (the direction must not change the values).
    for (int k = 0; k < fixtures::n_knots; ++k) {
      const auto sk = static_cast<std::size_t>(k);
      z1[sk] = Dual<1>(z[k], {1.0 + 0.25 * k});
      z12[sk] = Dual<12>::variable(z[k], k);
    }
    fixtures::price_book<Dual<1>>(b, z1.data(), out1.data(), out1.data() + b.n_swaps);
    for (std::size_t o = 0; o < n_out; ++o) value[o] = out1[o].v;
    total += mismatches(value.data(), expect, n_out, where + ", Dual<1> value");
    fixtures::price_book<Dual<12>>(b, z12.data(), out12.data(), out12.data() + b.n_swaps);
    for (std::size_t o = 0; o < n_out; ++o) value[o] = out12[o].v;
    total += mismatches(value.data(), expect, n_out, where + ", Dual<12> value");
  }
  EXPECT_EQ(total, 0u);
}

TEST(DualM1E0, HelpersCarryTheOracleValuesBitwise) {
  const Fixture& f = fixture();
  const fixtures::Book& b = f.book;
  const auto n_out = static_cast<std::size_t>(b.n_swaps + 1);
  double zbuf[fixtures::n_knots];
  double direction[fixtures::n_knots];
  for (int k = 0; k < fixtures::n_knots; ++k) direction[k] = ((k % 2) ? -1.0 : 1.0) * (1.0 + k) / 12.0;
  std::size_t total = 0;
  for (int s : {-1, 0, 7, 31, 63}) {
    const double* z = state_of(f, s, zbuf);
    const double* expect = oracle_of(f, s);
    const std::string where = s < 0 ? "record point" : "state " + std::to_string(s);
    const fixtures::Tangent t = fixtures::tangent(b, z, direction);
    ASSERT_EQ(t.value.size(), n_out);
    total += mismatches(t.value.data(), expect, n_out, where + ", tangent().value");
    const fixtures::Jacobian jn = fixtures::jacobian_forward(b, z);
    ASSERT_EQ(jn.value.size(), n_out);
    total += mismatches(jn.value.data(), expect, n_out, where + ", jacobian_forward().value");
    const fixtures::Jacobian jw = fixtures::jacobian_forward_wide(b, z);
    ASSERT_EQ(jw.value.size(), n_out);
    total += mismatches(jw.value.data(), expect, n_out, where + ", jacobian_forward_wide().value");
  }
  EXPECT_EQ(total, 0u);
}

TEST(DualM1E0, WideJacobianIsTheNarrowPassesBitwise) {
  const Fixture& f = fixture();
  const fixtures::Book& b = f.book;
  double zbuf[fixtures::n_knots];
  std::size_t total = 0;
  std::size_t entries = 0;
  for (int s = -1; s < f.batch.n_states; ++s) {
    const double* z = state_of(f, s, zbuf);
    const std::string where = s < 0 ? "record point" : "state " + std::to_string(s);
    const fixtures::Jacobian narrow = fixtures::jacobian_forward(b, z);
    const fixtures::Jacobian wide = fixtures::jacobian_forward_wide(b, z);
    ASSERT_EQ(narrow.n_outputs, b.n_swaps + 1);
    ASSERT_EQ(narrow.n_inputs, fixtures::n_knots);
    ASSERT_EQ(wide.n_outputs, narrow.n_outputs);
    ASSERT_EQ(wide.n_inputs, narrow.n_inputs);
    ASSERT_EQ(wide.jac.size(), narrow.jac.size());
    total += mismatches(wide.jac.data(), narrow.jac.data(), narrow.jac.size(), where + ", Jacobian");
    total += mismatches(wide.value.data(), narrow.value.data(), narrow.value.size(), where + ", value");
    entries += narrow.jac.size();
  }
  EXPECT_EQ(total, 0u);
  EXPECT_EQ(entries, 65u * 1001u * 12u);
}

// A general 3-slot pass (two coordinate directions and a mixed one) is bitwise the three
// single-direction passes: each slot is an independent Dual<1>.
TEST(DualM1E0, SlotsAreIndependentSingleDirectionPassesBitwise) {
  const Fixture& f = fixture();
  const fixtures::Book& b = f.book;
  const auto n_out = static_cast<std::size_t>(b.n_swaps + 1);
  const double* z = b.z0.data();
  double dirs[3][fixtures::n_knots] = {};
  dirs[0][0] = 1.0;
  dirs[1][5] = 1.0;
  for (int k = 0; k < fixtures::n_knots; ++k) dirs[2][k] = ((k % 3) - 1) * (0.5 + k);
  std::vector<Dual<3>> z3(static_cast<std::size_t>(fixtures::n_knots));
  for (int k = 0; k < fixtures::n_knots; ++k) {
    z3[static_cast<std::size_t>(k)] = Dual<3>(z[k], {dirs[0][k], dirs[1][k], dirs[2][k]});
  }
  std::vector<Dual<3>> out3(n_out);
  fixtures::price_book<Dual<3>>(b, z3.data(), out3.data(), out3.data() + b.n_swaps);
  std::vector<double> slot(n_out);
  std::size_t total = 0;
  for (int i = 0; i < 3; ++i) {
    const fixtures::Tangent t = fixtures::tangent(b, z, dirs[i]);
    for (std::size_t o = 0; o < n_out; ++o) slot[o] = out3[o].d[static_cast<std::size_t>(i)];
    total += mismatches(slot.data(), t.tangent.data(), n_out, "slot " + std::to_string(i));
    for (std::size_t o = 0; o < n_out; ++o) slot[o] = out3[o].v;
    total += mismatches(slot.data(), t.value.data(), n_out, "value");
  }
  EXPECT_EQ(total, 0u);
}

// The operator rules are exactly their textbook forms in this TU (no contraction).
TEST(DualM1E0, OperatorRulesAreExactForms) {
  const Dual<1> a(3.1, {0.7});
  const Dual<1> b(-0.35, {2.2});
  EXPECT_EQ((a * b).d[0], 0.7 * -0.35 + 3.1 * 2.2);
  EXPECT_EQ((a / b).d[0], (0.7 - (3.1 / -0.35) * 2.2) / -0.35);
  EXPECT_EQ((2.0 / b).d[0], -((2.0 / -0.35) * 2.2) / -0.35);
  EXPECT_EQ(fma(a, b, a).d[0], 0.7 * -0.35 + 3.1 * 2.2 + 0.7);
  EXPECT_EQ(exp(a).d[0], std::exp(3.1) * 0.7);
  EXPECT_EQ(log(a).d[0], 0.7 / 3.1);
  EXPECT_EQ(sqrt(a).d[0], 0.7 / (2.0 * std::sqrt(3.1)));
  EXPECT_EQ(recip(a).d[0], -((1.0 / 3.1) * (1.0 / 3.1)) * 0.7);
}
