// EpykosEngine — the M1-style book priced off any discount function (M3/G1; a test-only fixture,
// D28): the leg, swap and book folds of fixtures/m1_price.hpp with the curve abstracted into a
// callable `df(double t) -> Scalar`, so the same seeded book prices off every scheme, variable and
// composite of maths/curve/. The coupon formulas are the engine's (maths/swap/ois.hpp); the fold
// order is m1_price.hpp's (left folds, coupon rows in order), so a linear-zero curve prices the
// book with the M1 operations.
//
// Header-only on purpose: the including TU's contraction setting governs the arithmetic (the E0
// gates are *_e0_test.cpp TUs).
#pragma once

#include <cstddef>

#include "epykos/fixtures/m1_book.hpp"
#include "epykos/maths/swap/ois.hpp"

namespace epykos::fixtures {

// Σ_j N·τ_j·K·DF(e_j)
template <class Scalar, class DF>
Scalar fixed_leg_pv_on(const Book& b, int i, DF&& df) {
  const int T = b.tenor[static_cast<std::size_t>(i)];
  const int r0 = fixed_row_begin(b, i);
  const double N = b.notional[static_cast<std::size_t>(i)];
  const double K = b.fixed_rate[static_cast<std::size_t>(i)];
  auto term = [&](int j) {
    const std::size_t r = static_cast<std::size_t>(r0 + j);
    const Scalar df_e = df(b.row_t_end[r]);
    return ois::fixed_coupon_pv(N, b.row_tau[r], K, df_e);
  };
  Scalar acc = term(0);
  for (int j = 1; j < T; ++j) acc = acc + term(j);
  return acc;
}

// Σ_j N·τ_j·fwd_j·DF(e_j), the first coupon realised when the row says so.
template <class Scalar, class DF>
Scalar float_leg_pv_on(const Book& b, int i, DF&& df) {
  const int T = b.tenor[static_cast<std::size_t>(i)];
  const int r0 = float_row_begin(b, i);
  const double N = b.notional[static_cast<std::size_t>(i)];
  const double R = b.realised_rate[static_cast<std::size_t>(i)];
  auto term = [&](int j) {
    const std::size_t r = static_cast<std::size_t>(r0 + j);
    const Scalar df_e = df(b.row_t_end[r]);
    if (b.row_is_realised_first[r]) return ois::realised_coupon_pv(N, b.row_tau[r], R, df_e);
    const Scalar df_s = df(b.row_t_start[r]);
    return ois::float_coupon_pv(N, b.row_tau[r], df_s, df_e);
  };
  Scalar acc = term(0);
  for (int j = 1; j < T; ++j) acc = acc + term(j);
  return acc;
}

// pv_i = s_i·(Σ fixed − Σ float)
template <class Scalar, class DF>
Scalar swap_pv_on(const Book& b, int i, DF&& df) {
  const double s = static_cast<double>(b.side[static_cast<std::size_t>(i)]);
  const Scalar fixed = fixed_leg_pv_on<Scalar>(b, i, df);
  const Scalar flt = float_leg_pv_on<Scalar>(b, i, df);
  return s * (fixed - flt);
}

// swap_pv_out[0..n_swaps) = pv_i ;  *book_pv_out = Σ_i pv_i (left fold in swap order).
template <class Scalar, class DF>
void price_book_on(const Book& b, DF&& df, Scalar* swap_pv_out, Scalar* book_pv_out) {
  for (int i = 0; i < b.n_swaps; ++i) swap_pv_out[i] = swap_pv_on<Scalar>(b, i, df);
  Scalar acc = swap_pv_out[0];
  for (int i = 1; i < b.n_swaps; ++i) acc = acc + swap_pv_out[i];
  *book_pv_out = acc;
}

}  // namespace epykos::fixtures
