#include "epykos/rewrite/greedy.hpp"

#include <algorithm>
#include <stdexcept>

namespace epykos::rewrite {

void merge_annotations(ir::PlanAnnotations& dst, const ir::PlanAnnotations& src) {
  if (src.domain.size() > dst.domain.size()) dst.domain.resize(src.domain.size());
  for (std::size_t d = 0; d < src.domain.size(); ++d) {
    if (!(src.domain[d] == ir::DomainPlan{})) dst.domain[d] = src.domain[d];
  }
  if (src.group.size() > dst.group.size()) dst.group.resize(src.group.size());
  for (std::size_t d = 0; d < src.group.size(); ++d) {
    if (!(src.group[d] == ir::GroupPlan{})) dst.group[d] = src.group[d];
  }
  if (src.emitted.size() > dst.emitted.size()) dst.emitted.resize(src.emitted.size(), 0);
  for (std::size_t v = 0; v < src.emitted.size(); ++v) {
    if (src.emitted[v]) dst.emitted[v] = 1;
  }
  for (const auto& [key, mode] : src.jacobian.mode) dst.jacobian.mode[key] = mode;
}

std::size_t apply_greedy(const Rule& rule, ir::Program& program, ir::PlanAnnotations& plan) {
  const std::vector<MatchSite> sites = rule.match(program, plan);
  std::size_t applied = 0;
  for (const MatchSite& site : sites) {
    Proposal p = rule.propose(program, plan, site);
    if (p.empty()) {
      throw std::logic_error("rewrite: rule '" + rule.name() + "' matched a site it would not propose at");
    }
    if (p.is_structural()) {
      // A structural rewrite stands on its own; every other site this call's match() found was
      // computed against the pre-rewrite program and must not be applied afterwards (their
      // indices may no longer refer to the same domains/steps). Rule authors: return one
      // Program-kind site per apply_greedy call when the rewrite is structural.
      program = std::move(*p.program);
      ++applied;
      return applied;
    }
    merge_annotations(plan, *p.annotations);
    ++applied;
  }
  return applied;
}

void run_pipeline(const std::vector<const Rule*>& rules, ir::Program& program, ir::PlanAnnotations& plan) {
  for (const Rule* rule : rules) {
    if (rule != nullptr) apply_greedy(*rule, program, plan);
  }
}

}  // namespace epykos::rewrite
