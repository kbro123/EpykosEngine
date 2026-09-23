// EpykosEngine — the M1 book pricer (test-only fixture): the leg, swap and book folds of
// docs/WORKLOADS.md §M1 over the seeded Book's row table, written once and templated on Scalar
// (D3). The coupon formulas are the engine's (maths/swap/ois.hpp) and the curve is the engine's
// linear-in-zero-rate scheme (maths/curve/linear.hpp); this header only walks the fixture's rows:
//
//   fixed leg    Σ_j N·τ_j·K·DF(e_j)
//   float leg    Σ_j N·τ_j·fwd_j·DF(e_j), fwd_j = (DF(s_j)/DF(e_j) − 1)/τ_j, the first coupon
//                realised (fwd_1 replaced by R) when the row says so
//   swap         pv = s_i·(Σ fixed − Σ float)
//   book         pv = Σ_i pv_i
//
// Instantiated on double it is the oracle (m1_reference.hpp); on Rec it records (record_m1.hpp).
// Telescoping the float leg is the fusion pass's job, never the author's. Sums are left folds so a
// fold-sum pass sees a left-deep add chain and the double instantiation is a fixed evaluation
// order. The arithmetic is M1's maths/m1/price.hpp unchanged (D28), so every E0 gate that replays
// against it stays bitwise.
//
// Scalar needs: copy, + − * / between Scalars, unary minus, exp (found by ADL or std::), and mixed
// arithmetic with double on either side. No comparison, no conversion to double, no std::max/min/abs.
// Every branch below is on structure (row flags, indices, times), never on a Scalar value.
#pragma once

#include "epykos/fixtures/m1_book.hpp"
#include "epykos/maths/curve/linear.hpp"
#include "epykos/maths/swap/ois.hpp"

namespace epykos::fixtures {

// Σ_j N·τ_j·DF(e_j) over the fixed leg's rows (the fixed leg with K = 1).
template <class Scalar>
Scalar annuity(const Book& b, int i, const Scalar* z) {
  const int T = b.tenor[static_cast<std::size_t>(i)];
  const int r0 = fixed_row_begin(b, i);
  const double N = b.notional[static_cast<std::size_t>(i)];
  auto term = [&](int j) {
    const std::size_t r = static_cast<std::size_t>(r0 + j);
    const Scalar df_e = curve::linear::df(b.knot_t.data(), z, n_knots, b.row_t_end[r]);
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
    const Scalar df_e = curve::linear::df(b.knot_t.data(), z, n_knots, b.row_t_end[r]);
    return ois::fixed_coupon_pv(N, b.row_tau[r], K, df_e);
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
    const Scalar df_e = curve::linear::df(b.knot_t.data(), z, n_knots, b.row_t_end[r]);
    if (b.row_is_realised_first[r]) return ois::realised_coupon_pv(N, b.row_tau[r], R, df_e);
    const Scalar df_s = curve::linear::df(b.knot_t.data(), z, n_knots, b.row_t_start[r]);
    return ois::float_coupon_pv(N, b.row_tau[r], df_s, df_e);
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

}  // namespace epykos::fixtures
