#include "epykos/optimise/plan_bridge.hpp"

#include <cstddef>
#include <utility>

#include "epykos/mutation/mutation.hpp"

namespace epykos::optimise {

namespace {

// D63: the step pairings `exec::Interpreter` would really run for THIS annotation, mirroring
// Interpreter::Impl::build_plan + build_group exactly:
//   * the annotation is `empty()` (nothing decided at all) -> the interpreter derives its own
//     default plan, so the pairings are infer_plan's (which calls the planner's own rules);
//   * `group.size() == program.domains.size()` -> those pairings verbatim;
//   * otherwise (a non-empty annotation whose `group` was never populated — e.g. a plan whose
//     history is `planner.reduction_fusion` alone, or one built with `fuse_pairs` off) -> NO
//     pairings, because build_group's own `if (d < plan_.group.size())` finds none and runs one
//     kernel per step. This is the difference D63 exists to price: before it, such a plan was
//     indistinguishable from the fully-paired default.
// `already_inferred`: `out` came straight from infer_plan, so it ALREADY carries the pairings
// infer_plan would infer -- do not compute them a second time. Extraction prices the empty root
// candidate of every program node, and on a Stage-A-sized Program infer_plan is not free.
void apply_pairings(const ir::Program& program, const ir::PlanAnnotations& plan, int tile, int lane_tile, bool already_inferred,
                    Plan& out) {
  const std::size_t n = program.domains.size();
  // The mutant is the pre-D63 bridge: `ir::PlanAnnotations::group` never reached optimise::Plan
  // at all, so a rewrite that changed only step pairing was invisible to the price. It falls
  // through to the unconditional clear at the bottom, which is exactly that behaviour.
  if (!epykos::mutant("plan_bridge.discards_group")) {
    if (plan.group.size() == n) {
      for (std::size_t d = 0; d < n; ++d) out.domains[d].pairings = plan.group[d].pairings;
      return;
    }
    if (plan.empty()) {
      if (already_inferred) return;
      const Plan inferred = infer_plan(program, tile, lane_tile);
      for (std::size_t d = 0; d < n && d < inferred.domains.size(); ++d) out.domains[d].pairings = inferred.domains[d].pairings;
      return;
    }
  }
  for (std::size_t d = 0; d < n; ++d) out.domains[d].pairings.clear();
}

}  // namespace

Plan plan_from_annotations(const ir::Program& program, const std::vector<DomainFacts>& facts,
                            const ir::PlanAnnotations& plan, int tile, int lane_tile) {
  const std::size_t n = program.domains.size();
  if (plan.domain.size() != n) {
    // Either "nothing decided" (annotate.hpp point 3: domain.size() == 0) or a shape no rule in
    // this codebase produces (ir::PlanAnnotations's own contract: domain.size() is 0 or exactly
    // program.domains.size(), never partial) — either way, CM's structural guess is the stated
    // fallback (D48 point 6) for the per-domain TREATMENT. The pairings are still whatever this
    // annotation itself says (apply_pairings): a plan that names a `group` but no `domain` is
    // run by the interpreter with those pairings, not with infer_plan's.
    Plan out = infer_plan(program, tile, lane_tile);
    apply_pairings(program, plan, tile, lane_tile, /*already_inferred=*/true, out);
    return out;
  }

  Plan out;
  out.tile = tile;
  out.lane_tile = lane_tile;
  out.domains.resize(n);
  for (std::size_t d = 0; d < n; ++d) {
    const ir::DomainPlan& src = plan.domain[d];
    DomainPlan dst;
    dst.domain = static_cast<ir::domain_id>(d);
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
    out.domains[d] = std::move(dst);
  }
  apply_pairings(program, plan, tile, lane_tile, /*already_inferred=*/false, out);
  return out;
}

Plan plan_from_annotations(const ir::Program& program, const ir::PlanAnnotations& plan, int tile, int lane_tile) {
  const std::vector<DomainFacts> facts = analyze(program);
  return plan_from_annotations(program, facts, plan, tile, lane_tile);
}

}  // namespace epykos::optimise
