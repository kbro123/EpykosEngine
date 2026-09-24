#include "epykos/rewrite/planner_rules.hpp"

#include "epykos/rewrite/greedy.hpp"

namespace epykos::rewrite::planner {

namespace {
std::vector<MatchSite> single_site_if(bool applies) {
  if (!applies) return {};
  return {MatchSite::whole_program()};
}
}  // namespace

// -------------------------------------------------------------------------------------------
// ReductionFusionRule
// -------------------------------------------------------------------------------------------

ReductionFusionRule::ReductionFusionRule(int lane_tile, ReductionFusionParams params)
    : lane_tile_(lane_tile), params_(params), name_("planner.reduction_fusion") {}

const std::string& ReductionFusionRule::name() const noexcept { return name_; }

std::vector<MatchSite> ReductionFusionRule::match(const ir::Program& program, const ir::PlanAnnotations&) const {
  return single_site_if(!program.domains.empty());
}

Proposal ReductionFusionRule::propose(const ir::Program& program, const ir::PlanAnnotations&, const MatchSite&) const {
  Proposal p;
  p.annotations = reduction_fusion_plan(program, lane_tile_, params_);
  return p;
}

// -------------------------------------------------------------------------------------------
// EmitOutputsRule
// -------------------------------------------------------------------------------------------

EmitOutputsRule::EmitOutputsRule(int lane_tile, EmitOutputsParams params)
    : lane_tile_(lane_tile), params_(params), name_("planner.emit_outputs") {}

const std::string& EmitOutputsRule::name() const noexcept { return name_; }

std::vector<MatchSite> EmitOutputsRule::match(const ir::Program& program, const ir::PlanAnnotations&) const {
  return single_site_if(!program.domains.empty());
}

Proposal EmitOutputsRule::propose(const ir::Program& program, const ir::PlanAnnotations& plan, const MatchSite&) const {
  Proposal p;
  p.annotations = emit_outputs_plan(program, plan, lane_tile_, params_);
  return p;
}

// -------------------------------------------------------------------------------------------
// InlineProducersRule
// -------------------------------------------------------------------------------------------

InlineProducersRule::InlineProducersRule(int lane_tile, InlineProducersParams params)
    : lane_tile_(lane_tile), params_(params), name_("planner.inline_producers") {}

const std::string& InlineProducersRule::name() const noexcept { return name_; }

std::vector<MatchSite> InlineProducersRule::match(const ir::Program& program, const ir::PlanAnnotations&) const {
  return single_site_if(!program.domains.empty() && row_fusion_pays(lane_tile_));
}

Proposal InlineProducersRule::propose(const ir::Program& program, const ir::PlanAnnotations& plan,
                                      const MatchSite&) const {
  Proposal p;
  p.annotations = inline_producers_plan(program, plan, lane_tile_, params_);
  return p;
}

// -------------------------------------------------------------------------------------------
// FusedPairsRule
// -------------------------------------------------------------------------------------------

FusedPairsRule::FusedPairsRule() : name_("planner.fused_pairs") {}

const std::string& FusedPairsRule::name() const noexcept { return name_; }

std::vector<MatchSite> FusedPairsRule::match(const ir::Program& program, const ir::PlanAnnotations&) const {
  return single_site_if(!program.domains.empty());
}

Proposal FusedPairsRule::propose(const ir::Program& program, const ir::PlanAnnotations&, const MatchSite&) const {
  Proposal p;
  p.annotations = fused_pairs_plan(program);
  return p;
}

// -------------------------------------------------------------------------------------------
// ChainTailsRule
// -------------------------------------------------------------------------------------------

ChainTailsRule::ChainTailsRule(int lane_tile, int max_tail)
    : lane_tile_(lane_tile), max_tail_(max_tail), name_("planner.chain_tails") {}

const std::string& ChainTailsRule::name() const noexcept { return name_; }

std::vector<MatchSite> ChainTailsRule::match(const ir::Program& program, const ir::PlanAnnotations& plan) const {
  return single_site_if(!program.domains.empty() && row_fusion_pays(lane_tile_) && !plan.group.empty());
}

Proposal ChainTailsRule::propose(const ir::Program& program, const ir::PlanAnnotations& plan, const MatchSite&) const {
  Proposal p;
  p.annotations = chain_tails_plan(program, plan, lane_tile_, max_tail_);
  return p;
}

// -------------------------------------------------------------------------------------------
// DefaultPlanner
// -------------------------------------------------------------------------------------------

namespace {
// A rule whose Options flag is off contributes nothing: an identity Proposal (default-
// constructed, empty PlanAnnotations) at a Program site that always matches — merging it is a
// no-op, exactly like the M1 planner's own `if (opt.fuse_reductions) { ... }` skip.
class OffRule final : public Rule {
 public:
  explicit OffRule(std::string name) : name_(std::move(name)) {}
  const std::string& name() const noexcept override { return name_; }
  Exactness exactness_class() const noexcept override { return Exactness::E0; }
  std::vector<MatchSite> match(const ir::Program&, const ir::PlanAnnotations&) const override { return {}; }
  Proposal propose(const ir::Program&, const ir::PlanAnnotations&, const MatchSite&) const override { return {}; }

 private:
  std::string name_;
};
}  // namespace

DefaultPlanner::DefaultPlanner(DefaultPlanOptions options)
    : reduction_fusion_(options.lane_tile),
      fused_pairs_(),
      chain_tails_(options.lane_tile),
      inline_producers_(options.lane_tile),
      emit_outputs_(options.lane_tile) {
  static const OffRule off_reduction("planner.reduction_fusion");
  static const OffRule off_pairs("planner.fused_pairs");
  static const OffRule off_tails("planner.chain_tails");
  static const OffRule off_inline("planner.inline_producers");
  rules_.push_back(options.fuse_reductions ? static_cast<const Rule*>(&reduction_fusion_) : &off_reduction);
  rules_.push_back(options.fuse_pairs ? static_cast<const Rule*>(&fused_pairs_) : &off_pairs);
  rules_.push_back(options.fuse_pairs ? static_cast<const Rule*>(&chain_tails_) : &off_tails);
  rules_.push_back(options.inline_producers ? static_cast<const Rule*>(&inline_producers_) : &off_inline);
  // emit_outputs depends only on reduction_fusion's own annotations (empty when that rule is
  // off, so emit_outputs_plan finds nothing to emit): always runs, mirroring decide_fusion's
  // single `if (opt.fuse_reductions) { ... the whole decision, including emit ... }` block.
  rules_.push_back(&emit_outputs_);
}

ir::PlanAnnotations default_plan(const ir::Program& program, DefaultPlanOptions options) {
  // rewrite::run_pipeline (rewrite/greedy.hpp) takes `ir::Program&` because a general rule may
  // rewrite the IR itself (R1-R7); none of the five planner rules ever do (their Proposal is
  // always Proposal::is_annotation()), so this loop works against `program` by const reference
  // throughout and never pays for a copy of it — the M1 bench's "time-identical" bar (PROBLEM.md
  // §7) would see a Stage-A-sized (517,036-node) copy on every Interpreter/Adjoint construction
  // otherwise. A rule that DID need the structural path would use run_pipeline instead.
  ir::PlanAnnotations plan;
  DefaultPlanner planner(options);
  for (const Rule* rule : planner.rules()) {
    for (const MatchSite& site : rule->match(program, plan)) {
      Proposal p = rule->propose(program, plan, site);
      if (p.is_annotation()) merge_annotations(plan, *p.annotations);
    }
  }
  return plan;
}

}  // namespace epykos::rewrite::planner
