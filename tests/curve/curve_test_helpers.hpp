// Shared helpers for the curve tests (tests/curve/*_test.cpp): the 10-swap M1-style book, the
// composite definitions under test, recording a curve-priced book (with optional DF outputs at a
// time grid and optional select exports), and the double oracle of the same maths.
//
// Header-only: the including TU's contraction setting governs the arithmetic (E0 gates are
// *_e0_test.cpp TUs).
#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "epykos/fixtures/curve_book.hpp"
#include "epykos/fixtures/m1_book.hpp"
#include "epykos/maths/curve/composite.hpp"
#include "epykos/maths/curve/curve.hpp"
#include "epykos/scalar/dual.hpp"
#include "epykos/scalar/rec.hpp"
#include "epykos/tape/passes.hpp"
#include "epykos/tape/select_export.hpp"
#include "epykos/tape/tape.hpp"
#include "ir/ir_test_helpers.hpp"

namespace epykos::test {

namespace curve = epykos::curve;
namespace fixtures = epykos::fixtures;

// The 10-swap M1-style book: swaps 0..4 (seasoned, a realised first fixing) and 200..204 of the
// seeded M1 book, their rows renumbered (the "10 swaps" sub-book of the M1 round-trip gate).
inline const fixtures::Book& curve_book() {
  static const fixtures::Book book = sub_book(fixtures::make_m1_book(), {0, 1, 2, 3, 4, 200, 201, 202, 203, 204});
  return book;
}

// The M4 composite of docs/WORKLOADS.md on the M1 knots: [0, 1Y) Linear on zero, [1Y, 10Y)
// MonotoneCubic on zero, [10Y, ∞) Linear on logdf.
inline std::vector<curve::RegionSpec> m4_composite_regions() {
  constexpr double inf = std::numeric_limits<double>::infinity();
  return {{0.0, 1.0, curve::SchemeKind::linear, curve::Variable::zero},
          {1.0, 10.0, curve::SchemeKind::monotone_cubic, curve::Variable::zero},
          {10.0, inf, curve::SchemeKind::linear, curve::Variable::logdf}};
}
inline curve::Composite m4_composite() {
  return curve::Composite(std::vector<double>(fixtures::knot_times.begin(), fixtures::knot_times.end()), m4_composite_regions());
}
// A single-scheme curve as a one-region composite over the M1 knots.
inline curve::Composite single_composite(curve::SchemeKind scheme, curve::Variable variable) {
  return curve::Composite(std::vector<double>(fixtures::knot_times.begin(), fixtures::knot_times.end()),
                          {{0.0, std::numeric_limits<double>::infinity(), scheme, variable}});
}

// Every (scheme, variable) pair the variable admits: six schemes on zero and logdf, Flat and
// Linear on forward (14 pairs).
struct SchemeVariable {
  curve::SchemeKind scheme;
  curve::Variable variable;
  std::string name() const { return std::string(curve::to_string(scheme)) + "/" + curve::to_string(variable); }
};
inline std::vector<SchemeVariable> all_scheme_variables() {
  std::vector<SchemeVariable> out;
  for (int s = 0; s <= static_cast<int>(curve::SchemeKind::bspline); ++s) {
    for (int v = 0; v <= static_cast<int>(curve::Variable::forward); ++v) {
      const auto sk = static_cast<curve::SchemeKind>(s);
      const auto vv = static_cast<curve::Variable>(v);
      if (vv == curve::Variable::forward && sk != curve::SchemeKind::flat && sk != curve::SchemeKind::linear) continue;
      out.push_back({sk, vv});
    }
  }
  return out;
}

// The pricing of a book off a composite curve at state v (n_knots values): outputs are the swap
// PVs, the book PV, then DF(t) for every t of `df_times`.
template <class Scalar>
std::vector<Scalar> price_on(const curve::Composite& c, const fixtures::Book& book, const Scalar* v, const std::vector<double>& df_times) {
  const auto st = c.prepare(v);
  auto df = [&](double t) { return c.df(st, t); };
  std::vector<Scalar> out(static_cast<std::size_t>(book.n_swaps) + 1 + df_times.size());
  fixtures::price_book_on<Scalar>(book, df, out.data(), out.data() + book.n_swaps);
  for (std::size_t i = 0; i < df_times.size(); ++i) out[static_cast<std::size_t>(book.n_swaps) + 1 + i] = df(df_times[i]);
  return out;
}

inline std::vector<double> oracle_on(const curve::Composite& c, const fixtures::Book& book, const double* z, const std::vector<double>& df_times) {
  return price_on<double>(c, book, z, df_times);
}

struct Recording {
  Tape tape;
  int n_outputs = 0;         // swaps + book + df_times
  SelectExports exports;     // empty unless requested
};

// Records price_on<Rec> at the book's record point: inputs are the 12 knots in ordinal order.
inline Recording record_on(const curve::Composite& c, const fixtures::Book& book, const std::vector<double>& df_times,
                           bool with_exports = false, bool run_passes = true) {
  Recording r;
  {
    Tape::Scope scope(r.tape);
    std::vector<Rec> z(static_cast<std::size_t>(fixtures::n_knots));
    for (int k = 0; k < fixtures::n_knots; ++k) z[static_cast<std::size_t>(k)] = make_input(r.tape, book.z0[static_cast<std::size_t>(k)]);
    const std::vector<Rec> out = price_on<Rec>(c, book, z.data(), df_times);
    for (const Rec& x : out) register_output(r.tape, x);
    r.n_outputs = static_cast<int>(out.size());
  }
  r.tape.validate();
  if (with_exports) r.exports = export_selects(r.tape);
  if (run_passes) standard_passes(r.tape);
  r.tape.validate();
  return r;
}

// The Dual<n_knots> Jacobian of price_on at z: value[o] and jac[o][k].
struct DualJacobian {
  std::vector<double> value;
  std::vector<std::vector<double>> jac;
};
inline DualJacobian dual_jacobian_on(const curve::Composite& c, const fixtures::Book& book, const double* z, const std::vector<double>& df_times) {
  using D = Dual<fixtures::n_knots>;
  std::vector<D> zd(static_cast<std::size_t>(fixtures::n_knots));
  for (int k = 0; k < fixtures::n_knots; ++k) zd[static_cast<std::size_t>(k)] = D::variable(z[k], k);
  const std::vector<D> out = price_on<D>(c, book, zd.data(), df_times);
  DualJacobian j;
  j.value.resize(out.size());
  j.jac.assign(out.size(), std::vector<double>(static_cast<std::size_t>(fixtures::n_knots), 0.0));
  for (std::size_t o = 0; o < out.size(); ++o) {
    j.value[o] = out[o].v;
    for (int k = 0; k < fixtures::n_knots; ++k) j.jac[o][static_cast<std::size_t>(k)] = out[o].d[static_cast<std::size_t>(k)];
  }
  return j;
}

// Ops of the tainted nodes a node depends on (its upstream closure, itself included).
inline std::vector<char> upstream_of(const Tape& t, node_id root) {
  std::vector<char> mark(t.size(), 0);
  std::vector<node_id> stack{root};
  while (!stack.empty()) {
    const node_id id = stack.back();
    stack.pop_back();
    if (mark[static_cast<std::size_t>(id)]) continue;
    mark[static_cast<std::size_t>(id)] = 1;
    const Node& n = t[id];
    if (op_is_variadic(n.op)) {
      for (node_id a : t.args(n)) stack.push_back(a);
    } else {
      const int arity = op_arity(n.op);
      if (arity >= 1) stack.push_back(n.a);
      if (arity >= 2) stack.push_back(n.b);
      if (arity >= 3) stack.push_back(n.c);
    }
  }
  return mark;
}

}  // namespace epykos::test
