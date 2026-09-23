// Shared helpers for the IR tests (tests/ir/*_test.cpp).
#pragma once

#include <cstddef>
#include <vector>

#include "epykos/maths/m1/book.hpp"

namespace epykos::test {

// A smaller seeded book: the given swaps of `full` (in the given order) with their coupon rows,
// renumbered. Every per-swap value (tenor, notional, K, R, ...) is the seeded one, so the small
// books are fixed by the seed too. Nothing else changes: the same maths prices it.
inline m1::Book sub_book(const m1::Book& full, const std::vector<int>& swaps) {
  m1::Book s;
  s.knot_t = full.knot_t;
  s.z0 = full.z0;
  s.n_swaps = static_cast<int>(swaps.size());
  s.row_begin.push_back(0);
  for (std::size_t k = 0; k < swaps.size(); ++k) {
    const std::size_t i = static_cast<std::size_t>(swaps[k]);
    s.tenor.push_back(full.tenor[i]);
    s.notional.push_back(full.notional[i]);
    s.side.push_back(full.side[i]);
    s.d0.push_back(full.d0[i]);
    s.fixed_rate.push_back(full.fixed_rate[i]);
    s.seasoned.push_back(full.seasoned[i]);
    s.realised_rate.push_back(full.realised_rate[i]);
    s.eps.push_back(full.eps[i]);
    s.par.push_back(full.par[i]);
    const int r0 = full.row_begin[i];
    const int r1 = full.row_begin[i + 1];
    for (int r = r0; r < r1; ++r) {
      const std::size_t rr = static_cast<std::size_t>(r);
      s.row_swap.push_back(static_cast<int>(k));
      s.row_leg.push_back(full.row_leg[rr]);
      s.row_start_day.push_back(full.row_start_day[rr]);
      s.row_end_day.push_back(full.row_end_day[rr]);
      s.row_tau.push_back(full.row_tau[rr]);
      s.row_t_start.push_back(full.row_t_start[rr]);
      s.row_t_end.push_back(full.row_t_end[rr]);
      s.row_is_realised_first.push_back(full.row_is_realised_first[rr]);
    }
    s.row_begin.push_back(s.row_begin.back() + (r1 - r0));
  }
  s.n_rows = s.row_begin.back();
  return s;
}

}  // namespace epykos::test
