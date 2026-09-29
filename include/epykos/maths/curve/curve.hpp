// EpykosEngine — the interpolation variable and the single-scheme curve (docs/WORKLOADS.md §M4
// "Interpolation variable"; M3/G1).
//
// A curve is a scheme (scheme.hpp) applied to one of three variables. The variable is STRUCTURE:
// a template parameter chosen at construction, never a select (the same maths on double, Rec and
// Dual<N>; D3, D5):
//
//   zero      v(t) = z(t), the continuously compounded zero rate:  log DF(t) = −z(t)·t
//             (the M1 curve of maths/curve/linear.hpp is Curve<Linear, Variable::zero>: the same
//             statements, bitwise; flat beyond both ends, DF(0) = exp(−z·0) = 1 with no special case)
//   logdf     v(t) = y(t) = log DF(t) itself: DF(t) = exp(y(t)). Linear on logdf is the
//             piecewise-constant-forward (log-linear DF) scheme. DF(0) = 1 is a structural fact,
//             so when the first knot is at t_0 > 0 the scheme runs over the extended grid
//             [0, t_0, ...] with the constant knot value 0 at the origin (a Scalar(0.0) leaf): the
//             interpolant passes through (0, 0). Flat beyond the last knot means a constant DF,
//             i.e. a zero forward beyond it — a stated simplification of this milestone: a curve
//             definition on logdf should place its last knot at or beyond the last cash flow.
//   forward   v(t) = f(t), the instantaneous forward: log DF(t) = −∫_0^t f, in closed form — Flat
//             (piecewise-constant forward) and Linear (piecewise-linear forward) only, a compile-
//             time restriction: the integral of the other schemes is not written here.
//
// Interface (Scalar = double | Rec | Dual<N>):
//   Curve<S, V> c(knot_t, n);                    n state knots at times knot_t (strictly increasing)
//   auto st = c.prepare(v);                      one state: the (extended) knot values and the
//                                                scheme's coefficients — once per curve and state
//   c.value(st, t)  c.log_df(st, t)  c.df(st, t) the variable's function, log DF and DF at t
//   c.df(v, t)                                   prepare + df (per-call convenience)
// Structure lookups (which segment, which region) happen on double t at record time; only the
// knot values are Scalar. `prepare` allocates (a small vector); nothing else does.
#pragma once

#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

#include "epykos/maths/curve/scheme.hpp"

namespace epykos::curve {

enum class Variable : int { zero = 0, logdf = 1, forward = 2 };

inline const char* to_string(Variable v) noexcept {
  switch (v) {
    case Variable::zero: return "zero";
    case Variable::logdf: return "logdf";
    case Variable::forward: return "forward";
  }
  return "?";
}

template <class S>
inline constexpr bool scheme_has_integral = std::is_same_v<S, Flat> || std::is_same_v<S, Linear>;

namespace detail {

// log DF at time t from a value x of variable V (zero or logdf; forward carries no level).
template <class Scalar>
Scalar to_logdf(Variable V, const Scalar& x, double t) {
  if (V == Variable::zero) return -x * t;
  return x;
}
// The value of variable V at time t > 0 (V zero) that has log DF = y. z = y·(−1/t): the
// reciprocal is a structural constant, so the conversion is one Const × Scalar product.
template <class Scalar>
Scalar from_logdf(Variable V, const Scalar& y, double t) {
  if (V == Variable::zero) return y * (-1.0 / t);
  return y;
}
// A value of variable `from` converted to variable `to` at time t (the identity when equal).
template <class Scalar>
Scalar convert(Variable from, Variable to, const Scalar& x, double t) {
  if (from == to) return x;
  return from_logdf(to, to_logdf(from, x, t), t);
}

// The grid a scheme runs over for variable V: [0, knots...] for logdf with t_0 > 0, else the knots.
inline std::vector<double> variable_grid(Variable V, const double* knot_t, int n) {
  std::vector<double> t;
  if (V == Variable::logdf && n > 0 && knot_t[0] > 0.0) t.push_back(0.0);
  t.insert(t.end(), knot_t, knot_t + n);
  return t;
}

}  // namespace detail

template <class S, Variable V>
class Curve {
 public:
  static_assert(V != Variable::forward || scheme_has_integral<S>,
                "Curve: the forward variable is defined for Flat and Linear only (DF = exp(-integral) in closed form)");
  using scheme_type = S;
  static constexpr Variable variable = V;

  Curve() = default;
  // Throws std::invalid_argument on a bad grid (scheme.hpp) or a knot at t < 0 (not a curve time).
  Curve(const double* knot_t, int n) : knot_t_(knot_t, knot_t + (n > 0 ? n : 0)), scheme_(Grid(detail::variable_grid(V, knot_t, n))) {
    if (n < 1) throw std::invalid_argument("Curve: at least one knot");
    if (knot_t[0] < 0.0) throw std::invalid_argument("Curve: knot times must be >= 0");
    origin_ = scheme_.n() == n + 1;
  }

  int n() const noexcept { return static_cast<int>(knot_t_.size()); }
  const std::vector<double>& knot_times() const noexcept { return knot_t_; }
  const S& scheme() const noexcept { return scheme_; }
  // The scheme's grid has the origin knot (logdf with t_0 > 0) in front of the state knots.
  bool has_origin_knot() const noexcept { return origin_; }

  template <class Scalar>
  struct State {
    std::vector<Scalar> v;     // the scheme's knot values: [0 at the origin,] the state
    std::vector<Scalar> coef;  // the scheme's prepared coefficients
  };

  template <class Scalar>
  State<Scalar> prepare(const Scalar* v) const {
    State<Scalar> st;
    st.v.reserve(static_cast<std::size_t>(scheme_.n()));
    if (origin_) st.v.push_back(Scalar(0.0));
    for (int k = 0; k < n(); ++k) st.v.push_back(v[k]);
    st.coef = scheme_.prepare(st.v.data());
    return st;
  }

  // The variable's function at t.
  template <class Scalar>
  Scalar value(const State<Scalar>& st, double t) const {
    return scheme_.value(st.v.data(), st.coef, t);
  }
  template <class Scalar>
  Scalar log_df(const State<Scalar>& st, double t) const {
    if constexpr (V == Variable::zero) {
      const Scalar zt = value(st, t);
      return -zt * t;
    } else if constexpr (V == Variable::logdf) {
      return value(st, t);
    } else {
      return -scheme_.integral(st.v.data(), 0.0, t);
    }
  }
  template <class Scalar>
  Scalar df(const State<Scalar>& st, double t) const {
    using std::exp;
    const Scalar y = log_df(st, t);
    return exp(y);
  }
  template <class Scalar>
  Scalar df(const Scalar* v, double t) const {
    return df(prepare(v), t);
  }
  template <class Scalar>
  Scalar value(const Scalar* v, double t) const {
    return value(prepare(v), t);
  }

 private:
  std::vector<double> knot_t_;
  S scheme_;
  bool origin_ = false;
};

}  // namespace epykos::curve
