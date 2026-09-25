#include "epykos/optimise/plan_bridge.hpp"

#include <cstddef>
#include <utility>

#include "epykos/mutation/mutation.hpp"

namespace epykos::optimise {

namespace {

// D63: the step pairings `exec::Interpreter` would really run for THIS annotation. Called only
// for a NON-EMPTY annotation (an empty one means the interpreter derives its own default plan,
// which `plan_from_annotations` answers with `infer_plan` before it gets here), so there are
// exactly two cases, and both mirror `Interpreter::Impl::build_group`:
//   * `group.size() == program.domains.size()` -> those pairings verbatim;
//   * anything else -> NO pairings, because `build_group`'s own
//     `if (d < plan_.group.size())` finds none and runs one kernel per step. That is the case of
//     a plan whose history is `planner.reduction_fusion` alone, and of any plan built with
//     `exec::Options::fuse_pairs` off. Before D63 it was indistinguishable from the fully-paired
//     default, which is the defect this file exists to have fixed.
void apply_pairings(const ir::Program& program, const ir::PlanAnnotations& plan, Plan& out) {
  const std::size_t n = program.domains.size();
  // The mutant is the pre-D63 bridge: `ir::PlanAnnotations::group` never reached optimise::Plan
  // at all, so a rewrite that changed only step pairing was invisible to the price. It falls
  // through to the unconditional clear below, which is exactly that behaviour.
  if (!epykos::mutant("plan_bridge.discards_group") && plan.group.size() == n) {
    for (std::size_t d = 0; d < n; ++d) out.domains[d].pairings = plan.group[d].pairings;
    return;
  }
  for (std::size_t d = 0; d < n; ++d) out.domains[d].pairings.clear();
}

}  // namespace

Plan plan_from_annotations(const ir::Program& program, const std::vector<DomainFacts>& facts,
                            const ir::PlanAnnotations& plan, int tile, int lane_tile) {
  const std::size_t n = program.domains.size();

  // "Nothing decided at all" is the ONE case that falls back to a guess, and it is not really a
  // guess: `Interpreter::Impl::build_plan` derives `rewrite::planner::default_plan` for a Program
  // whose `plan` is `empty()`, and that is exactly what `infer_plan` now returns (D63).
  if (plan.empty()) return infer_plan(program, tile, lane_tile);

  Plan out;
  out.tile = tile;
  out.lane_tile = lane_tile;
  out.domains.resize(n);
  for (std::size_t d = 0; d < n; ++d) out.domains[d].domain = static_cast<ir::domain_id>(d);

  // Every other annotation is translated as the interpreter reads it, including the shapes that
  // do not cover every domain. `Interpreter::Impl::decide_fusion` loops `d < plan_.domain.size()`
  // and leaves every domain past that end alone, so an annotation with no `domain` at all (a
  // `planner.fused_pairs`-only or `planner.emit_outputs`-only plan node, both of which the
  // e-graph really produces) runs with NOTHING fused and NOTHING inlined — not with infer_plan's
  // decision, which is what this file used to substitute for it. D63: that substitution was the
  // same defect as the discarded `group`, in the opposite direction — it priced such a candidate
  // as if it had reduction fusion the interpreter would not give it.
  for (std::size_t d = 0; d < n && d < plan.domain.size(); ++d) {
    const ir::DomainPlan& src = plan.domain[d];
    DomainPlan& dst = out.domains[d];
    switch (src.choice) {
      case ir::Materialise::Materialize:
        dst.treatment = Treatment::Materialized;
        dst.kept_rows = 0;
        break;
      case ir::Materialise::FuseIntoReduction:
        dst.treatment = Treatment::FusedIntoReduction;
        dst.kept_rows = src.keep_rows.size();
        dst.consumers = facts[d].segment_readers;
        break;
      case ir::Materialise::InlineIntoConsumer:
        dst.treatment = Treatment::Inlined;
        dst.kept_rows = 0;
        dst.consumers = {src.inline_consumer};
        break;
    }
  }
  apply_pairings(program, plan, out);
  return out;
}

Plan plan_from_annotations(const ir::Program& program, const ir::PlanAnnotations& plan, int tile, int lane_tile) {
  const std::vector<DomainFacts> facts = analyze(program);
  return plan_from_annotations(program, facts, plan, tile, lane_tile);
}

}  // namespace epykos::optimise
