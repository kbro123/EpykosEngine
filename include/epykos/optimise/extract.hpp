// EpykosEngine — extraction: the cheapest (program, plan) e-node under the CM cost model at a
// requested exactness class (M4/EG "core"; docs/PROBLEM.md §7).
//
// Greedy bottom-up over the e-graph's two tiers (egraph.hpp): for every PROGRAM e-class
// (representative node, deterministically the lowest id — today, with R1-R7 still stub rules,
// always exactly one: the root program) and every PLAN e-class of that program (representative
// node, same tie-break), price the pair with `optimise::estimate_program` (via plan_bridge.hpp's
// `plan_from_annotations`, D48 point 6's own reconciliation) at the caller's `(B, tile,
// lane_tile)`, filtering out any node whose accumulated exactness exceeds the request — E0
// extraction may not select a node any E1 rule contributed to, per PROBLEM.md §7's own gate ("E0
// extraction may not use E1-class rewrites") — and keep the minimum. This is a DP in the sense the
// package spec asks for (no candidate's cost is computed twice: a PLAN e-class's representative is
// priced exactly once, its content and cost held for whichever PROGRAM class it belongs to; a
// caller costing many candidates of one program in a loop should reuse one `optimise::analyze`
// result via the two-argument `plan_from_annotations` rather than the convenience overload), not a
// search: with no external ILP/SAT solver (D12) and a small, already-enumerated candidate set
// (egraph.hpp's own saturation bound), a bottom-up scan over representatives is exact, not a
// heuristic relaxation of one.
//
// Determinism: candidates are visited in ascending (program node id, plan node id) order — the
// SAME order two `saturate()` runs over equal inputs produce (egraph.hpp's rules are pure
// functions of (program, plan, site), matched and queued in the same rule/program/site order both
// times) — and a tie in `estimated_ns` (a real possibility: two annotations that both cost bytes
// at the same cache tier) keeps the FIRST one seen, never the caller's, so two extractions of the
// same saturated graph always return bit-identical results.
#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "epykos/ir/annotate.hpp"
#include "epykos/ir/program.hpp"
#include "epykos/optimise/cost.hpp"
#include "epykos/optimise/egraph.hpp"
#include "epykos/rewrite/rule.hpp"

namespace epykos::optimise {

struct ExtractOptions {
  rewrite::Exactness max_exactness = rewrite::Exactness::E0;  // E0: no E1 rule anywhere in history
  int B = 1;
  int tile = 256;
  int lane_tile = 8;
  // M4/EG "integration" (D53; PROBLEM.md §5 / §7's cross-stage sharing gate): a PROGRAM-level
  // invariant a candidate must hold to be extractable at all -- true REJECTS the program node
  // (every one of its plan candidates is skipped, the same treatment an exactness overrun gets,
  // never a silent drop). nullptr (the default): no program is ever rejected, so a caller that
  // never sets this sees exactly the behaviour this file had before D53 -- this option cannot
  // change any existing caller's result. rewrite::cross_stage_sharing_guard (rewrite/
  // cross_stage_sharing.hpp) builds the one this package actually uses (PROBLEM.md §5: "the
  // residual's DF domain and the book's DF domain merged under every rewrite"); it is a plain
  // predicate here, not a Rule, because rules only ever ADD e-graph candidates (rule.hpp point
  // 2) -- something has to be able to say no to one once every rule has finished proposing.
  std::function<bool(const ir::Program&)> reject;
};

struct ExtractResult {
  bool found = false;  // false: every candidate exceeded max_exactness or failed `reject` (an
                       // empty e-graph cannot happen — the root's own history is always empty,
                       // hence always E0 and, ordinarily, not rejected — so `found` is only ever
                       // false when the caller passes an EGraph this file did not build, e.g. a
                       // unit test's hand-rolled one with no eligible candidate at all, or a
                       // `reject` that also rejects the root)
  int program_id = -1;
  int plan_id = -1;
  ir::Program program;
  ir::PlanAnnotations plan;
  double estimated_ns = 0.0;
  std::vector<std::string> history;         // program history followed by plan history
  std::size_t candidates_considered = 0;
  std::size_t candidates_rejected_exactness = 0;
  std::size_t candidates_rejected_guard = 0;  // rejected by `options.reject`, D53
};

ExtractResult extract(const EGraph& graph, const CostModel& model, const ExtractOptions& options = {});

}  // namespace epykos::optimise
