// EpykosEngine — the M1 pricing maths, written once and templated on Scalar (D3), exactly as the
// coupon formulas of docs/WORKLOADS.md §M1:
//
//   fixed coupon            pv = N·τ_j·K·DF(e_j)
//   float coupon            fwd_j = (DF(s_j)/DF(e_j) − 1)/τ_j,  pv = N·τ_j·fwd_j·DF(e_j)
//   seasoned first coupon   fwd_1 replaced by the realised rate R; τ_1 still accrues from s_1 to e_1
//   swap                    pv = s_i·(Σ fixed − Σ float)
//   book                    pv = Σ_i pv_i
//
// Telescoping the float leg is the fusion pass's job, never the author's. Sums are left folds so a
// fold-sum pass sees a left-deep add chain and the double instantiation is a fixed evaluation order.
//
// Scalar needs: copy, + − * / between Scalars, unary minus, exp (found by ADL or std::), and mixed
// arithmetic with double on either side. No comparison, no conversion to double, no std::max/min/abs.
// Every branch below is on structure (row flags, indices, times), never on a Scalar value.
#pragma once

#include "epykos/maths/m1/book.hpp"
#include "epykos/maths/m1/curve.hpp"

namespace epykos::m1 {

// pv = N·τ·K·DF(e)
template <class Scalar>
Scalar fixed_coupon_pv(double N, double tau, double K, const Scalar& df_e) {
  return N * tau * K * df_e;
}

// fwd = (DF(s)/DF(e) − 1)/τ ;  pv = N·τ·fwd·DF(e)
template <class Scalar>
Scalar float_coupon_pv(double N, double tau, const Scalar& df_s, const Scalar& df_e) {
  const Scalar fwd = (df_s / df_e - 1.0) / tau;
  return N * tau * fwd * df_e;
}

// The seasoned first float coupon: fwd replaced by the realised rate R.  pv = N·τ·R·DF(e)
template <class Scalar>
Scalar realised_coupon_pv(double N, double tau, double R, const Scalar& df_e) {
  return N * tau * R * df_e;
}

// Σ_j N·τ_j·DF(e_j) over the fixed leg's rows (the fixed leg with K = 1).
template <class Scalar>
Scalar annuity(const Book& b, int i, const Scalar* z) {
  const int T = b.tenor[static_cast<std::size_t>(i)];
  const int r0 = fixed_row_begin(b, i);
  const double N = b.notional[static_cast<std::size_t>(i)];
  auto term = [&](int j) {
    const std::size_t r = static_cast<std::size_t>(r0 + j);
    const Scalar df_e = df(b.knot_t.data(), z, n_knots, b.row_t_end[r]);
    return N * b.row_tau[r] * df_e;
  };
  Scalar acc = term(0);
  for (int j = 1; j < T; ++j) acc = acc + term(j);
  return acc;
}

// Σ_j N·τ_j·K·DF(e_j)
template <class Scalar>
Scalar fixed_leg_pv(const Book& b, int i, const Scalar* z) {
  const int T = b.tenor[static_cast<std::size_t>(i)];
  const int r0 = fixed_row_begin(b, i);
  const double N = b.notional[static_cast<std::size_t>(i)];
  const double K = b.fixed_rate[static_cast<std::size_t>(i)];
  auto term = [&](int j) {
    const std::size_t r = static_cast<std::size_t>(r0 + j);
    const Scalar df_e = df(b.knot_t.data(), z, n_knots, b.row_t_end[r]);
    return fixed_coupon_pv(N, b.row_tau[r], K, df_e);
  };
  Scalar acc = term(0);
  for (int j = 1; j < T; ++j) acc = acc + term(j);
  return acc;
}

// Σ_j N·τ_j·fwd_j·DF(e_j), the first coupon realised when the row says so.
template <class Scalar>
Scalar float_leg_pv(const Book& b, int i, const Scalar* z) {
  const int T = b.tenor[static_cast<std::size_t>(i)];
  const int r0 = float_row_begin(b, i);
  const double N = b.notional[static_cast<std::size_t>(i)];
  const double R = b.realised_rate[static_cast<std::size_t>(i)];
  auto term = [&](int j) {
    const std::size_t r = static_cast<std::size_t>(r0 + j);
    const Scalar df_e = df(b.knot_t.data(), z, n_knots, b.row_t_end[r]);
    if (b.row_is_realised_first[r]) return realised_coupon_pv(N, b.row_tau[r], R, df_e);
    const Scalar df_s = df(b.knot_t.data(), z, n_knots, b.row_t_start[r]);
    return float_coupon_pv(N, b.row_tau[r], df_s, df_e);
  };
  Scalar acc = term(0);
  for (int j = 1; j < T; ++j) acc = acc + term(j);
  return acc;
}

// pv_i = s_i·(Σ fixed − Σ float)
template <class Scalar>
Scalar swap_pv(const Book& b, int i, const Scalar* z) {
  const double s = static_cast<double>(b.side[static_cast<std::size_t>(i)]);
  const Scalar fixed = fixed_leg_pv(b, i, z);
  const Scalar flt = float_leg_pv(b, i, z);
  return s * (fixed - flt);
}

// swap_pv_out[0..n_swaps) = pv_i ;  *book_pv_out = Σ_i pv_i (left fold in swap order).
template <class Scalar>
void price_book(const Book& b, const Scalar* z, Scalar* swap_pv_out, Scalar* book_pv_out) {
  for (int i = 0; i < b.n_swaps; ++i) swap_pv_out[i] = swap_pv(b, i, z);
  Scalar acc = swap_pv_out[0];
  for (int i = 1; i < b.n_swaps; ++i) acc = acc + swap_pv_out[i];
  *book_pv_out = acc;
}

}  // namespace epykos::m1
