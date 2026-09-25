// EpykosEngine — the expander and the round-trip identity check (DESIGN.md §5.7, D10).
//
// expand() regenerates a scalar tape from a Program: domains in order, rows in order, the
// group's steps per row. Literals and columns become Const leaves (deduplicated by the tape, as
// in the recording), gathers become operand references (a scan's carry gather references the
// previous row of the chain, emitted just before, so a scan unrolls back into the recorded
// chain), segments become Sum / Affine operands, a fixed-arity Sum becomes a variadic Sum node
// over its operands, Input rows become Input nodes in ordinal order, and the outputs are
// registered in ordinal order. It performs no arithmetic.
//
// roundtrip_identical() compares two tapes node-for-node up to a canonical renumbering:
//   1. a Merkle hash per node (op, constant bits, input ordinal, coefficients, operand hashes;
//      commutative operand hashes sorted);
//   2. a canonical order: Inputs by ordinal, then a depth-first walk from the outputs in ordinal
//      order, operands visited in recorded order except commutative operands, visited in hash
//      order (a tie keeps the recorded order), nodes numbered in post-order;
//   3. the canonical node lists, the input list and the output list must be equal, with
//      commutative operand pairs compared as unordered pairs (a + b and b + a are one node:
//      IEEE addition and multiplication are commutative bitwise).
// Identity, not tolerance: a differing constant bit, operand or op is a failure. `diff`, when
// given, receives every difference (up to a cap) with both nodes printed, or "" when identical.
// Nodes that no output reaches and that are not Inputs are ignored on both sides but counted in
// the diff when the counts differ.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "epykos/ir/program.hpp"
#include "epykos/tape/tape.hpp"

namespace epykos::ir {

Tape expand(const Program& program);

// A tape in canonical form: one entry per reachable node in canonical order.
struct CanonicalNode {
  Op op = Op::Const;
  std::uint64_t konst_bits = 0;   // Const / Input value, Affine c_0
  std::int32_t input_ordinal = -1;
  std::vector<std::int32_t> operands;  // canonical ids; commutative pairs sorted
  std::vector<std::uint64_t> coefs;    // Affine coefficient bits
};
struct CanonicalTape {
  std::vector<CanonicalNode> nodes;
  std::vector<std::int32_t> inputs;   // canonical ids per ordinal
  std::vector<std::int32_t> outputs;  // canonical ids per ordinal
  std::size_t unreachable = 0;        // nodes dropped (not Inputs, not reached from an output)
};
CanonicalTape canonical_form(const Tape& tape);

bool roundtrip_identical(const Tape& original, const Tape& expanded, std::string* diff = nullptr);

}  // namespace epykos::ir
