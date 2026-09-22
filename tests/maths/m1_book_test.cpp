// M1/P1: the seeded kill-test book and batch (docs/WORKLOADS.md §M1).
#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

#include "epykos/maths/calendar.hpp"
#include "epykos/maths/m1/book.hpp"
#include "epykos/maths/m1/curve.hpp"

namespace m1 = epykos::m1;
namespace cal = epykos::calendar;

namespace {

template <class T>
bool bitwise_equal(const std::vector<T>& a, const std::vector<T>& b) {
  return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(T)) == 0);
}

}  // namespace

TEST(M1Book, Statistics) {
  const m1::Book b = m1::make_m1_book();
  ASSERT_EQ(b.n_swaps, 1000);
  ASSERT_EQ(b.tenor.size(), 1000u);
  int seasoned = 0;
  int receive = 0;
  int tenor_hist[31] = {0};
  for (int i = 0; i < b.n_swaps; ++i) {
    const auto s = static_cast<std::size_t>(i);
    EXPECT_GE(b.tenor[s], 1);
    EXPECT_LE(b.tenor[s], 30);
    ++tenor_hist[b.tenor[s]];
    EXPECT_GE(b.notional[s], 1e6);
    EXPECT_LE(b.notional[s], 1e8);
    EXPECT_TRUE(b.side[s] == 1 || b.side[s] == -1);
    receive += b.side[s] == 1;
    EXPECT_GT(b.eps[s], -0.05);
    EXPECT_LT(b.eps[s], 0.05);
    EXPECT_GT(b.par[s], 0.0);
    EXPECT_GT(b.fixed_rate[s], 0.0);
    if (b.seasoned[s]) {
      ++seasoned;
      EXPECT_LT(i, 200);
      EXPECT_GE(b.d0[s], -300);
      EXPECT_LE(b.d0[s], -30);
      EXPECT_GT(b.realised_rate[s], 0.0420 * 0.9 - 1e-12);
      EXPECT_LT(b.realised_rate[s], 0.0420 * 1.1 + 1e-12);
    } else {
      EXPECT_GE(i, 200);
      EXPECT_EQ(b.d0[s], 0);
      EXPECT_EQ(b.realised_rate[s], 0.0);
    }
  }
  EXPECT_EQ(seasoned, 200);
  EXPECT_GT(receive, 400);  // equiprobable sides
  EXPECT_LT(receive, 600);
  for (int t = 1; t <= 30; ++t) EXPECT_GT(tenor_hist[t], 0) << "tenor " << t << " never drawn";
  EXPECT_EQ(b.knot_t, m1::knot_times);
  EXPECT_EQ(b.z0, m1::record_state);
}

TEST(M1Book, RowTableIsConsistentWithTheCalendar) {
  const m1::Book b = m1::make_m1_book();
  ASSERT_EQ(b.row_begin.size(), 1001u);
  EXPECT_EQ(b.row_begin[0], 0);
  int expected_rows = 0;
  for (int i = 0; i < b.n_swaps; ++i) expected_rows += 2 * b.tenor[static_cast<std::size_t>(i)];
  EXPECT_EQ(b.n_rows, expected_rows);
  EXPECT_EQ(b.row_begin[1000], expected_rows);
  ASSERT_EQ(b.row_tau.size(), static_cast<std::size_t>(b.n_rows));

  int realised_rows = 0;
  for (int i = 0; i < b.n_swaps; ++i) {
    const auto s = static_cast<std::size_t>(i);
    const int T = b.tenor[s];
    EXPECT_EQ(b.row_begin[s + 1] - b.row_begin[s], 2 * T);
    EXPECT_EQ(m1::fixed_row_begin(b, i), b.row_begin[s]);
    EXPECT_EQ(m1::float_row_begin(b, i), b.row_begin[s] + T);
    for (int leg = 0; leg < 2; ++leg) {
      for (int j = 1; j <= T; ++j) {
        const auto r = static_cast<std::size_t>(b.row_begin[s] + leg * T + j - 1);
        EXPECT_EQ(b.row_swap[r], i);
        EXPECT_EQ(b.row_leg[r], leg);
        const int sd = b.d0[s] + std::lround(365.25 * (j - 1));
        const int ed = b.d0[s] + std::lround(365.25 * j);
        EXPECT_EQ(b.row_start_day[r], sd);
        EXPECT_EQ(b.row_end_day[r], ed);
        EXPECT_EQ(b.row_tau[r], static_cast<double>(ed - sd) / 360.0);
        EXPECT_EQ(b.row_t_start[r], static_cast<double>(sd) / 365.0);
        EXPECT_EQ(b.row_t_end[r], static_cast<double>(ed) / 365.0);
        const bool realised = leg == m1::float_leg && j == 1 && b.seasoned[s];
        EXPECT_EQ(b.row_is_realised_first[r], realised ? 1 : 0);
        realised_rows += b.row_is_realised_first[r];
        if (realised) EXPECT_LT(b.row_start_day[r], 0);
      }
    }
  }
  EXPECT_EQ(realised_rows, 200);
}

TEST(M1Book, DeterministicBitwise) {
  const m1::Book a = m1::make_m1_book(20260922);
  const m1::Book b = m1::make_m1_book(20260922);
  EXPECT_TRUE(bitwise_equal(a.tenor, b.tenor));
  EXPECT_TRUE(bitwise_equal(a.notional, b.notional));
  EXPECT_TRUE(bitwise_equal(a.side, b.side));
  EXPECT_TRUE(bitwise_equal(a.d0, b.d0));
  EXPECT_TRUE(bitwise_equal(a.fixed_rate, b.fixed_rate));
  EXPECT_TRUE(bitwise_equal(a.seasoned, b.seasoned));
  EXPECT_TRUE(bitwise_equal(a.realised_rate, b.realised_rate));
  EXPECT_TRUE(bitwise_equal(a.eps, b.eps));
  EXPECT_TRUE(bitwise_equal(a.par, b.par));
  EXPECT_TRUE(bitwise_equal(a.row_begin, b.row_begin));
  EXPECT_TRUE(bitwise_equal(a.row_tau, b.row_tau));
  EXPECT_TRUE(bitwise_equal(a.row_t_start, b.row_t_start));
  EXPECT_TRUE(bitwise_equal(a.row_t_end, b.row_t_end));
  EXPECT_TRUE(bitwise_equal(a.row_is_realised_first, b.row_is_realised_first));
  // A different seed is a different book.
  const m1::Book c = m1::make_m1_book(1);
  EXPECT_FALSE(bitwise_equal(a.notional, c.notional));
}

TEST(M1Book, ParRatesReproduceKOverOnePlusEps) {
  const m1::Book b = m1::make_m1_book();
  for (int i = 0; i < b.n_swaps; ++i) {
    const auto s = static_cast<std::size_t>(i);
    const double par = m1::par_rate(b, i, b.z0.data());
    EXPECT_EQ(par, b.par[s]);
    EXPECT_NEAR(par, b.fixed_rate[s] / (1.0 + b.eps[s]), 4e-16 * par) << i;
    // Par rates of an OIS swap on this curve sit near the zero rates (3.9%–4.6%).
    EXPECT_GT(par, 0.030) << i;
    EXPECT_LT(par, 0.050) << i;
  }
}

TEST(M1Book, RealisedRateIsZ1TimesOnePlusU) {
  const m1::Book b = m1::make_m1_book();
  const double z1 = m1::zero_rate(b.knot_t.data(), b.z0.data(), m1::n_knots, 1.0);
  EXPECT_EQ(z1, 0.0420);
  for (int i = 0; i < 200; ++i) {
    const double u = b.realised_rate[static_cast<std::size_t>(i)] / z1 - 1.0;
    EXPECT_GE(u, -0.1 - 1e-12);
    EXPECT_LT(u, 0.1 + 1e-12);
  }
}

TEST(M1Batch, LayoutAndRecordPoint) {
  const m1::Batch bt = m1::make_m1_batch();
  ASSERT_EQ(bt.n_states, 64);
  ASSERT_EQ(bt.n_knots, 12);
  ASSERT_EQ(bt.z.size(), 12u * 64u);
  double z[12];
  bt.state(0, z);
  for (int k = 0; k < 12; ++k) {
    EXPECT_EQ(z[k], m1::record_state[static_cast<std::size_t>(k)]);
    EXPECT_EQ(bt.at(k, 0), m1::record_state[static_cast<std::size_t>(k)]);
    EXPECT_EQ(bt.z[static_cast<std::size_t>(k * 64)], m1::record_state[static_cast<std::size_t>(k)]);
  }
  double s1 = 0.0;
  double s2 = 0.0;
  int n = 0;
  for (int b = 1; b < 64; ++b) {
    bt.state(b, z);
    bool any_diff = false;
    for (int k = 0; k < 12; ++k) {
      const double delta = z[k] - m1::record_state[static_cast<std::size_t>(k)];
      EXPECT_LT(std::fabs(delta), 6.0 * 0.0010) << "b=" << b << " k=" << k;
      any_diff = any_diff || delta != 0.0;
      s1 += delta;
      s2 += delta * delta;
      ++n;
    }
    EXPECT_TRUE(any_diff) << b;
  }
  // 756 draws of N(0, 0.0010): sample sd within ±15% of 0.0010, mean within 4 se.
  const double mean = s1 / n;
  const double sd = std::sqrt(s2 / n - mean * mean);
  EXPECT_NEAR(mean, 0.0, 4.0 * 0.0010 / std::sqrt(static_cast<double>(n)));
  EXPECT_NEAR(sd, 0.0010, 0.15e-3);
  // Deterministic.
  const m1::Batch again = m1::make_m1_batch();
  EXPECT_TRUE(bitwise_equal(bt.z, again.z));
}

TEST(M1Book, DumpCsvWritesBothTables) {
  const m1::Book b = m1::make_m1_book();
  const std::string path = "m1_book_dump_test.csv";
  m1::dump_csv(b, path);
  std::ifstream in(path);
  ASSERT_TRUE(in.good());
  std::string line;
  int lines = 0;
  int blank = 0;
  while (std::getline(in, line)) {
    ++lines;
    blank += line.empty();
  }
  in.close();
  EXPECT_EQ(blank, 2);
  EXPECT_EQ(lines, (1 + b.n_swaps) + 1 + (1 + b.n_rows) + 1 + (1 + m1::n_knots));
  std::remove(path.c_str());
}
