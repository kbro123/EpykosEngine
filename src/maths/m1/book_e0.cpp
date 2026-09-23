// The M1 book generator, batch, par rate and CSV dump (docs/WORKLOADS.md §M1).
//
// Fixture bits must not depend on the build preset or the compiler, so this is an E0 TU
// (src/**/*_e0.cpp, root CMakeLists.txt): compiled with -ffp-contract=off in every preset on
// every compiler, which also governs the templates instantiated here (D13, D25). The test
// maths_m1_fixture_e0_test recomputes the arithmetic of this TU in a contraction-free TU and
// checks it bitwise.
#ifndef EPYKOS_FP_CONTRACT_OFF
#error "book_e0.cpp must be compiled with -ffp-contract=off (see the *_e0.cpp rule in CMakeLists.txt)"
#endif


#include "epykos/maths/m1/book.hpp"

#include <cstddef>
#include <fstream>
#include <iomanip>
#include <stdexcept>

#include "epykos/maths/calendar.hpp"
#include "epykos/maths/m1/curve.hpp"
#include "epykos/maths/m1/price.hpp"
#include "epykos/rng/philox.hpp"

namespace epykos::m1 {

namespace {

std::size_t idx(int i) { return static_cast<std::size_t>(i); }

}  // namespace

double par_rate(const Book& book, int i, const double* z) {
  const double flt = float_leg_pv<double>(book, i, z);
  const double ann = annuity<double>(book, i, z);
  return flt / ann;
}

Book make_m1_book(std::uint64_t seed) {
  Book b;
  b.knot_t = knot_times;
  b.z0 = record_state;
  b.n_swaps = n_swaps;

  const std::size_t n = idx(n_swaps);
  b.tenor.resize(n);
  b.notional.resize(n);
  b.side.resize(n);
  b.d0.resize(n);
  b.fixed_rate.assign(n, 0.0);
  b.seasoned.resize(n);
  b.realised_rate.resize(n);
  b.eps.resize(n);
  b.par.assign(n, 0.0);

  // z(1) on the record-point curve, for the realised rates.
  const double z1 = zero_rate<double>(b.knot_t.data(), b.z0.data(), n_knots, 1.0);

  // Swap data: sub-stream i, draws 0..5 in a fixed order (unused draws are still consumed so that
  // each quantity is a fixed function of (seed, i)).
  for (int i = 0; i < n_swaps; ++i) {
    rng::Philox g(seed, static_cast<std::uint64_t>(i));
    const int tenor = static_cast<int>(g.uniform_int(1, 30));             // draw 0
    const double notional = g.log_uniform(1e6, 1e8);                      // draw 1
    const int side = g.uniform() < 0.5 ? +1 : -1;                         // draw 2
    const int start_offset = static_cast<int>(g.uniform_int(30, 300));    // draw 3
    const double eps = g.uniform_range(-0.05, 0.05);                      // draw 4
    const double r_mult = g.uniform_range(-0.1, 0.1);                     // draw 5
    const bool seasoned = i < n_seasoned;

    b.tenor[idx(i)] = tenor;
    b.notional[idx(i)] = notional;
    b.side[idx(i)] = side;
    b.d0[idx(i)] = seasoned ? -start_offset : calendar::valuation_day;
    b.seasoned[idx(i)] = seasoned ? 1 : 0;
    b.realised_rate[idx(i)] = seasoned ? z1 * (1.0 + r_mult) : 0.0;
    b.eps[idx(i)] = eps;
  }

  // Coupon rows: per swap, T fixed rows then T float rows.
  b.row_begin.resize(n + 1);
  b.row_begin[0] = 0;
  for (int i = 0; i < n_swaps; ++i) b.row_begin[idx(i + 1)] = b.row_begin[idx(i)] + 2 * b.tenor[idx(i)];
  b.n_rows = b.row_begin[n];

  const std::size_t nr = idx(b.n_rows);
  b.row_swap.resize(nr);
  b.row_leg.resize(nr);
  b.row_start_day.resize(nr);
  b.row_end_day.resize(nr);
  b.row_tau.resize(nr);
  b.row_t_start.resize(nr);
  b.row_t_end.resize(nr);
  b.row_is_realised_first.resize(nr);

  for (int i = 0; i < n_swaps; ++i) {
    const int T = b.tenor[idx(i)];
    const int d0 = b.d0[idx(i)];
    for (int leg = fixed_leg; leg <= float_leg; ++leg) {
      const int base = b.row_begin[idx(i)] + leg * T;
      for (int j = 1; j <= T; ++j) {
        const std::size_t r = idx(base + j - 1);
        const int s = calendar::schedule_day(d0, j - 1);
        const int e = calendar::schedule_day(d0, j);
        b.row_swap[r] = i;
        b.row_leg[r] = leg;
        b.row_start_day[r] = s;
        b.row_end_day[r] = e;
        b.row_tau[r] = calendar::year_fraction(s, e);
        b.row_t_start[r] = calendar::time_of_day(s);
        b.row_t_end[r] = calendar::time_of_day(e);
        b.row_is_realised_first[r] = (leg == float_leg && j == 1 && b.seasoned[idx(i)]) ? 1 : 0;
      }
    }
  }

  // Fixed rates: K_i = par_i·(1 + ε_i) on the record-point curve.
  for (int i = 0; i < n_swaps; ++i) {
    const double par = par_rate(b, i, b.z0.data());
    b.par[idx(i)] = par;
    b.fixed_rate[idx(i)] = par * (1.0 + b.eps[idx(i)]);
  }
  return b;
}

Batch make_m1_batch(std::uint64_t seed) {
  Batch bt;
  bt.n_states = n_states;
  bt.n_knots = n_knots;
  bt.z.assign(idx(n_knots) * idx(n_states), 0.0);
  for (int b = 0; b < n_states; ++b) {
    rng::Philox g(seed, batch_substream_base + static_cast<std::uint64_t>(b));
    for (int k = 0; k < n_knots; ++k) {
      const double delta = (b == 0) ? 0.0 : batch_sigma * g.gaussian();  // draw k
      bt.z[idx(k) * idx(n_states) + idx(b)] = record_state[idx(k)] + delta;
    }
  }
  return bt;
}

void dump_csv(const Book& book, const std::string& path) {
  std::ofstream out(path);
  if (!out) throw std::runtime_error("dump_csv: cannot open " + path);
  out << std::setprecision(17);
  out << "swap,tenor,notional,side,d0,fixed_rate,seasoned,realised_rate,eps,par,row_begin\n";
  for (int i = 0; i < book.n_swaps; ++i) {
    out << i << ',' << book.tenor[idx(i)] << ',' << book.notional[idx(i)] << ',' << book.side[idx(i)]
        << ',' << book.d0[idx(i)] << ',' << book.fixed_rate[idx(i)] << ','
        << static_cast<int>(book.seasoned[idx(i)]) << ',' << book.realised_rate[idx(i)] << ','
        << book.eps[idx(i)] << ',' << book.par[idx(i)] << ',' << book.row_begin[idx(i)] << '\n';
  }
  out << '\n';
  out << "row,swap,leg,start_day,end_day,tau,t_start,t_end,is_realised_first\n";
  for (int r = 0; r < book.n_rows; ++r) {
    out << r << ',' << book.row_swap[idx(r)] << ',' << book.row_leg[idx(r)] << ','
        << book.row_start_day[idx(r)] << ',' << book.row_end_day[idx(r)] << ',' << book.row_tau[idx(r)]
        << ',' << book.row_t_start[idx(r)] << ',' << book.row_t_end[idx(r)] << ','
        << static_cast<int>(book.row_is_realised_first[idx(r)]) << '\n';
  }
  out << '\n';
  out << "knot,t,z0\n";
  for (int k = 0; k < n_knots; ++k) out << k << ',' << book.knot_t[idx(k)] << ',' << book.z0[idx(k)] << '\n';
}

}  // namespace epykos::m1
