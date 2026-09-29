// The RFR compounding book generator (fixtures/rfr_book.hpp). An E0 TU (src/**/*_e0.cpp, root
// CMakeLists.txt, D25): fixture bits must not depend on the preset or the compiler, and the par
// rates are computed here through the templated pricer on double.
#ifndef EPYKOS_FP_CONTRACT_OFF
#error "rfr_book_e0.cpp must be compiled with -ffp-contract=off (see the *_e0.cpp rule in CMakeLists.txt)"
#endif

#include "epykos/fixtures/rfr_book.hpp"

#include <cstddef>

#include "epykos/fixtures/rfr_price.hpp"
#include "epykos/maths/calendar.hpp"
#include "epykos/maths/curve/linear.hpp"
#include "epykos/rng/philox.hpp"

namespace epykos::fixtures {

namespace {
std::size_t idx(int i) { return static_cast<std::size_t>(i); }
}  // namespace

int RfrBook::max_day() const {
  int m = 0;
  for (int e : row_end_day) m = e > m ? e : m;
  return m;
}

double rfr_par_rate(const RfrBook& book, int i, const double* z) {
  const double flt = rfr_float_leg_pv<double>(book, i, z);
  const double ann = rfr_annuity<double>(book, i, z);
  return flt / ann;
}

RfrBook make_rfr_book(std::uint64_t seed) {
  RfrBook b;
  b.knot_t = knot_times;
  b.z0 = record_state;
  b.n_swaps = n_rfr_swaps;
  const std::size_t n = idx(n_rfr_swaps);
  b.tenor.resize(n);
  b.notional.resize(n);
  b.side.resize(n);
  b.d0.resize(n);
  b.fixed_rate.assign(n, 0.0);
  b.seasoned.resize(n);
  b.eps.resize(n);
  b.par.assign(n, 0.0);

  // The overnight rate the fixings scatter around: z at t = 1/365 on the record-point curve.
  const double z_on = curve::linear::zero_rate<double>(b.knot_t.data(), b.z0.data(), n_knots, calendar::time_of_day(1));

  for (int i = 0; i < n_rfr_swaps; ++i) {
    rng::Philox g(seed, rfr_substream_base + static_cast<std::uint64_t>(i));
    const int tenor = static_cast<int>(g.uniform_int(1, 5));               // draw 0
    const double notional = g.log_uniform(1e6, 1e8);                       // draw 1
    const int side = g.uniform() < 0.5 ? +1 : -1;                          // draw 2
    const int start_offset = static_cast<int>(g.uniform_int(10, 60));      // draw 3
    const double eps = g.uniform_range(-0.05, 0.05);                       // draw 4
    const bool seasoned = i < n_rfr_seasoned;
    b.tenor[idx(i)] = tenor;
    b.notional[idx(i)] = notional;
    b.side[idx(i)] = side;
    b.d0[idx(i)] = seasoned ? -start_offset : calendar::valuation_day;
    b.seasoned[idx(i)] = seasoned ? 1 : 0;
    b.eps[idx(i)] = eps;
  }

  // Coupon rows: per swap, 4T fixed rows then 4T float rows on the quarterly schedule from d0.
  b.row_begin.resize(n + 1);
  b.row_begin[0] = 0;
  for (int i = 0; i < n_rfr_swaps; ++i) b.row_begin[idx(i + 1)] = b.row_begin[idx(i)] + 2 * rfr_periods(b, i);
  b.n_rows = b.row_begin[n];
  const std::size_t nr = idx(b.n_rows);
  b.row_swap.resize(nr);
  b.row_leg.resize(nr);
  b.row_start_day.resize(nr);
  b.row_end_day.resize(nr);
  b.row_tau.resize(nr);
  b.row_t_end.resize(nr);
  for (int i = 0; i < n_rfr_swaps; ++i) {
    const int P = rfr_periods(b, i);
    const int d0 = b.d0[idx(i)];
    for (int leg = fixed_leg; leg <= float_leg; ++leg) {
      const int base = b.row_begin[idx(i)] + leg * P;
      for (int j = 1; j <= P; ++j) {
        const std::size_t r = idx(base + j - 1);
        const int s = d0 + rfr_quarter_offset(j - 1);
        const int e = d0 + rfr_quarter_offset(j);
        b.row_swap[r] = i;
        b.row_leg[r] = leg;
        b.row_start_day[r] = s;
        b.row_end_day[r] = e;
        b.row_tau[r] = calendar::year_fraction(s, e);
        b.row_t_end[r] = calendar::time_of_day(e);
      }
    }
  }

  // Realised fixings for the days before the valuation date of the seasoned swaps.
  b.fix_begin.resize(n + 1);
  b.fix_begin[0] = 0;
  for (int i = 0; i < n_rfr_swaps; ++i) {
    const int days = b.seasoned[idx(i)] ? -b.d0[idx(i)] : 0;
    b.fix_begin[idx(i + 1)] = b.fix_begin[idx(i)] + days;
    rng::Philox g(seed, rfr_fixing_substream_base + static_cast<std::uint64_t>(i));
    for (int k = 0; k < days; ++k) b.fixing.push_back(z_on * (1.0 + g.uniform_range(-0.1, 0.1)));  // draw k
  }

  // Fixed rates: K_i = par_i·(1 + ε_i) on the record-point curve.
  for (int i = 0; i < n_rfr_swaps; ++i) {
    const double par = rfr_par_rate(b, i, b.z0.data());
    b.par[idx(i)] = par;
    b.fixed_rate[idx(i)] = par * (1.0 + b.eps[idx(i)]);
  }
  return b;
}

}  // namespace epykos::fixtures
