// EpykosEngine — the forward-mode rule of the implicit node, for Dual (M3/G4).
//
// At a solution of F(z, p) = 0, dz = −F_z⁻¹ F_p dp. Factors::ift_tangent applies it to a
// recorded block (solver/residual.hpp). This header gives the same rule for the templated
// maths itself, so that a calibration written once on Scalar can be instantiated on Dual<N>
// (scalar/dual.hpp) and serve as an oracle for the IFT adjoint (the M2 pattern: adjoint vs
// forward mode at 1e-12):
//
//   solve_newton(R, n_z, n_p, n_r, p, z, opts)   Newton / damped Newton on double with the
//                                                Jacobian from n_z passes of R on Dual<1>
//   implicit_dual<N>(R, n_z, n_p, n_r, p, z, opts)  the value channel by solve_newton at p.v, the
//                                                tangent channel dz = −F_z⁻¹ (F_p dp) with F_p dp
//                                                from one pass of R on Dual<N> (z held constant)
//
// R is any callable `void R(const Scalar* z, const Scalar* p, Scalar* F)` usable on double,
// Dual<1> and Dual<N>. The dense solves are a small partial-pivoting LU written here (a header
// cannot pull Eigen: D14 keeps it off the Scalar-templated side; n_z is a few dozen). Test and
// oracle code; not a hot path.
#pragma once

#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <vector>

#include "epykos/scalar/dual.hpp"
#include "epykos/solver/implicit.hpp"

namespace epykos::solver {

namespace detail {

// In-place LU with partial pivoting of the row-major n × n matrix a; perm receives the row
// permutation. Throws std::runtime_error on a zero pivot.
inline void lu_factor(int n, std::vector<double>& a, std::vector<int>& perm) {
  const auto N = static_cast<std::size_t>(n);
  perm.resize(N);
  for (std::size_t i = 0; i < N; ++i) perm[i] = static_cast<int>(i);
  for (std::size_t k = 0; k < N; ++k) {
    std::size_t piv = k;
    double best = std::fabs(a[k * N + k]);
    for (std::size_t i = k + 1; i < N; ++i) {
      const double v = std::fabs(a[i * N + k]);
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
      const double l = a[i * N + k];
      for (std::size_t j = k + 1; j < N; ++j) a[i * N + j] -= l * a[k * N + j];
    }
  }
}

// Solves (LU) x = b with the factors of lu_factor; x may alias nothing.
inline void lu_solve(int n, const std::vector<double>& lu, const std::vector<int>& perm, const double* b, double* x) {
  const auto N = static_cast<std::size_t>(n);
  for (std::size_t i = 0; i < N; ++i) {
    double s = b[static_cast<std::size_t>(perm[i])];
    for (std::size_t j = 0; j < i; ++j) s -= lu[i * N + j] * x[j];
    x[i] = s;
  }
  for (std::size_t i = N; i-- > 0;) {
    double s = x[i];
    for (std::size_t j = i + 1; j < N; ++j) s -= lu[i * N + j] * x[j];
    x[i] = s / lu[i * N + i];
  }
}

}  // namespace detail

// The Jacobian F_z (row-major n_r × n_z) at (z, p) by n_z passes of R on Dual<1>; F receives
// the value channel.
template <class Residual>
void jacobian_dual(Residual& R, int n_z, int n_p, int n_r, const double* p, const double* z, double* F, double* Jz) {
  const auto Z = static_cast<std::size_t>(n_z);
  const auto P = static_cast<std::size_t>(n_p);
  const auto Rn = static_cast<std::size_t>(n_r);
  std::vector<Dual<1>> zd(Z), pd(P), Fd(Rn);
  for (std::size_t m = 0; m < P; ++m) pd[m] = Dual<1>(p[m]);
  for (std::size_t j = 0; j < Z; ++j) {
    for (std::size_t k = 0; k < Z; ++k) zd[k] = Dual<1>(z[k]);
    zd[j] = Dual<1>::variable(z[j], 0);
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

// The implicit node on Dual<N>: z (n_z, out) = the solution at p.v with tangents
// dz = −F_z⁻¹ (F_p dp), where F_p dp for the N directions comes from one pass of R on Dual<N>
// with z held constant. Square blocks only. Returns the value solve's report.
template <int N, class Residual>
SolveReport implicit_dual(Residual& R, int n_z, int n_p, int n_r, const Dual<N>* p, Dual<N>* z,
                          const SolveOptions& options = {}) {
  const auto Z = static_cast<std::size_t>(n_z);
  const auto P = static_cast<std::size_t>(n_p);
  const auto Rn = static_cast<std::size_t>(n_r);
  std::vector<double> pv(P), zv(Z);
  for (std::size_t m = 0; m < P; ++m) pv[m] = p[m].v;
  for (std::size_t j = 0; j < Z; ++j) zv[j] = z[j].v;
  const SolveReport rep = solve_newton(R, n_z, n_p, n_r, pv.data(), zv.data(), options);
  // F_z at the solution and its LU.
  std::vector<double> F(Rn), Jz(Rn * Z);
  jacobian_dual(R, n_z, n_p, n_r, pv.data(), zv.data(), F.data(), Jz.data());
  std::vector<int> perm;
  detail::lu_factor(n_z, Jz, perm);
  // F_p dp for every direction: one Dual<N> pass with z constant.
  std::vector<Dual<N>> zd(Z), Fd(Rn);
  for (std::size_t j = 0; j < Z; ++j) zd[j] = Dual<N>(zv[j]);
  R(zd.data(), p, Fd.data());
  std::vector<double> rhs(Rn), dz(Z);
  for (int dir = 0; dir < N; ++dir) {
    for (std::size_t i = 0; i < Rn; ++i) rhs[i] = -Fd[i].d[static_cast<std::size_t>(dir)];
    detail::lu_solve(n_z, Jz, perm, rhs.data(), dz.data());
    for (std::size_t j = 0; j < Z; ++j) z[j].d[static_cast<std::size_t>(dir)] = dz[j];
  }
  for (std::size_t j = 0; j < Z; ++j) z[j].v = zv[j];
  return rep;
}

}  // namespace epykos::solver
