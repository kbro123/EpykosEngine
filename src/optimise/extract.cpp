#include "epykos/optimise/extract.hpp"

#include <limits>

#include "epykos/optimise/plan_bridge.hpp"

namespace epykos::optimise {

namespace {
rewrite::Exactness combine(rewrite::Exactness a, rewrite::Exactness b) noexcept {
  return static_cast<int>(a) >= static_cast<int>(b) ? a : b;
}
}  // namespace

ExtractResult extract(const EGraph& graph, const CostModel& model, const ExtractOptions& options) {
  ExtractResult best;
  double best_ns = std::numeric_limits<double>::infinity();

  for (int program_id : graph.program_class_representatives()) {
    const ProgramNode& pnode = graph.program_node(program_id);
    if (static_cast<int>(pnode.exactness) > static_cast<int>(options.max_exactness)) {
      // No plan under this program can be cheaper than its own exactness floor; count every one
      // of its (as-yet-unpriced) plan candidates as rejected rather than silently skipping them.
      best.candidates_rejected_exactness += static_cast<std::size_t>(graph.num_plan_nodes(program_id));
      continue;
    }
    const std::vector<DomainFacts> facts = analyze(pnode.program);

    for (int plan_id : graph.plan_class_representatives(program_id)) {
      const PlanNode& plnode = graph.plan_node(program_id, plan_id);
      const rewrite::Exactness combined = combine(pnode.exactness, plnode.exactness);
      ++best.candidates_considered;
      if (static_cast<int>(combined) > static_cast<int>(options.max_exactness)) {
        ++best.candidates_rejected_exactness;
        continue;
      }

      const Plan plan = plan_from_annotations(pnode.program, facts, plnode.content, options.tile, options.lane_tile);
      const ProgramCost cost = estimate_program(pnode.program, plan, options.B, model);

      if (cost.total_ns < best_ns) {
        best_ns = cost.total_ns;
        best.found = true;
        best.program_id = program_id;
        best.plan_id = plan_id;
        best.program = pnode.program;
        best.plan = plnode.content;
        best.estimated_ns = cost.total_ns;
        best.history = pnode.history;
        best.history.insert(best.history.end(), plnode.history.begin(), plnode.history.end());
      }
    }
  }

  return best;
}

}  // namespace epykos::optimise
