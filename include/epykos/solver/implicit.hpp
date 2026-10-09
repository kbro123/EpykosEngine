// EpykosEngine — the implicit node (DESIGN.md §3.2 `implicit(F)`, D7; M3/G4).
//
// An implicit node turns a residual F(z, p) = 0 into a node of the tape: its inputs are the
// parameters p (quotes and, through them, anything recorded earlier — other curves' states), its
// outputs are the unknowns z. Forward: any solver, untaped. Backward: the implicit-function
// rule at the solution, z̄ → p̄ = −F_pᵀ F_z⁻ᵀ z̄. The tape's length never depends on how many
// iterations the solve took (the residual is recorded once, symbolically in z).
//
// How it sits in the one tape (D40). The unknowns are recorded as tape Inputs that the solver
// layer, not the caller, fills in ("solved inputs"); the residuals are recorded as tape outputs
// that the solver drives to zero ("residual outputs"); two diagnostics — ‖Jᵀr‖∞ and the
// iteration count — are solved inputs registered as outputs (O1). An ImplicitBlock is the
// record of one node: those ordinals plus the start point and the solver options, and an
// ImplicitRegistry holds every block recorded on a tape. Ordinals are stable across every tape
// pass, so the registry survives cse / dce / fold_sum / affine_collapse untouched, and every
// landed consumer of a tape (validate, the passes, infer, expand, the interpreter, the adjoint)
// sees a straight-line program over Inputs — which is exactly what the residual sub-program and
// the book are once z is known. The reserved opcode Op::Implicit stays reserved.
//
// ONE TAPE. `implicit()` records the residual on the tape that is current, in place: the nodes
// the residual computes (the calibration instruments' discount factors, say) are ordinary nodes
// of that tape, and whatever is recorded downstream from the same unknowns (the book) shares
// them through cse. The residual sub-program the solver runs is the backward slice of the tape
// from the residual outputs (tape/slice.hpp): the recorded maths, never a second copy.
//
// Record-time solve. `implicit()` slices the residual right after recording it, builds its
// interpreter and adjoint (solver/residual.hpp) and solves from `z0` with the other inputs at
// their record-point values, so that the returned Rec unknowns carry the calibrated values and
// tape.input_values() is the calibrated record point (Tape::set_input_value). Execution of the
// whole program (forward for a batch of lanes, the IFT adjoint) is solver/implicit_program.hpp.
//
// Usage (a fixture composing the tape; solver/curve_set.hpp does this for a set of curves):
//
//   Tape tape; Tape::Scope scope(tape); ImplicitRegistry reg;
//   std::vector<Rec> q = ...make_input per quote...;
//   ImplicitResult cal = implicit(tape, reg, "usd-ois", z0, n_quotes,
//                                 [&](const Rec* z, Rec* F) { for i: F[i] = par_rate<Rec>(z, i) - q[i]; });
//   price_book<Rec>(book, cal.z.data(), ...); register outputs; standard_passes(tape);
//   ImplicitProgram prog(tape, reg);  prog.run(...); prog.adjoint(...);
#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "epykos/scalar/rec.hpp"
#include "epykos/tape/tape.hpp"

namespace epykos::solver {

// How the Jacobian J_z is obtained during a lane's iteration.
enum class JacobianPolicy : int {
  per_iteration = 0,  // Newton / Levenberg–Marquardt: J_z at every iterate, factorised there
  chord = 1,          // the record-point factorisation for every lane's steps (Jacobian-free
                      // iterations); a lane whose contraction stalls refreshes its own J_z
};

struct SolveOptions {
  double tol = 1e-14;               // converged when ‖F‖∞ < tol
  int max_iterations = 50;
  double mu0 = 0.0;                 // Levenberg–Marquardt damping at the first step (0: Gauss–Newton)
  double mu_up = 10.0;              // damping multiplier after a rejected step
  double mu_down = 0.1;             // damping multiplier after an accepted step (a floor of 0 is kept at 0)
  int max_damping_tries = 12;       // rejected steps per iteration before giving up
  JacobianPolicy jacobian = JacobianPolicy::per_iteration;
  double chord_min_contraction = 0.5;  // chord: a step that shrinks ‖F‖∞ by less than this is a stall
  int chord_max_stalls = 2;            // chord: this many stalls and the lane refreshes its own J_z
  bool final_jacobian = true;       // J_z at the solution (exact IFT and ‖Jᵀr‖∞); false: the last iterate's
  // ‖JᵀF‖∞, the O1 optimality diagnostic. Since D89 it is the LARGEST single fixed cost of a warm
  // solve — 5.5 us of a 19.9 us re-quote, 28% of it, and more than half the 9.79 us floor at the
  // record point (D91 §2). It is one matrix-free reverse lane, already the cheap way to get it
  // (D87 §3: 4.91 us against 52.07 us for the matrix), so what is left is not computing it at all.
  //
  // A caller re-quoting a curve thousands of times and never reading the diagnostic should not pay
  // for it. Setting this false does NOT leave a stale or zero value behind: `SolveReport::jtr_inf`
  // and the block's diagnostic OUTPUT both become NaN, so a consumer that reads it gets an
  // unmistakable answer rather than a plausible one. Default true is today's behaviour bit-for-bit.
  //
  // `PROBLEM.md` §6's O1 gate reads this output, so the gates keep it on; this is a knob for the
  // hot path, not a weakening of what the engine reports by default.
  bool optimality_diagnostic = true;
  bool throw_on_failure = false;    // std::runtime_error when a lane does not converge
  // The structural gate (solver/sparsity.hpp): every unknown must reach at least one residual and
  // every residual must be reached by at least one unknown, checked ONCE at block registration by
  // reachability over the recorded forward program, before any market data exists. A violation
  // THROWS, because the behaviour it replaces is to return a state whose unconstrained entries sit
  // wherever the solve left them with no diagnostic (D110 §3 found exactly that in our own
  // `(forward, flat)` curve: square by count, an exact zero Jacobian column in substance).
  //
  // Set false for a DELIBERATELY under-determined solve — a block with a regulariser or a
  // smoothing term that pins the null directions outside F. That is a legitimate design and this
  // is its explicit opt-out; it is not a way to quiet an unexpected failure, and the message names
  // the unknown or residual at fault so that the distinction can be made before switching it off.
  //
  // THE OPT-OUT IS HONOURED FOR DEAD ROWS AND CANNOT BE HONOURED FOR DEAD COLUMNS, and the reason
  // is representational rather than a policy choice. A residual that reads no unknown is an
  // ordinary constant row: `ResidualProgram` binds it, the over-determined Gauss–Newton path
  // carries it, and switching this off genuinely admits the block (it will not converge, which is
  // the caller's business once they have said so). An UNKNOWN that no residual reads has no slot
  // to bind to at all — `ResidualProgram` maps each unknown to a sub-input ordinal of the slice,
  // and `slice()` only copies the Inputs the residuals reach — so that constructor throws on its
  // own, whatever this flag says. An under-determined calibration is therefore not merely
  // undetected in this engine today (D110 §3): below this gate it is UNREPRESENTABLE. Supporting
  // one needs the solver to carry an unknown the residual program does not read, plus the
  // regulariser that pins it, and neither exists. `tests/solver/structural_gate_test.cpp` measures
  // both halves of this paragraph rather than asserting it.
  bool require_structural_coupling = true;
};

struct SolveReport {
  bool converged = false;
  int iterations = 0;               // accepted steps
  int jacobians = 0;                // Jacobian evaluations (each = one batched adjoint, one lane per residual)
  int residual_evaluations = 0;
  int damping_retries = 0;          // rejected steps
  double residual_inf = 0.0;        // ‖F‖∞ at exit
  double jtr_inf = 0.0;             // ‖Jᵀ F‖∞ at exit, J at the exit point (the O1 optimality diagnostic)
  bool shared = false;              // chord: the record-point factorisation drove the iteration
  bool refreshed = false;           // chord: the lane fell back to its own Jacobian
};

// One implicit node as recorded on a tape (ordinals only: stable across every pass).
struct ImplicitBlock {
  std::string name;
  std::vector<int> unknowns;             // tape input ordinals of z (solved inputs)
  std::vector<int> residuals;            // tape output ordinals of F (residual outputs)
  int diag_jtr_input = -1;               // solved input holding ‖Jᵀr‖∞ ...
  int diag_jtr_output = -1;              // ... registered as an output (O1)
  int diag_iterations_input = -1;        // solved input holding the iteration count ...
  int diag_iterations_output = -1;       // ... registered as an output (O1)
  std::vector<double> start;             // z0, the start of every solve (record time and run time)
  SolveOptions options;

  int n_unknowns() const noexcept { return static_cast<int>(unknowns.size()); }
  int n_residuals() const noexcept { return static_cast<int>(residuals.size()); }
};

// Every implicit node recorded on one tape, in recording order (a later block may read an
// earlier block's unknowns, never the other way round: blocks form a DAG in tape order).
struct ImplicitRegistry {
  std::vector<ImplicitBlock> blocks;

  // Is tape input `ordinal` written by a block (an unknown or a diagnostic)?
  bool is_solved_input(int ordinal) const noexcept;
  // Is tape output `ordinal` a residual or a diagnostic of a block?
  bool is_block_output(int ordinal) const noexcept;
  // The tape inputs no block writes, ascending: the free state of the whole program.
  std::vector<int> free_inputs(int n_tape_inputs) const;
};

struct ImplicitResult {
  int block = -1;            // index into ImplicitRegistry::blocks
  std::vector<Rec> z;        // the unknowns; their values are the record-time solution
  Rec jtr_inf;               // ‖Jᵀr‖∞ (a solved input, registered as an output)
  Rec iterations;            // iteration count (a solved input, registered as an output)
  SolveReport report;        // the record-time solve
};

namespace detail {
// Creates the block: the unknown Inputs (values z0) and the two diagnostic Inputs (registered
// as outputs). `tape` must be the current recording tape. Throws RecordError otherwise, or for
// an empty z0 / n_residuals < z0.size().
ImplicitResult implicit_begin(Tape& tape, ImplicitRegistry& registry, std::string name,
                              std::span<const double> z0, int n_residuals, const SolveOptions& options);
// Registers the residual outputs, runs the STRUCTURAL GATE over the recorded program
// (solver/sparsity.hpp: every unknown reaches a residual, every residual is reached by an
// unknown), and runs the record-time solve; writes the solution into the tape's record point and
// into `result`. Throws std::invalid_argument when the gate fires.
//
// This comment used to claim the function "checks every unknown is read by some residual". It did
// not: it checked `Tape::tainted` per residual, which asks whether a residual depends on ANY input
// and is satisfied by one that reads a quote and no unknown at all. The unknown direction was
// enforced further downstream, as a by-product of `slice()` inside `ResidualProgram`. Both
// directions are now checked here, explicitly, before anything is solved.
void implicit_end(Tape& tape, ImplicitRegistry& registry, ImplicitResult& result, std::span<const Rec> F);
}  // namespace detail

// Records one implicit node on `tape` (the current recording tape): n = z0.size() unknowns
// starting at z0, then `residual(const Rec* z, Rec* F)` called exactly once to record the
// n_residuals residuals (n_residuals >= n; every unknown must be read by at least one residual),
// then the record-time solve. Returns the unknowns as Recs carrying the solved values, the two
// diagnostic Recs and the solve report. Throws RecordError on a recording-discipline violation
// and std::runtime_error when options.throw_on_failure is set and the solve does not converge.
template <class Residual>
ImplicitResult implicit(Tape& tape, ImplicitRegistry& registry, std::string name, std::span<const double> z0,
                        int n_residuals, Residual&& residual, const SolveOptions& options = {}) {
  ImplicitResult r = detail::implicit_begin(tape, registry, std::move(name), z0, n_residuals, options);
  std::vector<Rec> F(static_cast<std::size_t>(n_residuals));
  const Rec* z = r.z.data();
  residual(z, F.data());
  detail::implicit_end(tape, registry, r, F);
  return r;
}

const char* to_string(JacobianPolicy policy) noexcept;
std::string to_string(const SolveReport& report);

}  // namespace epykos::solver
