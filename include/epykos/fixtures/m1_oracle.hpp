// EpykosEngine — the M1 book against ground truth (a test-only fixture; PRINCIPLES.md §4, D72).
//
// The whole oracle is `price_book<Wide>`. That is the point of D3 and the reason this package was
// cheap: the maths was templated on Scalar on day one, so ground truth is an INSTANTIATION of the
// same source text the `double` path runs, not a second implementation that could disagree with it
// for reasons of its own.
//
// Header-only, like fixtures/m1_differential.hpp, so `price_book<double>` carries the including
// TU's contraction setting and an `*_e0_test.cpp` TU measures the contraction-free naive path.
// `price_book<Wide>` does NOT vary that way (scalar/wide.hpp), so the truth side is the same
// number in either TU and the difference between the two measurements is entirely the naive
// path's — which is what makes "measure the naive path's error" a well-posed request.
#pragma once

#include <cstddef>
#include <vector>

#include "epykos/fixtures/m1_book.hpp"
#include "epykos/fixtures/m1_price.hpp"
#include "epykos/scalar/wide.hpp"
#include "epykos/verify/oracle.hpp"

namespace epykos::fixtures {

// Ground truth: out[0..n_swaps) = swap PVs, out[n_swaps] = the book PV, at 106 significand bits.
// The book must outlive the function.
inline verify::OracleFn m1_oracle_fn(const Book& book) {
  return [&book](const Wide* z, Wide* out) { price_book<Wide>(book, z, out, out + book.n_swaps); };
}

// PRINCIPLES.md §4's output classes for the M1 book. A swap PV and the book PV are both valuation,
// but they are kept apart because the book is a 1,000-term fold of cancelling swap PVs and has a
// visibly different error profile — reporting them as one number would hide exactly the effect
// the oracle exists to expose.
inline std::vector<verify::OutputClass> m1_output_classes(const Book& book) {
  return {verify::OutputClass{"valuation: swap PV", 0, book.n_swaps},
          verify::OutputClass{"valuation: book PV", book.n_swaps, 1}};
}

// The oracle's Jacobian of the book PV with respect to the 12 curve knots, by a central difference
// at the oracle's precision (verify/oracle.hpp argues the step and the error).
// Returns n_outputs x n_knots row-major, the same layout oracle_jacobian gives.
inline std::vector<Wide> m1_oracle_jacobian(const Book& book, const double* z) {
  return verify::oracle_jacobian(m1_oracle_fn(book), z, n_knots, book.n_swaps + 1);
}

}  // namespace epykos::fixtures
