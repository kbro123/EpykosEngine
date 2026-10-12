// PRINCIPLES.md §1b invariant I2, E0 gate: nesting the forward-mode scalar must not disturb the
// first-order answer. One pass of price_book<Dual<12, Dual<12>>> is checked against the two passes
// it must agree with, BITWISE:
//
//   (1) its value channel r.v.v            is price_book<double>;
//   (2) its outer tangent channel r.d[i].v is the Dual<12> pass's slot i;
//   (3) its inner tangent channel r.v.d[j] is the same Dual<12> pass's slot j.
//
// §5.2a case 1: all three sides are the same expressions evaluated by the same sequence of IEEE
// operations on the same inputs — the Scalar differs, the arithmetic does not. So this stays
// bitwise, and a failure is a defect in the scalar, never a reason to loosen the gate.
//
// This TU is compiled with -ffp-contract=off in every preset (the _e0_test.cpp convention), which
// is what makes (1)–(3) theorems. Under contraction they are NOT bitwise and must not be asserted
// so: measured on this book at -O2 -march=x86-64-v3 with contraction allowed, the value channel
// drifts up to 1.3e5 ulp on the near-par swap PVs (cancellation, ~1e-11 relative) and the outer
// tangent up to 512 ulp, because the compiler contracts the two instantiations differently. That
// is the whole reason the E0 gates live in their own TUs (D8, D25).
//
// The second derivative itself has no bitwise reference — nothing else in the engine computes one
// — so it is gated against central differences in tests/maths/second_order_test.cpp.
#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>
#include <vector>

#include "epykos/fixtures/m1_book.hpp"
#include "epykos/fixtures/m1_price.hpp"
#include "epykos/fixtures/m1_tangent.hpp"
#include "epykos/fixtures/second_order.hpp"
#include "epykos/scalar/dual.hpp"

#ifndef EPYKOS_FP_CONTRACT_OFF
#error "an _e0_test.cpp TU must see EPYKOS_FP_CONTRACT_OFF"
#endif

namespace fx = epykos::fixtures;
using epykos::Dual;

namespace {

constexpr int N = fx::n_knots;  // 12
using Inner = Dual<N>;
using Outer = Dual<N, Inner>;

std::uint64_t bits(double x) {
  std::uint64_t b = 0;
  std::memcpy(&b, &x, sizeof b);
  return b;
}

const fx::Book& book() {
  static const fx::Book b = fx::make_m1_book();
  return b;
}

// One nested pass over the whole book at state z: every swap PV and the book PV.
std::vector<Outer> nested_pass(const double* z) {
  std::array<Outer, N> x{};
  for (int k = 0; k < N; ++k) x[static_cast<std::size_t>(k)] = Outer::variable(Inner::variable(z[k], k), k);
  std::vector<Outer> out(static_cast<std::size_t>(book().n_swaps) + 1);
  fx::price_book<Outer>(book(), x.data(), out.data(), out.data() + book().n_swaps);
  return out;
}

void check_state(const double* z, const std::string& what) {
  const std::vector<Outer> nested = nested_pass(z);

  // (1) the value channel against the double oracle, computed in this contraction-free TU.
  std::vector<double> dv(static_cast<std::size_t>(book().n_swaps) + 1);
  fx::price_book<double>(book(), z, dv.data(), dv.data() + book().n_swaps);
  std::size_t bad = 0;
  for (std::size_t o = 0; o < dv.size(); ++o) {
    if (bits(nested[o].v.v) != bits(dv[o])) {
      if (bad < 3) {
        ADD_FAILURE() << what << ": value channel of output " << o << " is " << nested[o].v.v << " (" << std::hex
                      << bits(nested[o].v.v) << "), price_book<double> is " << std::dec << dv[o] << " (" << std::hex
                      << bits(dv[o]) << ")" << std::dec;
      }
      ++bad;
    }
  }
  EXPECT_EQ(bad, 0u) << what << ": " << bad << " of " << dv.size() << " value channels are not bitwise the double pass";

  // (2) and (3) both first-derivative channels against the existing Dual<12> pass.
  const fx::Jacobian jw = fx::jacobian_forward_wide(book(), z);
  ASSERT_EQ(jw.n_outputs, static_cast<int>(dv.size()));
  std::size_t bad_outer = 0, bad_inner = 0;
  for (int o = 0; o < jw.n_outputs; ++o) {
    const Outer& r = nested[static_cast<std::size_t>(o)];
    for (int k = 0; k < N; ++k) {
      const double want = jw.at(o, k);
      if (bits(r.d[static_cast<std::size_t>(k)].v) != bits(want)) {
        if (bad_outer < 3) {
          ADD_FAILURE() << what << ": outer tangent [" << o << "][" << k << "] is "
                        << r.d[static_cast<std::size_t>(k)].v << ", Dual<12> gives " << want;
        }
        ++bad_outer;
      }
      if (bits(r.v.d[static_cast<std::size_t>(k)]) != bits(want)) {
        if (bad_inner < 3) {
          ADD_FAILURE() << what << ": inner tangent [" << o << "][" << k << "] is "
                        << r.v.d[static_cast<std::size_t>(k)] << ", Dual<12> gives " << want;
        }
        ++bad_inner;
      }
    }
  }
  EXPECT_EQ(bad_outer, 0u) << what << ": " << bad_outer << " outer tangent slots differ from the Dual<12> pass";
  EXPECT_EQ(bad_inner, 0u) << what << ": " << bad_inner << " inner tangent slots differ from the Dual<12> pass";
}

}  // namespace

TEST(SecondOrderE0, NestedPassKeepsTheFirstOrderAnswerAtTheRecordPoint) {
  std::vector<double> z(fx::record_state.begin(), fx::record_state.end());
  check_state(z.data(), "record point");
}

TEST(SecondOrderE0, NestedPassKeepsTheFirstOrderAnswerOnBatchStates) {
  const fx::Batch batch = fx::make_m1_batch();
  std::vector<double> z(static_cast<std::size_t>(N));
  for (int b : {1, 31, 63}) {
    batch.state(b, z.data());
    check_state(z.data(), "batch state " + std::to_string(b));
  }
}

// The two first-derivative channels are the same gradient by two different routes through the
// nested scalar. Under -ffp-contract=off they are the same operations, so they agree bitwise —
// which is the cheapest possible check that the seeding convention is self-consistent.
TEST(SecondOrderE0, TheTwoGradientChannelsAgreeBitwise) {
  std::vector<double> z(fx::record_state.begin(), fx::record_state.end());
  auto f = [](const auto* x) {
    using Scalar = std::decay_t<decltype(*x)>;
    std::vector<Scalar> out(static_cast<std::size_t>(book().n_swaps));
    Scalar total;
    fx::price_book<Scalar>(book(), x, out.data(), &total);
    return total;
  };
  const fx::SecondOrder<N> s = fx::second_order<N>(f, z.data());
  for (int k = 0; k < N; ++k) {
    EXPECT_EQ(bits(s.grad[static_cast<std::size_t>(k)]), bits(s.grad_inner[static_cast<std::size_t>(k)]))
        << "gradient channel " << k << ": outer " << s.grad[static_cast<std::size_t>(k)] << ", inner "
        << s.grad_inner[static_cast<std::size_t>(k)];
  }
}
