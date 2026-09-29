// EpykosEngine — the residual sub-program of an implicit node and the untaped solve over it
// (M3/G4; DESIGN.md §3.2).
//
// ResidualProgram lifts a block's residuals out of the tape (tape/slice.hpp: the backward slice
// from the residual outputs, Inputs kept as Inputs), infers the domain IR of that slice and
// builds its interpreter and adjoint. Its inputs are the block's unknowns z and its parameters p
// — every other tape input the residuals reach: quotes and earlier blocks' unknowns — and it
// offers the three evaluations a solver needs, all on double, all through the recorded maths:
//
//   evaluate     F(z, p)                               one interpreter lane
//   jacobian     F, J_z = ∂F/∂z, J_p = ∂F/∂p           one batched adjoint, one lane per residual
//                                                      (out_bar = e_i; rows of J = state_bar lanes)
//   jt_product   F, Jᵀv                                one adjoint lane (matrix-free)
//
// Factors is the factorised J_z (with J_p alongside) at a point — what the Newton step, the IFT
// reverse rule λ = J_z⁻ᵀ z̄, p̄ −= J_pᵀ λ and the forward rule dz = −J_z⁻¹ J_p dp use. Square
// systems are LU with partial pivoting; an over-determined block (more residuals than unknowns)
// uses the Gauss–Newton normal equations, stated as such. Eigen does the linear algebra, on the
// double side only (D12, D14), hidden behind this header.
//
// BlockSolver runs Gauss–Newton / Levenberg–Marquardt for one lane: step J_z δ = −F (damped
// (J_zᵀJ_z + μ diag) δ = −J_zᵀF after a rejected step), accept when ‖F‖∞ falls, stop at
// ‖F‖∞ < tol or max_iterations. Under JacobianPolicy::chord the steps use a factorisation given
// by the caller (the record-point one, shared by every lane) until the contraction stalls, then
// the lane's own. After convergence the Jacobian at the solution is evaluated once (options
// .final_jacobian) for the O1 diagnostic ‖JᵀF‖∞ and for the IFT.
//
// Thread safety: each object owns its scratch; one per thread.
#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "epykos/adjoint/adjoint.hpp"
#include "epykos/exec/interpreter.hpp"
#include "epykos/ir/program.hpp"
#include "epykos/solver/implicit.hpp"
#include "epykos/tape/slice.hpp"
#include "epykos/tape/tape.hpp"

namespace epykos::solver {

class ResidualProgram {
 public:
  // Slices `tape` from block.residuals, runs the E0 tape passes on the slice when `passes` (cse,
  // dce, fold_sum, affine_collapse, dce: the record-time solve on a raw recording), infers the
  // IR and builds the interpreter (lane tile 1) and the adjoint (up to `max_batch` lanes per
  // call). Throws std::invalid_argument when an unknown is read by no residual (a zero Jacobian
  // column) or the block has fewer residuals than unknowns.
  ResidualProgram(const Tape& tape, const ImplicitBlock& block, bool passes = true, int max_batch = 64);
  ~ResidualProgram();
  ResidualProgram(const ResidualProgram&) = delete;
  ResidualProgram& operator=(const ResidualProgram&) = delete;

  int n_unknowns() const noexcept { return n_z_; }
  int n_residuals() const noexcept { return n_r_; }
  int n_params() const noexcept { return n_p_; }
  // Tape input ordinals of the parameters, ascending (index m of p), and of the unknowns (index j of z).
  const std::vector<int>& param_ordinals() const noexcept { return param_ordinals_; }
  const std::vector<int>& unknown_ordinals() const noexcept { return unknown_ordinals_; }

  // F[i], i < n_residuals, at (z[0..n_z), p[0..n_p)).
  void evaluate(const double* z, const double* p, double* F);
  // F, Jz[i·n_z + j] = ∂F_i/∂z_j, Jp[i·n_p + m] = ∂F_i/∂p_m (Jp may be null when n_p == 0).
  void jacobian(const double* z, const double* p, double* F, double* Jz, double* Jp);
  // F and Jᵀv: jtz[j] = Σ_i v_i ∂F_i/∂z_j, jtp[m] = Σ_i v_i ∂F_i/∂p_m (jtp may be null).
  void jt_product(const double* z, const double* p, const double* v, double* F, double* jtz, double* jtp);

  const ir::Program& program() const noexcept { return program_; }
  const Tape& tape() const noexcept { return slice_.tape; }
  const Slice& slice() const noexcept { return slice_; }
  std::size_t evaluations() const noexcept { return evaluations_; }
  std::size_t jacobians() const noexcept { return jacobians_; }
  std::string describe() const;

 private:
  void fill_state(const double* z, const double* p, int B);

  Slice slice_;
  ir::Program program_;
  int n_z_ = 0, n_r_ = 0, n_p_ = 0;
  std::vector<int> param_ordinals_;
  std::vector<int> unknown_ordinals_;
  std::vector<int> z_slot_;   // sub input ordinal of unknown j
  std::vector<int> p_slot_;   // sub input ordinal of parameter m
  std::unique_ptr<exec::Interpreter> interp_;
  std::unique_ptr<adjoint::Adjoint> adj_;
  int adj_batch_ = 0;
  std::vector<double> state_;      // (n_z + n_p) × B, batch innermost
  std::vector<double> out_;        // n_r × B
  std::vector<double> out_bar_;    // n_r × B
  std::vector<double> state_bar_;  // (n_z + n_p) × B
  std::size_t evaluations_ = 0;
  std::size_t jacobians_ = 0;
};

// The factorised Jacobian of a block at one point: J_z (n_r × n_z, LU when square, else the
// normal equations) and J_p (n_r × n_p). Opaque; Eigen inside.
class Factors {
 public:
  Factors();
  ~Factors();
  Factors(Factors&&) noexcept;
  Factors& operator=(Factors&&) noexcept;
  Factors(const Factors&);
  Factors& operator=(const Factors&);

  // Row-major Jz[i·n_z + j], Jp[i·n_p + m] (Jp may be null when n_p == 0).
  void compute(int n_r, int n_z, int n_p, const double* Jz, const double* Jp);
  bool valid() const noexcept;
  int n_residuals() const noexcept;
  int n_unknowns() const noexcept;
  int n_params() const noexcept;
  bool square() const noexcept;

  // dz = J_z⁻¹ rhs (least squares through the normal equations when not square).
  void solve(const double* rhs, double* dz) const;
  // λ = J_z⁻ᵀ z̄ (square) or J_z (J_zᵀJ_z)⁻¹ z̄ (Gauss–Newton IFT for the over-determined case): the
  // multiplier of the implicit-function rule; λ has n_residuals entries.
  void solve_transposed(const double* z_bar, double* lambda) const;
  // out[m] = Σ_i λ_i J_p[i, m]  (J_pᵀ λ, n_params entries).
  void jp_transposed(const double* lambda, double* out) const;
  // out[i] = Σ_m J_p[i, m] dp[m]  (J_p dp, n_residuals entries).
  void jp_times(const double* dp, double* out) const;

  // The IFT reverse rule: p_bar[m] -= (J_pᵀ J_z⁻ᵀ z̄)[m]. `lambda` receives J_z⁻ᵀ z̄ (n_residuals).
  void ift_adjoint(const double* z_bar, double* lambda, double* p_bar) const;
  // The forward rule: dz = −J_z⁻¹ (J_p dp). `scratch` has n_residuals entries.
  void ift_tangent(const double* dp, double* scratch, double* dz) const;

  std::size_t bytes() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

class BlockSolver {
 public:
  BlockSolver(ResidualProgram& rp, const SolveOptions& options);
  ~BlockSolver();
  BlockSolver(const BlockSolver&) = delete;
  BlockSolver& operator=(const BlockSolver&) = delete;

  // Solves F(z, p) = 0 from z in place. `factors` receives J at the exit point (the solution
  // when it converged and options.final_jacobian; else the last iterate's, or, under the chord
  // policy without a refresh and without final_jacobian, a copy of `shared`). `shared`, when
  // given under JacobianPolicy::chord, drives the steps. residual() holds F at exit.
  SolveReport solve(const double* p, double* z, Factors& factors, const Factors* shared = nullptr);
  const std::vector<double>& residual() const noexcept { return F_; }
  const SolveOptions& options() const noexcept { return options_; }

 private:
  ResidualProgram& rp_;
  SolveOptions options_;
  std::vector<double> F_, F_try_, z_try_, delta_, Jz_, Jp_, jtf_;
  Factors own_;
  struct Impl;
  std::unique_ptr<Impl> impl_;  // damped-step workspace (Eigen)
};

}  // namespace epykos::solver
