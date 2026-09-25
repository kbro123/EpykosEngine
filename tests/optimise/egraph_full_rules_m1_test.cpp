// M4/EG "integration" — experiment (1) REDISCOVERY (PROBLEM.md §7 / RESUME.md §3's EG row: "with
// the planner's hard-coded rules off, e-graph with all rules ... rediscovers M1's three kill-path
// fusions unaided"). egraph_m1_extract_test.cpp (M4/EG "core") proved the machinery with the five
// planner rules plus two still-empty R1/R3 stubs; R-a/R-b/R-c have since landed R1-R7 and
// fma_contraction for real (D49-D52). This file re-runs the SAME shape with the FULL rule set —
// R1..R7, fma, and the five planner rules, all in ONE e-graph, nothing named by the planner's own
// fixed pipeline order — and checks PROBLEM.md §7's own gate in miniature: the extracted E0 plan
// passes §6 (rewrite::verify_annotations, the M2 differential harness) and its estimated cost is
// no worse than the M1 default plan's (a specific point the search space contained the whole
// time). The measured wall-clock comparison (B=1 / B=64, the "1.02x" target, and the plan diff)
// is bench/optimise/egraph_full_rules_m1_bench.cpp (informational, D9: never ctest/CI-gated).
#include <gtest/gtest.h>

#include <iostream>
#include <memory>
#include <string>
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
#include "epykos/rewrite/fma_contraction.hpp"
#include "epykos/rewrite/planner_rules.hpp"
#include "epykos/rewrite/r1_fold_uniform_columns.hpp"
#include "epykos/rewrite/r2_bucket_rows.hpp"
#include "epykos/rewrite/r3_elide_trivial_maps.hpp"
#include "epykos/rewrite/r4a_push_unary_through_gathers.hpp"
#include "epykos/rewrite/r4b_shared_reciprocal.hpp"
#include "epykos/rewrite/r5_group_formation.hpp"
#include "epykos/rewrite/r6_materialise_boundaries.hpp"
#include "epykos/rewrite/r7_block_linmap.hpp"
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

// Every landed M4 rule (R1-R7, fma, the five planner rules) in one vector, owning the R1-R7/fma
// objects and the DefaultPlanner for the caller's lifetime -- egraph_m1_bench.cpp's own
// documented reason (DefaultPlanner::rules() returns pointers into itself: it must never move).
struct FullRuleSet {
  std::unique_ptr<rewrite::planner::DefaultPlanner> planner;
  rewrite::R1FoldUniformColumns r1;
  rewrite::R2BucketRows r2;
  rewrite::R3ElideTrivialMaps r3;
  rewrite::R4aPushUnaryThroughGathers r4a;
  rewrite::R4bSharedReciprocal r4b;
  rewrite::R5GroupFormation r5;
  rewrite::R6MaterialiseBoundaries r6;
  rewrite::R7BlockLinmap r7;
  rewrite::FmaContractionRule fma;
  std::vector<const rewrite::Rule*> rules;
};

std::unique_ptr<FullRuleSet> make_full_rule_set(int lane_tile) {
  auto s = std::make_unique<FullRuleSet>();
  s->planner = std::make_unique<rewrite::planner::DefaultPlanner>(
      rewrite::planner::DefaultPlanOptions{/*fuse_reductions=*/true, /*fuse_pairs=*/true, /*inline_producers=*/true, lane_tile});
  s->rules = {&s->r1, &s->r2, &s->r3, &s->r4a, &s->r4b, &s->r5, &s->r6, &s->r7, &s->fma};
  for (const rewrite::Rule* r : s->planner->rules()) s->rules.push_back(r);
  return s;
}

optimise::CostModel test_model() { return optimise::CostModel{optimise::CostCoefficients::defaults(), "test", false}; }

// D57: the names of the E1-classed rules in `rules` (fma / r4b today), for
// rewrite::verify_extraction's own `e1_rule_names` parameter -- built from each rule's own
// `exactness_class()`, never hard-coded (HARD RULE 9).
std::vector<std::string> e1_rule_names(const std::vector<const rewrite::Rule*>& rules) {
  std::vector<std::string> names;
  for (const rewrite::Rule* r : rules) {
    if (r->exactness_class() == rewrite::Exactness::E1) names.push_back(r->name());
  }
  return names;
}

}  // namespace

TEST(EGraphFullRulesM1, SaturatesWithEveryLandedM4RuleWithoutHittingTheBound) {
  const ir::Program& program = m1_program();
  const std::unique_ptr<FullRuleSet> rule_set = make_full_rule_set(/*lane_tile=*/8);

  optimise::EGraph graph(program);
  const optimise::SaturationReport report =
      graph.saturate(rule_set->rules, optimise::SaturationLimits{/*max_iterations=*/24, /*max_plan_nodes=*/200000, /*max_program_nodes=*/2000});
  EXPECT_FALSE(report.bound_hit) << report.bound_reason;
  EXPECT_TRUE(graph.congruent());
  std::size_t structural_hits = 0;
  for (const optimise::SaturationLogEntry& e : report.log) {
    if (e.rule == "r1.fold_uniform_columns" || e.rule == "r2.bucket_rows" || e.rule == "r3.elide_trivial_maps" ||
        e.rule.rfind("r4a.", 0) == 0 || e.rule.rfind("r4b.", 0) == 0 || e.rule.rfind("r5.", 0) == 0 ||
        e.rule.rfind("r6.", 0) == 0 || e.rule.rfind("r7.", 0) == 0 || e.rule.rfind("fma.", 0) == 0) {
      structural_hits += e.sites_matched;
    }
  }
  std::cout << "[ egraph  ] M1 book, full rule set: " << graph.num_programs() << " program node(s), "
            << report.iterations_run << " iterations, " << structural_hits << " structural rule site-matches total\n";
}

TEST(EGraphFullRulesM1, ExtractedE0PlanPassesVerificationAndIsNoWorseThanTheDefaultPlan) {
  const ir::Program& program = m1_program();
  const std::unique_ptr<FullRuleSet> rule_set = make_full_rule_set(/*lane_tile=*/8);

  optimise::EGraph graph(program);
  const optimise::SaturationReport report =
      graph.saturate(rule_set->rules, optimise::SaturationLimits{/*max_iterations=*/24, /*max_plan_nodes=*/200000, /*max_program_nodes=*/2000});
  ASSERT_FALSE(report.bound_hit) << report.bound_reason;

  const optimise::CostModel model = test_model();
  optimise::ExtractOptions options;
  options.max_exactness = rewrite::Exactness::E0;  // excludes fma / r4b (both E1) from this extraction
  options.B = 1;
  options.tile = 256;
  options.lane_tile = 8;
  const optimise::ExtractResult result = optimise::extract(graph, model, options);
  ASSERT_TRUE(result.found);
  std::cout << "[ extract ] program node " << result.program_id << " (" << result.program.domains.size() << " domains), plan node "
            << result.plan_id << ", estimated " << result.estimated_ns << " ns, history:";
  for (const std::string& h : result.history) std::cout << ' ' << h;
  std::cout << '\n';

  const fixtures::Book book = fixtures::make_m1_book();
  const int n_inputs = fixtures::n_knots;
  const int n_outputs = static_cast<int>(program.outputs.size());
  const rewrite::VerifyReport verify = rewrite::verify_annotations(result.program, result.plan, book.z0.data(), n_inputs, n_outputs);
  EXPECT_TRUE(verify.passed()) << verify.summary();

  // D57 (alongside, not instead of, verify_annotations above): compares the extracted PROGRAM
  // against the TRUE, unrewritten M1 program -- verify_annotations alone only ever compares
  // `result.program` against itself, so it cannot see whether the structural rewrite CHAIN that
  // produced it (R1-R7 + fma, composed by the e-graph, `result.history`) drifted from the
  // original tape. `options.max_exactness = E0` above means no E1 rule (fma / r4b) can appear in
  // `result.history`, so this is a bitwise check here -- the strongest one available.
  const rewrite::VerifyReport extraction_check =
      rewrite::verify_extraction(program, result.program, result.plan, result.history, e1_rule_names(rule_set->rules), book.z0.data(),
                                 n_inputs, n_outputs);
  EXPECT_TRUE(extraction_check.passed()) << "extracted program vs the true original (D57): " << extraction_check.summary();

  std::size_t extracted_paired_steps = 0, default_paired_steps = 0;
  for (const ir::GroupPlan& g : result.plan.group) extracted_paired_steps += g.pairings.size();
  const ir::PlanAnnotations default_plan = rewrite::planner::default_plan(program, rewrite::planner::DefaultPlanOptions{});
  for (const ir::GroupPlan& g : default_plan.group) default_paired_steps += g.pairings.size();
  std::cout << "[ diagnose ] extracted plan: group.size()=" << result.plan.group.size() << ", total pairings=" << extracted_paired_steps
            << "; default plan: group.size()=" << default_plan.group.size() << ", total pairings=" << default_paired_steps << '\n';
  const optimise::Plan default_bridged = optimise::plan_from_annotations(program, default_plan, options.tile, options.lane_tile);
  const optimise::ProgramCost default_cost = optimise::estimate_program(program, default_bridged, options.B, model);
  std::cout << "[ compare ] default (M1 greedy) plan estimated " << default_cost.total_ns << " ns; ratio "
            << (result.estimated_ns / default_cost.total_ns) << '\n';
  // PROBLEM.md §7's own target is "within 1.02x measured wall-clock" (the bench file); at the
  // ESTIMATE level (this gate; measured numbers are informational, D9) the global minimum over a
  // search space that always contained the default plan's own point can never exceed it.
  EXPECT_LE(result.estimated_ns, default_cost.total_ns + 1e-6);
}
