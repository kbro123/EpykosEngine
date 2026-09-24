// M4/EG "core" integration gate: saturate a REAL e-graph over the M1 book with the REAL M4/R0
// planner rules (rewrite/planner_rules.hpp) mixed with two of the still-empty R1-R7 stubs (to
// demonstrate PROBLEM.md §7's own "design so adding a rule needs no e-graph changes" concretely,
// not just in a comment), extract the cheapest E0 plan under the CM cost model, and check
// PROBLEM.md §7's own gate in miniature: "every extracted program passes §6 at its declared
// class" (here: rewrite::verify_annotations, the same M2 differential harness M4/R0 itself uses)
// and the extraction is at least as good as one specific fixed configuration (M1's own default
// plan) it had in its search space the whole time.
#include <gtest/gtest.h>

#include <vector>

#include "epykos/exec/interpreter.hpp"
#include "epykos/fixtures/m1_book.hpp"
#include "epykos/fixtures/record_m1.hpp"
#include "epykos/ir/program.hpp"
#include "epykos/ir/signature.hpp"
#include "epykos/optimise/cost.hpp"
#include "epykos/optimise/egraph.hpp"
#include "epykos/optimise/extract.hpp"
#include "epykos/optimise/plan_bridge.hpp"
#include "epykos/rewrite/planner_rules.hpp"
#include "epykos/rewrite/r1_fold_uniform_columns.hpp"
#include "epykos/rewrite/r3_elide_trivial_maps.hpp"
#include "epykos/rewrite/rule.hpp"
#include "epykos/rewrite/verifier.hpp"
#include "epykos/tape/tape.hpp"

namespace ir = epykos::ir;
namespace rewrite = epykos::rewrite;
namespace optimise = epykos::optimise;
namespace fixtures = epykos::fixtures;

namespace {

const ir::Program& m1_program() {
  static const ir::Program program = [] {
    const fixtures::Book book = fixtures::make_m1_book();
    const epykos::Tape tape = fixtures::record_m1(book);
    return ir::infer(tape);
  }();
  return program;
}

}  // namespace

TEST(EGraphM1, SaturatesWithRealRulesAndUnrelatedStubsMixedIn) {
  const ir::Program& program = m1_program();
  optimise::EGraph graph(program);

  const rewrite::planner::DefaultPlanner planner(rewrite::planner::DefaultPlanOptions{});
  rewrite::R1FoldUniformColumns r1;   // still an identity stub (R-a's job) -- match() always empty
  rewrite::R3ElideTrivialMaps r3;     // ditto
  std::vector<const rewrite::Rule*> rules = planner.rules();
  rules.push_back(&r1);
  rules.push_back(&r3);

  const optimise::SaturationReport report = graph.saturate(rules, optimise::SaturationLimits{/*max_iterations=*/8});
  EXPECT_FALSE(report.bound_hit) << report.bound_reason;
  // The stubs never matched (by construction): they contributed nothing, and nothing needed to
  // know that in advance -- PROBLEM.md §7's own "adding a rule needs no e-graph changes".
  for (const optimise::SaturationLogEntry& entry : report.log) {
    if (entry.rule == r1.name() || entry.rule == r3.name()) EXPECT_EQ(entry.sites_matched, 0u);
  }
  EXPECT_GT(graph.num_plan_nodes(0), 1) << "the real planner rules should have proposed at least one alternative";
  EXPECT_TRUE(graph.congruent());
}

TEST(EGraphM1, ExtractedE0PlanPassesVerificationAndBeatsTheDefaultPlan) {
  const ir::Program& program = m1_program();
  optimise::EGraph graph(program);

  const rewrite::planner::DefaultPlanner planner(rewrite::planner::DefaultPlanOptions{});
  const optimise::SaturationReport report = graph.saturate(planner.rules(), optimise::SaturationLimits{/*max_iterations=*/8});
  ASSERT_FALSE(report.bound_hit) << report.bound_reason;

  const optimise::CostModel model{optimise::CostCoefficients::defaults(), "test", false};
  optimise::ExtractOptions options;
  options.max_exactness = rewrite::Exactness::E0;
  options.B = 1;
  options.tile = 256;
  options.lane_tile = 8;
  const optimise::ExtractResult result = optimise::extract(graph, model, options);
  ASSERT_TRUE(result.found);

  // §6 at E0: the extracted plan must be bit-identical to the unannotated program on the M1 book
  // and its differential ball (rewrite::verify_annotations reuses the M2 harness M4/R0 itself
  // gates its own rules with).
  std::vector<double> state(fixtures::record_state.begin(), fixtures::record_state.end());
  const int n_inputs = fixtures::n_knots;
  const int n_outputs = static_cast<int>(program.outputs.size());
  const rewrite::VerifyReport verify = rewrite::verify_annotations(program, result.plan, state.data(), n_inputs, n_outputs);
  EXPECT_TRUE(verify.passed()) << verify.summary();

  // The default (all-five-rules-in-fixed-order, lane_tile 8) plan was one specific point in the
  // e-graph's own search space the whole time; the global extraction minimum can never be worse.
  const ir::PlanAnnotations default_plan = rewrite::planner::default_plan(program, rewrite::planner::DefaultPlanOptions{});
  const optimise::Plan default_bridged = optimise::plan_from_annotations(program, default_plan, options.tile, options.lane_tile);
  const optimise::ProgramCost default_cost = optimise::estimate_program(program, default_bridged, options.B, model);
  EXPECT_LE(result.estimated_ns, default_cost.total_ns + 1e-6);
}
