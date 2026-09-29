// M2/Q3 tolerance gates on the M1 book (docs/WORKLOADS.md §M2 "Adjoint"):
//   (1) d(book pv)/dz from the adjoint vs central finite differences of price_book<double>
//       (h = 1e-6) at the record point and at 8 states of the M2 ball;
//   (2) per-swap gradients for the 50 swaps drawn from sub-stream 300000, seeded out_bar = e_i,
//       one lane per swap in a single batched call, vs the same finite differences;
//   (3) linearity: the adjoint of out_bar1 + out_bar2 equals the sum of the two adjoints.
//
// Tolerance: every gradient component at relative 1e-6 with an absolute floor of 1e-9,
// |adj − fd| <= max(1e-6·|fd|, 1e-9). A central difference with h = 1e-6 carries rounding noise
// of about eps·|leg|/h in the units of the derivative (legs of size N·K·annuity, up to ~1e8, so
// up to ~1e-2 absolute), which is far below 1e-6 of every non-zero component on this book (the
// smallest are ~1e2, a 0.004 knot weight on a seasoned swap's first coupon); the test prints the
// worst literal ratio and, for any failing component, the FD noise bound, so that a failure can
// be read as the adjoint's or the difference's. Exactly-zero derivatives (knots no coupon time
// reaches, and the 7-day knot, which only DF(0) = exp(−z·0) touches) are exact zeros on both sides.
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

#include "epykos/adjoint/adjoint.hpp"
#include "epykos/fixtures/m1_book.hpp"
#include "epykos/fixtures/m1_price.hpp"
#include "epykos/fixtures/record_m1.hpp"
#include "epykos/ir/program.hpp"
#include "epykos/ir/signature.hpp"
#include "epykos/rng/philox.hpp"
#include "epykos/tape/tape.hpp"

namespace fixtures = epykos::fixtures;
namespace ir = epykos::ir;
namespace adjoint = epykos::adjoint;

namespace {

constexpr int n_knots = fixtures::n_knots;
constexpr int n_out = fixtures::n_swaps + 1;
constexpr int book_ordinal = fixtures::n_swaps;
constexpr double fd_h = 1e-6;
constexpr double rel_tol = 1e-6;
constexpr double abs_floor = 1e-9;
constexpr double eps = std::numeric_limits<double>::epsilon();

struct Fixture {
  fixtures::Book book;
  epykos::Tape tape;
  ir::Program program;
};

const Fixture& fixture() {
  static const Fixture f = [] {
    Fixture x;
    x.book = fixtures::make_m1_book();
    x.tape = fixtures::record_m1(x.book);
    x.program = ir::infer(x.tape);
    return x;
  }();
  return f;
}

// The M2 state ball (WORKLOADS.md §M2): z^(r) = z + ρ·u, u uniform on [−1, 1]^12 from sub-stream
// 200000 + r (draw k for knot k), ρ = 0.0050.
std::vector<double> ball_state(const fixtures::Book& book, int r) {
  std::vector<double> z(static_cast<std::size_t>(n_knots));
  epykos::rng::Philox g(fixtures::default_seed, 200000 + static_cast<std::uint64_t>(r));
  for (int k = 0; k < n_knots; ++k) z[static_cast<std::size_t>(k)] = book.z0[static_cast<std::size_t>(k)] + 0.0050 * g.uniform_range(-1.0, 1.0);
  return z;
}

// The 50 swaps of sub-stream 300000: draw j is a uniform integer in [0, n_swaps).
std::vector<int> fd_swaps() {
  std::vector<int> s;
  epykos::rng::Philox g(fixtures::default_seed, 300000);
  for (int j = 0; j < 50; ++j) s.push_back(static_cast<int>(g.uniform_int(0, fixtures::n_swaps - 1)));
  return s;
}

std::vector<double> price(const fixtures::Book& book, const std::vector<double>& z) {
  std::vector<double> out(static_cast<std::size_t>(n_out));
  fixtures::price_book<double>(book, z.data(), out.data(), out.data() + book_ordinal);
  return out;
}

// Central differences of every output with respect to every knot: fd[k][o], and a per-output
// scale of the FD rounding noise, noise[o] = 4·eps·(|fixed| + |float|)/(2h) with the leg sizes at z
// (for the book: the sum over swaps) — a scale for reading a failure, not a bound (the O(h²)
// truncation term is of the same order on the largest components).
struct FdTable {
  std::vector<std::vector<double>> fd;  // [k][o]
  std::vector<double> noise;            // [o]
};

FdTable fd_table(const fixtures::Book& book, const std::vector<double>& z) {
  FdTable t;
  t.fd.assign(static_cast<std::size_t>(n_knots), std::vector<double>(static_cast<std::size_t>(n_out), 0.0));
  for (int k = 0; k < n_knots; ++k) {
    std::vector<double> zp = z, zm = z;
    zp[static_cast<std::size_t>(k)] += fd_h;
    zm[static_cast<std::size_t>(k)] -= fd_h;
    const std::vector<double> fp = price(book, zp);
    const std::vector<double> fm = price(book, zm);
    for (int o = 0; o < n_out; ++o) t.fd[static_cast<std::size_t>(k)][static_cast<std::size_t>(o)] = (fp[static_cast<std::size_t>(o)] - fm[static_cast<std::size_t>(o)]) / (2.0 * fd_h);
  }
  t.noise.assign(static_cast<std::size_t>(n_out), 0.0);
  double book_scale = 0.0;
  for (int i = 0; i < book.n_swaps; ++i) {
    const double scale = std::fabs(fixtures::fixed_leg_pv<double>(book, i, z.data())) + std::fabs(fixtures::float_leg_pv<double>(book, i, z.data()));
    t.noise[static_cast<std::size_t>(i)] = 4.0 * eps * scale / (2.0 * fd_h);
    book_scale += scale;
  }
  t.noise[static_cast<std::size_t>(book_ordinal)] = 4.0 * eps * book_scale / (2.0 * fd_h);
  return t;
}

struct Comparison {
  std::size_t compared = 0;
  std::size_t failures = 0;           // relative 1e-6 / floor 1e-9 not met
  std::size_t exact_zeros = 0;        // components that are exactly zero on both sides
  double worst_ratio = 0.0;           // worst |adj − fd| / max(|fd|, floor / rel)
  double worst_noise_share = 0.0;     // worst |adj − fd| / (FD noise scale of that output)
};

// Compares the adjoint gradient adj[k] of output o with fd[k][o].
void compare(Comparison& c, const double* adj, const FdTable& t, int o, const std::string& where) {
  for (int k = 0; k < n_knots; ++k) {
    const double a = adj[k];
    const double f = t.fd[static_cast<std::size_t>(k)][static_cast<std::size_t>(o)];
    const double diff = std::fabs(a - f);
    const double tol = std::max(rel_tol * std::fabs(f), abs_floor);
    const double noise = t.noise[static_cast<std::size_t>(o)];
    ++c.compared;
    if (a == 0.0 && f == 0.0) ++c.exact_zeros;
    c.worst_ratio = std::max(c.worst_ratio, diff / std::max(std::fabs(f), abs_floor / rel_tol));
    c.worst_noise_share = std::max(c.worst_noise_share, diff / noise);
    if (diff <= tol) continue;
    ++c.failures;
    ADD_FAILURE() << where << ", output " << o << ", knot " << k << ": adjoint " << a << " vs fd " << f << " (diff " << diff
                  << ", tol " << tol << ", FD rounding-noise bound " << noise << (diff <= noise ? ": within the FD noise scale" : "") << ")";
  }
}

void report(const Comparison& c, const char* what) {
  std::cout << "[  fd      ] " << what << ": " << c.compared << " components (" << c.exact_zeros
            << " exact zeros on both sides), worst |adj - fd| / |fd| " << c.worst_ratio << ", worst |adj - fd| / FD noise scale "
            << c.worst_noise_share << ", " << c.failures << " outside 1e-6 rel / 1e-9 abs\n";
}

}  // namespace

TEST(M1Adjoint, BookGradientMatchesCentralDifferencesAtRecordPointAndBallStates) {
  const Fixture& f = fixture();
  adjoint::Adjoint ad(f.program);
  std::vector<double> out_bar(static_cast<std::size_t>(n_out), 0.0);
  out_bar[book_ordinal] = 1.0;
  std::vector<double> out(static_cast<std::size_t>(n_out)), state_bar(static_cast<std::size_t>(n_knots));
  Comparison c;
  for (int s = 0; s <= 8; ++s) {
    const std::vector<double> z = s == 0 ? std::vector<double>(f.book.z0.begin(), f.book.z0.end()) : ball_state(f.book, s - 1);
    const FdTable t = fd_table(f.book, z);
    ad.run(z.data(), 1, out_bar.data(), out.data(), state_bar.data());
    // The forward outputs are the oracle's (bitwise under the reference preset; here to rounding).
    const std::vector<double> ref = price(f.book, z);
    EXPECT_NEAR(out[book_ordinal], ref[book_ordinal], 1e-9 * std::fabs(ref[book_ordinal]));
    const std::string where = s == 0 ? "record point" : "ball state " + std::to_string(s - 1);
    compare(c, state_bar.data(), t, book_ordinal, where);
    if (s == 0) {
      std::cout << "[  grad    ] d(book)/dz at the record point:";
      for (int k = 0; k < n_knots; ++k) std::cout << ' ' << state_bar[static_cast<std::size_t>(k)];
      std::cout << '\n';
    }
  }
  report(c, "book gradient, 9 states");
  EXPECT_EQ(c.failures, 0u) << "book gradient components outside 1e-6 rel / 1e-9 abs";
  EXPECT_LE(c.worst_ratio, rel_tol);
}

TEST(M1Adjoint, PerSwapGradientsOfThe50SampledSwapsMatchCentralDifferences) {
  const Fixture& f = fixture();
  const std::vector<int> swaps = fd_swaps();
  const int B = static_cast<int>(swaps.size());
  const std::size_t Bs = static_cast<std::size_t>(B);
  adjoint::Adjoint ad(f.program);
  Comparison c;
  for (int s = 0; s < 3; ++s) {
    const std::vector<double> z = s == 0 ? std::vector<double>(f.book.z0.begin(), f.book.z0.end()) : ball_state(f.book, s - 1);
    const FdTable t = fd_table(f.book, z);
    // One lane per swap: the same state in every lane, out_bar = e_i in lane b.
    std::vector<double> state(static_cast<std::size_t>(n_knots) * Bs), out_bar(static_cast<std::size_t>(n_out) * Bs, 0.0);
    for (std::size_t k = 0; k < static_cast<std::size_t>(n_knots); ++k) {
      for (std::size_t b = 0; b < Bs; ++b) state[k * Bs + b] = z[k];
    }
    for (std::size_t b = 0; b < Bs; ++b) out_bar[static_cast<std::size_t>(swaps[b]) * Bs + b] = 1.0;
    std::vector<double> out(static_cast<std::size_t>(n_out) * Bs), state_bar(static_cast<std::size_t>(n_knots) * Bs);
    ad.run(state.data(), B, out_bar.data(), out.data(), state_bar.data());
    const std::string where = s == 0 ? "record point" : "ball state " + std::to_string(s - 1);
    for (std::size_t b = 0; b < Bs; ++b) {
      double adj[n_knots];
      for (std::size_t k = 0; k < static_cast<std::size_t>(n_knots); ++k) adj[k] = state_bar[k * Bs + b];
      compare(c, adj, t, swaps[b], where + ", swap " + std::to_string(swaps[b]));
    }
  }
  report(c, "50 sampled swaps x 3 states");
  std::cout << "[  swaps   ]";
  for (int i : swaps) std::cout << ' ' << i;
  std::cout << '\n';
  EXPECT_EQ(c.failures, 0u) << "per-swap gradient components outside 1e-6 rel / 1e-9 abs";
  EXPECT_LE(c.worst_ratio, rel_tol);
}

TEST(M1Adjoint, AdjointIsLinearInTheSeed) {
  const Fixture& f = fixture();
  adjoint::Adjoint ad(f.program);
  const std::vector<double> z(f.book.z0.begin(), f.book.z0.end());
  epykos::rng::Philox g(fixtures::default_seed, 500000);
  std::vector<double> ob1(static_cast<std::size_t>(n_out)), ob2(static_cast<std::size_t>(n_out)), ob12(static_cast<std::size_t>(n_out));
  for (std::size_t o = 0; o < ob1.size(); ++o) {
    ob1[o] = g.uniform_range(-1.0, 1.0);
    ob2[o] = g.uniform_range(-1.0, 1.0);
    ob12[o] = ob1[o] + ob2[o];
  }
  std::vector<double> sb1(static_cast<std::size_t>(n_knots)), sb2(static_cast<std::size_t>(n_knots)), sb12(static_cast<std::size_t>(n_knots));
  ad.run(z.data(), 1, ob1.data(), nullptr, sb1.data());
  ad.run(z.data(), 1, ob2.data(), nullptr, sb2.data());
  ad.run(z.data(), 1, ob12.data(), nullptr, sb12.data());
  double worst = 0.0;
  for (std::size_t k = 0; k < static_cast<std::size_t>(n_knots); ++k) {
    const double lhs = sb12[k];
    const double rhs = sb1[k] + sb2[k];
    const double scale = std::fabs(sb1[k]) + std::fabs(sb2[k]);
    const double err = std::fabs(lhs - rhs) / (scale > 0.0 ? scale : 1.0);
    worst = std::max(worst, err);
    EXPECT_LE(std::fabs(lhs - rhs), 1e-13 * scale) << "knot " << k << ": " << lhs << " vs " << rhs;
  }
  std::cout << "[  linear  ] worst |adj(b1+b2) - adj(b1) - adj(b2)| / (|adj(b1)| + |adj(b2)|) = " << worst << '\n';
  // Homogeneity too: a scaled seed scales the adjoint (exactly for a power of two).
  std::vector<double> ob4(static_cast<std::size_t>(n_out)), sb4(static_cast<std::size_t>(n_knots));
  for (std::size_t o = 0; o < ob4.size(); ++o) ob4[o] = 4.0 * ob1[o];
  ad.run(z.data(), 1, ob4.data(), nullptr, sb4.data());
  for (std::size_t k = 0; k < static_cast<std::size_t>(n_knots); ++k) EXPECT_EQ(sb4[k], 4.0 * sb1[k]) << "knot " << k;
}

TEST(M1Adjoint, ZeroSeedGivesZeroGradient) {
  const Fixture& f = fixture();
  adjoint::Adjoint ad(f.program);
  const std::vector<double> z(f.book.z0.begin(), f.book.z0.end());
  std::vector<double> ob(static_cast<std::size_t>(n_out), 0.0), sb(static_cast<std::size_t>(n_knots), 1.0);
  ad.run(z.data(), 1, ob.data(), nullptr, sb.data());
  for (std::size_t k = 0; k < static_cast<std::size_t>(n_knots); ++k) EXPECT_EQ(sb[k], 0.0) << "knot " << k;
}
