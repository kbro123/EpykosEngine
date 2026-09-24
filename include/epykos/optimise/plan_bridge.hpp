// EpykosEngine — bridging a rewrite::Rule pipeline's ir::PlanAnnotations onto the cost model's own
// optimise::Plan (M4/EG "core"; docs/DECISIONS.md D48 point 6).
//
// M4/R0 and M4/CM landed in parallel from the same base (D47, D48) and, by both packages' own
// stated design, neither reads the other's type: `rewrite::Rule` proposes / merges
// `ir::PlanAnnotations` (ir/annotate.hpp); `optimise::estimate_program` prices an `optimise::Plan`
// (optimise/cost.hpp) that `infer_plan` reconstructs from IR structure alone, because CM's own
// brief is to price a candidate program that may never become a real `exec::Interpreter` at all.
// D48 point 6 names the follow-up this file is: "a follow-up should retire infer_plan's own
// approximation of the fusion / inlining rules in favour of reading ir::PlanAnnotations when one
// is attached to the Program being costed, keeping infer_plan's structural inference only as EG's
// fallback for a candidate program that has no attached plan yet (an unmaterialised e-class
// member)". That is exactly egraph::EGraph's own shape: every PLAN-tier e-node the saturation
// loop discovers IS a live `ir::PlanAnnotations` (R0's rules produced it), so extraction can cost
// it with the real decision instead of CM's structural guess; the guess remains the fallback for
// `ir::PlanAnnotations{}` (the empty root plan node every program starts with, i.e. "nothing
// decided yet", ir/annotate.hpp point 3) and for any domain a partial/malformed annotation
// (`plan.domain.size()` not 0 and not `program.domains.size()`) does not cover.
//
// What this file does NOT attempt: `optimise::Plan` prices per-domain MATERIALISATION only (see
// cost.hpp's own "what this cannot capture" — fused pairs / chain tails are two ops and two
// dispatches either way); `ir::PlanAnnotations::group` (StepPairing) and `::emitted` therefore
// have no effect on the price this file produces, exactly as they have none on `infer_plan`'s
// own estimate today. A rewrite that changes only step pairing looks free to extraction — a bias
// toward "no worse than the interpreter really does" (cost.hpp's own documented direction), not a
// hidden inaccuracy this file introduces on its own.
#pragma once

#include <vector>

#include "epykos/ir/annotate.hpp"
#include "epykos/ir/program.hpp"
#include "epykos/optimise/cost.hpp"

namespace epykos::optimise {

// `plan.domain.empty()`: every domain's treatment comes from `infer_plan(program, tile,
// lane_tile)` (CM's own structural guess — the fallback D48 point 6 names). Otherwise (size ==
// program.domains.size(), the only other state ir::annotate.hpp's own contract allows): domain d
// translates 1:1 — Materialize -> Materialized; FuseIntoReduction -> FusedIntoReduction with
// `kept_rows = keep_rows.size()` and `consumers = facts[d].segment_readers` (the reductions that
// read it — ir::DomainPlan itself does not store consumer ids, only which rows survive);
// InlineIntoConsumer -> Inlined with `consumers = {inline_consumer}`. A `plan.domain.size()` that
// is neither 0 nor `program.domains.size()` (a malformed partial annotation no rule in this
// codebase produces) falls back to infer_plan entirely, rather than guessing which domains the
// caller meant to cover.
Plan plan_from_annotations(const ir::Program& program, const std::vector<DomainFacts>& facts,
                            const ir::PlanAnnotations& plan, int tile, int lane_tile);

// Convenience: calls optimise::analyze(program) itself. Prefer the overload above (passing an
// already-computed `facts`) inside a search loop that costs many candidates of the SAME program,
// since DomainFacts depends only on structure, never on a plan.
Plan plan_from_annotations(const ir::Program& program, const ir::PlanAnnotations& plan, int tile, int lane_tile);

}  // namespace epykos::optimise
