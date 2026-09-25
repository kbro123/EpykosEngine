// EpykosEngine — the M1 book on the differential tester (a test-only fixture): the M2 state ball
// around the book's record point, price_book<double> as the reference, and the D26 leg scales.
//
// Header-only on purpose: price_book<double> is instantiated in the including TU, so the
// reference carries that TU's contraction setting — an *_e0_test.cpp TU (or the reference
// preset) compares against a contraction-free reference, any other TU of the release preset
// against a reference the compiler was free to contract (E1). See
// tests/verify/m1_differential_e0_test.cpp for why that matters.
#pragma once

#include <cmath>
#include <cstddef>
#include <limits>

#include "epykos/fixtures/m1_book.hpp"
#include "epykos/fixtures/m1_price.hpp"
#include "epykos/verify/differential.hpp"

namespace epykos::fixtures {

// z^(r) = z0 + ρ·u^(r) with docs/WORKLOADS.md §M2's defaults (ρ = 0.005, 256 draws, sub-stream
// 200000 + r); the 12 knots are the inputs in ordinal order, the tape's input layout.
inline verify::StateBall m1_state_ball(const Book& book, const verify::BallOptions& options = {}) {
  return verify::make_state_ball(book.z0.data(), n_knots, options);
}

// The reference: out[0..n_swaps) = swap PVs, out[n_swaps] = the book PV (the tape's output
// layout), price_book<double> in the including TU. The book must outlive the function.
inline verify::ScalarFn m1_reference_fn(const Book& book) {
  return [&book](const double* z, double* out) { price_book<double>(book, z, out, out + book.n_swaps); };
}

// The E1 scale of D26: |fixed_i| + |float_i| for swap i and Σ_i |pv_i| for the book, at the given
// state, in the including TU. A swap PV is a difference of legs that can cancel to 1e-6 of the
// leg size, so an ulp of the PV itself is not the rounding unit of the computation that produced it.
inline verify::ScaleFn m1_leg_scale_fn(const Book& book) {
  return [&book](const double* z, double* scale) {
    double sum_abs_pv = 0.0;
    for (int i = 0; i < book.n_swaps; ++i) {
      const double fixed = fixed_leg_pv<double>(book, i, z);
      const double flt = float_leg_pv<double>(book, i, z);
      scale[static_cast<std::size_t>(i)] = std::fabs(fixed) + std::fabs(flt);
      sum_abs_pv += std::fabs(fixed - flt);
    }
    scale[static_cast<std::size_t>(book.n_swaps)] = sum_abs_pv;
  };
}

// D26's literal check, "asserted in addition wherever it is well posed": a scale of 0 (the bound
// is relative to the value itself) for every output whose |value| >= well_posed × its D26 scale,
// and +inf (exempt) for the ill-conditioned ones. Also counts, per call, how many outputs were
// exempted when `exempted` is given.
inline verify::ScaleFn m1_literal_mask_fn(const Book& book, double well_posed = 1e-2, int* exempted = nullptr) {
  return [&book, well_posed, exempted](const double* z, double* scale) {
    constexpr double inf = std::numeric_limits<double>::infinity();
    double sum_abs_pv = 0.0;
    double book_pv = 0.0;
    for (int i = 0; i < book.n_swaps; ++i) {
      const double fixed = fixed_leg_pv<double>(book, i, z);
      const double flt = float_leg_pv<double>(book, i, z);
      const double pv = static_cast<double>(book.side[static_cast<std::size_t>(i)]) * (fixed - flt);
      const bool ok = std::fabs(pv) >= well_posed * (std::fabs(fixed) + std::fabs(flt));
      scale[static_cast<std::size_t>(i)] = ok ? 0.0 : inf;
      if (!ok && exempted != nullptr) ++*exempted;
      sum_abs_pv += std::fabs(pv);
      book_pv += pv;
    }
    const bool ok = std::fabs(book_pv) >= well_posed * sum_abs_pv;
    scale[static_cast<std::size_t>(book.n_swaps)] = ok ? 0.0 : inf;
    if (!ok && exempted != nullptr) ++*exempted;
  };
}

}  // namespace epykos::fixtures
