// EpykosEngine — AD mode per Jacobian block, chosen by cost (M4/EG "integration"; PROBLEM.md §7,
// DESIGN.md §7, D47/D49/D50's own "scoped out, deliberately": ir::PlanAnnotations::JacobianBlockPlan
// has NO CONSUMER before this file: rewrite::R7BlockLinmap's own header names is_linmap_domain as
// "the fact M4/EG needs to make that call once it has a consumer to gate it with" and D50 point 5
// restates it — wiring AdMode::ClosedFormAffine (and the forward/reverse choice alongside it) is
// this package's job. See include/epykos/adjoint/block_jacobian.hpp for the consumer this rule's
// decision feeds: without it, a decision here could not be checked by any of CLAUDE.md's gates
// (a differential/mutation test needs something that actually computes a Jacobian differently
// per mode) — exactly the situation that header exists to close.
//
// A "Jacobian block" here is a caller-named group of Program outputs (`output_domains`, the
// domain ids whose rows are this block's Jacobian rows) differentiated with respect to a set of
// input ordinals (`n_inputs`) — e.g. one calibration block's residual w.r.t. its own quotes, or
// the whole risk ladder's outputs w.r.t. every free quote. The rule itself does not know what a
// "calibration block" or a "risk ladder" is (HARD RULE 9): a caller (a test, a benchmark, an
// experiment driver) derives `JacobianBlockSpec::output_domains` from whatever registry or output
// grouping ITS OWN problem uses (solver::ImplicitRegistry's blocks, PROBLEM.md §5's O3 sample) and
// hands them to this rule as plain IR structure (domain ids) plus two integers.
//
// Cost, by optimise::estimate_jacobian_ns (docs/PROBLEM.md §7's "AD mode per Jacobian block is a
// rule"; docs/RESUME.md §3 EG row): forward = n_inputs whole-program passes, reverse = n_outputs
// adjoint lanes at `adjoint_multiplier` per pass, closed-form-affine = one dense n_outputs ×
// n_inputs product, ELIGIBLE ONLY when every one of `output_domains` is a linmap domain
// (rewrite::is_linmap_domain, R7's own fact) whose members are themselves Program Inputs — a
// stated, checked precondition (adjoint/block_jacobian.hpp's own scope), not assumed silently:
// propose() never marks a block ClosedFormAffine-eligible unless is_linmap_domain holds for
// EVERY domain in the block, but it does not itself walk the block's members back to Inputs (that
// walk, and its own narrower "direct Input" scope, live in the consumer that actually builds the
// dense Jacobian — a mismatch between the two would be a `block_jacobian` bug, not a rule bug,
// and `block_jacobian` throws loudly rather than silently mis-differentiating).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "epykos/ir/annotate.hpp"
#include "epykos/ir/program.hpp"
#include "epykos/optimise/cost.hpp"
#include "epykos/rewrite/rule.hpp"

namespace epykos::rewrite {

struct JacobianBlockSpec {
  std::string name;                            // PlanAnnotations::jacobian.mode's key
  std::vector<ir::domain_id> output_domains;    // this block's Jacobian ROWS live in these domains' rows
  int n_inputs = 0;                            // Jacobian columns (a directional/adjoint pass count)
  int n_outputs = 0;                           // Jacobian rows (may exceed sum of the domains' rows
                                                // when the caller's own block reads them more than
                                                // once per output, e.g. one lane per scenario trade)
};

struct ADModeRuleParams {
  int tile = 256;
  int lane_tile = 8;
  double adjoint_multiplier = 8.0;  // optimise::estimate_jacobian_ns's own default, named here so a
                                     // caller can override it consistently with its own measured ratio
};

// Chooses, per named block, whichever of {Forward, Reverse, ClosedFormAffine} optimise::
// estimate_jacobian_ns prices cheapest given one whole-program B=1 pass under the plan decided so
// far (optimise::plan_from_annotations + optimise::estimate_program) — ClosedFormAffine excluded
// unless every domain of the block is a linmap domain. E0: choosing HOW to compute a Jacobian
// changes nothing about what is computed (the three modes must, and are tested to, agree).
//
// One MatchSite::of_domain(block.output_domains.front()) per block whose name is not yet a key of
// `plan.jacobian.mode` — a real IR-structural site (R0's own convention: a decision keyed on the
// domain it concerns), and idempotent (match() returns nothing once every block is decided, so a
// saturation loop reaches a fixpoint instead of re-proposing the same decision every round).
class ADModePerBlockRule final : public Rule {
 public:
  ADModePerBlockRule(std::vector<JacobianBlockSpec> blocks, optimise::CostModel model, ADModeRuleParams params = {});

  const std::string& name() const noexcept override;
  Exactness exactness_class() const noexcept override { return Exactness::E0; }
  std::vector<MatchSite> match(const ir::Program& program, const ir::PlanAnnotations& plan) const override;
  Proposal propose(const ir::Program& program, const ir::PlanAnnotations& plan, const MatchSite& site) const override;

  const std::vector<JacobianBlockSpec>& blocks() const noexcept { return blocks_; }

 private:
  std::vector<JacobianBlockSpec> blocks_;
  optimise::CostModel model_;
  ADModeRuleParams params_;
  std::string name_;
};

// The pure decision (propose()'s own logic, exposed for a direct unit test / experiment report
// without building a whole Program + Rule machinery around it): the mode `estimate_jacobian_ns`
// prices cheapest for one block at `one_pass_ns`, and whether that block is ClosedFormAffine-
// eligible per every domain of `block.output_domains` being a linmap domain of `program`.
struct ADModeDecision {
  ir::AdMode mode = ir::AdMode::Reverse;
  bool affine_eligible = false;
  double forward_ns = 0.0, reverse_ns = 0.0, affine_ns = 0.0;
};

ADModeDecision decide_ad_mode(const ir::Program& program, const JacobianBlockSpec& block, double one_pass_ns,
                              const optimise::CostModel& model, double adjoint_multiplier = 8.0);

}  // namespace epykos::rewrite
