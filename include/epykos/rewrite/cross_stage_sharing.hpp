// EpykosEngine — the cross-stage sharing gate as an extraction-time guard (M4/EG "integration";
// PROBLEM.md §5 "A gate of every stage: the IR shows the cross-stage sharing" and §7 "keep the
// residual's DF domain and the book's DF domain merged under every rewrite (a rewrite that would
// split them is rejected or costed)", D53).
//
// ir/sharing.hpp already answers "is this Program's sharing intact" (SharingReport,
// assert_all_shared) for a Tape-level caller (M3/G4, M3/G5) that names its own output groups by
// ORDINAL — the stable identifier across a rewrite: R1-R7 fold, split or bucket DOMAINS (whose
// ids and the value ids inside them renumber under a rewrite) but never reorder or drop an
// OUTPUT (rewrite/rule.hpp's own contract: a structural Proposal changes what computes a value,
// never which ordinal position it is returned at), so a group defined once against the ROOT
// program (e.g. {residual output ordinals}, {book output ordinals}) stays meaningful against
// every candidate program an e-graph saturation discovers from it.
//
// `cross_stage_sharing_guard` turns that same check into the
// `std::function<bool(const ir::Program&)>` `optimise::ExtractOptions::reject` (extract.hpp, D53)
// expects: a candidate program REJECTED here is never priced or extracted, however cheap its
// plan — "a rewrite that would split them is rejected", never merely deprioritised — matching
// this package's OWN "costed" alternative (a caller that wants the softer form instead can price
// the split candidate anyway and compare `optimise::estimate_program` against a version with this
// guard on, an experiment egraph_stage_a_experiment_test.cpp runs).
//
// Not a rewrite::Rule: nothing here ever adds an e-graph candidate (rule.hpp point 2's "produce
// the alternative without discarding the original" is what RULES do); this is the veto a fixed
// pipeline gets for free by never proposing the split in the first place and an e-graph, which
// explores every rule's proposal independently of every other, needs an explicit place to put.
#pragma once

#include <functional>
#include <vector>

#include "epykos/ir/program.hpp"
#include "epykos/ir/sharing.hpp"
#include "epykos/tape/op.hpp"

namespace epykos::rewrite {

// True (reject) iff `ir::assert_all_shared` fails for `ir::sharing(program, output_groups, op)` —
// i.e. some domain whose group's last op is `op` (Op::Exp: discount factors, D22/PROBLEM.md §5's
// own example) feeds some but not every one of `output_groups` (a partial or unshared domain: the
// residual and the book no longer read the SAME discount-factor rows). `output_groups` is a list
// of output-ordinal lists (ir::reach's own shape), fixed once against the program the e-graph was
// seeded with (egraph.hpp's node 0) — a caller derives it from its own registry/output layout
// (HARD RULE 9: no group definition here is specific to any one problem).
std::function<bool(const ir::Program&)> cross_stage_sharing_guard(std::vector<std::vector<int>> output_groups,
                                                                   epykos::Op op = epykos::Op::Exp);

}  // namespace epykos::rewrite
