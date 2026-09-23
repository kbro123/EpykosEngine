// M2/Q2 gate (RESUME.md §3 M2 "Q2 forward mode: tangent vs FD 1e-6"): the tangent of the book PV
// by one pass of price_book<Dual<1>> per coordinate direction against the central finite difference
// of price_book<double>, h = 1e-6, |Δ| <= max(1e-6·|FD|, 1e-9), at the record point and at batch
// states. Supplementary: every swap PV's gradient against the same FD with the FD's own rounding
// floor; the Jacobian helpers agree with the tangent passes; the tangent is linear in the direction.
//
// Release flags (this TU may contract): the FD comparison is tolerance-based by nature. Bitwise
// facts (value channel = double, wide = narrow) are asserted in dual_m1_e0_test.cpp.
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <vector>

#include "epykos/fixtures/m1_book.hpp"
#include "epykos/fixtures/m1_price.hpp"
#include "epykos/fixtures/m1_tangent.hpp"
#include "epykos/scalar/dual.hpp"

namespace fixtures = epykos::fixtures;
using epykos::Dual;

namespace {

constexpr double kH = 1e-6;
constexpr double kRel = 1e-6;
constexpr double kAbsFloor = 1e-9;

struct Fixture {
  fixtures::Book book;
  fixtures::Batch batch;
};

const Fixture& fixture() {
  static const Fixture f = [] {
    Fixture x;
    x.book = fixtures::make_m1_book();
    x.batch = fixtures::make_m1_batch();
    return x;
  }();
  return f;
}

// price_book<double>: out[0..n_swaps) swap PVs, out[n_swaps] the book PV.
std::vector<double> price_double(const fixtures::Book& b, const double* z) {
  std::vector<double> out(static_cast<std::size_t>(b.n_swaps + 1));
  fixtures::price_book<double>(b, z, out.data(), out.data() + b.n_swaps);
  return out;
}

// Central finite difference of every output along coordinate k.
std::vector<double> central_fd(const fixtures::Book& b, const double* z, int k, double h) {
  std::vector<double> zp(z, z + fixtures::n_knots);
  std::vector<double> zm(z, z + fixtures::n_knots);
  zp[static_cast<std::size_t>(k)] += h;
  zm[static_cast<std::size_t>(k)] -= h;
  const std::vector<double> up = price_double(b, zp.data());
  const std::vector<double> um = price_double(b, zm.data());
  std::vector<double> fd(up.size());
  for (std::size_t o = 0; o < fd.size(); ++o) fd[o] = (up[o] - um[o]) / (2.0 * h);
  return fd;
}

// |fixed_i| + |float_i| per swap (D26's leg scale) and Σ_i over the book at index n_swaps.
std::vector<double> leg_scales(const fixtures::Book& b, const double* z) {
  std::vector<double> s(static_cast<std::size_t>(b.n_swaps + 1), 0.0);
  double total = 0.0;
  for (int i = 0; i < b.n_swaps; ++i) {
    const double fx = fixtures::fixed_leg_pv<double>(b, i, z);
    const double fl = fixtures::float_leg_pv<double>(b, i, z);
    s[static_cast<std::size_t>(i)] = std::fabs(fx) + std::fabs(fl);
    total += s[static_cast<std::size_t>(i)];
  }
  s[static_cast<std::size_t>(b.n_swaps)] = total;
  return s;
}

// The gate at one state: 12 coordinate directions, the book PV's tangent vs central FD.
// Returns the worst |Δ| / tol.
double book_gate(const fixtures::Book& b, const double* z, const char* where) {
  double worst = 0.0;
  double direction[fixtures::n_knots] = {};
  for (int k = 0; k < fixtures::n_knots; ++k) {
    direction[k] = 1.0;
    const fixtures::Tangent t = fixtures::tangent(b, z, direction);
    direction[k] = 0.0;
    const std::vector<double> fd = central_fd(b, z, k, kH);
    const auto o = static_cast<std::size_t>(b.n_swaps);
    const double got = t.tangent[o];
    const double ref = fd[o];
    const double tol = std::max(kRel * std::fabs(ref), kAbsFloor);
    const double err = std::fabs(got - ref);
    worst = std::max(worst, err / tol);
    EXPECT_LE(err, tol) << where << ", knot " << k << ": Dual " << std::setprecision(17) << got << " vs FD " << ref;
    EXPECT_TRUE(std::isfinite(got)) << where << ", knot " << k;
  }
  return worst;
}

}  // namespace

// ---- the gate ----------------------------------------------------------------------------------

TEST(DualM1Tangent, BookPvTangentVsCentralFDAtTheRecordPoint) {
  const Fixture& f = fixture();
  const double worst = book_gate(f.book, f.book.z0.data(), "record point");
  std::cout << "dual m1 tangent gate (record point): worst |Δ|/tol = " << worst << " (tol = max(1e-6·|FD|, 1e-9), h = 1e-6)\n";
  RecordProperty("worst_delta_over_tol_record", std::to_string(worst));
}

TEST(DualM1Tangent, BookPvTangentVsCentralFDAtBatchStates) {
  const Fixture& f = fixture();
  double z[fixtures::n_knots];
  double worst = 0.0;
  for (int b : {1, 17, 33, 63}) {
    f.batch.state(b, z);
    worst = std::max(worst, book_gate(f.book, z, ("batch state " + std::to_string(b)).c_str()));
  }
  std::cout << "dual m1 tangent gate (states 1, 17, 33, 63): worst |Δ|/tol = " << worst << "\n";
  RecordProperty("worst_delta_over_tol_batch", std::to_string(worst));
}

// ---- supplementary -------------------------------------------------------------------------------

// Every output's gradient vs central FD. Per-swap PVs are differences of legs (D26): the FD's own
// rounding error is ~ u·c·S_i / h with S_i = |fixed_i| + |float_i|, so the tolerance is
// 1e-6·|FD| + 1e-8·S_i (1e-8 = u·c/h with c ~ 100 roundings at h = 1e-6).
TEST(DualM1Tangent, EveryOutputGradientVsCentralFD) {
  const Fixture& f = fixture();
  const fixtures::Book& b = f.book;
  const double* z = b.z0.data();
  const fixtures::Jacobian j = fixtures::jacobian_forward(b, z);
  ASSERT_EQ(j.n_outputs, b.n_swaps + 1);
  ASSERT_EQ(j.n_inputs, fixtures::n_knots);
  const std::vector<double> scale = leg_scales(b, z);
  double worst_ratio = 0.0;
  double worst_literal = 0.0;  // |Δ|/|FD| over entries with |FD| >= 1e-2·S_i·(anything): informational
  std::size_t zero_entries = 0;
  for (int k = 0; k < fixtures::n_knots; ++k) {
    const std::vector<double> fd = central_fd(b, z, k, kH);
    for (int o = 0; o < j.n_outputs; ++o) {
      const auto s = static_cast<std::size_t>(o);
      const double got = j.at(o, k);
      const double ref = fd[s];
      const double tol = kRel * std::fabs(ref) + 1e-8 * scale[s];
      const double err = std::fabs(got - ref);
      worst_ratio = std::max(worst_ratio, err / tol);
      if (err > tol) {
        ADD_FAILURE() << "output " << o << ", knot " << k << ": Dual " << std::setprecision(17) << got << " vs FD "
                      << ref << " (tol " << tol << ")";
      }
      if (ref == 0.0) {
        ++zero_entries;
        EXPECT_EQ(got, 0.0) << "output " << o << ", knot " << k << ": FD is exactly zero (no dependence) but Dual is " << got;
      } else {
        worst_literal = std::max(worst_literal, err / std::fabs(ref));
      }
    }
  }
  std::cout << "dual m1 jacobian vs FD: worst |Δ|/tol = " << worst_ratio << ", worst literal |Δ|/|FD| = " << worst_literal
            << ", exact-zero entries " << zero_entries << " of " << j.jac.size() << "\n";
}

// jacobian_forward is the n_knots tangent passes (the same instantiation, the same inputs: bitwise
// in any TU), and its value channel is the pricer's.
TEST(DualM1Tangent, JacobianRowsAreTheTangentPasses) {
  const Fixture& f = fixture();
  const fixtures::Book& b = f.book;
  const double* z = b.z0.data();
  const fixtures::Jacobian j = fixtures::jacobian_forward(b, z);
  const std::vector<double> pv = price_double(b, z);
  ASSERT_EQ(j.value.size(), pv.size());
  double direction[fixtures::n_knots] = {};
  for (int k = 0; k < fixtures::n_knots; ++k) {
    direction[k] = 1.0;
    const fixtures::Tangent t = fixtures::tangent(b, z, direction);
    direction[k] = 0.0;
    ASSERT_EQ(t.n_outputs, j.n_outputs);
    EXPECT_EQ(t.value, j.value) << k;
    for (int o = 0; o < j.n_outputs; ++o) {
      EXPECT_EQ(j.at(o, k), t.tangent[static_cast<std::size_t>(o)]) << "output " << o << ", knot " << k;
    }
  }
  // The book row is the left fold of the swap rows, to rounding (the Dual sums tangents in the
  // same fold as the values).
  for (int k = 0; k < fixtures::n_knots; ++k) {
    double acc = j.at(0, k);
    double abs_acc = std::fabs(acc);
    for (int i = 1; i < b.n_swaps; ++i) {
      acc += j.at(i, k);
      abs_acc += std::fabs(j.at(i, k));
    }
    EXPECT_NEAR(j.at(b.n_swaps, k), acc, 1e-13 * abs_acc) << k;
  }
}

// One pass of Dual<12> agrees with 12 passes of Dual<1> to rounding (bitwise in the E0 TU).
TEST(DualM1Tangent, WideJacobianAgreesWithNarrow) {
  const Fixture& f = fixture();
  const fixtures::Book& b = f.book;
  const double* z = b.z0.data();
  const fixtures::Jacobian narrow = fixtures::jacobian_forward(b, z);
  const fixtures::Jacobian wide = fixtures::jacobian_forward_wide(b, z);
  ASSERT_EQ(wide.n_outputs, narrow.n_outputs);
  ASSERT_EQ(wide.n_inputs, narrow.n_inputs);
  ASSERT_EQ(wide.jac.size(), narrow.jac.size());
  EXPECT_EQ(wide.value, narrow.value);
  std::size_t bitwise_mismatches = 0;
  for (int o = 0; o < narrow.n_outputs; ++o) {
    double row_scale = 0.0;
    for (int k = 0; k < narrow.n_inputs; ++k) row_scale = std::max(row_scale, std::fabs(narrow.at(o, k)));
    for (int k = 0; k < narrow.n_inputs; ++k) {
      const double a = wide.at(o, k);
      const double c = narrow.at(o, k);
      if (std::memcmp(&a, &c, sizeof a) != 0) ++bitwise_mismatches;
      EXPECT_LE(std::fabs(a - c), 1e-11 * row_scale) << "output " << o << ", knot " << k;
    }
  }
  std::cout << "dual m1 wide vs narrow (this TU's flags): " << bitwise_mismatches << " of " << narrow.jac.size()
            << " entries differ bitwise\n";
}

// tangent(u) = J·u to rounding for a general direction (a different summation order, so not bitwise).
TEST(DualM1Tangent, TangentIsLinearInTheDirection) {
  const Fixture& f = fixture();
  const fixtures::Book& b = f.book;
  const double* z = b.z0.data();
  const fixtures::Jacobian j = fixtures::jacobian_forward(b, z);
  double u[fixtures::n_knots];
  for (int k = 0; k < fixtures::n_knots; ++k) u[k] = ((k % 2) ? -1.0 : 1.0) * (1.0 + k) / 12.0;
  const fixtures::Tangent t = fixtures::tangent(b, z, u);
  EXPECT_EQ(t.value, j.value);
  for (int o = 0; o < j.n_outputs; ++o) {
    double ju = 0.0;
    double abs_ju = 0.0;
    for (int k = 0; k < fixtures::n_knots; ++k) {
      ju += j.at(o, k) * u[k];
      abs_ju += std::fabs(j.at(o, k) * u[k]);
    }
    EXPECT_NEAR(t.tangent[static_cast<std::size_t>(o)], ju, 1e-12 * abs_ju) << o;
  }
  // The zero direction has a zero tangent everywhere.
  double zero[fixtures::n_knots] = {};
  const fixtures::Tangent t0 = fixtures::tangent(b, z, zero);
  for (double d : t0.tangent) EXPECT_EQ(d, 0.0);
}
