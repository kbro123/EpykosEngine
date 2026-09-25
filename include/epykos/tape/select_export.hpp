// EpykosEngine — select exports: mask, margin and arm gap of every recorded Select, as outputs
// (DESIGN.md §3.1 "select ... exports mask + margin + arm gap", §6 flip classification, §8 class
// B; docs/PROBLEM.md O6 diagnostics; M3/G1).
//
// A Select node c ? a : b (D5: both arms recorded) carries three diagnostics per state:
//   mask    the predicate's value, 1.0 (the true arm was taken) or 0.0;
//   margin  the signed distance of the predicate from its flip: for a comparison x OP y it is
//           y − x for OP in {<, <=} and x − y for {>, >=} (positive iff the predicate holds, zero
//           on the boundary), x − y for == (zero on the boundary; the sign carries no meaning),
//           and the mask itself when the predicate is not a comparison (a constant predicate);
//   gap     the arm gap a − b: what the value jumps by if the mask flips at this state.
// export_selects() appends them to the tape as three outputs per Select (in tape order, after the
// outputs already registered): margin and gap are new Sub nodes over the predicate's operands and
// the arms, the mask is the predicate node registered as an output. From then on they are
// ordinary outputs of the program: the tape passes keep them, the signature pass materialises
// them, exec::Interpreter::run writes them at out[o·B + b] next to the pricing outputs, and
// adjoint::Adjoint seeds them with whatever out_bar says (zero, for a diagnostic). No flag in the
// IR and no kernel is needed: the export IS the program's output list, and every consumer of the
// IR sees it. The SelectExports table returned says which output ordinal is what.
//
// Flip classification (DESIGN.md §6, from the measured degenerate-tie flips): between two states
// a select FLIPS when its mask differs; the flip is SIGNIFICANT only if the arm gap at either
// state exceeds a threshold — a max(x, y) whose arms cross has a gap of exactly the margin, so a
// flip near the tie moves the value by ~nothing and is degenerate, whereas a select whose arms
// differ by a finite amount at the boundary is a jump. classify_flip() applies that rule.
#pragma once

#include <cstddef>
#include <vector>

#include "epykos/tape/tape.hpp"

namespace epykos {

struct SelectExport {
  node_id select = invalid_node;     // the Select node (ids as of the export; passes renumber)
  node_id predicate = invalid_node;  // its predicate node
  Op predicate_op = Op::Const;       // the comparison (or Const when the predicate is not one)
  int mask = -1;                     // output ordinals
  int margin = -1;
  int gap = -1;
};

struct SelectExports {
  int first_output = 0;               // the ordinal of the first exported output (n_outputs before)
  std::vector<SelectExport> selects;  // one per Select, tape order

  int n() const noexcept { return static_cast<int>(selects.size()); }
  int n_outputs() const noexcept { return 3 * n(); }

  // Readers over a batched output buffer out[o·B + b] (exec::Interpreter::run's layout).
  double mask(int s, const double* out, int B, int b) const noexcept {
    return out[static_cast<std::size_t>(selects[static_cast<std::size_t>(s)].mask) * static_cast<std::size_t>(B) + static_cast<std::size_t>(b)];
  }
  double margin(int s, const double* out, int B, int b) const noexcept {
    return out[static_cast<std::size_t>(selects[static_cast<std::size_t>(s)].margin) * static_cast<std::size_t>(B) + static_cast<std::size_t>(b)];
  }
  double gap(int s, const double* out, int B, int b) const noexcept {
    return out[static_cast<std::size_t>(selects[static_cast<std::size_t>(s)].gap) * static_cast<std::size_t>(B) + static_cast<std::size_t>(b)];
  }
};

// Appends the three exports of every Select node of `tape` (tape order) to its outputs and
// returns the table. A tape with no Select gets no outputs (n() == 0). Call it on the raw
// recording, before the passes (they renumber nodes but keep output ordinals), or after them
// (the exports are then appended to the rewritten tape; passes run later still keep them).
// Throws RecordError if the tape does not validate.
SelectExports export_selects(Tape& tape);

// The flip of one select between state A and state B.
struct Flip {
  bool flipped = false;      // the mask differs
  bool significant = false;  // flipped and max(|gap_a|, |gap_b|) > threshold
  double margin_a = 0.0, margin_b = 0.0;
  double gap_a = 0.0, gap_b = 0.0;
};
Flip classify_flip(double mask_a, double margin_a, double gap_a, double mask_b, double margin_b, double gap_b,
                   double threshold) noexcept;

}  // namespace epykos
