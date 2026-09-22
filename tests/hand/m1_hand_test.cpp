// M1/P5: the correctness gate of the hand-fused kernel (fast, fused arithmetic) against the
// templated double maths, E1 (D8), at the record point and at all 64 batch states; the batch path
// bitwise against the single-state path; zero allocations per eval.
//
// On the tolerance. A swap pv is side·(fixed − float), a difference of two legs that at the record
// point cancel to |ε_i|·par·annuity: 1000 draws of ε ~ U(−0.05, 0.05) include |ε| ~ 1e-5, and at the
// batch states the 10 bp shocks make some swaps cancel to 1e-6 of their leg size. Rounding-level
// (E1) agreement of the legs, ~1e-15, is therefore up to 1e-9 *relative to such a pv*, for any
// kernel that does not reproduce the oracle's operations bit for bit (the reference-arithmetic
// mode does, and is gated E0 in m1_hand_e0_test.cpp). The well-posed E1 statement is relative to
// the scale of the computation that produced the value:
//   swap i:   |Δpv_i|  <= 1e-12 · (|fixed_i| + |float_i|)
//   book:     |Δbook|  <= 1e-12 · Σ_i |pv_i|
// Those are the gate. The literal "1e-12 relative to the value itself" is asserted too wherever
// it is well posed (|pv_i| >= 1e-2 · (|fixed_i| + |float_i|)), and for the book pv as specified;
// the count of ill-conditioned swap-states where the literal check cannot hold is reported.
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <new>
#include <vector>

#include "epykos/hand/m1_hand_kernel.hpp"
#include "epykos/maths/m1/book.hpp"
#include "epykos/maths/m1/price.hpp"
#include "epykos/maths/m1/reference.hpp"
#include "epykos/version.hpp"

namespace m1 = epykos::m1;
using epykos::hand::HandArith;
using epykos::hand::HandExp;
using epykos::hand::M1HandKernel;
using epykos::hand::M1HandOptions;

// ---- allocation counter: every operator new in this program passes through here.
namespace {
std::uint64_t g_allocations = 0;
}
void* operator new(std::size_t n) {
  ++g_allocations;
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) {
  ++g_allocations;
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {

constexpr double kTol = 1e-12;

const m1::Book& book() {
  static const m1::Book b = m1::make_m1_book();
  return b;
}
const m1::Batch& batch() {
  static const m1::Batch b = m1::make_m1_batch();
  return b;
}
// The oracle in this TU (the preset's contraction; E1 either way).
const m1::ReferenceTable& oracle() {
  static const m1::ReferenceTable t = m1::m1_reference_values(book(), batch());
  return t;
}
// |fixed_i| + |float_i| per state (index b = -1 is the record point), the scale of each swap pv.
const std::vector<double>& leg_scale() {
  static const std::vector<double> s = [] {
    std::vector<double> v(65u * 1000u);
    double z[12];
    for (int b = -1; b < 64; ++b) {
      if (b < 0) {
        for (int k = 0; k < 12; ++k) z[k] = book().z0[static_cast<std::size_t>(k)];
      } else {
        batch().state(b, z);
      }
      for (int i = 0; i < 1000; ++i) {
        v[static_cast<std::size_t>(b + 1) * 1000u + static_cast<std::size_t>(i)] =
            std::fabs(m1::fixed_leg_pv<double>(book(), i, z)) + std::fabs(m1::float_leg_pv<double>(book(), i, z));
      }
    }
    return v;
  }();
  return s;
}

struct GateStats {
  double worst_swap_scaled = 0.0;   // max |Δpv| / (|fixed| + |float|)
  double worst_swap_literal = 0.0;  // max |Δpv| / |pv| over well-conditioned swap-states
  double worst_book_scaled = 0.0;          // max |Δbook| / Σ|pv|
  double worst_book_literal = 0.0;         // max |Δbook| / |book| over well-conditioned states
  double worst_book_literal_all = 0.0;     // the same over every state (reported)
  int ill_conditioned_states = 0;          // states with |book| < 1e-2 · Σ|pv|
  int ill_conditioned = 0;          // swap-states with |pv| < 1e-2 · scale
  int ill_conditioned_literal_fail = 0;
  int failures = 0;
};

// Runs eval at the record point and the 64 states and applies the gate.
GateStats run_gate(const M1HandKernel& k, const char* label) {
  GateStats g;
  const m1::ReferenceTable& t = oracle();
  const std::vector<double>& scale = leg_scale();
  std::vector<double> pv(1001);
  double z[12];
  for (int b = -1; b < 64; ++b) {
    const double* ref = b < 0 ? t.record.data() : t.state(b);
    if (b < 0) {
      for (int kk = 0; kk < 12; ++kk) z[kk] = book().z0[static_cast<std::size_t>(kk)];
    } else {
      batch().state(b, z);
    }
    k.eval(z, pv.data(), pv.data() + 1000);
    double sum_abs = 0.0;
    for (int i = 0; i < 1000; ++i) {
      const std::size_t si = static_cast<std::size_t>(i);
      const double sc = scale[static_cast<std::size_t>(b + 1) * 1000u + si];
      const double d = std::fabs(pv[si] - ref[i]);
      sum_abs += std::fabs(ref[i]);
      const double scaled = d / sc;
      g.worst_swap_scaled = std::max(g.worst_swap_scaled, scaled);
      if (!(scaled <= kTol)) {
        if (++g.failures <= 10) {
          ADD_FAILURE() << label << ": swap " << i << " state " << b << ": |Δ|/(|fixed|+|float|) = " << scaled;
        }
      }
      const double literal = d / std::fabs(ref[i]);
      if (std::fabs(ref[i]) >= 1e-2 * sc) {
        g.worst_swap_literal = std::max(g.worst_swap_literal, literal);
        if (!(literal <= kTol)) {
          if (++g.failures <= 10) {
            ADD_FAILURE() << label << ": swap " << i << " state " << b << ": |Δ|/|pv| = " << literal
                          << " with |pv|/scale = " << std::fabs(ref[i]) / sc;
          }
        }
      } else {
        ++g.ill_conditioned;
        if (!(literal <= kTol)) ++g.ill_conditioned_literal_fail;
      }
    }
    const double db = std::fabs(pv[1000] - ref[1000]);
    const double bscaled = db / sum_abs;
    const double bliteral = db / std::fabs(ref[1000]);
    g.worst_book_scaled = std::max(g.worst_book_scaled, bscaled);
    g.worst_book_literal_all = std::max(g.worst_book_literal_all, bliteral);
    if (!(bscaled <= kTol)) {
      ++g.failures;
      ADD_FAILURE() << label << ": book state " << b << ": |Δ|/Σ|pv| = " << bscaled;
    }
    if (std::fabs(ref[1000]) >= 1e-2 * sum_abs) {
      g.worst_book_literal = std::max(g.worst_book_literal, bliteral);
      if (!(bliteral <= kTol)) {
        ++g.failures;
        ADD_FAILURE() << label << ": book state " << b << ": |Δ|/|book| = " << bliteral
                      << " with |book|/Σ|pv| = " << std::fabs(ref[1000]) / sum_abs;
      }
    } else {
      ++g.ill_conditioned_states;
    }
  }
  std::cout << std::setprecision(3) << label << ": worst |Δpv|/(|fixed|+|float|) = " << g.worst_swap_scaled
            << ", worst |Δpv|/|pv| (well-conditioned) = " << g.worst_swap_literal
            << ", worst |Δbook|/Σ|pv| = " << g.worst_book_scaled << ", worst |Δbook|/|book| = " << g.worst_book_literal
            << " (well-conditioned) / " << g.worst_book_literal_all << " (all 65 states, " << g.ill_conditioned_states
            << " of them with |book| < 1e-2·Σ|pv|)"
            << "; ill-conditioned swap-states " << g.ill_conditioned << " (literal check fails on "
            << g.ill_conditioned_literal_fail << " of them, expected)\n";
  return g;
}

}  // namespace

TEST(M1Hand, Structure) {
  const M1HandKernel k(book());
  EXPECT_EQ(k.n_swaps(), 1000);
  EXPECT_EQ(k.n_knots(), 12);
  // Every coupon row of the book is exactly one hand row: 200 realised first coupons are plain.
  EXPECT_EQ(k.n_plain_rows() + k.n_float_rows(), book().n_rows);
  EXPECT_EQ(k.n_float_rows(), book().n_rows / 2 - 200);
  EXPECT_EQ(k.n_plain_rows(), book().n_rows / 2 + 200);
  // Unique times: {0} ∪ every coupon end; far fewer than rows (exp once per time).
  EXPECT_GT(k.n_times(), 30);
  EXPECT_LT(k.n_times(), book().n_rows / 4);
  // Times are numbered in first-use order, so they are a permutation of the sorted unique times.
  std::vector<double> t = k.times();
  ASSERT_EQ(static_cast<int>(t.size()), k.n_times());
  std::sort(t.begin(), t.end());
  EXPECT_EQ(t.front(), 0.0);
  for (std::size_t u = 1; u < t.size(); ++u) EXPECT_LT(t[u - 1], t[u]);  // unique
  EXPECT_EQ(k.max_batch(), 64);
  EXPECT_EQ(k.lane_tile(), 32);
  // The processing order is a permutation of the swaps, grouped by (tenor, seasoned): at most
  // 30 tenors × 2, and the seasoned swaps (i < 200) never share a group with the others.
  const std::vector<int>& order = k.swap_order();
  ASSERT_EQ(order.size(), 1000u);
  std::vector<int> seen(1000, 0);
  for (int i : order) {
    ASSERT_GE(i, 0);
    ASSERT_LT(i, 1000);
    ++seen[static_cast<std::size_t>(i)];
  }
  for (int c : seen) EXPECT_EQ(c, 1);
  EXPECT_GT(k.n_groups(), 30);
  EXPECT_LE(k.n_groups(), 60);
  int groups = 1;
  for (std::size_t pos = 1; pos < order.size(); ++pos) {
    const std::size_t a = static_cast<std::size_t>(order[pos - 1]), b = static_cast<std::size_t>(order[pos]);
    if (book().tenor[a] != book().tenor[b] || book().seasoned[a] != book().seasoned[b]) ++groups;
  }
  EXPECT_EQ(groups, k.n_groups());
  std::cout << "hand tables: " << k.n_times() << " unique times, " << k.n_plain_rows() << " plain rows, "
            << k.n_float_rows() << " float rows, " << k.n_groups() << " (tenor, seasoned) groups\n";
}

TEST(M1Hand, GateDefaultOptions) {
  const M1HandKernel k(book());
  const GateStats g = run_gate(k, "fused, shared reciprocal, exp_poly");
  EXPECT_EQ(g.failures, 0);
  EXPECT_LE(g.worst_swap_scaled, kTol);
  EXPECT_LE(g.worst_book_literal, kTol);
}

TEST(M1Hand, GatePerRowDivision) {
  M1HandOptions o;
  o.shared_reciprocal = false;
  const M1HandKernel k(book(), o);
  EXPECT_EQ(run_gate(k, "fused, per-row division, exp_poly").failures, 0);
}

TEST(M1Hand, GateStdExp) {
  M1HandOptions o;
  o.exp = HandExp::std_exp;
  const M1HandKernel k(book(), o);
  EXPECT_EQ(run_gate(k, "fused, shared reciprocal, std::exp").failures, 0);
}

// The fast kernel really is E1 and not E0: it differs from the oracle somewhere (otherwise the
// fused mode would not be exercising its rewrites).
TEST(M1Hand, FusedModeDiffersFromOracleByRoundingOnly) {
  const M1HandKernel k(book());
  const m1::ReferenceTable& t = oracle();
  std::vector<double> pv(1001);
  k.eval(book().z0.data(), pv.data(), pv.data() + 1000);
  EXPECT_NE(std::memcmp(pv.data(), t.record.data(), 1001 * sizeof(double)), 0);
}

// eval_batch(B = 64) is bitwise 64 calls of eval; likewise for a B that is not a multiple of the
// lane tile and for B = 1.
TEST(M1Hand, BatchIsSingleStateBitwise) {
  for (M1HandOptions o : {M1HandOptions{}, [] {
                            M1HandOptions x;
                            x.shared_reciprocal = false;
                            return x;
                          }(),
                          [] {
                            M1HandOptions x;
                            x.exp = HandExp::std_exp;
                            return x;
                          }()}) {
    const M1HandKernel k(book(), o);
    std::vector<double> single(1001);
    double z[12];
    for (int B : {64, 1, 5, 8, 13}) {
      std::vector<double> zb(12u * static_cast<std::size_t>(B)), pv(1000u * static_cast<std::size_t>(B)),
          bpv(static_cast<std::size_t>(B));
      for (int kk = 0; kk < 12; ++kk) {
        for (int b = 0; b < B; ++b) zb[static_cast<std::size_t>(kk) * static_cast<std::size_t>(B) + static_cast<std::size_t>(b)] = batch().at(kk, b);
      }
      k.eval_batch(zb.data(), B, pv.data(), bpv.data());
      int bad = 0;
      for (int b = 0; b < B; ++b) {
        batch().state(b, z);
        k.eval(z, single.data(), single.data() + 1000);
        for (int i = 0; i < 1000; ++i) {
          if (std::memcmp(&pv[static_cast<std::size_t>(i) * static_cast<std::size_t>(B) + static_cast<std::size_t>(b)],
                          &single[static_cast<std::size_t>(i)], sizeof(double)) != 0) {
            ++bad;
          }
        }
        if (std::memcmp(&bpv[static_cast<std::size_t>(b)], &single[1000], sizeof(double)) != 0) ++bad;
      }
      EXPECT_EQ(bad, 0) << "B = " << B << ", shared_reciprocal = " << o.shared_reciprocal
                        << ", exp = " << static_cast<int>(o.exp);
    }
  }
}

TEST(M1Hand, BatchRejectsOutOfRangeB) {
  M1HandOptions o;
  o.max_batch = 16;
  const M1HandKernel k(book(), o);
  std::vector<double> zb(12u * 17u), pv(1000u * 17u), bpv(17);
  EXPECT_THROW(k.eval_batch(zb.data(), 17, pv.data(), bpv.data()), std::invalid_argument);
  EXPECT_THROW(k.eval_batch(zb.data(), 0, pv.data(), bpv.data()), std::invalid_argument);
  for (int kk = 0; kk < 12; ++kk) {
    for (int b = 0; b < 16; ++b) zb[static_cast<std::size_t>(kk) * 16u + static_cast<std::size_t>(b)] = batch().at(kk, b);
  }
  EXPECT_NO_THROW(k.eval_batch(zb.data(), 16, pv.data(), bpv.data()));
}

TEST(M1Hand, ZeroAllocationsPerEval) {
  const M1HandKernel k(book());
  std::vector<double> pv(1000u * 64u), bpv(64), single(1001);
  // Warm once (nothing should allocate even on the first call, but be strict about the steady state).
  k.eval(book().z0.data(), single.data(), single.data() + 1000);
  k.eval_batch(batch().z.data(), 64, pv.data(), bpv.data());
  const std::uint64_t before = g_allocations;
  for (int rep = 0; rep < 3; ++rep) {
    k.eval(book().z0.data(), single.data(), single.data() + 1000);
    k.eval_batch(batch().z.data(), 64, pv.data(), bpv.data());
    k.eval_batch(batch().z.data(), 5, pv.data(), bpv.data());
  }
  EXPECT_EQ(g_allocations - before, 0u);
  // And the first call after construction does not allocate either.
  const M1HandKernel fresh(book());
  const std::uint64_t before2 = g_allocations;
  fresh.eval(book().z0.data(), single.data(), single.data() + 1000);
  fresh.eval_batch(batch().z.data(), 64, pv.data(), bpv.data());
  EXPECT_EQ(g_allocations - before2, 0u);
}

TEST(M1Hand, Report) {
  const M1HandKernel k(book());
  std::vector<double> pv(1001);
  k.eval(book().z0.data(), pv.data(), pv.data() + 1000);
  std::cout << std::setprecision(17) << "m1 hand: build " << epykos::build_config() << " [" << epykos::build_flags()
            << "]\n  record point: book pv = " << pv[1000] << " (oracle " << oracle().record_book_pv() << ")\n";
  SUCCEED();
}
