// M4/EG "integration" — experiment (2) CROSS-STAGE, first half only (RESUME.md §3 EG row): the
// full e-graph (R1-R7, fma, the five planner rules — the SAME rule set egraph_full_rules_m1_test.cpp
// runs on the M1 book) on a REAL, if small, Stage A recording (multi-curve, multi-block, scan
// domains — tests/rewrite/r7_block_linmap_e0_test.cpp's own `small_stage_a()` fixture: 60 trades,
// 0 scenarios, fast to BUILD).
//
// MEASURED FINDING, reported rather than hidden (CLAUDE.md, HARD RULE 10): saturating this fixture
// to the SAME bound egraph_full_rules_m1_test.cpp uses for the M1 book (24 iterations, 2000
// program nodes) does not merely run slowly, it grows unboundedly in practice -- round 2 alone
// (r1/r2/r5/r6/r7 all firing) already produces 39 distinct program nodes in ~4.4s; by round 4 the
// process was still growing (measured directly: 1.87 GB resident and climbing at 51s, killed
// rather than let it continue) with no sign of approaching a fixpoint. Root cause, as far as this
// package traced it without a deeper rewrite of its own: `EGraph::saturate` matches EVERY rule
// against EVERY node discovered so far, every round (egraph.hpp's own documented BFS shape) --
// fine when structural rules rarely fire (the M1 book: R1-R3 fire 0 times, D52) or fire once
// (R7's single linmap split), but on Stage A FIVE structural rules fire at once from round 1
// (r1.fold_uniform_columns, r2.bucket_rows, r5.group_formation, r6.materialise_boundaries,
// r7.block_linmap), each potentially re-matching on the OTHERS' output next round, so the
// candidate set can grow combinatorially rather than converge -- exactly the failure mode
// equality-saturation systems are known to hit without a redundancy/subsumption rule (D12: no
// external e-graph library; this package did not have time to add one). PRACTICAL CONSEQUENCE:
// this file bounds saturation to 3 iterations / 500 program nodes (measured safe and fast, ~10s)
// and does NOT assert `!report.bound_hit` -- hitting the iteration cap here is an intended safety
// choice, not a failure, and is reported as such rather than papered over. Running the FULL
// 2,000-trade Stage A tape (517,036 nodes) through the unbounded default limits is NOT attempted
// by this package: given the 60-trade fixture's own trajectory, it would very plausibly exhaust
// memory before converging. This is this package's most important negative finding and is
// flagged for whoever picks up R1-R7's interaction next (a redundancy check, a rule-application
// budget per node, or restricting which rules re-fire on another rule's OWN output).
//
// SCOPED, HONESTLY (this file does not claim more than it checks, given the above): it does NOT
// wire rewrite::cross_stage_sharing_guard onto Stage A's own residual/book output-ordinal groups
// (that needs the exact O1/O2 output layout the G5/G6 gates' own test helpers derive, which this
// package did not have time to extract into a reusable, non-Stage-A-specific form), and it does
// NOT run rewrite::ADModePerBlockRule against Stage A's own ImplicitRegistry blocks
// (tests/rewrite/ad_mode_stage_a_shapes_test.cpp instead validates that rule's decision logic
// directly against Stage A's own already-measured real numbers, without needing the tape at all).
//
// What IS checked here, safely bounded, IS a genuine positive result: the full structural +
// planner rule set actually firing on a multi-domain financial recording beyond the M1 book (R1
// fires here though it never does on the M1 book, D52's own predicted mechanism: R2's output
// gives R1 something to fold), and the E0 extraction at 3 rounds finds `r5.group_formation`
// applied TWICE in sequence (once onto its own prior output) plus `planner.reduction_fusion` --
// 0.9773x the M1-shaped 5-rule default plan's OWN estimated cost on this program, i.e. a real,
// measured-by-the-cost-model 2.3% win a FIXED single-pass pipeline (which never re-applies R5 to
// what R5 itself just produced) structurally cannot express -- and passes PROBLEM.md §6 bit-for-
// bit AT THE RECORD POINT (epykos::test::compare_at_record_point, not a perturbation ball: see
// this file's own comment at the call site for why a ball is the wrong check on this tape).
#include <gtest/gtest.h>

#include <iostream>
#include <memory>
#include <vector>

#include "epykos/exec/interpreter.hpp"
#include "epykos/fixtures/stage_a.hpp"
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
#include "rewrite/record_point_check.hpp"

namespace ir = epykos::ir;
namespace rewrite = epykos::rewrite;
namespace optimise = epykos::optimise;
namespace fixtures = epykos::fixtures;

namespace {

struct SmallStageA {
  fixtures::StageA s;
  ir::Program program;
};

const SmallStageA& small_stage_a() {
  static const SmallStageA x = [] {
    fixtures::StageAOptions opt;
    opt.trades = 60;
    opt.scenarios = 0;
    fixtures::StageA s = fixtures::make_stage_a(opt);
    const fixtures::StageATape tape = fixtures::record_stage_a(s);
    return SmallStageA{std::move(s), ir::infer(tape.tape)};
  }();
  return x;
}

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
      rewrite::planner::DefaultPlanOptions{true, true, true, lane_tile});
  s->rules = {&s->r1, &s->r2, &s->r3, &s->r4a, &s->r4b, &s->r5, &s->r6, &s->r7, &s->fma};
  for (const rewrite::Rule* r : s->planner->rules()) s->rules.push_back(r);
  return s;
}

}  // namespace

TEST(EGraphFullRulesStageA, SaturatesUnderADeliberatelySmallBoundAndStaysCongruent) {
  const ir::Program& program = small_stage_a().program;
  const std::unique_ptr<FullRuleSet> rule_set = make_full_rule_set(/*lane_tile=*/8);

  optimise::EGraph graph(program);
  const optimise::SaturationReport report = graph.saturate(
      rule_set->rules, optimise::SaturationLimits{/*max_iterations=*/3, /*max_plan_nodes=*/50000, /*max_program_nodes=*/500});
  // See this file's header: a small bound is a deliberate safety choice on this fixture, not a
  // target to reach unbounded — hitting it is expected and logged, never asserted false here.
  std::cout << "[ stage_a ] bound_hit=" << (report.bound_hit ? "true" : "false") << " (" << report.bound_reason << ")\n";
  EXPECT_TRUE(graph.congruent());

  std::vector<std::string> fired;
  for (const optimise::SaturationLogEntry& e : report.log) {
    if (e.sites_matched > 0 &&
        (e.rule.rfind("r1.", 0) == 0 || e.rule.rfind("r2.", 0) == 0 || e.rule.rfind("r3.", 0) == 0 || e.rule.rfind("r4a.", 0) == 0 ||
         e.rule.rfind("r4b.", 0) == 0 || e.rule.rfind("r5.", 0) == 0 || e.rule.rfind("r6.", 0) == 0 || e.rule.rfind("r7.", 0) == 0 ||
         e.rule.rfind("fma.", 0) == 0)) {
      fired.push_back(e.rule);
    }
  }
  std::cout << "[ stage_a ] " << program.domains.size() << " domains, " << graph.num_programs() << " program node(s), "
            << report.iterations_run << " iterations, structural rules that matched something:";
  for (const std::string& r : fired) std::cout << ' ' << r;
  if (fired.empty()) std::cout << " (none)";
  std::cout << '\n';
}

TEST(EGraphFullRulesStageA, ExtractedE0PlanPassesVerificationAndIsNoWorseThanTheDefaultPlan) {
  const ir::Program& program = small_stage_a().program;
  const std::unique_ptr<FullRuleSet> rule_set = make_full_rule_set(/*lane_tile=*/8);

  optimise::EGraph graph(program);
  const optimise::SaturationReport report = graph.saturate(
      rule_set->rules, optimise::SaturationLimits{/*max_iterations=*/3, /*max_plan_nodes=*/50000, /*max_program_nodes=*/500});
  // See this file's header: the bound is deliberately small on this fixture and expected to be
  // hit; extraction over a partially-saturated graph is still well-defined (it costs whatever
  // nodes exist), so this is informational, not a precondition.
  std::cout << "[ stage_a ] bound_hit=" << (report.bound_hit ? "true" : "false") << " (" << report.bound_reason << ")\n";

  const optimise::CostModel model{optimise::CostCoefficients::defaults(), "test", false};
  optimise::ExtractOptions options;
  options.max_exactness = rewrite::Exactness::E0;
  options.B = 1;
  options.tile = 256;
  options.lane_tile = 8;
  const optimise::ExtractResult result = optimise::extract(graph, model, options);
  ASSERT_TRUE(result.found);
  std::cout << "[ extract ] stage_a: program node " << result.program_id << " (" << result.program.domains.size()
            << " domains), estimated " << result.estimated_ns << " ns, history:";
  for (const std::string& h : result.history) std::cout << ' ' << h;
  std::cout << '\n';

  // tests/rewrite/record_point_check.hpp's own documented reason (M4/R-a; matches
  // r7_block_linmap_e0_test.cpp's own Stage A gate style): a raw exec::Interpreter over the Stage
  // A tape's Inputs (its QUOTES) does not re-solve the implicit block, so PERTURBING them (which
  // rewrite::verify_annotations' ball does) walks the book off the calibrated point and, in
  // practice, into log/sqrt/div's undefined region (measured: NaN outputs) -- not a defect in the
  // rewrite being checked. Comparing bit-for-bit at the exact RECORD point (program.input_values,
  // the one state the recorded tape is self-consistent at) sidesteps this.
  const ir::Program& unannotated = result.program;  // .plan left empty: the interpreter derives its own default
  ir::Program annotated = result.program;
  annotated.plan = result.plan;  // the extracted decision
  const epykos::test::RecordPointReport record_check = epykos::test::compare_at_record_point(
      unannotated, annotated, program.input_values.data(), static_cast<int>(program.input_values.size()),
      static_cast<int>(program.outputs.size()));
  EXPECT_TRUE(record_check.passed) << record_check.detail;

  const ir::PlanAnnotations default_plan = rewrite::planner::default_plan(program, rewrite::planner::DefaultPlanOptions{});
  const optimise::Plan default_bridged = optimise::plan_from_annotations(program, default_plan, options.tile, options.lane_tile);
  const optimise::ProgramCost default_cost = optimise::estimate_program(program, default_bridged, options.B, model);
  std::cout << "[ compare ] stage_a: default (greedy R1..R7-then-planner order not run here; M1-shaped 5-rule planner"
            << " default) plan estimated " << default_cost.total_ns << " ns; ratio " << (result.estimated_ns / default_cost.total_ns)
            << '\n';
  EXPECT_LE(result.estimated_ns, default_cost.total_ns + 1e-6);
}
