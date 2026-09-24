// M4/EG "integration" — experiment (2) CROSS-STAGE, first half only (RESUME.md §3 EG row): the
// full e-graph (R1-R7, fma, the five planner rules — the SAME rule set egraph_full_rules_m1_test.cpp
// runs on the M1 book) on a REAL, if small, Stage A recording (multi-curve, multi-block, scan
// domains — tests/rewrite/r7_block_linmap_e0_test.cpp's own `small_stage_a()` fixture: 60 trades,
// 0 scenarios, fast to BUILD).
//
// MEASURED FINDING as this file first landed (D54), reported rather than hidden (CLAUDE.md, HARD
// RULE 10): saturating this fixture to the SAME bound egraph_full_rules_m1_test.cpp uses for the
// M1 book (24 iterations, 2000 program nodes) did not merely run slowly, it grew unboundedly in
// practice -- round 2 alone (r1/r2/r5/r6/r7 all firing) already produced 39 distinct program nodes
// in ~4.4s; by round 4 the process was still growing (measured directly: 1.87 GB resident and
// climbing at 51s, killed rather than let it continue) with no sign of approaching a fixpoint.
// Root cause, as far as D54 traced it: `EGraph::saturate` matched EVERY rule against EVERY node
// discovered so far, every round (egraph.hpp's own documented BFS shape) -- fine when structural
// rules rarely fire (the M1 book: R1-R3 fire 0 times, D52) or fire once (R7's single linmap
// split), but on Stage A FIVE structural rules fire at once from round 1
// (r1.fold_uniform_columns, r2.bucket_rows, r5.group_formation, r6.materialise_boundaries,
// r7.block_linmap), each re-matching on the OTHERS' output next round, so the candidate set grew
// combinatorially rather than converging -- exactly the failure mode equality-saturation systems
// are known to hit without a redundancy/subsumption rule (D12: no external e-graph library).
//
// CLOSED BY D62, re-measured here rather than asserted: `EGraph::saturate` now memoises each
// (program node, rule, site) application (its two inputs are fixed for a node's whole life, so
// re-deriving it can only reproduce content the graph already holds), matches only a program
// CLASS's representative, and by default applies `RefirePolicy::NoFreshCrossRule`. On THIS fixture
// under this file's own 3-iteration bound that turns 191 program nodes into 35 and, importantly,
// `report.bound_hit` into false -- the 500-node cap below is now slack, not a tourniquet. Run to a
// real fixpoint (8 rounds, `RefirePolicy::PipelineOrderedPlans`) the same fixture settles at 39
// program nodes / 22 classes / 641 plan nodes and 483 MiB, and the FULL 2,000-trade, 517,036-node
// Stage A tape -- not attempted at all before D62 -- reaches a fixpoint in 7 rounds at 3.1 GB
// (tools/egraph_scale/, D62's own numbers, fingerprint d448afd70180). The bound below and the
// deliberate absence of an `EXPECT_FALSE(report.bound_hit)` are kept as they were: this file's job
// is the structural result, not the scaling result, and a bound being hit must stay a logged
// event rather than a failure whichever way the search is configured.
//
// SCOPED, HONESTLY (this file does not claim more than it checks, given the above): it does NOT
// wire rewrite::cross_stage_sharing_guard onto Stage A's own residual/book output-ordinal groups
// (that needs the exact O1/O2 output layout the G5/G6 gates' own test helpers derive, which this
// package did not have time to extract into a reusable, non-Stage-A-specific form), and it does
// NOT run rewrite::ADModePerBlockRule against Stage A's own ImplicitRegistry blocks
// (tests/rewrite/ad_mode_stage_a_shapes_test.cpp instead validates that rule's decision logic
// directly against Stage A's own already-measured real numbers, without needing the tape at all).
//
// What IS checked here, safely bounded, is a genuine positive result at the STRUCTURAL level: the
// full structural + planner rule set actually firing on a multi-domain financial recording beyond
// the M1 book (R1 fires here though it never does on the M1 book, D52's own predicted mechanism:
// R2's output gives R1 something to fold), and the E0 extraction at 3 rounds finds
// `r5.group_formation` applied TWICE in sequence (once onto its own prior output) plus
// `planner.reduction_fusion` -- a candidate a FIXED single-pass pipeline (which never re-applies
// R5 to what R5 itself just produced) structurally cannot express -- and passes PROBLEM.md §6
// bit-for-bit AT THE RECORD POINT (epykos::test::compare_at_record_point, not a perturbation
// ball: see this file's own comment at the call site for why a ball is the wrong check on this
// tape). CORRECTED BY D57 (a review finding on the M4-gate-1 landing, docs/DECISIONS.md D57):
// this file used to also claim that candidate was "0.9773x the default plan's cost, i.e. a real,
// measured-by-the-cost-model 2.3% win". That number came from `optimise::CostCoefficients::
// defaults()`, a synthetic, never-fitted, uniform-per-op cost model -- under this machine's real,
// fitted `bench/results/<fingerprint>/cost_model.json` (D48) the ratio is 0.999716, i.e. noise,
// and there is no wall-clock bench corroborating either number. See the second TEST below for the
// honest, both-models comparison; PROBLEM.md §7's cross-stage-win gate item is NOT considered
// satisfied by this experiment.
#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
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
namespace fs = std::filesystem;

namespace {

// scripts/fingerprint.sh --id, or "" if it cannot be run — the SAME popen pattern
// tests/optimise/cost_test.cpp and tests/scripts/perf_gate_test.cpp already use, repeated here
// rather than shared (neither of those is a header this file could include without pulling in
// their own test-only fixtures). D9: a cost-model ratio only means something for THIS machine's
// own fitted coefficients, never compared across fingerprints.
fs::path repo_root() {
  if (const char* e = std::getenv("EPYKOS_SOURCE_DIR")) return fs::path(e);
  return fs::path(__FILE__).parent_path().parent_path().parent_path();
}

std::string shell_quote(const std::string& s) {
  std::string r = "'";
  for (char c : s) {
    if (c == '\'') r += "'\\''";
    else r += c;
  }
  return r + "'";
}

std::string current_fingerprint_id() {
  const fs::path script = repo_root() / "scripts" / "fingerprint.sh";
  if (!fs::exists(script)) return "";
  const std::string cmd = shell_quote(script.string()) + " --id 2>/dev/null";
  FILE* p = popen(cmd.c_str(), "r");
  if (p == nullptr) return "";
  std::string out;
  char buf[256];
  while (std::size_t n = std::fread(buf, 1, sizeof buf, p)) out.append(buf, n);
  pclose(p);
  while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ')) out.pop_back();
  return out;
}

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

  // D57 (supersedes the cross-stage-win claim of D54/D56): this test used to cost both candidates
  // with `optimise::CostCoefficients::defaults()` -- a SYNTHETIC, uniform 1-ns-per-op fallback
  // that has never been fitted to any machine -- and reported the resulting ratio (0.9773) as "a
  // real, measured-by-the-cost-model 2.3% win". It is not: every op costs the SAME under
  // `defaults()`, so the ratio it produces reflects only which candidate has fewer *steps*, never
  // this machine's actual per-op timings, and this experiment has no wall-clock companion bench
  // to corroborate it either way (contrast `bench/optimise/egraph_full_rules_m1_bench.cpp` for
  // experiment (1), D54 point 4). Loading the real, fingerprint-fitted model this project is
  // gated against everywhere else (`bench/results/<fingerprint>/cost_model.json`, D48) is the
  // honest comparison: under it the "win" collapses to noise (measured while fixing this finding,
  // on fingerprint d448afd70180: ratio 0.999716, i.e. ~0.03%, not 2.3%). `load_or_default` (not
  // `load`) is used deliberately: this test also runs on CI hosts (a different fingerprint, no
  // committed cost_model.json there) and must not fail merely because no fitted model exists for
  // the CURRENT machine (D9's own cross-fingerprint discipline says nothing here is comparable
  // across machines anyway) -- `loaded_from_file` and both ratios are printed either way so a
  // human reading this test's own stdout on ANY host sees which model actually priced the
  // candidates, never silently. Only the tautological "no worse than the model's own default
  // candidate" check is asserted (`EXPECT_LE` below: the e-graph's search space always contains
  // the default plan, so this can never be a real gate against a defaults-shaped cost model,
  // fitted or not) -- this test no longer asserts, or lets a reader infer, that the cost model
  // found "a win"; that claim now requires a real wall-clock bench, not yet written for Stage A
  // (D57). PROBLEM.md §7's cross-stage-win gate item is therefore NOT considered satisfied by
  // this experiment alone.
  const std::string fingerprint_id = current_fingerprint_id();
  const optimise::CostModel synthetic_model{optimise::CostCoefficients::defaults(), "test", false};
  const optimise::CostModel fitted_model =
      fingerprint_id.empty() ? synthetic_model : optimise::CostModel::load_or_default(fingerprint_id, "bench/results", &std::cerr);
  std::cout << "[ cost model ] fingerprint '" << fingerprint_id << "', fitted coefficients "
            << (fitted_model.loaded_from_file ? "LOADED" : "NOT FOUND (using synthetic defaults -- see this test's own header comment)") << '\n';
  const optimise::CostModel& model = fitted_model;
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

  // D57 (alongside, not instead of, the check above): the check above only ever compares
  // `result.program` against ITSELF (plan cleared vs the extracted plan) -- it is structurally
  // blind to whether the e-graph's OWN structural rewrite chain (`result.history`: this fixture's
  // own extraction finds `r5.group_formation` applied TWICE in sequence, this file's header) drifted
  // from the TRUE, unrewritten 60-trade Stage A tape (`program`, this function's own first local,
  // never mutated). `options.max_exactness = E0` means no E1 rule can be in `result.history`, so
  // this is a bitwise check, same record-point reasoning as above (a ball is still the wrong check
  // on an implicit-node tape's quotes).
  const epykos::test::RecordPointReport extraction_check = epykos::test::compare_at_record_point(
      program, annotated, program.input_values.data(), static_cast<int>(program.input_values.size()),
      static_cast<int>(program.outputs.size()));
  EXPECT_TRUE(extraction_check.passed) << "extracted program vs the true original Stage A tape (D57): " << extraction_check.detail;

  const ir::PlanAnnotations default_plan = rewrite::planner::default_plan(program, rewrite::planner::DefaultPlanOptions{});
  const optimise::Plan default_bridged = optimise::plan_from_annotations(program, default_plan, options.tile, options.lane_tile);
  const optimise::ProgramCost default_cost_fitted = optimise::estimate_program(program, default_bridged, options.B, fitted_model);
  const optimise::ProgramCost default_cost_synthetic = optimise::estimate_program(program, default_bridged, options.B, synthetic_model);
  const optimise::ExtractResult result_synthetic = optimise::extract(graph, synthetic_model, options);
  ASSERT_TRUE(result_synthetic.found);
  std::cout << "[ compare ] stage_a: default (greedy R1..R7-then-planner order not run here; M1-shaped 5-rule planner"
            << " default) plan estimated " << default_cost_fitted.total_ns << " ns fitted / " << default_cost_synthetic.total_ns
            << " ns synthetic; ratio " << (result.estimated_ns / default_cost_fitted.total_ns) << " fitted / "
            << (result_synthetic.estimated_ns / default_cost_synthetic.total_ns)
            << " synthetic -- ONLY the fitted ratio (when a fitted model was found for this host) is anything close to a real number;"
               " the synthetic one is what D54/D56 mistakenly reported as a 2.3% win (D57)\n";
  // Tautological by construction (extract.hpp's own header: the search space always contains the
  // default candidate), kept as a smoke check that extraction never regresses vs. the default
  // under WHICHEVER model priced it -- not evidence of a win, under either model.
  EXPECT_LE(result.estimated_ns, default_cost_fitted.total_ns + 1e-6);
}
