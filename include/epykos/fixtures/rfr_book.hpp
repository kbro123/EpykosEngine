// EpykosEngine — the RFR compounding book (M3/G3; a test-only fixture, D28): ten quarterly OIS
// swaps on the M1 12-knot curve whose float coupons compound DAILY over their accrual period,
// written as the natural product loop (maths/swap/compounding.hpp) — an alternative float leg
// to the M1 telescoped one, and the fixture on which the signature pass must detect the scan
// without hints. Synthetic: generated from the seed, no data files (D16, D36). Plain arrays.
//
// Conventions are the M1 synthetic calendar's (maths/calendar.hpp): day 0 is the valuation
// date, no holidays, ACT/360 accrual, t = d/365. A quarterly schedule from d0 has period ends
// d0 + round(365.25·j/4). A sub-period is one calendar day [d, d + 1), τ_d = 1/360; its rate is
// the daily forward (DF(t_d)/DF(t_{d+1}) − 1)/τ_d, or, for a day before the valuation date, a
// realised fixing (a plain double from the fixings history).
//
// Per swap i (sub-stream rfr_substream_base + i, draws 0..4 in this order): tenor T_i ∈ {1..5}
// years, notional log-uniform in [1e6, 1e8], side ±1, a start offset U{10..60} (used when
// seasoned), ε_i ~ U(−0.05, 0.05); K_i = par_i·(1 + ε_i) with par_i on the record-point curve,
// fixings included. Swaps 0..n_rfr_seasoned−1 are seasoned: d0 = −offset and every day d0 ≤ d < 0
// carries a realised fixing R_d = z(t = 1/365)·(1 + U(−0.1, 0.1)) (sub-stream
// rfr_fixing_substream_base + i, draw d − d0).
//
// Coupon rows: swap i owns rows row_begin[i] .. row_begin[i+1): its 4T_i fixed-leg rows (period
// j = 1..4T_i) then its 4T_i float-leg rows. A float row compounds the days [start, end).
#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "epykos/fixtures/m1_book.hpp"

namespace epykos::fixtures {

inline constexpr int n_rfr_swaps = 10;
inline constexpr int n_rfr_seasoned = 3;
inline constexpr int rfr_periods_per_year = 4;
inline constexpr std::uint64_t rfr_substream_base = 500000;
inline constexpr std::uint64_t rfr_fixing_substream_base = 510000;
inline constexpr double rfr_sub_period_tau = 1.0 / 360.0;  // one calendar day, ACT/360

struct RfrBook {
  std::array<double, n_knots> knot_t{};
  std::array<double, n_knots> z0{};  // record-point state (the M1 record point)

  int n_swaps = 0;
  std::vector<int> tenor;              // years
  std::vector<double> notional;
  std::vector<int> side;               // +1 receive fixed, −1 pay fixed
  std::vector<int> d0;                 // start day (<= 0)
  std::vector<double> fixed_rate;
  std::vector<std::uint8_t> seasoned;
  std::vector<double> eps;
  std::vector<double> par;

  int n_rows = 0;
  std::vector<int> row_begin;          // n_swaps + 1
  std::vector<int> row_swap;
  std::vector<int> row_leg;            // fixed_leg / float_leg (m1_book.hpp)
  std::vector<int> row_start_day;
  std::vector<int> row_end_day;
  std::vector<double> row_tau;         // (end − start)/360
  std::vector<double> row_t_end;       // end/365

  // Realised fixings of swap i: fixing[fix_begin[i] + (d − d0[i])] for d0[i] <= d < 0.
  std::vector<int> fix_begin;          // n_swaps + 1
  std::vector<double> fixing;

  int max_day() const;                 // the last period end over the book
};

inline int rfr_fixed_row_begin(const RfrBook& b, int i) { return b.row_begin[static_cast<std::size_t>(i)]; }
inline int rfr_float_row_begin(const RfrBook& b, int i) {
  return b.row_begin[static_cast<std::size_t>(i)] + rfr_periods_per_year * b.tenor[static_cast<std::size_t>(i)];
}
inline int rfr_periods(const RfrBook& b, int i) { return rfr_periods_per_year * b.tenor[static_cast<std::size_t>(i)]; }

// round(365.25·j/4) in integers: (1461·j + 8) / 16.
constexpr int rfr_quarter_offset(int j) noexcept { return (1461 * j + 8) / 16; }

// The book for this seed; par rates from the double pricer (fixtures/rfr_price.hpp).
RfrBook make_rfr_book(std::uint64_t seed = default_seed);

// Par rate of swap i on the double curve z: float leg pv / fixed-leg annuity.
double rfr_par_rate(const RfrBook& book, int i, const double* z);

}  // namespace epykos::fixtures
