// EpykosEngine — the signature pass: scalar tape -> domain IR (DESIGN.md §5 steps 3–6, D4).
//
// Boundaries (the nodes that become rows, i.e. materialised values): Inputs, Sums, Affines,
// every node with a use count other than one, every node used by a variadic op or registered as
// an output, and — so that every instance of a computation is treated alike — every node whose
// deep expression (the op tree seen through shared nodes, modulo constants) matches that of a
// boundary: the fan-out rule is applied per computation, not per node (D22). Every other node
// is an inner node of exactly one boundary's expression tree.
//
// Signature of a boundary: the hash of its tree — op, the tree of each inner operand, a
// "constant slot" token for a Const operand and a "reference" token for a boundary operand.
// References carry no class: a coupon that discounts with a knot-time DF and one that discounts
// with an interpolated DF have the same signature, so upstream buckets never fragment
// downstream domains. Commutative operands (Add, Mul, CmpEq) are ordered by the hash of their
// operand tokens; an equal-hash tie keeps the recorded order. Sum and Affine are order- and
// arity-insensitive: their operands become segment members.
//
// Partition: one domain per signature; rows in tape order; constant slots with one bit pattern
// across all rows become literals, the others columns; reference slots become gathers (a value
// id per row); Sum / Affine operands become segments. Const nodes that are themselves Sum
// members or outputs form the "const" domain (one Const step, one column).
//
// Recurrences (DESIGN.md §5.6, D41): before the partition, chains x_{k+1} = f(x_k, ...) are
// detected without hints — a node whose expression tree, with one node cut out and replaced by
// a carry token, hashes like the cut node's own tree with its own cut, for three or more steps
// in a row (the natural product loop acc = acc·(1 + r·τ), a path x_{k+1} = a_k·x_k + b_k). The
// steps of a chain become boundaries, the cut becomes the carry — a reference like any other,
// to the previous step or, for the first step, to the initial value (a Const initial value is
// a row of the const domain) — and the class of the steps is one *scan domain*: recurrent, its
// rows chain-major, `Program::scans` describing the chains and the carry gather. A Sum on the
// carry's path (fold_sum's `+` of a·x + b) is a fixed-arity Sum step of the group. An inner
// running sum (x + a_k with x used once) is a reduction and is left to fold_sum, never a
// scan; a chain of fewer than three steps is straight-line code.
//
// A class that reads itself in any other way — the carry inside an Affine, a step reading a
// class that reads the scan, a short chain — is split by dependency level like any class in a
// cycle, and every level-domain carries `Domain::scan_class` with `Domain::recurrent` false
// (D22, D23). Classes that reach themselves only through other classes (legs -> swaps -> book)
// are split the same way, so that every domain is evaluable in one pass; domains are then
// ordered topologically (ties by first appearance on the tape). The M1 book has no chain of any
// kind: its program is unchanged by the detection (asserted by its tests).

//
// Reserved ops (Gather … Pin) are rejected with std::invalid_argument; the tape is validated
// first (RecordError on a malformed tape).
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "epykos/ir/program.hpp"
#include "epykos/tape/tape.hpp"

namespace epykos::ir {

struct InferStats {
  std::size_t nodes = 0;             // tape nodes
  std::size_t boundaries = 0;        // rows over all domains (== program.num_values())
  std::size_t inner = 0;             // nodes folded into a row's op sequence
  std::size_t const_leaves = 0;      // Const nodes that only appear as constant slots
  std::size_t classes = 0;           // signature classes before level splitting
  std::size_t domains = 0;           // after level splitting
  std::size_t boundary_rounds = 0;   // passes of the sharing rule (1; 2 when chains were found)
  std::size_t class_promoted = 0;    // nodes made boundaries by the sharing rule
  std::size_t chains = 0;            // chains accepted by the scan detection (D41)
  std::size_t chain_nodes = 0;       // their steps (rows of scan domains)
  std::size_t scan_classes = 0;      // classes laid out as scan domains
  std::size_t scan_rounds = 0;       // inference rounds (1 + retries after a scan class could not be laid out)
  std::string scan_retries;          // why each retry happened (empty when scan_rounds == 1)
};

Program infer(const Tape& tape, InferStats* stats = nullptr);

}  // namespace epykos::ir
