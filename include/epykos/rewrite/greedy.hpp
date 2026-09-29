// EpykosEngine — the greedy rule driver (M4/R0; PROBLEM.md §7 "Rewrites ... expressed as rules").
//
// "Apply everywhere it matches" for one rule (apply_greedy) and, for the M1 planner's five
// decisions run in their fixed dependency order, a whole pipeline (run_pipeline). Neither
// function mutates a caller's `ir::Program`: a structural proposal REPLACES the working program
// value the caller passed by reference to hold the result; an annotations proposal is folded
// into the working PlanAnnotations by merge_annotations. See rewrite/rule.hpp for the contract
// an e-graph driver (M4/EG) would use instead of this one.
#pragma once

#include <cstddef>
#include <vector>

#include "epykos/ir/annotate.hpp"
#include "epykos/ir/program.hpp"
#include "epykos/rewrite/rule.hpp"

namespace epykos::rewrite {

// Applies `rule` at every site match(program, plan) finds, left to right as returned. Each
// site's Proposal is folded immediately (so a later site's match already saw an earlier site's
// effect only for annotation rules, where later sites are independent by construction — see
// individual rules' docs; a structural rule is expected to return at most one Program-kind site
// per call, since accepting it invalidates every other site's indices, and apply_greedy asserts
// this in debug builds by re-deriving match() only when the caller calls it again). Returns the
// number of sites applied.
std::size_t apply_greedy(const Rule& rule, ir::Program& program, ir::PlanAnnotations& plan);

// Runs `rules` in order, each once via apply_greedy, threading one working (program, plan) pair
// through the whole sequence — the order later rules may depend on earlier ones' annotations
// (e.g. planner.emit_outputs reads planner.reduction_fusion's domain choices).
void run_pipeline(const std::vector<const Rule*>& rules, ir::Program& program, ir::PlanAnnotations& plan);

// Folds `src` onto `dst` entry by entry:
//   domain / group:  dst[i] = src[i] for every i where src[i] is not the default-constructed
//                    entry (a rule only ever proposes entries for the domains its own match()
//                    named; every other index in `src`, sized to cover the whole program by
//                    convention, stays default and is left alone in `dst`);
//   emitted:         OR'd in (a later rule marking a value emitted never un-marks one an earlier
//                    rule set); grown to the larger of the two sizes first, zero-filled;
//   jacobian.mode:   inserted-or-overwritten by key (the latest rule to name a block wins).
// `dst` is grown (never shrunk) to at least `src`'s sizes for `domain` / `group`.
void merge_annotations(ir::PlanAnnotations& dst, const ir::PlanAnnotations& src);

}  // namespace epykos::rewrite
