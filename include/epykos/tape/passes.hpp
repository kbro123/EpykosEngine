// EpykosEngine — scalar-tape passes: CSE, DCE, fold-sum, affine collapse (DESIGN.md §5.2, D14).
//
// Every pass rewrites the tape in place into a fresh, compacted, topologically ordered node
// table and returns the old -> new node map. Input ordinals and output ordinals are preserved;
// node ids held by Rec values become stale. All four passes are exactness class E0 (D8): the
// replay of the tape after a pass is bit-identical to the replay before it, provided the
// evaluator does not contract multiply-adds (the reference preset / *_e0_test.cpp TUs).
//
//   cse             hash-cons: nodes with the same op, operands (commutative operands in
//                   canonical order) and constant bit pattern are merged into the first one.
//                   Inputs are never merged. Outputs are remapped, never dropped.
//   dce             drops nodes that no output depends on. Input nodes are always kept so
//                   input ordinals stay stable.
//   fold_sum        a left-deep Add chain ((x_0 + x_1) + x_2) + ... whose inner Adds have a
//                   single use becomes one Sum(x_0, x_1, x_2, ...) with the fold order kept.
//                   By default every Add becomes a Sum (min_terms = 2) so that the signature
//                   pass sees one op for every segment; set min_terms = 3 to keep lone Adds.
//   affine_collapse maximal left-deep Add/Sub/Sum chains whose addends are Inputs, Affine
//                   nodes, Const nodes or untainted nodes — each optionally scaled by a Const
//                   (Mul) or negated (Neg) — become one Affine node
//                   ((c_0 + k_0*x_0) + k_1*x_1) + ... in the recorded order. Exact because
//                   every rewritten step is an IEEE identity: k*x == x*k, 1*x == x,
//                   (-1)*x == -x, a - k*x == a + (-k)*x, and -0.0 + y == y (the c_0 of a
//                   chain with no leading constant is -0.0). Sums of tainted non-affine nodes
//                   (coupon sums, leg sums) are left alone, so segment structure survives.
#pragma once

#include <cstddef>
#include <vector>

#include "epykos/tape/tape.hpp"

namespace epykos {

struct PassResult {
  std::size_t nodes_before = 0;
  std::size_t nodes_after = 0;
  // Nodes merged (cse), removed (dce), folded into a Sum (fold_sum) or into an Affine
  // (affine_collapse): the number of old nodes that no longer exist as their own node.
  std::size_t changed = 0;
  // remap[old_id] = new id, or invalid_node if the node was removed.
  std::vector<node_id> remap;
};

PassResult cse(Tape& tape);
PassResult dce(Tape& tape);

struct FoldSumOptions {
  int min_terms = 2;  // fold chains with at least this many addends (>= 2)
};
PassResult fold_sum(Tape& tape, FoldSumOptions options = {});

PassResult affine_collapse(Tape& tape);

// cse, dce, fold_sum, affine_collapse, dce — in that order. Returns the composed remap.
PassResult standard_passes(Tape& tape, FoldSumOptions fold = {});

// Use counts per node: references from operands, variadic args and the output list.
std::vector<std::int32_t> use_counts(const Tape& tape);

}  // namespace epykos
