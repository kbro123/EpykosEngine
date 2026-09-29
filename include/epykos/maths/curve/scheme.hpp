// EpykosEngine — the interpolation schemes of a curve (docs/PROBLEM.md §3 "interpolation",
// docs/WORKLOADS.md §M4 "Schemes", DESIGN.md §8 classes A and B; M3/G1).
//
// A scheme interpolates a value function v(t) through n knots (t_k, v_k), t_0 < ... < t_{n-1}. The
// knot TIMES are structure (double, fixed at construction: bracket searches, Hermite basis values,
// Thomas pivots, B-spline knot vectors are all plain double arithmetic); the knot VALUES are
// Scalar (D3: double is the oracle, Rec records, Dual<N> differentiates). Nothing here branches
// on a Scalar value or converts one to double: every branch is on structure (t against the knot
// times, the knot index, the scheme's tables), and MonotoneCubic's limiter — the one scheme whose
// value depends on the ORDER of knot values — is written with select() (D5, class B of
// DESIGN.md §8), every arm finite for every finite input (the safe-arm table, DESIGN.md §10).
//
// Concept. A scheme S over a grid (double* t, int n) provides:
//   S(const double* t, int n)                         structure tables from the knot times
//   int n() const;  const Grid& grid() const;         the knots
//   template <class Scalar> std::vector<Scalar> prepare(const Scalar* v) const;
//       the Scalar coefficients of one state (tangents, second derivatives; empty for the
//       schemes that need none), computed once per curve and state, not per t
//   template <class Scalar> Scalar value(const Scalar* v, const std::vector<Scalar>& coef, double t) const;
//   template <class Scalar> Scalar value(const Scalar* v, double t) const;   // prepare + value
// and Flat and Linear also
//   template <class Scalar> Scalar integral(const Scalar* v, double from, double to) const;
//       ∫_from^to v(s) ds in closed form (the `forward` variable of curve.hpp, DF = exp(−∫f)).
//
// Every scheme is FLAT outside [t_0, t_{n-1}] (v_0 before the first knot, v_{n-1} at and beyond
// the last; the M1 convention of linear.hpp), and every scheme but BSpline returns the knot value
// itself at a knot time (a structural check on t, so DF is bitwise continuous where a composite
// curve's regions meet on a knot, composite.hpp). BSpline interpolates its END knots only: its
// state is the de Boor control points, not values on the grid.
//
// The schemes:
//   Flat           v(t) = v_k on [t_k, t_{k+1}) (a step function, right-continuous)
//   Linear         linear between knots — the M1 curve of maths/curve/linear.hpp, same operations
//   Hermite        cubic Hermite with Bessel (parabolic) tangents: interior m_k = (h_k d_{k−1} +
//                  h_{k−1} d_k)/(h_{k−1} + h_k), the parabola through three points at the ends;
//                  reproduces quadratics; C1; linear in the knot values
//   NaturalCubic   the C2 cubic spline with v'' = 0 at both ends: the tridiagonal system for the
//                  second derivatives M_k is solved by the Thomas algorithm as a Scalar loop; its
//                  pivots depend on the knot times only and are precomputed with their
//                  reciprocals, so the sweep is a chain of scaled affine steps that affine_collapse
//                  turns into Affine nodes (a division by a constant is NOT a multiplication by its
//                  reciprocal bitwise, so the reciprocal is the structural constant the maths uses;
//                  the value at t is then a four-term affine combination of v_k, v_{k+1}, M_k, M_{k+1})
//   MonotoneCubic  Hermite with Fritsch–Carlson initial tangents (the three-point formula, i.e.
//                  the Bessel tangents above) passed through the Hyman filter: at a knot whose
//                  two secants differ in sign (or one is zero) the tangent is 0; otherwise it is
//                  clipped to the secants' sign and to |m_k| <= 3·min(|d_{k−1}|, |d_k|) (the
//                  Fritsch–Carlson box [0, 3]², sufficient for monotonicity on every interval). The
//                  end tangents are clipped the same way against their one secant. Every sign /
//                  abs / min / max is a select with both arms finite: sign(x) is select(x < 0, −1,
//                  +1), never x/|x|; the limiter multiplies, it never divides by a secant.
//   BSpline        the clamped B-spline of degree p = min(3, n − 1) whose de Boor points are the
//                  n knot values: knot vector [t_0]^(p+1), u_{p+j} = (t_j + ... + t_{j+p−1})/p for
//                  j = 1 .. n−p−1 (the de Boor averaging of the knot times, Piegl & Tiller eq. 9.8,
//                  so the Greville abscissae sit at the knot times' running means), [t_{n−1}]^(p+1);
//                  Cox–de Boor basis values in double (Piegl & Tiller A2.2); interpolates the end
//                  points; partition of unity; the value at t is an affine combination of p+1
//                  consecutive knot values.
//
// Recording (D5, CLAUDE.md): constants are literals (weights are plain doubles when they meet a
// Scalar), sums are written as left folds, and nothing is telescoped or pre-folded by hand: the
// affine collapse recovers the linear map of the linear-in-values schemes (Flat, Linear, Hermite,
// NaturalCubic, BSpline) and MonotoneCubic records Select nodes, which the exports of
// tape/select_export.hpp make observable.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "epykos/scalar/select.hpp"

namespace epykos::curve {

// ---------------------------------------------------------------------------------------------
// The knot grid (structure)
// ---------------------------------------------------------------------------------------------

class Grid {
 public:
  Grid() = default;
  // Throws std::invalid_argument unless n >= 1 and t is finite and strictly increasing.
  Grid(const double* t, int n) : t_(t, t + (n > 0 ? n : 0)) { check(n); }
  explicit Grid(std::vector<double> t) : t_(std::move(t)) { check(static_cast<int>(t_.size())); }

  int n() const noexcept { return static_cast<int>(t_.size()); }
  const std::vector<double>& times() const noexcept { return t_; }
  const double* data() const noexcept { return t_.data(); }
  double operator[](int k) const noexcept { return t_[static_cast<std::size_t>(k)]; }
  double front() const noexcept { return t_.front(); }
  double back() const noexcept { return t_.back(); }
  // h_k = t_{k+1} − t_k.
  double h(int k) const noexcept { return t_[static_cast<std::size_t>(k) + 1] - t_[static_cast<std::size_t>(k)]; }

  // The segment k with t_k <= t < t_{k+1}. Requires n >= 2 and front() <= t < back() (the same
  // walk as maths/curve/linear.hpp).
  int segment(double t) const noexcept {
    int k = 0;
    while (t >= t_[static_cast<std::size_t>(k) + 1]) ++k;
    return k;
  }
  // The knot exactly at t, or −1.
  int knot_at(double t) const noexcept {
    for (int k = 0; k < n(); ++k) {
      if (t == t_[static_cast<std::size_t>(k)]) return k;
    }
    return -1;
  }

 private:
  void check(int n) const {
    if (n < 1) throw std::invalid_argument("curve::Grid: at least one knot");
    for (int k = 0; k < n; ++k) {
      const double x = t_[static_cast<std::size_t>(k)];
      if (!std::isfinite(x)) throw std::invalid_argument("curve::Grid: knot time " + std::to_string(k) + " is not finite");
      if (k > 0 && !(x > t_[static_cast<std::size_t>(k) - 1])) {
        throw std::invalid_argument("curve::Grid: knot times must be strictly increasing (knot " + std::to_string(k) + ")");
      }
    }
  }
  std::vector<double> t_;
};

namespace detail {

// A left fold Σ_i w_i·x[idx_i] over the listed terms (at least one), written as the recorder
// wants it: every product is Const × Scalar, every step one Add.
template <class Scalar>
Scalar weighted_sum(const Scalar* x, const std::vector<std::pair<int, double>>& terms) {
  Scalar acc = terms[0].second * x[terms[0].first];
  for (std::size_t i = 1; i < terms.size(); ++i) acc = acc + terms[i].second * x[terms[i].first];
  return acc;
}

// Integral weights of the piecewise-constant (Flat) or piecewise-linear (Linear) function through
// the grid over [from, to] (from <= to), flat outside the grid: per-knot masses w_k with
// ∫_from^to v = Σ_k w_k v_k. Knots with zero mass are omitted, except that the list is never
// empty: when the interval has zero length the knot governing `from` is listed with weight 0, so
// that a recorded integral is always a product and never a bare constant.
inline std::vector<std::pair<int, double>> flat_masses(const Grid& g, double from, double to) {
  std::vector<std::pair<int, double>> terms;
  const int n = g.n();
  auto overlap = [&](double lo, double hi) { return std::max(0.0, std::min(to, hi) - std::max(from, lo)); };
  constexpr double inf = std::numeric_limits<double>::infinity();
  // Cell of knot k: (−inf, t_1) for k = 0 (the flat extension merged with the first cell),
  // [t_k, t_{k+1}) for interior k, [t_{n−1}, inf) for the last knot.
  for (int k = 0; k < n; ++k) {
    const double lo = k == 0 ? -inf : g[k];
    const double hi = k + 1 < n ? g[k + 1] : inf;
    const double w = overlap(lo, hi);
    if (w > 0.0) terms.emplace_back(k, w);
  }
  if (terms.empty()) {
    int k = 0;
    while (k + 1 < n && from >= g[k + 1]) ++k;
    terms.emplace_back(k, 0.0);
  }
  return terms;
}

inline std::vector<std::pair<int, double>> linear_masses(const Grid& g, double from, double to) {
  const int n = g.n();
  if (n == 1) return flat_masses(g, from, to);
  std::vector<double> mass(static_cast<std::size_t>(n), 0.0);
  constexpr double inf = std::numeric_limits<double>::infinity();
  // Flat extensions.
  mass[0] += std::max(0.0, std::min(to, g.front()) - std::max(from, -inf));
  mass[static_cast<std::size_t>(n) - 1] += std::max(0.0, std::min(to, inf) - std::max(from, g.back()));
  // Linear cells: ∫ (1 − s) v_k + s v_{k+1} over [x0, x1] ⊂ [t_k, t_{k+1}], s = (x − t_k)/h.
  for (int k = 0; k + 1 < n; ++k) {
    const double lo = g[k], hi = g[k + 1], h = hi - lo;
    const double x0 = std::max(from, lo), x1 = std::min(to, hi);
    if (!(x1 > x0)) continue;
    const double s0 = (x0 - lo) / h, s1 = (x1 - lo) / h;
    const double b = 0.5 * h * (s1 * s1 - s0 * s0);  // ∫ s dx
    const double a = (x1 - x0) - b;                   // ∫ (1 − s) dx
    mass[static_cast<std::size_t>(k)] += a;
    mass[static_cast<std::size_t>(k) + 1] += b;
  }
  std::vector<std::pair<int, double>> terms;
  for (int k = 0; k < n; ++k) {
    if (mass[static_cast<std::size_t>(k)] != 0.0) terms.emplace_back(k, mass[static_cast<std::size_t>(k)]);
  }
  if (terms.empty()) {
    int k = 0;
    while (k + 1 < n && from >= g[k + 1]) ++k;
    terms.emplace_back(k, 0.0);
  }
  return terms;
}

// The Hermite basis at s = (t − t_k)/h, and the value h00 v_k + h·h10 m_k + h01 v_{k+1} + h·h11 m_{k+1}.
template <class Scalar>
Scalar hermite_value(const Grid& g, const Scalar* v, const std::vector<Scalar>& m, int k, double t) {
  const double h = g.h(k);
  const double s = (t - g[k]) / h;
  const double u = 1.0 - s;
  const double h00 = (1.0 + 2.0 * s) * u * u;
  const double h10 = s * u * u;
  const double h01 = s * s * (3.0 - 2.0 * s);
  const double h11 = s * s * (s - 1.0);
  Scalar acc = h00 * v[k];
  acc = acc + (h10 * h) * m[static_cast<std::size_t>(k)];
  acc = acc + h01 * v[k + 1];
  acc = acc + (h11 * h) * m[static_cast<std::size_t>(k) + 1];
  return acc;
}

// The structural tangent rule of the Bessel / three-point scheme: tangent k is
// w1·d[i1] (+ w2·d[i2] when i2 >= 0) over the secants d.
struct TangentRule {
  int i1 = 0;
  double w1 = 1.0;
  int i2 = -1;
  double w2 = 0.0;
};

inline std::vector<TangentRule> bessel_rules(const Grid& g) {
  const int n = g.n();
  std::vector<TangentRule> rules(static_cast<std::size_t>(n));
  if (n < 2) return rules;  // n = 1: no tangent
  if (n == 2) {
    rules[0] = {0, 1.0, -1, 0.0};
    rules[1] = {0, 1.0, -1, 0.0};
    return rules;
  }
  for (int k = 1; k + 1 < n; ++k) {
    const double hl = g.h(k - 1), hr = g.h(k);
    rules[static_cast<std::size_t>(k)] = {k - 1, hr / (hl + hr), k, hl / (hl + hr)};
  }
  {
    const double h0 = g.h(0), h1 = g.h(1);
    rules[0] = {0, (2.0 * h0 + h1) / (h0 + h1), 1, -h0 / (h0 + h1)};
  }
  {
    const double hl = g.h(n - 3), hr = g.h(n - 2);
    rules[static_cast<std::size_t>(n) - 1] = {n - 2, (2.0 * hr + hl) / (hl + hr), n - 3, -hr / (hl + hr)};
  }
  return rules;
}

// Secants d_k = (v_{k+1} − v_k)·(1/h_k), k = 0 .. n−2 (inv_h precomputed: structure).
template <class Scalar>
std::vector<Scalar> secants(const Scalar* v, const std::vector<double>& inv_h) {
  std::vector<Scalar> d;
  d.reserve(inv_h.size());
  for (std::size_t k = 0; k < inv_h.size(); ++k) d.push_back((v[k + 1] - v[k]) * inv_h[k]);
  return d;
}

template <class Scalar>
std::vector<Scalar> bessel_tangents(const std::vector<TangentRule>& rules, const std::vector<Scalar>& d) {
  std::vector<Scalar> m;
  m.reserve(rules.size());
  for (const TangentRule& r : rules) {
    Scalar mk = r.w1 * d[static_cast<std::size_t>(r.i1)];
    if (r.i2 >= 0) mk = mk + r.w2 * d[static_cast<std::size_t>(r.i2)];
    m.push_back(mk);
  }
  return m;
}

}  // namespace detail

// ---------------------------------------------------------------------------------------------
// Flat
// ---------------------------------------------------------------------------------------------

class Flat {
 public:
  Flat() = default;
  Flat(const double* t, int n) : grid_(t, n) {}
  explicit Flat(Grid grid) : grid_(std::move(grid)) {}
  static const char* name() noexcept { return "flat"; }
  int n() const noexcept { return grid_.n(); }
  const Grid& grid() const noexcept { return grid_; }

  template <class Scalar>
  std::vector<Scalar> prepare(const Scalar*) const {
    return {};
  }
  template <class Scalar>
  Scalar value(const Scalar* v, const std::vector<Scalar>&, double t) const {
    return value(v, t);
  }
  template <class Scalar>
  Scalar value(const Scalar* v, double t) const {
    if (t < grid_.front()) return v[0];
    if (t >= grid_.back()) return v[n() - 1];
    return v[grid_.segment(t)];
  }
  // ∫_from^to v(s) ds, flat outside the grid; a negative interval is the negated integral.
  template <class Scalar>
  Scalar integral(const Scalar* v, double from, double to) const {
    if (to < from) return -integral(v, to, from);
    return detail::weighted_sum(v, detail::flat_masses(grid_, from, to));
  }

 private:
  Grid grid_;
};

// ---------------------------------------------------------------------------------------------
// Linear — the M1 scheme, the operations of maths/curve/linear.hpp
// ---------------------------------------------------------------------------------------------

class Linear {
 public:
  Linear() = default;
  Linear(const double* t, int n) : grid_(t, n) {}
  explicit Linear(Grid grid) : grid_(std::move(grid)) {}
  static const char* name() noexcept { return "linear"; }
  int n() const noexcept { return grid_.n(); }
  const Grid& grid() const noexcept { return grid_; }

  template <class Scalar>
  std::vector<Scalar> prepare(const Scalar*) const {
    return {};
  }
  template <class Scalar>
  Scalar value(const Scalar* v, const std::vector<Scalar>&, double t) const {
    return value(v, t);
  }
  template <class Scalar>
  Scalar value(const Scalar* v, double t) const {
    // The same statements as linear::zero_rate (D28): bits unchanged on the M1 book.
    if (t <= grid_.front()) return v[0];
    if (t >= grid_.back()) return v[n() - 1];
    const int k = grid_.segment(t);
    if (t == grid_[k]) return v[k];
    const double w = (t - grid_[k]) / (grid_[k + 1] - grid_[k]);
    return (1.0 - w) * v[k] + w * v[k + 1];
  }
  template <class Scalar>
  Scalar integral(const Scalar* v, double from, double to) const {
    if (to < from) return -integral(v, to, from);
    return detail::weighted_sum(v, detail::linear_masses(grid_, from, to));
  }

 private:
  Grid grid_;
};

// ---------------------------------------------------------------------------------------------
// Hermite — Bessel tangents
// ---------------------------------------------------------------------------------------------

class Hermite {
 public:
  Hermite() = default;
  Hermite(const double* t, int n) : grid_(t, n) { build(); }
  explicit Hermite(Grid grid) : grid_(std::move(grid)) { build(); }
  static const char* name() noexcept { return "hermite"; }
  int n() const noexcept { return grid_.n(); }
  const Grid& grid() const noexcept { return grid_; }
  const std::vector<detail::TangentRule>& tangent_rules() const noexcept { return rules_; }

  // The tangents m_0 .. m_{n−1} (empty for n = 1).
  template <class Scalar>
  std::vector<Scalar> prepare(const Scalar* v) const {
    if (n() < 2) return {};
    return detail::bessel_tangents(rules_, detail::secants(v, inv_h_));
  }
  template <class Scalar>
  Scalar value(const Scalar* v, const std::vector<Scalar>& m, double t) const {
    if (t <= grid_.front()) return v[0];
    if (t >= grid_.back()) return v[n() - 1];
    const int k = grid_.segment(t);
    if (t == grid_[k]) return v[k];
    return detail::hermite_value(grid_, v, m, k, t);
  }
  template <class Scalar>
  Scalar value(const Scalar* v, double t) const {
    return value(v, prepare(v), t);
  }

 private:
  void build() {
    inv_h_.clear();
    for (int k = 0; k + 1 < n(); ++k) inv_h_.push_back(1.0 / grid_.h(k));
    rules_ = detail::bessel_rules(grid_);
  }
  Grid grid_;
  std::vector<double> inv_h_;
  std::vector<detail::TangentRule> rules_;
};

// ---------------------------------------------------------------------------------------------
// NaturalCubic — the C2 spline, Thomas algorithm over Scalar with structural pivots
// ---------------------------------------------------------------------------------------------

class NaturalCubic {
 public:
  NaturalCubic() = default;
  NaturalCubic(const double* t, int n) : grid_(t, n) { build(); }
  explicit NaturalCubic(Grid grid) : grid_(std::move(grid)) { build(); }
  static const char* name() noexcept { return "natural_cubic"; }
  int n() const noexcept { return grid_.n(); }
  const Grid& grid() const noexcept { return grid_; }

  // The second derivatives at the INTERIOR knots, M_1 .. M_{n−2} (M_0 = M_{n−1} = 0 are the
  // natural end conditions and are structure); empty for n <= 2 (the spline is the line).
  //
  // System, i = 1 .. n−2:  (h_{i−1}/6) M_{i−1} + ((h_{i−1} + h_i)/3) M_i + (h_i/6) M_{i+1} = d_i − d_{i−1}.
  // Thomas: pivots p_i = b_i − a_i·c'_{i−1}, c'_i = c_i/p_i are structure (built once, with 1/p_i);
  // the Scalar sweep is d'_i = (r_i − a_i·d'_{i−1})·(1/p_i), then M_i = d'_i − c'_i·M_{i+1}.
  template <class Scalar>
  std::vector<Scalar> prepare(const Scalar* v) const {
    const int m = n() - 2;  // unknowns
    if (m <= 0) return {};
    const std::vector<Scalar> d = detail::secants(v, inv_h_);
    std::vector<Scalar> dp;
    dp.reserve(static_cast<std::size_t>(m));
    for (int i = 1; i <= m; ++i) {
      const Scalar r = d[static_cast<std::size_t>(i)] - d[static_cast<std::size_t>(i) - 1];
      if (i == 1) {
        dp.push_back(r * inv_p_[0]);
      } else {
        dp.push_back((r - sub_[static_cast<std::size_t>(i) - 1] * dp.back()) * inv_p_[static_cast<std::size_t>(i) - 1]);
      }
    }
    std::vector<Scalar> M(static_cast<std::size_t>(m), Scalar(0.0));
    M[static_cast<std::size_t>(m) - 1] = dp[static_cast<std::size_t>(m) - 1];
    for (int i = m - 1; i >= 1; --i) {
      M[static_cast<std::size_t>(i) - 1] =
          dp[static_cast<std::size_t>(i) - 1] - cp_[static_cast<std::size_t>(i) - 1] * M[static_cast<std::size_t>(i)];
    }
    return M;
  }
  // v(t) = A v_k + B v_{k+1} + C M_k + D M_{k+1} with A = (t_{k+1} − t)/h, B = (t − t_k)/h,
  // C = (A³ − A) h²/6, D = (B³ − B) h²/6; the end terms with M = 0 are structure and are not written.
  template <class Scalar>
  Scalar value(const Scalar* v, const std::vector<Scalar>& M, double t) const {
    if (t <= grid_.front()) return v[0];
    if (t >= grid_.back()) return v[n() - 1];
    const int k = grid_.segment(t);
    if (t == grid_[k]) return v[k];
    const double h = grid_.h(k);
    const double A = (grid_[k + 1] - t) / h;
    const double B = (t - grid_[k]) / h;
    const double C = (A * A * A - A) * (h * h) / 6.0;
    const double D = (B * B * B - B) * (h * h) / 6.0;
    Scalar acc = A * v[k];
    acc = acc + B * v[k + 1];
    if (k >= 1) acc = acc + C * M[static_cast<std::size_t>(k) - 1];
    if (k + 1 <= n() - 2) acc = acc + D * M[static_cast<std::size_t>(k)];
    return acc;
  }
  template <class Scalar>
  Scalar value(const Scalar* v, double t) const {
    return value(v, prepare(v), t);
  }

 private:
  void build() {
    inv_h_.clear();
    sub_.clear();
    inv_p_.clear();
    cp_.clear();
    for (int k = 0; k + 1 < n(); ++k) inv_h_.push_back(1.0 / grid_.h(k));
    const int m = n() - 2;
    if (m <= 0) return;
    std::vector<double> a(static_cast<std::size_t>(m)), b(static_cast<std::size_t>(m)), c(static_cast<std::size_t>(m));
    for (int i = 1; i <= m; ++i) {
      const double hl = grid_.h(i - 1), hr = grid_.h(i);
      a[static_cast<std::size_t>(i) - 1] = hl / 6.0;
      b[static_cast<std::size_t>(i) - 1] = (hl + hr) / 3.0;
      c[static_cast<std::size_t>(i) - 1] = hr / 6.0;
    }
    std::vector<double> p(static_cast<std::size_t>(m));
    p[0] = b[0];
    cp_.push_back(c[0] / p[0]);
    for (int i = 1; i < m; ++i) {
      const std::size_t s = static_cast<std::size_t>(i);
      p[s] = b[s] - a[s] * cp_[s - 1];
      cp_.push_back(c[s] / p[s]);
    }
    sub_ = a;
    for (double x : p) inv_p_.push_back(1.0 / x);
  }
  Grid grid_;
  std::vector<double> inv_h_;
  std::vector<double> sub_;    // a_i (sub-diagonal), i = 1 .. m
  std::vector<double> inv_p_;  // 1/p_i
  std::vector<double> cp_;     // c'_i = c_i/p_i
};

// ---------------------------------------------------------------------------------------------
// MonotoneCubic — Fritsch–Carlson tangents through the Hyman filter, every limiter a select
// ---------------------------------------------------------------------------------------------

class MonotoneCubic {
 public:
  MonotoneCubic() = default;
  MonotoneCubic(const double* t, int n) : grid_(t, n) { build(); }
  explicit MonotoneCubic(Grid grid) : grid_(std::move(grid)) { build(); }
  static const char* name() noexcept { return "monotone_cubic"; }
  int n() const noexcept { return grid_.n(); }
  const Grid& grid() const noexcept { return grid_; }

  // The filtered tangents m_0 .. m_{n−1} (empty for n = 1).
  template <class Scalar>
  std::vector<Scalar> prepare(const Scalar* v) const {
    const int nn = n();
    if (nn < 2) return {};
    const std::vector<Scalar> d = detail::secants(v, inv_h_);
    const std::vector<Scalar> m0 = detail::bessel_tangents(rules_, d);
    std::vector<Scalar> m;
    m.reserve(static_cast<std::size_t>(nn));
    for (int k = 0; k < nn; ++k) {
      const bool has_left = k > 0, has_right = k + 1 < nn;
      const Scalar& dl = d[static_cast<std::size_t>(has_left ? k - 1 : k)];
      const Scalar& dr = d[static_cast<std::size_t>(has_right ? k : k - 1)];
      m.push_back(limited(m0[static_cast<std::size_t>(k)], dl, dr, has_left && has_right));
    }
    return m;
  }
  template <class Scalar>
  Scalar value(const Scalar* v, const std::vector<Scalar>& m, double t) const {
    if (t <= grid_.front()) return v[0];
    if (t >= grid_.back()) return v[n() - 1];
    const int k = grid_.segment(t);
    if (t == grid_[k]) return v[k];
    return detail::hermite_value(grid_, v, m, k, t);
  }
  template <class Scalar>
  Scalar value(const Scalar* v, double t) const {
    return value(v, prepare(v), t);
  }

  // The Hyman limiter of one tangent estimate m0 between the secants dl (left) and dr (right);
  // at an end knot both are the one secant and `interior` is false: the bound is 3|d| and there
  // is no sign-change test — structure, so no select whose two arms are the same value is written.
  //   σ    = sign(dr)                              select(dr < 0, −1, +1)         (never dr/|dr|)
  //   lim  = 3·min(|dl|, |dr|)                     abs and min by select (3|dr| at an end knot)
  //   same = σ·m0 clipped to [0, lim]              max / min by select
  //   m    = interior ? select(dl·dr > 0, σ·same, 0) : σ·same
  // Every arm is a product or a selection of finite values: no arm can overflow, divide by zero
  // or produce a NaN for finite inputs (DESIGN.md §10, safe-arm table).
  template <class Scalar>
  static Scalar limited(const Scalar& m0, const Scalar& dl, const Scalar& dr, bool interior) {
    const Scalar sigma = select(dr < 0.0, -1.0, 1.0);
    const Scalar abs_r = select(dr < 0.0, -dr, dr);
    Scalar lim = 3.0 * abs_r;
    if (interior) {
      const Scalar abs_l = select(dl < 0.0, -dl, dl);
      lim = 3.0 * select(abs_r < abs_l, abs_r, abs_l);
    }
    const Scalar sm = sigma * m0;
    const Scalar floored = select(sm < 0.0, 0.0, sm);
    const Scalar clipped = select(lim < floored, lim, floored);
    const Scalar same = sigma * clipped;
    if (!interior) return same;
    return select(dl * dr > 0.0, same, 0.0);
  }

 private:
  void build() {
    inv_h_.clear();
    for (int k = 0; k + 1 < n(); ++k) inv_h_.push_back(1.0 / grid_.h(k));
    rules_ = detail::bessel_rules(grid_);
  }
  Grid grid_;
  std::vector<double> inv_h_;
  std::vector<detail::TangentRule> rules_;
};

// ---------------------------------------------------------------------------------------------
// BSpline — clamped, degree min(3, n − 1), the knot values are the de Boor points
// ---------------------------------------------------------------------------------------------

class BSpline {
 public:
  BSpline() = default;
  BSpline(const double* t, int n) : grid_(t, n) { build(); }
  explicit BSpline(Grid grid) : grid_(std::move(grid)) { build(); }
  static const char* name() noexcept { return "bspline"; }
  int n() const noexcept { return grid_.n(); }
  const Grid& grid() const noexcept { return grid_; }
  int degree() const noexcept { return p_; }
  // The knot vector, n + p + 1 entries.
  const std::vector<double>& knot_vector() const noexcept { return U_; }
  // Greville abscissa of control point j: (u_{j+1} + ... + u_{j+p})/p (t_0 for p = 0).
  double greville(int j) const noexcept {
    if (p_ == 0) return grid_.front();
    double s = 0.0;
    for (int i = 1; i <= p_; ++i) s += U_[static_cast<std::size_t>(j + i)];
    return s / p_;
  }
  // The p+1 basis values at t (inside [t_0, t_{n−1}]) and the first control point they weight.
  // Cox–de Boor (Piegl & Tiller A2.2), double arithmetic.
  void basis(double t, int* first, double* N) const noexcept {
    const int n = this->n();
    int i;
    if (t >= U_[static_cast<std::size_t>(n)]) {
      i = n - 1;  // the last span, closed at its right end
    } else {
      i = p_;
      while (t >= U_[static_cast<std::size_t>(i) + 1]) ++i;
    }
    N[0] = 1.0;
    double left[4], right[4];
    for (int j = 1; j <= p_; ++j) {
      left[j] = t - U_[static_cast<std::size_t>(i + 1 - j)];
      right[j] = U_[static_cast<std::size_t>(i + j)] - t;
      double saved = 0.0;
      for (int r = 0; r < j; ++r) {
        const double temp = N[r] / (right[r + 1] + left[j - r]);
        N[r] = saved + right[r + 1] * temp;
        saved = left[j - r] * temp;
      }
      N[j] = saved;
    }
    *first = i - p_;
  }

  template <class Scalar>
  std::vector<Scalar> prepare(const Scalar*) const {
    return {};
  }
  template <class Scalar>
  Scalar value(const Scalar* v, const std::vector<Scalar>&, double t) const {
    return value(v, t);
  }
  template <class Scalar>
  Scalar value(const Scalar* v, double t) const {
    if (t <= grid_.front()) return v[0];
    if (t >= grid_.back()) return v[n() - 1];
    int first = 0;
    double N[4] = {0.0, 0.0, 0.0, 0.0};
    basis(t, &first, N);
    Scalar acc = N[0] * v[first];
    for (int j = 1; j <= p_; ++j) acc = acc + N[j] * v[first + j];
    return acc;
  }

 private:
  void build() {
    const int n = this->n();
    p_ = n - 1 < 3 ? n - 1 : 3;
    U_.clear();
    for (int i = 0; i <= p_; ++i) U_.push_back(grid_.front());
    for (int j = 1; j <= n - p_ - 1; ++j) {
      double s = 0.0;
      for (int i = j; i <= j + p_ - 1; ++i) s += grid_[i];
      U_.push_back(s / p_);
    }
    for (int i = 0; i <= p_; ++i) U_.push_back(grid_.back());
  }
  Grid grid_;
  int p_ = 0;
  std::vector<double> U_;
};

}  // namespace epykos::curve
