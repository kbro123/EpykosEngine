// EpykosEngine — the RFR compounding book pricer (test-only fixture): the leg, swap and book
// folds over the seeded RfrBook, written once and templated on Scalar (D3). The coupon maths is
// the engine's (maths/swap/ois.hpp for the fixed coupon, maths/swap/compounding.hpp for the
// daily-compounded float coupon) and the curve the engine's linear-in-zero-rate scheme.
//
//   fixed leg    Σ_j N·τ_j·K·DF(e_j)
//   float leg    Σ_j N·τ_j·R_j·DF(e_j),  R_j = (Π_{d ∈ [s_j, e_j)} (1 + r_d·τ_d) − 1)/τ_j, the
//                product as the natural loop; r_d the daily forward, or the realised fixing for
//                a day before the valuation date
//   swap         pv = s_i·(Σ fixed − Σ float)
//   book         pv = Σ_i pv_i
//
// Instantiated on double it is the oracle, on Rec it records (record_rfr below), on Dual<N> it
// is the forward-mode check of the adjoint. Sums are left folds. Every branch is on structure
// (row tables, day indices), never on a Scalar. Header-only so the including TU's contraction
// setting governs the arithmetic (an *_e0_test.cpp TU).
#pragma once

#include <cstddef>
#include <vector>

#include "epykos/fixtures/rfr_book.hpp"
#include "epykos/maths/calendar.hpp"
#include "epykos/maths/curve/linear.hpp"
#include "epykos/maths/swap/compounding.hpp"
#include "epykos/maths/swap/ois.hpp"
#include "epykos/scalar/rec.hpp"
#include "epykos/tape/passes.hpp"
#include "epykos/tape/tape.hpp"

namespace epykos::fixtures {

// DF(d/365) on the curve.
template <class Scalar>
Scalar rfr_df(const RfrBook& b, const Scalar* z, int day) {
  return curve::linear::df(b.knot_t.data(), z, n_knots, calendar::time_of_day(day));
}

// The compounded factor of float row r: the natural loop over its days, a realised fixing
// (a plain double) for the days before the valuation date, the daily forward otherwise.
template <class Scalar>
Scalar rfr_compounded_factor(const RfrBook& b, int i, int r, const Scalar* z) {
  const std::size_t rr = static_cast<std::size_t>(r);
  const int s = b.row_start_day[rr];
  const int e = b.row_end_day[rr];
  const int d0 = b.d0[static_cast<std::size_t>(i)];
  Scalar acc = 1.0;
  for (int d = s; d < e; ++d) {
    if (d < calendar::valuation_day) {
      const double fixing = b.fixing[static_cast<std::size_t>(b.fix_begin[static_cast<std::size_t>(i)] + (d - d0))];
      acc = ois::compound_step(acc, fixing, rfr_sub_period_tau);
    } else {
      const Scalar df_s = rfr_df(b, z, d);
      const Scalar df_e = rfr_df(b, z, d + 1);
      const Scalar fwd = ois::forward_rate(df_s, df_e, rfr_sub_period_tau);
      acc = ois::compound_step(acc, fwd, rfr_sub_period_tau);
    }
  }
  return acc;
}

template <class Scalar>
Scalar rfr_annuity(const RfrBook& b, int i, const Scalar* z) {
  const int P = rfr_periods(b, i);
  const int r0 = rfr_fixed_row_begin(b, i);
  const double N = b.notional[static_cast<std::size_t>(i)];
  auto term = [&](int j) {
    const std::size_t r = static_cast<std::size_t>(r0 + j);
    return N * b.row_tau[r] * rfr_df(b, z, b.row_end_day[r]);
  };
  Scalar acc = term(0);
  for (int j = 1; j < P; ++j) acc = acc + term(j);
  return acc;
}

template <class Scalar>
Scalar rfr_fixed_leg_pv(const RfrBook& b, int i, const Scalar* z) {
  const int P = rfr_periods(b, i);
  const int r0 = rfr_fixed_row_begin(b, i);
  const double N = b.notional[static_cast<std::size_t>(i)];
  const double K = b.fixed_rate[static_cast<std::size_t>(i)];
  auto term = [&](int j) {
    const std::size_t r = static_cast<std::size_t>(r0 + j);
    return ois::fixed_coupon_pv(N, b.row_tau[r], K, rfr_df(b, z, b.row_end_day[r]));
  };
  Scalar acc = term(0);
  for (int j = 1; j < P; ++j) acc = acc + term(j);
  return acc;
}

template <class Scalar>
Scalar rfr_float_leg_pv(const RfrBook& b, int i, const Scalar* z) {
  const int P = rfr_periods(b, i);
  const int r0 = rfr_float_row_begin(b, i);
  const double N = b.notional[static_cast<std::size_t>(i)];
  auto term = [&](int j) {
    const int r = r0 + j;
    const std::size_t rr = static_cast<std::size_t>(r);
    const Scalar factor = rfr_compounded_factor(b, i, r, z);
    return ois::compounded_coupon_pv(N, b.row_tau[rr], factor, rfr_df(b, z, b.row_end_day[rr]));
  };
  Scalar acc = term(0);
  for (int j = 1; j < P; ++j) acc = acc + term(j);
  return acc;
}

template <class Scalar>
Scalar rfr_swap_pv(const RfrBook& b, int i, const Scalar* z) {
  const double s = static_cast<double>(b.side[static_cast<std::size_t>(i)]);
  const Scalar fixed = rfr_fixed_leg_pv(b, i, z);
  const Scalar flt = rfr_float_leg_pv(b, i, z);
  return s * (fixed - flt);
}

// swap_pv_out[0..n_swaps) = pv_i ;  *book_pv_out = Σ_i pv_i (left fold in swap order).
template <class Scalar>
void price_rfr_book(const RfrBook& b, const Scalar* z, Scalar* swap_pv_out, Scalar* book_pv_out) {
  for (int i = 0; i < b.n_swaps; ++i) swap_pv_out[i] = rfr_swap_pv(b, i, z);
  Scalar acc = swap_pv_out[0];
  for (int i = 1; i < b.n_swaps; ++i) acc = acc + swap_pv_out[i];
  *book_pv_out = acc;
}

// One state on double: out[0..n_swaps) = swap PVs, out[n_swaps] = the book PV (the tape's
// output layout).
inline void rfr_reference_state(const RfrBook& b, const double* z, double* out) {
  price_rfr_book<double>(b, z, out, out + b.n_swaps);
}

// The recording: 12 inputs (the knots, record-point values z0), price_rfr_book<Rec> as written,
// n_swaps + 1 outputs; the E0 standard passes unless run_passes is false.
inline Tape record_rfr(const RfrBook& b, bool run_passes = true) {
  Tape tape;
  {
    Tape::Scope scope(tape);
    std::vector<Rec> z(static_cast<std::size_t>(n_knots));
    for (int k = 0; k < n_knots; ++k) z[static_cast<std::size_t>(k)] = make_input(tape, b.z0[static_cast<std::size_t>(k)]);
    std::vector<Rec> swap_pv(static_cast<std::size_t>(b.n_swaps));
    Rec book_pv;
    price_rfr_book<Rec>(b, z.data(), swap_pv.data(), &book_pv);
    for (const Rec& pv : swap_pv) register_output(tape, pv);
    register_output(tape, book_pv);
  }
  tape.validate();
  if (run_passes) {
    standard_passes(tape);
    tape.validate();
  }
  return tape;
}

}  // namespace epykos::fixtures
