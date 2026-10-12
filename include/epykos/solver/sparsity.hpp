// EpykosEngine — the STRUCTURAL sparsity of ∂F/∂z for a declared implicit block, and the gate
// that a declared solve's dependency structure is computed rather than assumed.
//
// WHY THIS IS ANSWERABLE TODAY. Which unknowns reach which residuals is a reachability property
// of the RECORDED FORWARD PROGRAM — of F, not of ∂F. `PRINCIPLES.md` §1 states the limit that
// bites every quantity defined by a derivative: differentiation is a pipeline STAGE, not an
// operation, so no pass can see ‖JᵀF‖∞ or the IFT's own products (D87). This analysis escapes
// that limit entirely, because it never mentions a derivative: it walks the operand edges of the
// tape backwards from the residual outputs and asks which Input nodes it arrives at. It needs no
// I1, no I3, no new op and no market data, and it is exact on the recorded program rather than a
// sample of it.
//
// WHAT IT IS AND IS NOT. The pattern is a CONSERVATIVE SUPERSET of the nonzero pattern of ∂F/∂z:
// `reads(i, j)` false implies ∂F_i/∂z_j is identically zero, while `reads(i, j)` true means only
// that the recorded expression for F_i mentions z_j. An expression may mention a value and have
// zero derivative in it (`x - x`, or a product with an algebraically zero coefficient), so a
// structurally present entry can still be numerically zero everywhere. For the shipped curve
// schemes the superset is TIGHT, and not by luck: `curve/scheme.hpp`'s `flat_masses` and
// `linear_masses` both omit zero-mass knots from the term list, so a knot that contributes
// nothing to an integral is absent from the tape rather than present with weight 0. That is
// what lets this analysis see the `(forward, flat)` zero column of D110 §3 — and it is a property
// of those two functions, so a scheme added later that emits its zeros explicitly would widen
// the superset and weaken the gate without weakening its soundness. `tools/sparsity/` measures
// the structural count against the numerical one on every shipped fixture and prints both.
//
// There is NO numerical rank information here and none is implied. `Factors` uses `PartialPivLU`,
// which is not rank-revealing, so the engine has no numerical rank today (D110 §3). This is the
// structural half alone: it catches a zero row or column, never an ill-conditioned one.
//
// THE GATE (the invariant: the dependency structure of a declared solve is computed, not assumed).
// For every `ImplicitBlock`: every unknown reaches at least one residual, and every residual is
// reached by at least one unknown. Both directions are necessary and neither implies the other —
// a dead column leaves an unknown that nothing constrains, a dead row leaves a residual that no
// step can move. It runs ONCE, at block registration (`solver::detail::implicit_end`), before any
// market data exists, and it THROWS: see `check_structure`.
//
// Half of this gate already existed and the comments describing it were wrong in both directions.
// `ResidualProgram`'s constructor has always thrown on a dead column — not by a reachability pass
// of its own but as a by-product of `slice()`, which copies only the Input nodes the roots reach,
// so an unreached unknown has no sub-ordinal to bind to. That check is sound and is kept as the
// backstop it is. What did NOT exist is the row direction: `implicit_end` checked `tape.tainted`,
// which asks whether a residual depends on ANY input and is satisfied by a residual that reads a
// quote and no unknown at all. `implicit.hpp`'s own summary of `implicit_end` claimed it "checks
// every unknown is read by some residual", which that function never did.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "epykos/solver/implicit.hpp"
#include "epykos/tape/tape.hpp"

namespace epykos::solver {

// The bipartite structure of one block: n_residuals rows by n_unknowns columns of bits.
struct SparsityPattern {
  int n_residuals = 0;
  int n_unknowns = 0;
  std::vector<std::uint64_t> bits;  // row-major, `words()` 64-bit words per row

  int words() const noexcept { return (n_unknowns + 63) / 64; }
  // Does the recorded expression for residual `i` read unknown `j`? (A superset of ∂F_i/∂z_j ≠ 0.)
  bool reads(int i, int j) const noexcept {
    const std::size_t w = static_cast<std::size_t>(i) * static_cast<std::size_t>(words()) +
                          static_cast<std::size_t>(j / 64);
    return (bits[w] >> (j % 64)) & 1u;
  }
  std::size_t nonzeros() const noexcept;
  // nonzeros / (n_residuals · n_unknowns), or 0 for an empty pattern.
  double density() const noexcept;
  int row_count(int i) const noexcept;    // unknowns residual i reads
  int column_count(int j) const noexcept; // residuals that read unknown j
  // Columns no residual reads: an unknown nothing constrains (an exact zero Jacobian column).
  std::vector<int> dead_unknowns() const;
  // Rows that read no unknown: a residual no step can move (an exact zero Jacobian row).
  std::vector<int> dead_residuals() const;
  std::string to_string() const;
};

// The pattern of `block` on `tape`, by one reverse pass over the operand edges from the block's
// residual output nodes. `tape` must be the tape the block was recorded on. O(nodes · words).
// Throws std::invalid_argument when an ordinal of the block is not on this tape.
SparsityPattern structural_pattern(const Tape& tape, const ImplicitBlock& block);

// The gate. Throws std::invalid_argument naming every dead unknown and every dead residual when
// `block.options.require_structural_coupling`; otherwise returns without looking.
//
// WHY IT THROWS RATHER THAN REPORTS. An unknown nothing constrains, or a residual nothing can
// move, makes the declared system singular in substance however square it looks by count, and
// the behaviour it replaces is to return a curve whose unconstrained knot sits wherever the solve
// left it, with no diagnostic — the silent-plausible-number shape this ledger keeps meeting
// (D81 §6(a) unwritten outputs reading back as 0.0, D89 §3's all-zero ladder, D94's zero-as-
// perfect-convergence). It is also not market dependent: the structure is fixed when the maths is
// recorded, so the throw happens at registration on the developer's machine and can never be a
// surprise in production on a moved market. A deliberate under-determined solve with a
// regulariser is legitimate, which is why `SolveOptions::require_structural_coupling` exists; it
// is an explicit per-block opt-out, not an absent check.
void check_structure(const SparsityPattern& pattern, const ImplicitBlock& block);

// ---- the colouring question (D92's method: measure the share before fitting) -----------------
//
// `ResidualProgram::jacobian` seeds one adjoint lane per residual. Two residuals that share no
// unknown have structurally orthogonal rows and could share a lane, which is the classic sparse-
// Jacobian graph colouring; the colour count is then the number of adjoint lanes a Jacobian
// build would cost instead of n_residuals. `colour_rows` is GREEDY, so its answer is an upper
// bound on the chromatic number and therefore a LOWER bound on the saving — the honest direction.
// Nothing in the engine consumes this: it exists to be measured by `tools/sparsity/` before
// anyone builds a colouring. D92 priced the Jacobian at 87% of cold calibration, ~0% of the warm
// path and 0.5% of a 256-row ladder, so that is the whole of what a colouring could pay back.
struct Colouring {
  std::vector<int> colour;  // per residual
  int n_colours = 0;
  // n_residuals / n_colours: the factor by which a coloured Jacobian build would cut lanes.
  double lane_reduction(int n_residuals) const noexcept;
};

// Greedy largest-first colouring of the row-conflict graph (rows i, k conflict when they share an
// unknown): rows in descending row_count order, each given the lowest colour no conflicting
// already-coloured row holds.
Colouring colour_rows(const SparsityPattern& pattern);

// A one-line shape verdict for a pattern: lower/upper triangular, banded with its bandwidth,
// block diagonal with its block sizes, or dense. Descriptive only; nothing branches on it.
std::string shape_of(const SparsityPattern& pattern);

}  // namespace epykos::solver
