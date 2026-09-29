// M4/EG "core": extraction (include/epykos/optimise/extract.hpp) -- the package's own gates:
// "extraction picks the cheaper of two known-equivalent forms", "E0 extraction never selects an
// E1 rewrite (a test with a fake E1 rule that is cheaper)" and "determinism (two runs give
// identical extraction)". Uses the same reduction fixture as plan_bridge_test.cpp: a producer
// domain (n rows past the model's own L1 byte budget, cost_test.cpp's own device for this) folded
// into one Sum, where infer_plan's own (already-tested) guess is cheaper than an explicit
// Materialize -- so "two known-equivalent forms, one cheaper" falls out of the real cost model,
// not a hand-picked number.
#include "epykos/optimise/extract.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "epykos/ir/program.hpp"
#include "epykos/optimise/cost.hpp"
#include "epykos/optimise/egraph.hpp"
#include "epykos/rewrite/rule.hpp"

namespace ir = epykos::ir;
namespace rewrite = epykos::rewrite;
namespace optimise = epykos::optimise;
using epykos::Op;

namespace {

ir::Program reduction_program(int n) {
  ir::Program p;
  ir::Domain producer;
  producer.rows = n;
  producer.value_base = 0;
  p.domains.push_back(producer);
  ir::Group producer_grp;
  producer_grp.domain = 0;
  // Op::Neg (not Op::Const): CostCoefficients::defaults() prices a leaf Const/Input at exactly
  // 0 ns ("no arithmetic, just occupy a row") -- a real op is needed so FusedIntoReduction (which
  // still charges the consumer for every row's arithmetic) and Inlined (charged only per GATHER
  // reference, here 0 -- the producer is read via a segment, never a gather) price differently.
  ir::Step neg_step;
  neg_step.op = Op::Neg;
  neg_step.a.kind = ir::SlotKind::Literal;
  neg_step.a.index = 0;
  producer_grp.steps.push_back(neg_step);
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

optimise::CostModel flat_model() { return optimise::CostModel{optimise::CostCoefficients::defaults(), "test", false}; }

// Proposes an EXPLICIT "materialize everything" plan (deliberately worse than infer_plan's own
// FusedIntoReduction guess for `reduction_program` -- see plan_bridge_test.cpp's own proof).
class ExplicitMaterializeRule : public rewrite::Rule {
 public:
  const std::string& name() const noexcept override {
    static const std::string n = "test.explicit_materialize";
    return n;
  }
  rewrite::Exactness exactness_class() const noexcept override { return rewrite::Exactness::E0; }
  std::vector<rewrite::MatchSite> match(const ir::Program&, const ir::PlanAnnotations&) const override {
    return {rewrite::MatchSite::whole_program()};
  }
  rewrite::Proposal propose(const ir::Program& program, const ir::PlanAnnotations&, const rewrite::MatchSite&) const override {
    rewrite::Proposal p;
    ir::PlanAnnotations a;
    a.domain.assign(program.domains.size(), ir::DomainPlan{});  // every entry default: Materialize
    p.annotations = a;
    return p;
  }
};

// A deliberately wrong-for-this-shape but CHEAP rewrite, tagged E1: treats the producer domain as
// Inlined into the reducer. `optimise::Treatment::Inlined` prices "every row, gather_refs times
// over" (cost.hpp), and nothing gathers the producer in this fixture (it is read only via a
// SEGMENT) -- so `folded_cost_ns` sees gather_refs == 0 and prices it at essentially nothing: a
// concrete stand-in for "a fake E1 rule the cost model prices lower than any real E0 alternative",
// which is exactly what PROBLEM.md §7's own gate ("E0 extraction may not use E1-class rewrites")
// exists to keep out. Not a real rewrite; a test fixture only, same spirit as
// tests/rewrite/rule_framework_test.cpp's own deliberately-wrong MarkEverythingFusedRule.
class FakeCheapE1InlineRule : public rewrite::Rule {
 public:
  const std::string& name() const noexcept override {
    static const std::string n = "test.fake_cheap_e1_inline";
    return n;
  }
  rewrite::Exactness exactness_class() const noexcept override { return rewrite::Exactness::E1; }
  std::vector<rewrite::MatchSite> match(const ir::Program&, const ir::PlanAnnotations&) const override {
    return {rewrite::MatchSite::whole_program()};
  }
  rewrite::Proposal propose(const ir::Program& program, const ir::PlanAnnotations&, const rewrite::MatchSite&) const override {
    rewrite::Proposal p;
    ir::PlanAnnotations a;
    a.domain.assign(program.domains.size(), ir::DomainPlan{});
    a.domain[0].choice = ir::Materialise::InlineIntoConsumer;
    a.domain[0].inline_consumer = 1;
    p.annotations = a;
    return p;
  }
};

}  // namespace

TEST(Extract, PicksCheaperOfTwoKnownEquivalentForms) {
  const int n = static_cast<int>(optimise::CostCoefficients::defaults().l1_bytes / 8) * 4;
  optimise::EGraph g(reduction_program(n));
  const ExplicitMaterializeRule rule;
  g.saturate({&rule});
  ASSERT_EQ(g.num_plan_nodes(0), 2);  // the root (infer_plan's fused guess) and the explicit one

  const optimise::ExtractResult result = optimise::extract(g, flat_model());
  ASSERT_TRUE(result.found);
  EXPECT_EQ(result.plan_id, 0);  // the ROOT (empty annotation -> infer_plan's cheaper guess) wins
  EXPECT_TRUE(result.history.empty());
  EXPECT_TRUE(optimise::EGraph::plan_content_equal(result.plan, ir::PlanAnnotations{}));

  // Cross-check against the two costs computed directly (plan_bridge_test.cpp's own proof).
  const optimise::Plan fused = optimise::infer_plan(result.program, 256, 8);
  const optimise::ProgramCost fused_cost = optimise::estimate_program(result.program, fused, 1, flat_model());
  EXPECT_DOUBLE_EQ(result.estimated_ns, fused_cost.total_ns);
}

TEST(Extract, E0ExtractionNeverSelectsAnE1RewriteEvenACheaperOne) {
  const int n = static_cast<int>(optimise::CostCoefficients::defaults().l1_bytes / 8) * 4;
  optimise::EGraph g(reduction_program(n));
  const ExplicitMaterializeRule materialize_rule;
  const FakeCheapE1InlineRule cheap_e1;
  g.saturate({&materialize_rule, &cheap_e1});

  optimise::ExtractOptions e0_options;
  e0_options.max_exactness = rewrite::Exactness::E0;
  const optimise::ExtractResult e0_result = optimise::extract(g, flat_model(), e0_options);
  ASSERT_TRUE(e0_result.found);
  for (const std::string& rule_name : e0_result.history) EXPECT_NE(rule_name, cheap_e1.name());
  EXPECT_GT(e0_result.candidates_rejected_exactness, 0u) << "the E1 candidate must have been seen and rejected, not absent";

  optimise::ExtractOptions e1_options;
  e1_options.max_exactness = rewrite::Exactness::E1;
  const optimise::ExtractResult e1_result = optimise::extract(g, flat_model(), e1_options);
  ASSERT_TRUE(e1_result.found);
  ASSERT_FALSE(e1_result.history.empty());
  EXPECT_EQ(e1_result.history.back(), cheap_e1.name());

  // The guard actually matters: unrestricted extraction finds something STRICTLY cheaper than the
  // E0-restricted answer (otherwise this test would pass even with a broken/absent exactness
  // filter).
  EXPECT_LT(e1_result.estimated_ns, e0_result.estimated_ns);
}

TEST(Extract, ExtractionIsDeterministicAcrossRepeatedCallsAndIndependentGraphs) {
  const int n = static_cast<int>(optimise::CostCoefficients::defaults().l1_bytes / 8) * 4;
  const ExplicitMaterializeRule rule;

  optimise::EGraph g1(reduction_program(n));
  g1.saturate({&rule});
  const optimise::ExtractResult r1a = optimise::extract(g1, flat_model());
  const optimise::ExtractResult r1b = optimise::extract(g1, flat_model());
  EXPECT_EQ(r1a.program_id, r1b.program_id);
  EXPECT_EQ(r1a.plan_id, r1b.plan_id);
  EXPECT_DOUBLE_EQ(r1a.estimated_ns, r1b.estimated_ns);
  EXPECT_EQ(r1a.history, r1b.history);

  optimise::EGraph g2(reduction_program(n));
  g2.saturate({&rule});
  const optimise::ExtractResult r2 = optimise::extract(g2, flat_model());
  EXPECT_EQ(r1a.program_id, r2.program_id);
  EXPECT_EQ(r1a.plan_id, r2.plan_id);
  EXPECT_DOUBLE_EQ(r1a.estimated_ns, r2.estimated_ns);
  EXPECT_EQ(r1a.history, r2.history);
}
