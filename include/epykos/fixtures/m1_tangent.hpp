// EpykosEngine — forward-mode derivatives of the M1 book (M2/Q2; a test-only fixture): the
// unmodified fixture pricer (fixtures/m1_price.hpp over the engine's curve and coupon maths)
// instantiated on Dual<1> and on Dual<n_knots> (scalar/dual.hpp).
//
// Inputs are the knots z[0..n_knots). Output ordering is the oracle's and the tape's: index i in
// [0, n_swaps) = swap PV i in swap order, index n_swaps = the book PV.
//
//   tangent(book, z, u)            one pass of price_book<Dual<1>> seeded with the direction u:
//                                  every output's value and (d output / d z) · u
//   jacobian_forward(book, z)      the (n_swaps + 1) × n_knots Jacobian by n_knots passes of
//                                  Dual<1> along the coordinate directions e_k
//   jacobian_forward_wide(book, z) the same Jacobian by one pass of Dual<n_knots>
//
// Header-only so the including TU's contraction setting governs the arithmetic: in an
// *_e0_test.cpp TU the value channel is bitwise price_book<double> and slot k of the wide pass is
// bitwise the narrow pass along e_k (tests/scalar/dual_m1_e0_test.cpp). Allocates (test code).
#pragma once

#include <cstddef>
#include <vector>

#include "epykos/fixtures/m1_book.hpp"
#include "epykos/fixtures/m1_price.hpp"
#include "epykos/scalar/dual.hpp"

namespace epykos::fixtures {

// Every output's value and directional derivative along one direction.
struct Tangent {
  int n_outputs = 0;            // n_swaps + 1
  std::vector<double> value;    // n_outputs
  std::vector<double> tangent;  // n_outputs: (d value / d z) · direction
};

// Every output's value and its gradient with respect to every knot.
struct Jacobian {
  int n_outputs = 0;          // n_swaps + 1
  int n_inputs = 0;           // n_knots
  std::vector<double> value;  // n_outputs
  std::vector<double> jac;    // n_outputs × n_inputs, row-major: jac[o·n_inputs + k] = d out_o / d z_k

  double at(int o, int k) const { return row(o)[k]; }
  const double* row(int o) const {
    return jac.data() + static_cast<std::size_t>(o) * static_cast<std::size_t>(n_inputs);
  }
  double* row(int o) { return jac.data() + static_cast<std::size_t>(o) * static_cast<std::size_t>(n_inputs); }
};

// One pass of price_book<Dual<1>> at z, seeded with direction[0..n_knots).
inline Tangent tangent(const Book& book, const double* z, const double* direction) {
  Tangent t;
  t.n_outputs = book.n_swaps + 1;
  const auto n_out = static_cast<std::size_t>(t.n_outputs);
  std::vector<Dual<1>> zd(static_cast<std::size_t>(n_knots));
  for (int k = 0; k < n_knots; ++k) {
    const auto s = static_cast<std::size_t>(k);
    zd[s] = Dual<1>(z[s], {direction[s]});
  }
  std::vector<Dual<1>> out(n_out);
  price_book<Dual<1>>(book, zd.data(), out.data(), out.data() + book.n_swaps);
  t.value.resize(n_out);
  t.tangent.resize(n_out);
  for (std::size_t o = 0; o < n_out; ++o) {
    t.value[o] = out[o].v;
    t.tangent[o] = out[o].d[0];
  }
  return t;
}

// n_knots passes of Dual<1>, one per coordinate direction e_k; the value channel is taken from
// the first pass (every pass computes the same values).
inline Jacobian jacobian_forward(const Book& book, const double* z) {
  Jacobian j;
  j.n_outputs = book.n_swaps + 1;
  j.n_inputs = n_knots;
  const auto n_out = static_cast<std::size_t>(j.n_outputs);
  j.jac.assign(n_out * static_cast<std::size_t>(n_knots), 0.0);
  double direction[n_knots] = {};
  for (int k = 0; k < n_knots; ++k) {
    direction[k] = 1.0;
    const Tangent t = tangent(book, z, direction);
    direction[k] = 0.0;
    if (k == 0) j.value = t.value;
    for (int o = 0; o < j.n_outputs; ++o) j.row(o)[k] = t.tangent[static_cast<std::size_t>(o)];
  }
  return j;
}

// One pass of Dual<n_knots>: slot k of every output is d out / d z_k.
inline Jacobian jacobian_forward_wide(const Book& book, const double* z) {
  using Wide = Dual<n_knots>;
  Jacobian j;
  j.n_outputs = book.n_swaps + 1;
  j.n_inputs = n_knots;
  const auto n_out = static_cast<std::size_t>(j.n_outputs);
  std::vector<Wide> zd(static_cast<std::size_t>(n_knots));
  for (int k = 0; k < n_knots; ++k) zd[static_cast<std::size_t>(k)] = Wide::variable(z[k], k);
  std::vector<Wide> out(n_out);
  price_book<Wide>(book, zd.data(), out.data(), out.data() + book.n_swaps);
  j.value.resize(n_out);
  j.jac.resize(n_out * static_cast<std::size_t>(n_knots));
  for (int o = 0; o < j.n_outputs; ++o) {
    const auto s = static_cast<std::size_t>(o);
    j.value[s] = out[s].v;
    for (int k = 0; k < n_knots; ++k) j.row(o)[k] = out[s].d[static_cast<std::size_t>(k)];
  }
  return j;
}

}  // namespace epykos::fixtures
