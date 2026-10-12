// EpykosEngine — the forward-mode rule of the implicit node, for Dual (M3/G4), on a NESTING
// scalar (PRINCIPLES.md §1b, invariant I2).
//
// At a solution of F(z, p) = 0, dz = −F_z⁻¹ F_p dp. Factors::ift_tangent applies it to a
// recorded block (solver/residual.hpp). This header gives the same rule for the templated
// maths itself, so that a calibration written once on Scalar can be instantiated on Dual
// (scalar/dual.hpp) and serve as an oracle for the IFT adjoint (the M2 pattern: adjoint vs
// forward mode at 1e-12):
//
//   solve_newton(R, n_z, n_p, n_r, p, z, opts)   Newton / damped Newton on double with the
//                                                Jacobian from n_z passes of R on Dual<1>
//   jacobian_dual(R, ..., p, z, F, Jz)           F and F_z on ANY scalar S: n_z passes of R on
//                                                Dual<1, S>, so for a nesting S the entries of
//                                                F_z carry their own derivatives
//   implicit_dual<N>(R, n_z, n_p, n_r, p, z, opts)  the implicit node on Dual<N, S>
//
// R is any callable `void R(const Scalar* z, const Scalar* p, Scalar* F)` usable on double and on
// every Dual the caller asks for. The dense solves are a small partial-pivoting LU written here
// (a header cannot pull Eigen: D14 keeps it off the Scalar-templated side; n_z is a few dozen).
// Test and oracle code; not a hot path.
//
// ---------------------------------------------------------------------------------------------
// SECOND ORDER THROUGH THE CALIBRATION, AND WHY NOTHING HERE WRITES A SECOND DERIVATIVE DOWN
// ---------------------------------------------------------------------------------------------
// `implicit_dual<N, S>` is the first-order rule and nothing else. Every quantity it builds —
// F_z, its LU factorisation, the triangular solves, F_p dp — is built on the scalar S, which may
// itself be a Dual. So instantiate it at S = Dual<M> and the rule differentiates once more by
// itself: F_z's entries arrive carrying dF_z/dw, the rhs arrives carrying d(F_p dp)/dw, and the
// solve returns dz carrying
//
//     d(dz)/dw = −F_z⁻¹ ( F_zz·ż·ż + 2·F_zp·ż + F_pp )
//
// in its inner tangent — not because that expression appears anywhere in this file, but because
// it is what differentiating −F_z⁻¹ F_p ṗ yields. That is the whole mechanism (CLAUDE.md, "no
// hand-written derivatives"): the same code path that computes a delta computes a gamma when the
// scalar under it nests. Third order needs no further code either.
//
// The depth recursion is in the VALUE channel, not in the rule. Solving for z on the scalar S is
//
//     S = double        -> solve_newton: the damped Newton iteration, on doubles
//     S = Dual<M, T>    -> implicit_dual<M, T>: ITS value channel recurses, ITS tangents are the
//                          first-order rule at depth M
//
// so a nested call runs exactly ONE Newton iteration however deep the nesting goes. That is the
// point of the implicit-function rule and PRINCIPLES.md §1b's reading of I3: the iteration is a
// fixpoint, not an expression, and nothing differentiates through it. `solve_newton` therefore
// stays on double and is not templated on S.
#pragma once

#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

#include "epykos/scalar/dual.hpp"
#include "epykos/solver/implicit.hpp"

namespace epykos::solver {

namespace detail {

// The value channel of a scalar as a plain double, at any nesting depth. Explicit: scalar/dual.hpp
// forbids an implicit Dual -> double at every depth, and this is a deliberate read of one channel,
// used only by the pivot search below.
inline double real_value(double x) noexcept { return x; }
template <int N, class S>
double real_value(const Dual<N, S>& x) noexcept {
  return real_value(x.v);
}

// In-place LU with partial pivoting of the row-major n × n matrix a over the scalar S (double, or
// a Dual whose entries carry their own derivatives); perm receives the row permutation. Throws
// std::runtime_error on a zero pivot.
//
// THE PIVOT, and why it is neither `select` nor `structural_if`. Partial pivoting compares
// magnitudes and interchanges rows. On a Dual the comparison is made on the VALUE CHANNEL alone
// (detail::real_value) and the interchange moves whole scalars, every channel with them. The two
// branch primitives of the recording discipline (CLAUDE.md) are both the wrong tool, for
// different reasons:
//
//   * `structural_if` asserts that the predicate provably does not depend on the differentiated
//     inputs, and throws when it demonstrably does. Here it demonstrably does — every entry of
//     F_z is a differentiable function of the quotes and carries a nonzero tangent — so
//     structural_if would throw DualError on the first nested solve. It is not that the check is
//     inconvenient: the predicate really is input-dependent.
//   * `select` is the vocabulary for a VALUE: select(c, a, b) returns one of two scalars with
//     that arm's tangents. Element by element it WOULD express the interchange, and would give
//     bit-identical results to the swap, because the arms are whole scalars and the chosen arm
//     carries all of its channels. It is still the wrong tool: a permutation is one decision for
//     the whole elimination, not n decisions about n values, and writing it as n selects per step
//     computes the same thing n times over.
//
// What makes the value channel the CORRECT channel to decide on is not economy, it is the two
// facts that x = A⁻¹b does not depend on the pivot order (in exact arithmetic every order returns
// the same solution, so the derivative of the solution is pivot-independent as well) while the
// ROUNDING does. Deciding on the value channel pins the elimination to the one the plain-`double`
// factorisation would have taken, which is what keeps the first-order answer bitwise what it was
// before S could nest. A pivot allowed to see a tangent would be a different elimination of the
// same matrix. tests/solver/second_order_ift_e0_test.cpp gates both halves of that: the pivot
// sequence against the double factorisation's, and the first-order answer bitwise.
template <class S>
void lu_factor(int n, std::vector<S>& a, std::vector<int>& perm) {
  const auto N = static_cast<std::size_t>(n);
  perm.resize(N);
  for (std::size_t i = 0; i < N; ++i) perm[i] = static_cast<int>(i);
  for (std::size_t k = 0; k < N; ++k) {
    std::size_t piv = k;
    double best = std::fabs(real_value(a[k * N + k]));
    for (std::size_t i = k + 1; i < N; ++i) {
      const double v = std::fabs(real_value(a[i * N + k]));
      if (v > best) {
        best = v;
        piv = i;
      }
    }
    if (best == 0.0) throw std::runtime_error("solver::lu_factor: singular Jacobian");
    if (piv != k) {
      for (std::size_t j = 0; j < N; ++j) std::swap(a[k * N + j], a[piv * N + j]);
      std::swap(perm[k], perm[piv]);
    }
    for (std::size_t i = k + 1; i < N; ++i) {
      a[i * N + k] /= a[k * N + k];
      const S l = a[i * N + k];
      for (std::size_t j = k + 1; j < N; ++j) a[i * N + j] -= l * a[k * N + j];
    }
  }
}

// Solves (LU) x = b with the factors of lu_factor, on the same scalar S; x may alias nothing.
template <class S>
void lu_solve(int n, const std::vector<S>& lu, const std::vector<int>& perm, const S* b, S* x) {
  const auto N = static_cast<std::size_t>(n);
  for (std::size_t i = 0; i < N; ++i) {
    S s = b[static_cast<std::size_t>(perm[i])];
    for (std::size_t j = 0; j < i; ++j) s -= lu[i * N + j] * x[j];
    x[i] = s;
  }
  for (std::size_t i = N; i-- > 0;) {
    S s = x[i];
    for (std::size_t j = i + 1; j < N; ++j) s -= lu[i * N + j] * x[j];
    x[i] = s / lu[i * N + i];
  }
}

}  // namespace detail

// The Jacobian F_z (row-major n_r × n_z) at (z, p) by n_z passes of R on Dual<1, S>; F receives
// the value channel. S = double gives the plain Jacobian; for a nesting S every entry of F_z and
// of F carries its own derivatives, which is what makes the second-order rule below mechanical.
template <class Residual, class S>
void jacobian_dual(Residual& R, int n_z, int n_p, int n_r, const S* p, const S* z, S* F, S* Jz) {
  using D1 = Dual<1, S>;
  const auto Z = static_cast<std::size_t>(n_z);
  const auto P = static_cast<std::size_t>(n_p);
  const auto Rn = static_cast<std::size_t>(n_r);
  std::vector<D1> zd(Z), pd(P), Fd(Rn);
  for (std::size_t m = 0; m < P; ++m) pd[m] = D1(p[m]);
  for (std::size_t j = 0; j < Z; ++j) {
    for (std::size_t k = 0; k < Z; ++k) zd[k] = D1(z[k]);
    zd[j] = D1::variable(z[j], 0);
    R(zd.data(), pd.data(), Fd.data());
    for (std::size_t i = 0; i < Rn; ++i) {
      Jz[i * Z + j] = Fd[i].d[0];
      if (j == 0) F[i] = Fd[i].v;
    }
  }
}

// Newton on double for a square block (n_r == n_z): steps F_z δ = −F with the Dual<1>
// Jacobian, halved while ‖F‖∞ does not fall (damped Newton), until ‖F‖∞ < options.tol or
// max_iterations. Returns the report; z is updated in place.
//
// On double, and deliberately so: the iteration is a fixpoint, not an expression, and the
// implicit-function rule exists precisely so that nothing differentiates through it. Every
// derivative of the solution, to any order, comes from implicit_dual below.
template <class Residual>
SolveReport solve_newton(Residual& R, int n_z, int n_p, int n_r, const double* p, double* z,
                         const SolveOptions& options = {}) {
  if (n_r != n_z) throw std::invalid_argument("solver::solve_newton: a square block is required");
  const auto Z = static_cast<std::size_t>(n_z);
  std::vector<double> F(Z), Jz(Z * Z), delta(Z), z_try(Z), F_try(Z), rhs(Z);
  std::vector<int> perm;
  SolveReport rep;
  auto inf_norm = [&](const std::vector<double>& v) {
    double m = 0.0;
    for (double x : v) m = std::fmax(m, std::fabs(x));
    return m;
  };
  jacobian_dual(R, n_z, n_p, n_r, p, z, F.data(), Jz.data());
  ++rep.jacobians;
  ++rep.residual_evaluations;
  rep.residual_inf = inf_norm(F);
  for (int it = 0; it < options.max_iterations && rep.residual_inf >= options.tol; ++it) {
    std::vector<double> lu = Jz;
    detail::lu_factor(n_z, lu, perm);
    for (std::size_t i = 0; i < Z; ++i) rhs[i] = -F[i];
    detail::lu_solve(n_z, lu, perm, rhs.data(), delta.data());
    double step = 1.0;
    bool accepted = false;
    for (int t = 0; t < options.max_damping_tries; ++t) {
      for (std::size_t j = 0; j < Z; ++j) z_try[j] = z[j] + step * delta[j];
      jacobian_dual(R, n_z, n_p, n_r, p, z_try.data(), F_try.data(), Jz.data());
      ++rep.jacobians;
      ++rep.residual_evaluations;
      const double r_try = inf_norm(F_try);
      if (r_try < rep.residual_inf) {
        for (std::size_t j = 0; j < Z; ++j) z[j] = z_try[j];
        F = F_try;
        rep.residual_inf = r_try;
        accepted = true;
        break;
      }
      ++rep.damping_retries;
      step *= 0.5;
    }
    ++rep.iterations;
    if (!accepted) break;
  }
  rep.converged = rep.residual_inf < options.tol;
  // ‖JᵀF‖∞ with the Jacobian at the exit point.
  jacobian_dual(R, n_z, n_p, n_r, p, z, F.data(), Jz.data());
  ++rep.jacobians;
  double jtr = 0.0;
  for (std::size_t j = 0; j < Z; ++j) {
    double s = 0.0;
    for (std::size_t i = 0; i < Z; ++i) s += Jz[i * Z + j] * F[i];
    jtr = std::fmax(jtr, std::fabs(s));
  }
  rep.jtr_inf = jtr;
  if (!rep.converged && options.throw_on_failure) throw std::runtime_error("solver::solve_newton: no convergence");
  return rep;
}

// The implicit node on Dual<N, S>: z (n_z, out) = the solution at p's value channel, with
// tangents dz = −F_z⁻¹ (F_p dp), where F_z comes from n_z passes of R on Dual<1, S> and F_p dp
// for the N directions from one pass of R on Dual<N, S> with z held constant. Square blocks only.
// Returns the value solve's report.
//
// S = double is the first-order rule, unchanged in every respect and bitwise what it was before
// this header could nest. S = Dual<M> is second order: see the header comment — the rule is the
// same rule, and the second-order implicit-function term is what differentiating it produces.
// Seeding, for a Hessian with respect to the parameters (fixtures/second_order.hpp does the same
// for the uncalibrated maths): parameter k gets the inner seed e_k in its value channel and the
// outer seed e_k in its tangent channel, and then
//   z[j].v.v        = z*_j                  z[j].v.d[i] = dz_j/dp_i   (the inner route)
//   z[j].d[i].v     = dz_j/dp_i             z[j].d[i].d[l] = d2 z_j / dp_i dp_l
// `z` carries the start point in on its value channel; its tangents on entry are ignored.
template <int N, class S, class Residual>
SolveReport implicit_dual(Residual& R, int n_z, int n_p, int n_r, const Dual<N, S>* p, Dual<N, S>* z,
                          const SolveOptions& options = {}) {
  using Scalar = Dual<N, S>;
  const auto Z = static_cast<std::size_t>(n_z);
  const auto P = static_cast<std::size_t>(n_p);
  const auto Rn = static_cast<std::size_t>(n_r);
  // The value channel: the solution on S, with every derivative S itself carries.
  std::vector<S> pv(P), zv(Z);
  for (std::size_t m = 0; m < P; ++m) pv[m] = p[m].v;
  for (std::size_t j = 0; j < Z; ++j) zv[j] = z[j].v;
  SolveReport rep;
  if constexpr (std::is_same_v<S, double>) {
    rep = solve_newton(R, n_z, n_p, n_r, pv.data(), zv.data(), options);
  } else {
    // One level down: the same rule, so S's own tangents are the next derivative. The Newton
    // iteration is reached exactly once, at the bottom of the recursion.
    rep = implicit_dual<S::n_tangents>(R, n_z, n_p, n_r, pv.data(), zv.data(), options);
  }
  // F_z at the solution and its LU, on S. For a nesting S these entries carry dF_z/dw, and that
  // is the only reason the second-order term appears at all.
  std::vector<S> F(Rn), Jz(Rn * Z);
  jacobian_dual(R, n_z, n_p, n_r, pv.data(), zv.data(), F.data(), Jz.data());
  std::vector<int> perm;
  detail::lu_factor(n_z, Jz, perm);
  // F_p dp for every direction: one Dual<N, S> pass with z constant at the solution. z enters as
  // a constant OF THIS LEVEL — its value channel keeps whatever derivatives S carries, which is
  // what makes the rhs's own derivative the total one (F_pz·ż + F_pp) rather than a partial.
  std::vector<Scalar> zd(Z), Fd(Rn);
  for (std::size_t j = 0; j < Z; ++j) zd[j] = Scalar(zv[j]);
  R(zd.data(), p, Fd.data());
  std::vector<S> rhs(Rn), dz(Z);
  for (int dir = 0; dir < N; ++dir) {
    for (std::size_t i = 0; i < Rn; ++i) rhs[i] = -Fd[i].d[static_cast<std::size_t>(dir)];
    detail::lu_solve(n_z, Jz, perm, rhs.data(), dz.data());
    for (std::size_t j = 0; j < Z; ++j) z[j].d[static_cast<std::size_t>(dir)] = dz[j];
  }
  for (std::size_t j = 0; j < Z; ++j) z[j].v = zv[j];
  return rep;
}

}  // namespace epykos::solver
