// EpykosEngine — the M1 planner's five decisions as rewrite::Rule objects (M4/R0; DESIGN.md §7).
//
// Each wraps one function of rewrite/planner.hpp: the pure decision. All five are E0 (a
// materialisation / pairing / emission choice changes nothing about the arithmetic — DESIGN.md
// §7's own framing: "a structure-only decision; the arithmetic is the same either way"). Every
// one is a whole-program decision (SiteKind::Program): the M1 planner's algorithms are
// inherently sequential (each domain's or step's verdict can depend on an earlier one's, within
// the SAME pass — e.g. planner.inline_producers walks domains left to right and a later domain
// can see an earlier one's acceptance), so there is no useful finer-grained site to offer a
// driver here; rewrite/rule.hpp documents this as the expected shape for a planner rule. R1-R7
// (rewrite/stub/*.hpp), by contrast, pattern-match one domain / step / gather / segment at a
// time, because DESIGN.md §6's rewrites are genuinely local.
//
// default_planner_rules() returns the five in PROBLEM.md §7 / RESUME.md's fixed order
// (reduction_fusion -> fused_pairs -> chain_tails -> inline_producers -> emit_outputs — the
// dependency order decide_fusion() then decide_inline() then the per-step loop ran in before
// M4/R0), gated by an exec::Options-shaped set of flags and Lt so run_pipeline(default_planner_
// rules(...), program, plan) reproduces exactly what an unannotated Interpreter/Adjoint computed
// before this package: this is PROBLEM.md §7's "Options flags keep working by selecting the
// greedy pass" and "the default plan must be bit-identical and time-identical to M1's".
#pragma once

#include <memory>
#include <vector>

#include "epykos/rewrite/planner.hpp"
#include "epykos/rewrite/rule.hpp"

namespace epykos::rewrite::planner {

class ReductionFusionRule final : public Rule {
 public:
  explicit ReductionFusionRule(int lane_tile, ReductionFusionParams params = {});
  const std::string& name() const noexcept override;
  Exactness exactness_class() const noexcept override { return Exactness::E0; }
  std::vector<MatchSite> match(const ir::Program& program, const ir::PlanAnnotations& plan) const override;
  Proposal propose(const ir::Program& program, const ir::PlanAnnotations& plan, const MatchSite& site) const override;

 private:
  int lane_tile_;
  ReductionFusionParams params_;
  std::string name_;
};

class EmitOutputsRule final : public Rule {
 public:
  EmitOutputsRule(int lane_tile, EmitOutputsParams params = {});
  const std::string& name() const noexcept override;
  Exactness exactness_class() const noexcept override { return Exactness::E0; }
  std::vector<MatchSite> match(const ir::Program& program, const ir::PlanAnnotations& plan) const override;
  Proposal propose(const ir::Program& program, const ir::PlanAnnotations& plan, const MatchSite& site) const override;

 private:
  int lane_tile_;
  EmitOutputsParams params_;
  std::string name_;
};

class InlineProducersRule final : public Rule {
 public:
  InlineProducersRule(int lane_tile, InlineProducersParams params = {});
  const std::string& name() const noexcept override;
  Exactness exactness_class() const noexcept override { return Exactness::E0; }
  std::vector<MatchSite> match(const ir::Program& program, const ir::PlanAnnotations& plan) const override;
  Proposal propose(const ir::Program& program, const ir::PlanAnnotations& plan, const MatchSite& site) const override;

 private:
  int lane_tile_;
  InlineProducersParams params_;
  std::string name_;
};

class FusedPairsRule final : public Rule {
 public:
  FusedPairsRule();
  const std::string& name() const noexcept override;
  Exactness exactness_class() const noexcept override { return Exactness::E0; }
  std::vector<MatchSite> match(const ir::Program& program, const ir::PlanAnnotations& plan) const override;
  Proposal propose(const ir::Program& program, const ir::PlanAnnotations& plan, const MatchSite& site) const override;

 private:
  std::string name_;
};

class ChainTailsRule final : public Rule {
 public:
  explicit ChainTailsRule(int lane_tile, int max_tail = 2);
  const std::string& name() const noexcept override;
  Exactness exactness_class() const noexcept override { return Exactness::E0; }
  std::vector<MatchSite> match(const ir::Program& program, const ir::PlanAnnotations& plan) const override;
  Proposal propose(const ir::Program& program, const ir::PlanAnnotations& plan, const MatchSite& site) const override;

 private:
  int lane_tile_;
  int max_tail_;
  std::string name_;
};

// The flags an exec::Options / adjoint::Options-shaped caller selects the default greedy pass
// with, plus the derived lane-chunk width (Lt = min(lane_tile, max_batch), DESIGN.md §7 D15) the
// row_fusion_pays / emit-byte-size gates need.
struct DefaultPlanOptions {
  bool fuse_reductions = true;
  bool fuse_pairs = true;
  bool inline_producers = true;
  int lane_tile = 8;  // Lt
};

// Owns the five rule objects for one DefaultPlanOptions (an Interpreter/Adjoint's own Options
// map onto this 1:1 — see exec::Interpreter's plan builder) and exposes them in pipeline order,
// omitting a rule whose Options flag is off (exactly as `if (opt.fuse_reductions) ...` /
// `if (opt.fuse_pairs) ...` gated the M1 planner's own decisions).
class DefaultPlanner {
 public:
  explicit DefaultPlanner(DefaultPlanOptions options);
  const std::vector<const Rule*>& rules() const noexcept { return rules_; }

 private:
  ReductionFusionRule reduction_fusion_;
  FusedPairsRule fused_pairs_;
  ChainTailsRule chain_tails_;
  InlineProducersRule inline_producers_;
  EmitOutputsRule emit_outputs_;
  std::vector<const Rule*> rules_;
};

// Runs DefaultPlanner(options)'s rules over `program`, from an empty PlanAnnotations, and
// returns the result — the plan exec::Interpreter / adjoint::Adjoint derive for themselves when
// a caller's `program.plan` is empty (ir/annotate.hpp point 3: never written back to `program`).
ir::PlanAnnotations default_plan(const ir::Program& program, DefaultPlanOptions options);

}  // namespace epykos::rewrite::planner
