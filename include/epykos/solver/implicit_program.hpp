// EpykosEngine — executing a tape with implicit nodes: forward for a batch of lanes, the IFT
// adjoint (M3/G4; DESIGN.md §3.2, PROBLEM.md §5).
//
// ImplicitProgram takes the one tape and its ImplicitRegistry and builds
//   * the whole program: ir::infer(tape) — one IR in which the residual sub-programs and the
//     book share their domains (the cross-stage sharing of PROBLEM.md §5 is visible here;
//     ir/sharing.hpp asserts it) — with its exec::Interpreter and adjoint::Adjoint over every
//     tape input (free and solved);
//   * per block, its ResidualProgram (the slice) and a BlockSolver.
//
// State layout (D15): the caller's state is the FREE inputs — the tape inputs no block writes
// (quotes, FX spots, model parameters), ascending ordinal, index k = free_inputs()[k] —
// state[k·B + b] for lane b; outputs are every tape output, out[o·B + b] (residuals and
// diagnostics included; ImplicitRegistry says which ordinals those are).
//
// run(state, B, out): for every lane, every block in tape order is solved with its parameters
// taken from that lane's free inputs and earlier blocks' solutions (a scenario per lane
// recalibrates); the solved unknowns and diagnostics complete the lane's full state; then one
// batched interpreter run over the whole program. Lanes are solved one at a time and never mix,
// so lane b of a batched run is bitwise the B = 1 run of its state. Lanes whose block
// parameters are bitwise identical (the risk ladder: one lane per output, the same state in
// every lane) share one solve and one factorisation (Options::dedup_lanes; a copy, so bitwise
// the same as solving each).
//
// adjoint(state, B, out_bar, out, state_bar): the forward above, keeping every lane's factorised
// Jacobian at its solution; the whole-program adjoint (state_bar over every tape input, the
// unknowns included); then per lane, blocks in reverse order, the implicit-function rule
// λ = J_z⁻ᵀ z̄, p̄ −= J_pᵀ λ — p̄ landing on free inputs (quotes) or on earlier blocks' unknowns,
// which their own rule then propagates; finally the free entries are returned. The diagnostics'
// adjoints are dropped (stop-gradient: an iteration count has no derivative; ‖Jᵀr‖∞ is a
// solver report, not a model quantity).
//
// Jacobian policy (Options::jacobian overrides every block's): per_iteration is Newton / LM with
// the Jacobian at every iterate; chord shares the record-point factorisation across every lane
// (Jacobian-free steps) and refreshes per lane when its contraction stalls. Measured in
// bench/solver/m1_implicit_bench.cpp; see RESUME.md §5 (M3/G4).
//
// Thread safety: run / adjoint use the object's own scratch; one ImplicitProgram per thread.
#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "epykos/adjoint/adjoint.hpp"
#include "epykos/exec/interpreter.hpp"
#include "epykos/ir/program.hpp"
#include "epykos/solver/implicit.hpp"
#include "epykos/solver/residual.hpp"
#include "epykos/tape/tape.hpp"

namespace epykos::solver {

struct ProgramOptions {
  exec::Options interpreter;        // the whole-program interpreter
  adjoint::Options adjoint;         // the whole-program adjoint (max_batch bounds B)
  int max_batch = 64;               // lanes run / adjoint accept (both engines are sized for it)
  bool dedup_lanes = true;          // lanes with identical block parameters share one solve
  bool residual_passes = true;      // E0 tape passes on each residual slice before inference
  int jacobian = -1;                // -1: each block's own policy; else a JacobianPolicy for every block
  bool warm_start = false;          // every lane starts from the tape's record-point solution instead of the block's start
  bool throw_on_failure = false;    // std::runtime_error when any lane's solve does not converge
};

class ImplicitProgram {
 public:
  ImplicitProgram(const Tape& tape, const ImplicitRegistry& registry, ProgramOptions options = {});
  ~ImplicitProgram();
  ImplicitProgram(const ImplicitProgram&) = delete;
  ImplicitProgram& operator=(const ImplicitProgram&) = delete;

  int n_state() const noexcept;      // free inputs
  int n_inputs() const noexcept;     // every tape input
  int n_outputs() const noexcept;    // every tape output
  int n_blocks() const noexcept;
  int max_batch() const noexcept;
  const std::vector<int>& free_inputs() const noexcept;   // tape input ordinals, ascending
  // The record point of the free inputs (tape.input_values() at those ordinals).
  const std::vector<double>& record_state() const noexcept;

  // state[k·B + b] over the free inputs; out[o·B + b] over every tape output. 1 <= B <= max_batch().
  void run(const double* state, int B, double* out);
  // state_bar[k·B + b] = Σ_o out_bar[o·B + b] · ∂out_o/∂state_k through every implicit node (IFT).
  // `out` may be null.
  void adjoint(const double* state, int B, const double* out_bar, double* out, double* state_bar);

  // Lane b's full input vector (every tape input, ordinal order) after the last run / adjoint.
  const double* full_state(int lane) const;
  // Lane b's solve of block k in the last run / adjoint.
  const SolveReport& report(int block, int lane) const;

  struct RunStats {
    int lanes = 0;
    int solves = 0;              // block solves actually run
    int shared_lanes = 0;        // block solves served by an identical earlier lane
    int refreshed = 0;           // chord lanes that fell back to their own Jacobian
    int not_converged = 0;
    std::size_t residual_evaluations = 0;
    std::size_t jacobians = 0;
    std::string to_string() const;
  };
  const RunStats& last_run() const noexcept;

  const ir::Program& program() const noexcept;
  const exec::Interpreter& interpreter() const noexcept;
  const adjoint::Adjoint& whole_adjoint() const noexcept;
  const ImplicitBlock& block(int k) const;
  const ResidualProgram& residual_program(int k) const;
  const ImplicitRegistry& registry() const noexcept;
  const ProgramOptions& options() const noexcept;
  std::string describe() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace epykos::solver
