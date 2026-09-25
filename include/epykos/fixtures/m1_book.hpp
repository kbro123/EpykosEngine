// EpykosEngine — the M1 kill-test book (docs/WORKLOADS.md §M1, D16): 1,000 annual OIS swaps on a
// 12-knot curve, generated from the seed. Plain arrays only (structure-of-arrays); no data files.
// A test-only fixture (namespace epykos::fixtures, D28), not engine API.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace epykos::fixtures {

inline constexpr int n_knots = 12;
inline constexpr int n_swaps = 1000;
inline constexpr int n_seasoned = 200;  // swaps 0..199 carry a realised first fixing
inline constexpr int n_states = 64;     // batch width
inline constexpr std::uint64_t default_seed = 20260922;
inline constexpr std::uint64_t batch_substream_base = 100000;
inline constexpr double batch_sigma = 0.0010;  // δ_k ~ N(0, σ = 0.0010), i.e. 10 bp

// Knot times in years: 7/365, 1/12, 0.25, 0.5, 1, 2, 3, 5, 7, 10, 20, 30.
inline constexpr std::array<double, n_knots> knot_times = {
    7.0 / 365.0, 1.0 / 12.0, 0.25, 0.5, 1.0, 2.0, 3.0, 5.0, 7.0, 10.0, 20.0, 30.0};

// Record-point state z.
inline constexpr std::array<double, n_knots> record_state = {
    0.0400, 0.0405, 0.0410, 0.0415, 0.0420, 0.0410, 0.0400, 0.0395, 0.0400, 0.0410, 0.0430, 0.0440};

enum Leg : int { fixed_leg = 0, float_leg = 1 };

// Per swap i (arrays of length n_swaps):
//   tenor[i]          T_i in years (1..30); both legs have T_i annual periods
//   notional[i]       N_i in [1e6, 1e8]
//   side[i]           +1 receive fixed, −1 pay fixed
//   d0[i]             start day: −U{30..300} if seasoned, else 0
//   fixed_rate[i]     K_i = par_i·(1 + ε_i)
//   seasoned[i]       1 if the first float coupon has a realised fixing (i < 200)
//   realised_rate[i]  R_i for seasoned swaps, 0 otherwise
//   eps[i], par[i]    ε_i and par_i on the record-point curve (kept for tests and dumps)
//
// Coupon rows (arrays of length n_rows = 2·Σ T_i). Swap i owns rows row_begin[i] .. row_begin[i+1):
// first its T_i fixed-leg rows (period j = 1..T_i in order), then its T_i float-leg rows.
//   row_swap[r], row_leg[r]                 swap index and leg (fixed_leg / float_leg)
//   row_start_day[r], row_end_day[r]        s_j = d_{j−1}, e_j = d_j with d_j = d0 + round(365.25·j)
//   row_tau[r]                              (e_j − s_j)/360
//   row_t_start[r], row_t_end[r]            s_j/365, e_j/365
//   row_is_realised_first[r]                1 on the float leg's first row of a seasoned swap
struct Book {
  std::array<double, n_knots> knot_t{};
  std::array<double, n_knots> z0{};  // record-point state

  int n_swaps = 0;
  std::vector<int> tenor;
  std::vector<double> notional;
  std::vector<int> side;
  std::vector<int> d0;
  std::vector<double> fixed_rate;
  std::vector<std::uint8_t> seasoned;
  std::vector<double> realised_rate;
  std::vector<double> eps;
  std::vector<double> par;

  int n_rows = 0;
  std::vector<int> row_begin;  // n_swaps + 1
  std::vector<int> row_swap;
  std::vector<int> row_leg;
  std::vector<int> row_start_day;
  std::vector<int> row_end_day;
  std::vector<double> row_tau;
  std::vector<double> row_t_start;
  std::vector<double> row_t_end;
  std::vector<std::uint8_t> row_is_realised_first;
};

// First fixed-leg row / first float-leg row of swap i; each leg has tenor[i] rows.
inline int fixed_row_begin(const Book& b, int i) { return b.row_begin[static_cast<std::size_t>(i)]; }
inline int float_row_begin(const Book& b, int i) {
  return b.row_begin[static_cast<std::size_t>(i)] + b.tenor[static_cast<std::size_t>(i)];
}

// The 64 batch states, SoA with the batch axis innermost (D15): z[k · n_states + b] is knot k of
// state b. State 0 is the record point; state b >= 1 is z + δ^(b), δ^(b)_k ~ N(0, 0.0010) i.i.d.
// from sub-stream 100000 + b (draw index k).
struct Batch {
  int n_states = 0;
  int n_knots = 0;
  std::vector<double> z;

  double at(int k, int b) const {
    return z[static_cast<std::size_t>(k) * static_cast<std::size_t>(n_states) + static_cast<std::size_t>(b)];
  }
  // Copies state b into out[0..n_knots).
  void state(int b, double* out) const {
    for (int k = 0; k < n_knots; ++k) out[k] = at(k, b);
  }
};

// The book of docs/WORKLOADS.md §M1 for this seed. Swap i draws from sub-stream i in this order:
// draw 0 tenor, 1 notional, 2 side, 3 start offset (used only if seasoned), 4 ε, 5 the realised-rate
// multiplier (used only if seasoned). K_i = par_i·(1 + ε_i) with par_i computed by par_rate() on
// the record-point curve, seasoned first-fixing rule included. R_i = z(1)·(1 + U(−0.1, 0.1)).
Book make_m1_book(std::uint64_t seed = default_seed);

// The 64 batch states for this seed (see Batch).
Batch make_m1_batch(std::uint64_t seed = default_seed);

// Par rate of swap i on the double curve z: float leg pv (realised first fixing included) divided
// by the fixed-leg annuity Σ N·τ_j·DF(e_j). Uses the row table and notional, not fixed_rate.
double par_rate(const Book& book, int i, const double* z);

// Debugging only: writes the swap table and the coupon-row table as CSV (two sections). Never an
// input.
void dump_csv(const Book& book, const std::string& path);

}  // namespace epykos::fixtures
