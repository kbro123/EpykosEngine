// M4/EG "core": plan_bridge.hpp's own gate -- D48 point 6's reconciliation between M4/R0's
// ir::PlanAnnotations and M4/CM's optimise::Plan. Uses cost_test.cpp's own reduction fixture
// (a producer domain feeding one Sum) so infer_plan's own answer (FusedIntoReduction) is known
// independently, letting the empty-annotation fallback path be checked against it directly.
#include "epykos/optimise/plan_bridge.hpp"

#include <gtest/gtest.h>

#include "epykos/ir/program.hpp"
#include "epykos/optimise/cost.hpp"

namespace ir = epykos::ir;
namespace optimise = epykos::optimise;
using epykos::Op;

namespace {

// Producer domain (n rows, a Const each) feeding one Sum reduction -- identical in shape to
// tests/optimise/cost_test.cpp's own MaterialisedCostsMoreThanFusedPastL1 fixture, so infer_plan's
// answer for it (FusedIntoReduction) is already an established, independently-checked fact.
ir::Program reduction_program(int n) {
  ir::Program p;
  ir::Domain producer;
  producer.rows = n;
  producer.value_base = 0;
  p.domains.push_back(producer);
  ir::Group producer_grp;
  producer_grp.domain = 0;
  ir::Step const_step;
  const_step.op = Op::Const;
  const_step.konst.kind = ir::SlotKind::Literal;
  const_step.konst.index = 0;
  producer_grp.steps.push_back(const_step);
  p.literals.push_back(1.0);
  p.groups.push_back(producer_grp);

  ir::Domain reducer;
  reducer.rows = 1;
  reducer.value_base = n;
  reducer.reads = {0};
  p.domains.push_back(reducer);
  ir::Segment seg;
  seg.domain = 1;
  seg.offsets = {0, n};
  for (int i = 0; i < n; ++i) seg.members.push_back(i);
  const std::int32_t seg_idx = static_cast<std::int32_t>(p.segments.size());
  p.segments.push_back(seg);
  ir::Group reducer_grp;
  reducer_grp.domain = 1;
  ir::Step sum_step;
  sum_step.op = Op::Sum;
  sum_step.a.kind = ir::SlotKind::Segment;
  sum_step.a.index = seg_idx;
  reducer_grp.steps.push_back(sum_step);
  p.groups.push_back(reducer_grp);
  p.outputs.push_back(n);
  return p;
}

}  // namespace

TEST(PlanBridge, EmptyAnnotationFallsBackToInferPlan) {
  const ir::Program p = reduction_program(4096);
  const optimise::Plan expected = optimise::infer_plan(p, /*tile=*/256, /*lane_tile=*/8);
  const optimise::Plan bridged = optimise::plan_from_annotations(p, ir::PlanAnnotations{}, /*tile=*/256, /*lane_tile=*/8);

  ASSERT_EQ(bridged.domains.size(), expected.domains.size());
  for (std::size_t d = 0; d < expected.domains.size(); ++d) {
    EXPECT_EQ(bridged.of(static_cast<ir::domain_id>(d)).treatment, expected.of(static_cast<ir::domain_id>(d)).treatment) << "domain " << d;
    EXPECT_EQ(bridged.of(static_cast<ir::domain_id>(d)).kept_rows, expected.of(static_cast<ir::domain_id>(d)).kept_rows) << "domain " << d;
  }
  EXPECT_EQ(bridged.of(0).treatment, optimise::Treatment::FusedIntoReduction);
}

TEST(PlanBridge, ExplicitMaterializeOverridesTheSmarterInferPlanGuess) {
  const ir::Program p = reduction_program(4096);
  ir::PlanAnnotations plan;
  plan.domain.assign(p.domains.size(), ir::DomainPlan{});  // every entry the default: Materialize
  const optimise::Plan bridged = optimise::plan_from_annotations(p, plan, /*tile=*/256, /*lane_tile=*/8);
  ASSERT_EQ(bridged.domains.size(), 2u);
  EXPECT_EQ(bridged.of(0).treatment, optimise::Treatment::Materialized);
  EXPECT_EQ(bridged.of(1).treatment, optimise::Treatment::Materialized);

  const optimise::CostModel model{optimise::CostCoefficients::defaults(), "test", false};
  const optimise::ProgramCost materialized = optimise::estimate_program(p, bridged, /*B=*/1, model);
  const optimise::Plan fused = optimise::infer_plan(p, /*tile=*/256, /*lane_tile=*/8);
  const optimise::ProgramCost fused_cost = optimise::estimate_program(p, fused, /*B=*/1, model);
  EXPECT_GT(materialized.total_ns, fused_cost.total_ns);
}

TEST(PlanBridge, ExplicitFuseAndInlineTranslateOneToOneWithConsumersFromFacts) {
  ir::Program p = reduction_program(16);
  // Give domain 1 (the reducer) a second row read by nothing, so InlineIntoConsumer has somewhere
  // sensible to point besides the trivial 2-domain graph -- a third domain gathering domain 1.
  ir::Domain consumer;
  consumer.rows = 1;
  consumer.value_base = static_cast<ir::value_id>(p.num_values());
  consumer.reads = {1};
  p.domains.push_back(consumer);
  ir::Gather g;
  g.domain = 2;
  g.index = {16};  // value id of the reducer's own row
  const std::int32_t gather_idx = static_cast<std::int32_t>(p.gathers.size());
  p.gathers.push_back(g);
  ir::Group consumer_grp;
  consumer_grp.domain = 2;
  ir::Step neg;
  neg.op = Op::Neg;
  neg.a.kind = ir::SlotKind::Gather;
  neg.a.index = gather_idx;
  consumer_grp.steps.push_back(neg);
  p.groups.push_back(consumer_grp);
  p.outputs = {static_cast<ir::value_id>(p.num_values()) - 1};

  ir::PlanAnnotations plan;
  plan.domain.assign(p.domains.size(), ir::DomainPlan{});
  plan.domain[0].choice = ir::Materialise::FuseIntoReduction;
  plan.domain[0].keep_rows = {2, 5};  // arbitrary, just non-empty
  plan.domain[1].choice = ir::Materialise::InlineIntoConsumer;
  plan.domain[1].inline_consumer = 2;

  const std::vector<optimise::DomainFacts> facts = optimise::analyze(p);
  const optimise::Plan bridged = optimise::plan_from_annotations(p, facts, plan, /*tile=*/256, /*lane_tile=*/8);
  ASSERT_EQ(bridged.domains.size(), 3u);
  EXPECT_EQ(bridged.of(0).treatment, optimise::Treatment::FusedIntoReduction);
  EXPECT_EQ(bridged.of(0).kept_rows, 2u);
  EXPECT_EQ(bridged.of(0).consumers, facts[0].segment_readers);
  EXPECT_EQ(bridged.of(1).treatment, optimise::Treatment::Inlined);
  ASSERT_EQ(bridged.of(1).consumers.size(), 1u);
  EXPECT_EQ(bridged.of(1).consumers[0], 2);
  EXPECT_EQ(bridged.of(2).treatment, optimise::Treatment::Materialized);
}

// D63 (was `MismatchedAnnotationSizeFallsBackToInferPlan`): a NON-EMPTY annotation whose `domain`
// vector does not reach every domain is not a reason to substitute infer_plan's decision. The
// interpreter's `decide_fusion` loops `d < plan_.domain.size()` and leaves every domain past that
// end Materialized, so the bridge must too — otherwise a `planner.fused_pairs`-only plan node
// (which the e-graph really produces, `domain` empty) is priced as if it had reduction fusion the
// interpreter would never give it, the same defect as the discarded `group` in the opposite
// direction.
TEST(PlanBridge, AnnotationShorterThanTheProgramLeavesTheRestMaterialised) {
  const ir::Program p = reduction_program(64);
  const optimise::Plan inferred = optimise::infer_plan(p, /*tile=*/256, /*lane_tile=*/8);
  ASSERT_EQ(inferred.of(0).treatment, optimise::Treatment::FusedIntoReduction)
      << "the fixture must be one where infer_plan and this annotation DISAGREE, or nothing is gated";

  ir::PlanAnnotations plan;
  plan.domain.assign(1, ir::DomainPlan{});  // neither 0 nor p.domains.size() (== 2); default = Materialize
  const optimise::Plan bridged = optimise::plan_from_annotations(p, plan, /*tile=*/256, /*lane_tile=*/8);
  ASSERT_EQ(bridged.domains.size(), p.domains.size());
  EXPECT_EQ(bridged.of(0).treatment, optimise::Treatment::Materialized) << "domain 0: the annotation's own choice";
  EXPECT_EQ(bridged.of(1).treatment, optimise::Treatment::Materialized) << "domain 1: past the annotation's end";
}

// The same rule for the shape that actually occurs: a plan node that decided step pairing and
// nothing else (`planner.fused_pairs` alone). Its pairings are real; its materialisation is not.
TEST(PlanBridge, AGroupOnlyAnnotationIsPairedButFusesNothing) {
  const ir::Program p = reduction_program(64);
  ir::PlanAnnotations plan;
  plan.group.assign(p.domains.size(), ir::GroupPlan{});
  ASSERT_FALSE(plan.empty());
  const optimise::Plan bridged = optimise::plan_from_annotations(p, plan, /*tile=*/256, /*lane_tile=*/8);
  for (std::size_t d = 0; d < p.domains.size(); ++d) {
    EXPECT_EQ(bridged.of(static_cast<ir::domain_id>(d)).treatment, optimise::Treatment::Materialized) << "domain " << d;
  }
}
