// The near-miss shapes fixture: a seeded set of small computations whose op trees differ from
// one another in exactly one respect a signature may overlook — where a constant sits, which way
// round a non-commutative op is, which arm of a select is a constant, how many members a sum has,
// whether an affine chain has a leading constant — instantiated many times on different data so
// that every shape is a class of several rows (M2/Q4).
//
// The M1 book's classes are far apart from one another; nothing in it can tell a signature pass
// that keeps two near-miss shapes apart from one that merges them, and nothing in it exercises a
// leading constant in an affine chain. This fixture is that check, for the signature pass (round-trip
// identity), the tape passes (E0 replay vs double) and the interpreter (E0 vs replay). Templated on
// Scalar (D3): double is the oracle, Rec records. Header-only so the including TU's contraction
// setting governs the arithmetic (an *_e0_test.cpp TU).
//
// Seeded: the constants of shape s, instance i come from sub-stream 400000 + s, draw i (D17);
// the state ball below from sub-stream 410000 + r. No data files.
//
// The Scalar vocabulary (select, recip, max, min, abs, exp, log, sqrt, fma) is called unqualified:
// the double and Rec overloads are namespace-scope functions of epykos (found by ordinary lookup
// from this nested namespace), Dual<N>'s are hidden friends found only by ADL, so a qualified
// recip(x) would not instantiate on Dual (the adjoint-vs-Dual gate, M2/Q4b).
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "epykos/rng/philox.hpp"
#include "epykos/scalar/rec.hpp"
#include "epykos/scalar/select.hpp"
#include "epykos/tape/tape.hpp"

namespace epykos::fixtures {

inline constexpr int nearmiss_inputs = 6;
inline constexpr std::uint64_t nearmiss_seed = 20260922;
inline constexpr std::uint64_t nearmiss_substream_base = 400000;
inline constexpr std::uint64_t nearmiss_ball_substream_base = 410000;

// Record-point state: six positive inputs (log and sqrt are among the shapes).
inline std::vector<double> nearmiss_record_state() { return {0.7, 1.3, 2.1, 0.4, 1.9, 1.1}; }

// The shapes, one function each: x, y, z are inputs (rows differ in which inputs), c, d constants
// (rows differ in their values). Every pair of neighbouring shapes is a near miss of the other.
template <class Scalar>
struct NearmissShapes {
  using S = Scalar;
  // Constant on the left / on the right of the non-commutative ops.
  static S sub_const_left(S x, S, S, double c, double) { return c - x; }
  static S sub_const_right(S x, S, S, double c, double) { return x - c; }
  static S div_const_left(S x, S, S, double c, double) { return c / x; }
  static S div_const_right(S x, S, S, double c, double) { return x / c; }
  // The same trees with a reference where the constant was.
  static S sub_refs(S x, S y, S, double, double) { return y - x; }
  static S div_refs(S x, S y, S, double, double) { return y / x; }
  // A constant in one slot of a two-op tree, in the other, in neither.
  static S mul_add_c_inner(S x, S y, S, double c, double) { return (x * c) + y; }
  static S mul_add_c_outer(S x, S y, S, double c, double) { return (x * y) + c; }
  static S mul_add_refs(S x, S y, S z, double, double) { return (x * y) + z; }
  static S mul_add_two_consts(S x, S, S, double c, double d) { return (x * c) + d; }
  // Select with a constant in the condition, the true arm, the false arm, both arms.
  static S select_const_cond(S x, S y, S z, double c, double) { return select(x < c, y, z); }
  static S select_const_true(S x, S y, S z, double c, double) { return select(x < y, S(c), z); }
  static S select_const_false(S x, S y, S z, double c, double) { return select(x < y, z, S(c)); }
  static S select_const_arms(S x, S y, S, double c, double d) { return select(x < y, S(c), S(d)); }
  static S select_refs(S x, S y, S z, double, double) { return select(x < y, y, z); }
  static S select_swapped_arms(S x, S y, S z, double, double) { return select(x < y, z, y); }
  // Sums of two, three and four terms, with and without a constant member.
  static S sum2(S x, S y, S, double, double) { return x + y; }
  static S sum3(S x, S y, S z, double, double) { return (x + y) + z; }
  static S sum4(S x, S y, S z, double, double) { return ((x + y) + z) + (x * y); }
  static S sum3_const(S x, S y, S, double c, double) { return (x + y) + c; }
  // Affine chains: with a leading constant, with a trailing constant, with none, with a negated
  // and a subtracted term, through a scale.
  static S affine_lead(S x, S y, S, double c, double d) { return (c + d * x) + 0.25 * y; }
  static S affine_trail(S x, S y, S, double c, double d) { return (d * x + 0.25 * y) + c; }
  static S affine_plain(S x, S y, S, double, double d) { return d * x + 0.25 * y; }
  static S affine_neg_sub(S x, S y, S z, double c, double d) { return ((c - d * x) + -y) - 0.5 * z; }
  static S affine_scaled(S x, S y, S, double c, double d) { return c * (d * x + 0.25 * y); }
  // Unary ops around the same tree, and the same op around different trees.
  static S exp_of_sum(S x, S y, S, double, double) { return exp(x + y); }
  static S exp_of_neg_sum(S x, S y, S, double, double) { return exp(-(x + y)); }
  static S log_of_sum(S x, S y, S, double, double) { return log(x + y); }
  static S sqrt_of_prod(S x, S y, S, double, double) { return sqrt(x * y); }
  static S neg_of_prod(S x, S y, S, double, double) { return -(x * y); }
  static S neg_of_prod_const(S x, S, S, double c, double) { return -(x * c); }
  // Fma with a constant in each slot.
  static S fma_c_a(S x, S y, S, double c, double) { return fma(S(c), x, y); }
  static S fma_c_b(S x, S y, S, double c, double) { return fma(x, S(c), y); }
  static S fma_c_c(S x, S y, S, double c, double) { return fma(x, y, S(c)); }
  static S fma_refs(S x, S y, S z, double, double) { return fma(x, y, z); }
  // The reciprocal and the quotient it stands in for.
  static S recip_of(S x, S, S, double, double) { return recip(x); }
  static S one_over(S x, S, S, double, double) { return 1.0 / x; }
  // Comparisons the other way round, and max / min / abs (selects with swapped arms).
  static S select_gt(S x, S y, S z, double, double) { return select(x > y, y, z); }
  static S select_le_const(S x, S y, S z, double c, double) { return select(c <= x, y, z); }
  static S max_of(S x, S y, S, double, double) { return max(x, y); }
  static S min_of(S x, S y, S, double, double) { return min(x, y); }
  static S abs_of(S x, S y, S, double, double) { return abs(x - y); }

  using Fn = S (*)(S, S, S, double, double);
  static std::vector<Fn> all() {
    return {sub_const_left,   sub_const_right,   div_const_left,    div_const_right,  sub_refs,          div_refs,
            mul_add_c_inner,  mul_add_c_outer,   mul_add_refs,      mul_add_two_consts, select_const_cond, select_const_true,
            select_const_false, select_const_arms, select_refs,     select_swapped_arms, sum2,            sum3,
            sum4,             sum3_const,        affine_lead,       affine_trail,     affine_plain,      affine_neg_sub,
            affine_scaled,    exp_of_sum,        exp_of_neg_sum,    log_of_sum,       sqrt_of_prod,      neg_of_prod,
            neg_of_prod_const, fma_c_a,          fma_c_b,           fma_c_c,          fma_refs,          recip_of,
            one_over,         select_gt,         select_le_const,   max_of,           min_of,            abs_of};
  }
};

inline constexpr int nearmiss_instances = 5;  // rows per shape

// Evaluates every shape on `nearmiss_instances` instances each (inputs rotated per instance,
// constants seeded per shape and instance), then a left fold of all rows into a total. Output
// order: shape-major, then the total. The constants are the same for double and Rec, so the two
// instantiations describe the same graph.
template <class Scalar>
std::vector<Scalar> nearmiss_evaluate(const Scalar* in) {
  using Shapes = NearmissShapes<Scalar>;
  const std::vector<typename Shapes::Fn> shapes = Shapes::all();
  std::vector<Scalar> out;
  out.reserve(shapes.size() * static_cast<std::size_t>(nearmiss_instances) + 1);
  for (std::size_t s = 0; s < shapes.size(); ++s) {
    rng::Philox draws(nearmiss_seed, nearmiss_substream_base + s);
    for (int i = 0; i < nearmiss_instances; ++i) {
      const double c = draws.uniform_range(0.5, 2.5);
      const double d = draws.uniform_range(-1.5, 1.5);
      const Scalar x = in[(i + 0) % nearmiss_inputs];
      const Scalar y = in[(i + 1) % nearmiss_inputs];
      const Scalar z = in[(i + 2) % nearmiss_inputs];
      out.push_back(shapes[s](x, y, z, c, d));
    }
  }
  Scalar total = out[0];
  for (std::size_t k = 1; k < out.size(); ++k) total = total + out[k];
  out.push_back(total);
  return out;
}

// Records the fixture at the record point: nearmiss_inputs Input nodes, every shape row and the
// total registered as outputs in that order.
inline Tape record_nearmiss() {
  Tape t;
  {
    Tape::Scope scope(t);
    const std::vector<double> z0 = nearmiss_record_state();
    std::vector<Rec> in;
    for (double v : z0) in.push_back(make_input(t, v));
    for (const Rec& r : nearmiss_evaluate<Rec>(in.data())) register_output(t, r);
  }
  t.validate();
  return t;
}

// The double instantiation at `state` (the oracle for that state).
inline std::vector<double> nearmiss_oracle(const std::vector<double>& state) { return nearmiss_evaluate<double>(state.data()); }

// A state ball of `n` draws around the record point: each input scaled by U(0.5, 1.5) from
// sub-stream 410000 + r (positive, so every shape stays finite); draw 0 is the record point.
inline std::vector<std::vector<double>> nearmiss_states(int n) {
  std::vector<std::vector<double>> states;
  const std::vector<double> z0 = nearmiss_record_state();
  for (int r = 0; r < n; ++r) {
    std::vector<double> s = z0;
    if (r > 0) {
      rng::Philox draws(nearmiss_seed, nearmiss_ball_substream_base + static_cast<std::uint64_t>(r));
      for (double& v : s) v = v * draws.uniform_range(0.5, 1.5);
    }
    states.push_back(s);
  }
  return states;
}

}  // namespace epykos::fixtures
