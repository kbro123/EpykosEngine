// EpykosEngine — scalar-tape passes: CSE, DCE, fold-sum, affine collapse, simplify.
//
// Every pass rewrites the tape in place into a fresh, compacted, topologically ordered node
// table and returns the old -> new node map. Input ordinals and output ordinals are preserved;
// node ids held by Rec values become stale.
//
// THE FOUR DATA-MOVEMENT PASSES — cse, dce, fold_sum, affine_collapse — are value-preserving:
// the replay of the tape after one is bit-identical to the replay before it, provided the
// evaluator does not contract multiply-adds. That was once stated as "exactness class E0 (D8)"
// and held for every pass there was. It is now a property of these four and not a contract on
// the file: D79 replaced the per-pass class with the pin (PRINCIPLES.md §1).
//
// `simplify` IS NOT ONE OF THEM. It is the algebra phase, it changes the arithmetic on purpose,
// and it is what the pin exists to license. A pass added here must say which it is.
//
//   cse             hash-cons: nodes with the same op, operands (commutative operands in
//                   canonical order) and constant bit pattern are merged into the first one.
//                   Inputs are never merged. Outputs are remapped, never dropped.
//   dce             drops nodes that no output depends on. Input nodes are always kept so
//                   input ordinals stay stable.
//   fold_sum        a left-deep Add chain ((x_0 + x_1) + x_2) + ... whose inner Adds have a
//                   single use becomes one Sum(x_0, x_1, x_2, ...) with the fold order kept.
//                   A single-use Sum in the left slot is spliced in the same way, so
//                   Sum(Sum(a, b), c) flattens and the pass is idempotent. By default every
//                   Add becomes a Sum (min_terms = 2) so that the signature pass sees one op
//                   for every segment; set min_terms = 3 to keep lone Adds.
//   affine_collapse maximal left-deep Add/Sub/Sum chains whose addends are Inputs, Affine
//                   nodes, Const nodes or untainted nodes — each optionally scaled by a Const
//                   (Mul) or negated (Neg) — become one Affine node
//                   ((c_0 + k_0*x_0) + k_1*x_1) + ... in the recorded order. Exact because
//                   every rewritten step is an IEEE identity: k*x == x*k, 1*x == x,
//                   (-1)*x == -x, a - k*x == a + (-k)*x, and -0.0 + y == y (the c_0 of a
//                   chain with no leading constant is -0.0). A scaled or negated addend is
//                   taken whatever its use count (the Affine recomputes the same product);
//                   the Mul/Neg node is dropped when the chain was its only use and kept for
//                   its other users otherwise (a later dce removes it once they all collapse).
//                   Sums of tainted non-affine nodes (coupon sums, leg sums) are left alone,
//                   so segment structure survives.
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
// Algebraic peepholes: cancellation and the telescope. ABOVE THE PIN (PRINCIPLES.md §1) — every
// rule is an identity in R and removes a rounding, so unlike the four passes above this one the
// replay after it is NOT bit-identical to the replay before. That is the point. See the rule set
// and its stated preconditions in src/tape/passes.cpp.
PassResult simplify(Tape& tape);

// Folds `step` into `total`: composes the two remaps and carries nodes_after / changed forward,
// so a sequence of passes reports one honest old -> new map. `standard_passes` and `compile` are
// both built out of it.
void compose(PassResult& total, const PassResult& step);

// cse, dce, fold_sum, affine_collapse, dce — in that order. Returns the composed remap.
PassResult standard_passes(Tape& tape, FoldSumOptions fold = {});

// Use counts per node: references from operands, variadic args and the output list.
std::vector<std::int32_t> use_counts(const Tape& tape);

}  // namespace epykos
