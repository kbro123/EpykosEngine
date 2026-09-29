// EpykosEngine — the M1 oracle values: the templated maths instantiated on double at the record
// point and at the 64 batch states (docs/WORKLOADS.md §M1 "Outputs"). Header-only so that the
// including TU's flags govern the arithmetic: an *_e0_test.cpp TU, or any TU of the reference
// preset, gets contraction-free values (D8, D13). The book itself is generated once, in libepykos,
// and is identical for every TU of a build.
//
// Output ordering per state: the 1,000 swap PVs (swap order) followed by the book PV.
#pragma once

#include <cstdint>
#include <vector>

#include "epykos/fixtures/m1_book.hpp"
#include "epykos/fixtures/m1_price.hpp"

namespace epykos::fixtures {

struct ReferenceTable {
  int n_swaps = 0;
  int n_states = 0;
  std::vector<double> record;  // stride() values: swap PVs then the book PV, at the record point
  std::vector<double> batch;   // n_states · stride() values, state b at [b·stride(), (b+1)·stride())

  int stride() const { return n_swaps + 1; }
  const double* state(int b) const {
    return batch.data() + static_cast<std::size_t>(b) * static_cast<std::size_t>(stride());
  }
  double swap_pv(int b, int i) const { return state(b)[i]; }
  double book_pv(int b) const { return state(b)[n_swaps]; }
  double record_swap_pv(int i) const { return record[static_cast<std::size_t>(i)]; }
  double record_book_pv() const { return record[static_cast<std::size_t>(n_swaps)]; }
};

// One state: out[0..n_swaps) = swap PVs, out[n_swaps] = book PV, on double, in this TU.
inline void m1_reference_state(const Book& book, const double* z, double* out) {
  price_book<double>(book, z, out, out + book.n_swaps);
}

// The record point and every batch state.
inline ReferenceTable m1_reference_values(const Book& book, const Batch& batch) {
  ReferenceTable t;
  t.n_swaps = book.n_swaps;
  t.n_states = batch.n_states;
  const std::size_t stride = static_cast<std::size_t>(t.stride());
  t.record.assign(stride, 0.0);
  m1_reference_state(book, book.z0.data(), t.record.data());
  t.batch.assign(stride * static_cast<std::size_t>(batch.n_states), 0.0);
  std::vector<double> z(static_cast<std::size_t>(batch.n_knots));
  for (int b = 0; b < batch.n_states; ++b) {
    batch.state(b, z.data());
    m1_reference_state(book, z.data(), t.batch.data() + static_cast<std::size_t>(b) * stride);
  }
  return t;
}

// Generates the book and the batch from the seed, then computes the table.
inline ReferenceTable m1_reference_values(std::uint64_t seed = default_seed) {
  return m1_reference_values(make_m1_book(seed), make_m1_batch(seed));
}

}  // namespace epykos::fixtures
