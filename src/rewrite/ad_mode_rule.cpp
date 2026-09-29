#include "epykos/rewrite/ad_mode_rule.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

#include "epykos/mutation/mutation.hpp"
#include "epykos/optimise/plan_bridge.hpp"
#include "epykos/rewrite/r7_block_linmap.hpp"

namespace epykos::rewrite {

namespace {

bool every_domain_is_linmap(const ir::Program& program, const std::vector<ir::domain_id>& domains) {
  if (domains.empty()) return false;
  for (ir::domain_id d : domains) {
    if (!is_linmap_domain(program, d)) return false;
  }
  return true;
}

const JacobianBlockSpec* find_block(const std::vector<JacobianBlockSpec>& blocks, ir::domain_id first_domain) {
  for (const JacobianBlockSpec& b : blocks) {
    if (!b.output_domains.empty() && b.output_domains.front() == first_domain) return &b;
  }
  return nullptr;
}

}  // namespace

ADModeDecision decide_ad_mode(const ir::Program& program, const JacobianBlockSpec& block, double one_pass_ns,
                              const optimise::CostModel& model, double adjoint_multiplier) {
  ADModeDecision d;
  // Mutant eg.ad_mode_affine_without_check: skip the structural eligibility check and treat every
  // block as affine-derived, so a genuinely nonlinear block's Jacobian would be built by the
  // closed-form (constant-coefficient) path and silently mis-differentiated -- caught directly by
  // ADModeRule.PicksTheCheapestModePerBlock's own `EXPECT_FALSE(nonlinear_decision.affine_eligible)`
  // on a nonlinear fixture (tests/rewrite/ad_mode_rule_e0_test.cpp; verified caught, 2026-09-24).
  d.affine_eligible = epykos::mutant("eg.ad_mode_affine_without_check") ? true : every_domain_is_linmap(program, block.output_domains);
  d.forward_ns = optimise::estimate_jacobian_ns(one_pass_ns, block.n_inputs, block.n_outputs, optimise::ADMode::Forward, model, adjoint_multiplier);
  d.reverse_ns = optimise::estimate_jacobian_ns(one_pass_ns, block.n_inputs, block.n_outputs, optimise::ADMode::Reverse, model, adjoint_multiplier);
  d.affine_ns = d.affine_eligible
                    ? optimise::estimate_jacobian_ns(one_pass_ns, block.n_inputs, block.n_outputs, optimise::ADMode::ClosedFormAffine, model, adjoint_multiplier)
                    : std::numeric_limits<double>::infinity();

  // Mutant eg.ad_mode_ignores_cost: always reverse, regardless of the three prices just computed
  // -- caught by any block whose measured shape favours forward or closed-form (a wide block: many
  // more outputs than inputs favours reverse, so the mutant's own test uses a NARROW one: few
  // outputs, many inputs, where forward is cheaper, or a linmap block where affine is free).
  if (epykos::mutant("eg.ad_mode_ignores_cost")) {
    d.mode = ir::AdMode::Reverse;
    return d;
  }
  double best = d.reverse_ns;
  d.mode = ir::AdMode::Reverse;
  if (d.forward_ns < best) {
    best = d.forward_ns;
    d.mode = ir::AdMode::Forward;
  }
  if (d.affine_ns < best) {
    d.mode = ir::AdMode::ClosedFormAffine;
  }
  return d;
}

ADModePerBlockRule::ADModePerBlockRule(std::vector<JacobianBlockSpec> blocks, optimise::CostModel model, ADModeRuleParams params)
    : blocks_(std::move(blocks)), model_(std::move(model)), params_(params), name_("eg.ad_mode_per_block") {
  for (const JacobianBlockSpec& b : blocks_) {
    if (b.output_domains.empty()) throw std::invalid_argument("ADModePerBlockRule: block '" + b.name + "' has no output_domains");
  }
}

const std::string& ADModePerBlockRule::name() const noexcept { return name_; }

std::vector<MatchSite> ADModePerBlockRule::match(const ir::Program& program, const ir::PlanAnnotations& plan) const {
  std::vector<MatchSite> sites;
  for (const JacobianBlockSpec& b : blocks_) {
    if (plan.jacobian.mode.count(b.name) != 0) continue;  // already decided: fixpoint
    ir::domain_id d = b.output_domains.front();
    if (d < 0 || static_cast<std::size_t>(d) >= program.domains.size()) continue;  // stale spec for this program
    sites.push_back(MatchSite::of_domain(d));
  }
  return sites;
}

Proposal ADModePerBlockRule::propose(const ir::Program& program, const ir::PlanAnnotations& plan, const MatchSite& site) const {
  const JacobianBlockSpec* block = find_block(blocks_, site.domain);
  if (block == nullptr) throw std::invalid_argument("ADModePerBlockRule::propose: site does not name one of this rule's blocks");

  const std::vector<optimise::DomainFacts> facts = optimise::analyze(program);
  const optimise::Plan bridged = optimise::plan_from_annotations(program, facts, plan, params_.tile, params_.lane_tile);
  const optimise::ProgramCost one_pass = optimise::estimate_program(program, bridged, /*B=*/1, model_);
  const ADModeDecision decision = decide_ad_mode(program, *block, one_pass.total_ns, model_, params_.adjoint_multiplier);

  ir::PlanAnnotations delta;
  delta.jacobian.mode[block->name] = decision.mode;
  Proposal p;
  p.annotations = std::move(delta);
  return p;
}

}  // namespace epykos::rewrite
