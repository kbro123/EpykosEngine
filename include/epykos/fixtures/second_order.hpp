// EpykosEngine — second order by nesting the forward-mode scalar (PRINCIPLES.md §1b invariant I2;
// a test-only fixture, D28, the sibling of fixtures/m1_tangent.hpp).
//
// `Dual<N, Dual<N>>` carries N outer directions of N inner ones, so ONE pass of maths written once
// on Scalar yields the value, both first-derivative channels and the whole N x N Hessian. There is
// no hand-written second derivative anywhere: the header's first-order rules applied to a scalar
// that is itself a Dual ARE the second-order rules (CLAUDE.md, "no hand-written derivatives").
//
//   second_order<N>(f, z)        one nested pass:  value, gradient, Hessian
//   hessian_central<N>(f, z, h)  the reference:    central differences of the EXACT first
//                                derivatives (one Dual<N> pass per shifted point) — 2N passes
//   agreement(H, Href)           the statistics the gate is stated in
//
// `f` is a generic callable `Scalar f(const Scalar*)`, i.e. the pricing maths with its curve and
// tables bound; it is instantiated here on Dual<N> and on Dual<N, Dual<N>>.
//
// Seeding (scalar/dual.hpp): input k gets the inner seed e_k in its VALUE channel and the outer
// seed e_k in its TANGENT channel, so r.d[i].d[j] is d2f/dz_i dz_j and r.d[i].v / r.v.d[j] are
// both the gradient. Header-only, so the including TU's contraction setting governs the
// arithmetic: under -ffp-contract=off the value channel is bitwise the double evaluation and both
// first-derivative channels are bitwise the Dual<N> pass (tests/maths/second_order_e0_test.cpp).
// Allocates nothing itself (test code; `f` may).
#pragma once

#include <array>
#include <cmath>
#include <cstddef>

#include "epykos/scalar/dual.hpp"

namespace epykos::fixtures {

// The result of one nested pass over n = N inputs.
template <int N>
struct SecondOrder {
  static constexpr int n = N;
  double value = 0.0;
  std::array<double, N> grad{};        // outer tangent channel: d f / d z_i
  std::array<double, N> grad_inner{};  // inner tangent channel: the same gradient by the other route
  std::array<double, N * N> hess{};    // hess[i*N + j] = d2 f / d z_i d z_j

  double at(int i, int j) const { return hess[static_cast<std::size_t>(i) * N + static_cast<std::size_t>(j)]; }
  // max |H_ij| — the scale every deviation below is stated against.
  double norm() const {
    double m = 0.0;
    for (double v : hess) m = std::fmax(m, std::fabs(v));
    return m;
  }
  // max |H_ij − H_ji| / max|H|: Clairaut's theorem, which the nested pass satisfies to roundoff
  // and no further (the two mixed partials are different operation orders).
  double asymmetry() const {
    const double s = norm();
    if (!(s > 0.0)) return 0.0;
    double m = 0.0;
    for (int i = 0; i < N; ++i) {
      for (int j = 0; j < N; ++j) m = std::fmax(m, std::fabs(at(i, j) - at(j, i)));
    }
    return m / s;
  }
};

// One pass of f on Dual<N, Dual<N>> at z.
template <int N, class F>
SecondOrder<N> second_order(F&& f, const double* z) {
  using Inner = Dual<N>;
  using Outer = Dual<N, Inner>;
  std::array<Outer, N> x{};
  for (int k = 0; k < N; ++k) {
    x[static_cast<std::size_t>(k)] = Outer::variable(Inner::variable(z[k], k), k);
  }
  const Outer r = f(x.data());
  SecondOrder<N> s;
  s.value = r.v.v;
  for (int i = 0; i < N; ++i) {
    const std::size_t si = static_cast<std::size_t>(i);
    s.grad[si] = r.d[si].v;
    s.grad_inner[si] = r.v.d[si];
    for (int j = 0; j < N; ++j) s.hess[si * N + static_cast<std::size_t>(j)] = r.d[si].d[static_cast<std::size_t>(j)];
  }
  return s;
}

// The exact gradient at z by one Dual<N> pass (no differencing).
template <int N, class F>
std::array<double, N> gradient(F&& f, const double* z) {
  using Inner = Dual<N>;
  std::array<Inner, N> x{};
  for (int k = 0; k < N; ++k) x[static_cast<std::size_t>(k)] = Inner::variable(z[k], k);
  const Inner y = f(x.data());
  std::array<double, N> g{};
  for (int k = 0; k < N; ++k) g[static_cast<std::size_t>(k)] = y.d[static_cast<std::size_t>(k)];
  return g;
}

// H[i][j] ~ (df/dz_i (z + h e_j) − df/dz_i (z − h e_j)) / 2h: central differences of the EXACT
// first derivatives, 2N forward passes. This is the independent reference the second derivative is
// judged against; its own error is O(h²) truncation plus O(eps/h) cancellation, and it — not the
// nested dual — is what limits the agreement.
template <int N, class F>
std::array<double, N * N> hessian_central(F&& f, const double* z, double h) {
  std::array<double, N * N> H{};
  std::array<double, N> zz{};
  for (int k = 0; k < N; ++k) zz[static_cast<std::size_t>(k)] = z[k];
  for (int j = 0; j < N; ++j) {
    const std::size_t sj = static_cast<std::size_t>(j);
    zz[sj] = z[j] + h;
    const std::array<double, N> gp = gradient<N>(f, zz.data());
    zz[sj] = z[j] - h;
    const std::array<double, N> gm = gradient<N>(f, zz.data());
    zz[sj] = z[j];
    for (int i = 0; i < N; ++i) {
      H[static_cast<std::size_t>(i) * N + sj] =
          (gp[static_cast<std::size_t>(i)] - gm[static_cast<std::size_t>(i)]) / (2.0 * h);
    }
  }
  return H;
}

// How far apart two Hessians are. `rel_norm` is the figure the gate is stated in: the largest
// deviation measured against the largest entry, which is the only scale a Hessian has. `rel_top`
// repeats it entry by entry over the DOMINANT entries (|H_ij| >= 0.1 max|H|), where the central
// difference still has digits to spare; below that cut the reference, not the dual, runs out.
struct Agreement {
  double norm = 0.0;      // max |H_ij| of the nested Hessian
  double max_abs = 0.0;   // max |H_ij − Href_ij|
  double rel_norm = 0.0;  // max_abs / norm
  double rel_top = 0.0;   // max |H_ij − Href_ij| / |H_ij| over |H_ij| >= 0.1 max|H|
  int n_top = 0;          // how many entries that cut kept
};

template <int N>
Agreement agreement(const SecondOrder<N>& s, const std::array<double, N * N>& ref) {
  Agreement a;
  a.norm = s.norm();
  for (std::size_t k = 0; k < s.hess.size(); ++k) {
    const double x = s.hess[k], y = ref[k];
    const double d = std::fabs(x - y);
    a.max_abs = std::fmax(a.max_abs, d);
    if (a.norm > 0.0 && std::fabs(x) >= 0.1 * a.norm) {
      a.rel_top = std::fmax(a.rel_top, d / std::fabs(x));
      ++a.n_top;
    }
  }
  if (a.norm > 0.0) a.rel_norm = a.max_abs / a.norm;
  return a;
}

}  // namespace epykos::fixtures
