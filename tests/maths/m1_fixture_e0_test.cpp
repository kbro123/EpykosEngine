// M1/P7: the fixture bits do not depend on the compiler or the preset (D13, D25).
//
// (1) Everything src/maths/m1/book_e0.cpp computes with floating-point arithmetic — par rates,
//     fixed rates, realised rates, accrual fractions, discount times, the batch states — is
//     recomputed here, in a contraction-free TU, from the same draws and the same templates, and
//     must be bitwise what libepykos holds. A compiler that contracted the book generator (GCC 13
//     -O3 in C++ mode, before D25) fails this.
// (2) The parts of the fixture that never touch libm — integer draws, uniform draws, ε, R, τ, t
//     — have literal digests: the same bits on every toolchain, or the fixture has changed.
// (3) libm-dependent parts (notionals through exp/log, par rates and fixed rates through exp,
//     the batch's tail Gaussians through log, the oracle) are printed as digests, never asserted
//     against a literal: exp and log differ across libms in the last bit.
#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <vector>

#include "epykos/maths/calendar.hpp"
#include "epykos/maths/m1/book.hpp"
#include "epykos/maths/m1/curve.hpp"
#include "epykos/maths/m1/price.hpp"
#include "epykos/maths/m1/reference.hpp"
#include "epykos/rng/philox.hpp"
#include "epykos/version.hpp"

#ifndef EPYKOS_FP_CONTRACT_OFF
#error "an _e0_test.cpp TU must see EPYKOS_FP_CONTRACT_OFF"
#endif

namespace m1 = epykos::m1;
namespace rng = epykos::rng;
namespace calendar = epykos::calendar;

namespace {

std::uint64_t bits(double v) {
  std::uint64_t u;
  std::memcpy(&u, &v, sizeof u);
  return u;
}

// FNV-1a over 64-bit words.
struct Digest {
  std::uint64_t h = 1469598103934665603ull;
  void add(std::uint64_t w) {
    for (int i = 0; i < 8; ++i) {
      h ^= (w >> (8 * i)) & 0xffu;
      h *= 1099511628211ull;
    }
  }
  void add(double d) { add(bits(d)); }
  void add(int i) { add(static_cast<std::uint64_t>(static_cast<std::int64_t>(i))); }
  template <class T>
  void add_all(const std::vector<T>& v) {
    for (const T& x : v) add(x);
  }
};

std::size_t idx(int i) { return static_cast<std::size_t>(i); }

const m1::Book& book() {
  static const m1::Book b = m1::make_m1_book();
  return b;
}
const m1::Batch& batch() {
  static const m1::Batch b = m1::make_m1_batch();
  return b;
}

}  // namespace

// (1) The generator's arithmetic, recomputed in this contraction-free TU, is bitwise the library's.
TEST(M1FixtureE0, BookArithmeticIsContractionFree) {
  const m1::Book& b = book();
  ASSERT_EQ(b.n_swaps, m1::n_swaps);
  const double z1 = m1::zero_rate<double>(b.knot_t.data(), b.z0.data(), m1::n_knots, 1.0);
  EXPECT_EQ(bits(z1), bits(b.z0[4])) << "t = 1 is knot 4";
  int bad_par = 0, bad_k = 0, bad_r = 0, bad_eps = 0;
  for (int i = 0; i < b.n_swaps; ++i) {
    // Draws 4 and 5 of sub-stream i (the generator's order: tenor, notional, side, offset, ε, R multiplier).
    rng::Philox g(m1::default_seed, static_cast<std::uint64_t>(i), 4);
    const double eps = g.uniform_range(-0.05, 0.05);
    const double r_mult = g.uniform_range(-0.1, 0.1);
    if (bits(eps) != bits(b.eps[idx(i)])) ++bad_eps;
    const double par = m1::float_leg_pv<double>(b, i, b.z0.data()) / m1::annuity<double>(b, i, b.z0.data());
    if (bits(par) != bits(b.par[idx(i)])) ++bad_par;
    if (bits(par * (1.0 + b.eps[idx(i)])) != bits(b.fixed_rate[idx(i)])) ++bad_k;
    const double r = b.seasoned[idx(i)] ? z1 * (1.0 + r_mult) : 0.0;
    if (bits(r) != bits(b.realised_rate[idx(i)])) ++bad_r;
  }
  EXPECT_EQ(bad_eps, 0) << "ε: uniform_range draws";
  EXPECT_EQ(bad_par, 0) << "par rates: float leg / annuity recomputed without contraction";
  EXPECT_EQ(bad_k, 0) << "K = par (1 + ε)";
  EXPECT_EQ(bad_r, 0) << "R = z(1) (1 + U)";
  int bad_rows = 0;
  for (int r = 0; r < b.n_rows; ++r) {
    const int s = b.row_start_day[idx(r)], e = b.row_end_day[idx(r)];
    if (bits(b.row_tau[idx(r)]) != bits(calendar::year_fraction(s, e)) ||
        bits(b.row_t_start[idx(r)]) != bits(calendar::time_of_day(s)) ||
        bits(b.row_t_end[idx(r)]) != bits(calendar::time_of_day(e))) {
      ++bad_rows;
    }
  }
  EXPECT_EQ(bad_rows, 0) << "τ and t per row";
}

TEST(M1FixtureE0, BatchArithmeticIsContractionFree) {
  const m1::Batch& bt = batch();
  ASSERT_EQ(bt.n_states, m1::n_states);
  int bad = 0;
  for (int s = 0; s < bt.n_states; ++s) {
    rng::Philox g(m1::default_seed, m1::batch_substream_base + static_cast<std::uint64_t>(s));
    for (int k = 0; k < m1::n_knots; ++k) {
      const double delta = (s == 0) ? 0.0 : m1::batch_sigma * g.gaussian();
      const double z = m1::record_state[idx(k)] + delta;  // the mul + add is the contraction candidate
      if (bits(z) != bits(bt.at(k, s))) ++bad;
    }
  }
  EXPECT_EQ(bad, 0) << "z^(b) = z + σ·N(0,1)";
}

// (2) Literal digests of the libm-independent fixture (identical on every toolchain). The values
// were taken on Apple clang 21 (x86-64) and confirmed on GCC 13 (Linux); a change means the
// fixture, the RNG or the calendar changed, which WORKLOADS.md §M1 does not allow.
TEST(M1FixtureE0, LibmIndependentDigestsAreToolchainInvariant) {
  const m1::Book& b = book();
  Digest ints;  // tenor, side, d0, seasoned, row table days and flags
  ints.add_all(b.tenor);
  ints.add_all(b.side);
  ints.add_all(b.d0);
  for (std::uint8_t s : b.seasoned) ints.add(static_cast<int>(s));
  ints.add_all(b.row_begin);
  ints.add_all(b.row_swap);
  ints.add_all(b.row_leg);
  ints.add_all(b.row_start_day);
  ints.add_all(b.row_end_day);
  for (std::uint8_t s : b.row_is_realised_first) ints.add(static_cast<int>(s));
  Digest reals;  // ε, R, τ, t: arithmetic on uniform draws and integers, no libm
  reals.add_all(b.eps);
  reals.add_all(b.realised_rate);
  reals.add_all(b.row_tau);
  reals.add_all(b.row_t_start);
  reals.add_all(b.row_t_end);
  Digest draws;  // the six uniform draws of every swap sub-stream and the 12 of every batch sub-stream
  for (int i = 0; i < m1::n_swaps; ++i) {
    const rng::Philox g(m1::default_seed, static_cast<std::uint64_t>(i));
    for (std::uint64_t n = 0; n < 6; ++n) draws.add(g.uniform_at(n));
  }
  for (int s = 0; s < m1::n_states; ++s) {
    const rng::Philox g(m1::default_seed, m1::batch_substream_base + static_cast<std::uint64_t>(s));
    for (std::uint64_t n = 0; n < static_cast<std::uint64_t>(m1::n_knots); ++n) draws.add(g.uniform_open_at(n));
  }
  std::cout << std::hex << "m1 fixture digests: ints " << ints.h << ", reals " << reals.h << ", draws " << draws.h
            << std::dec << "\n";
  EXPECT_EQ(ints.h, 0x8ca99279f39d4382ull);
  EXPECT_EQ(reals.h, 0x2c529acc14911f18ull);
  EXPECT_EQ(draws.h, 0x4eb5aa61b3a48fdeull);

}

// (3) libm-dependent digests: reported for the log, never asserted.
TEST(M1FixtureE0, ReportLibmDependentDigests) {
  const m1::Book& b = book();
  Digest notional, par, k, states;
  notional.add_all(b.notional);
  par.add_all(b.par);
  k.add_all(b.fixed_rate);
  states.add_all(batch().z);
  const m1::ReferenceTable t = m1::m1_reference_values(b, batch());
  Digest oracle;
  oracle.add_all(t.record);
  oracle.add_all(t.batch);
  std::cout << "m1 fixture (libm-dependent, informational): build " << epykos::build_config() << " ["
            << epykos::build_flags() << "]\n"
            << std::hex << "  notional " << notional.h << ", par " << par.h << ", K " << k.h << ", batch states "
            << states.h << ", oracle " << oracle.h << std::dec << "\n";
  SUCCEED();
}
