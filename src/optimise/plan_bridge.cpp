#include "epykos/optimise/plan_bridge.hpp"

#include <cstddef>
#include <utility>

namespace epykos::optimise {

Plan plan_from_annotations(const ir::Program& program, const std::vector<DomainFacts>& facts,
                            const ir::PlanAnnotations& plan, int tile, int lane_tile) {
  const std::size_t n = program.domains.size();
  if (plan.domain.size() != n) {
    // Either "nothing decided" (annotate.hpp point 3: domain.size() == 0) or a shape no rule in
    // this codebase produces (ir::PlanAnnotations's own contract: domain.size() is 0 or exactly
    // program.domains.size(), never partial) — either way, CM's structural guess is the stated
    // fallback (D48 point 6).
    return infer_plan(program, tile, lane_tile);
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
  return out;
}

Plan plan_from_annotations(const ir::Program& program, const ir::PlanAnnotations& plan, int tile, int lane_tile) {
  const std::vector<DomainFacts> facts = analyze(program);
  return plan_from_annotations(program, facts, plan, tile, lane_tile);
}

}  // namespace epykos::optimise
