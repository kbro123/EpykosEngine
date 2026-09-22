// M1/P1: closed-form properties of the templated pricing maths on double.
#include <gtest/gtest.h>

#include <cmath>
#include <vector>

#include "epykos/maths/m1/book.hpp"
#include "epykos/maths/m1/curve.hpp"
#include "epykos/maths/m1/price.hpp"

namespace m1 = epykos::m1;

namespace {

const m1::Book& book() {
  static const m1::Book b = m1::make_m1_book();
  return b;
}

double dfz(const m1::Book& b, const double* z, double t) { return m1::df(b.knot_t.data(), z, m1::n_knots, t); }

}  // namespace

// Unseasoned: Σ N·τ·fwd·DF(e) == N·(DF(s_1) − DF(e_T)) to 1e-13 relative.
TEST(M1Price, FloatLegTelescopes) {
  const m1::Book& b = book();
  const double* z = b.z0.data();
  int checked = 0;
  for (int i = 0; i < b.n_swaps; ++i) {
    const auto s = static_cast<std::size_t>(i);
    if (b.seasoned[s]) continue;
    const int r0 = m1::float_row_begin(b, i);
    const int rT = r0 + b.tenor[s] - 1;
    const double df_s1 = dfz(b, z, b.row_t_start[static_cast<std::size_t>(r0)]);
    const double df_eT = dfz(b, z, b.row_t_end[static_cast<std::size_t>(rT)]);
    const double closed = b.notional[s] * (df_s1 - df_eT);
    const double pv = m1::float_leg_pv(b, i, z);
    EXPECT_NEAR(pv, closed, 1e-13 * std::fabs(closed)) << i;
    EXPECT_EQ(df_s1, 1.0) << i;  // s_1 = 0 for unseasoned swaps
    ++checked;
  }
  EXPECT_EQ(checked, 800);
}

// Seasoned: first coupon N·τ_1·R·DF(e_1), then the rest telescopes from e_1 to e_T.
TEST(M1Price, SeasonedFloatLegUsesTheRealisedRateThenTelescopes) {
  const m1::Book& b = book();
  const double* z = b.z0.data();
  for (int i = 0; i < 200; ++i) {
    const auto s = static_cast<std::size_t>(i);
    ASSERT_TRUE(b.seasoned[s]);
    const int r0 = m1::float_row_begin(b, i);
    const int rT = r0 + b.tenor[s] - 1;
    const double df_e1 = dfz(b, z, b.row_t_end[static_cast<std::size_t>(r0)]);
    const double df_eT = dfz(b, z, b.row_t_end[static_cast<std::size_t>(rT)]);
    const double N = b.notional[s];
    const double closed = N * b.row_tau[static_cast<std::size_t>(r0)] * b.realised_rate[s] * df_e1 + N * (df_e1 - df_eT);
    const double pv = m1::float_leg_pv(b, i, z);
    EXPECT_NEAR(pv, closed, 1e-13 * std::fabs(closed)) << i;
    // The realised rate matters: pricing with the forward instead gives a different number.
    m1::Book alt = b;
    alt.row_is_realised_first[static_cast<std::size_t>(r0)] = 0;
    EXPECT_NE(m1::float_leg_pv(alt, i, z), pv) << i;
  }
}

TEST(M1Price, FixedLegIsKTimesAnnuity) {
  const m1::Book& b = book();
  const double* z = b.z0.data();
  for (int i = 0; i < b.n_swaps; ++i) {
    const auto s = static_cast<std::size_t>(i);
    const double ann = m1::annuity(b, i, z);
    const double pv = m1::fixed_leg_pv(b, i, z);
    EXPECT_NEAR(pv, b.fixed_rate[s] * ann, 1e-13 * std::fabs(pv)) << i;
    EXPECT_GT(ann, 0.0);
  }
}

// Pricing swap i with K = par_i gives |pv| < 1e-9·N.
TEST(M1Price, ParRateProperty) {
  m1::Book b = book();
  const double* z = b.z0.data();
  for (int i = 0; i < b.n_swaps; ++i) {
    const auto s = static_cast<std::size_t>(i);
    b.fixed_rate[s] = m1::par_rate(b, i, z);
  }
  for (int i = 0; i < b.n_swaps; ++i) {
    const auto s = static_cast<std::size_t>(i);
    EXPECT_LT(std::fabs(m1::swap_pv(b, i, z)), 1e-9 * b.notional[s]) << i;
  }
  // And with the book's K = par·(1+ε) the pv has the sign of side·ε (receive fixed above par wins).
  const m1::Book& orig = book();
  for (int i = 0; i < orig.n_swaps; ++i) {
    const auto s = static_cast<std::size_t>(i);
    const double pv = m1::swap_pv(orig, i, z);
    EXPECT_GT(pv * orig.side[s] * orig.eps[s], 0.0) << i;
  }
}

TEST(M1Price, SwapAndBookComposition) {
  const m1::Book& b = book();
  const double* z = b.z0.data();
  std::vector<double> pv(static_cast<std::size_t>(b.n_swaps + 1), 0.0);
  m1::price_book(b, z, pv.data(), pv.data() + b.n_swaps);
  double acc = 0.0;
  for (int i = 0; i < b.n_swaps; ++i) {
    const auto s = static_cast<std::size_t>(i);
    const double fixed = m1::fixed_leg_pv(b, i, z);
    const double flt = m1::float_leg_pv(b, i, z);
    const double expect = static_cast<double>(b.side[s]) * (fixed - flt);
    EXPECT_EQ(pv[s], expect) << i;
    EXPECT_EQ(pv[s], m1::swap_pv(b, i, z)) << i;
    acc = (i == 0) ? pv[s] : acc + pv[s];
  }
  EXPECT_EQ(pv[static_cast<std::size_t>(b.n_swaps)], acc);  // left fold, bitwise
  EXPECT_TRUE(std::isfinite(acc));
  // Order of magnitude: |pv_i| is a few percent of N at most (|ε| < 5% of a ~4% coupon over ≤30y).
  for (int i = 0; i < b.n_swaps; ++i) {
    const auto s = static_cast<std::size_t>(i);
    EXPECT_LT(std::fabs(pv[s]), 0.05 * b.notional[s]) << i;
  }
}

TEST(M1Price, MovesWithTheCurve) {
  const m1::Book& b = book();
  std::vector<double> up(m1::record_state.begin(), m1::record_state.end());
  for (double& v : up) v += 0.0001;  // +1 bp parallel
  for (int i : {0, 199, 200, 999}) {
    const auto s = static_cast<std::size_t>(i);
    const double d = m1::swap_pv(b, i, up.data()) - m1::swap_pv(b, i, b.z0.data());
    // Receive fixed loses when rates rise; pay fixed gains.
    EXPECT_LT(d * b.side[s], 0.0) << i;
    EXPECT_LT(std::fabs(d), 0.0001 * 30.0 * b.notional[s]) << i;
  }
}
