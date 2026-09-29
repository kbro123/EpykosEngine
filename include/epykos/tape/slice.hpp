// EpykosEngine — the backward slice of a tape: the sub-program that computes a set of nodes.
//
// slice(tape, roots) copies every node a root depends on into a new tape, in tape order, and
// registers the roots as that tape's outputs in the order given. Inputs stay Inputs (their
// ordinals are renumbered ascending; `input_ordinals` maps them back), constants are re-created
// through the new tape's own deduplication, and every other node is re-emitted with its operands
// remapped. No arithmetic happens: a replay of the slice is bit-identical to the same nodes of
// the original tape (E0 by construction), and the slice validates.
//
// The solver layer (solver/) uses it to lift a residual sub-program — the nodes between an
// implicit node's unknowns and its residuals — out of the one tape the whole problem is recorded
// on, so that the untaped solve runs the recorded maths (its Interpreter and Adjoint) and never a
// second copy of it.
#pragma once

#include <span>
#include <vector>

#include "epykos/tape/tape.hpp"

namespace epykos {

struct Slice {
  Tape tape;                         // the sub-program; roots are its outputs, in order
  std::vector<int> input_ordinals;   // sub input ordinal -> original input ordinal (ascending)
  std::vector<node_id> node_map;     // original node id -> sub node id, or invalid_node if not copied
};

// Throws RecordError for a root id the tape does not hold.
Slice slice(const Tape& tape, std::span<const node_id> roots);

}  // namespace epykos
