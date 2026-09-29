// M4/EG "integration" — experiment (4) E1 EXTRACTION (RESUME.md §3 EG row): the M1 book, every
// landed M4 rule (R1-R7, fma, the five planner rules) in one e-graph, extracted at E1 so
// fma_contraction and R4bSharedReciprocal (the only two E1-classed rules landed so far, D51) are
// eligible — informational vs bench/hand's "v0" reference kernel (D9, D27: libm std::exp on both
// sides is the E0 comparison; this file's own exec::Options::exp = poly (exp_poly, E1) is a
// SEPARATE axis cost.hpp's own header states it cannot price, so the comparison here is against
// the ALREADY-COMMITTED bench/results/d448afd70180/hand_m1_hand.json numbers, read and printed,
// never re-derived or asserted against — a measured number from a DIFFERENT bench binary is
// reported, not gated, exactly D9's own discipline for a cross-binary comparison).
#include <gtest/gtest.h>

#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
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
#include "epykos/verify/differential.hpp"

namespace ir = epykos::ir;
namespace rewrite = epykos::rewrite;
namespace optimise = epykos::optimise;
namespace fixtures = epykos::fixtures;
namespace exec = epykos::exec;
namespace verify = epykos::verify;

namespace {

const ir::Program& m1_program() {
  static const ir::Program program = [] {
    const fixtures::Book book = fixtures::make_m1_book();
    const epykos::Tape tape = fixtures::record_m1(book);
    return ir::infer(tape);
  }();
  return program;
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

// bench/results/<fp>/hand_m1_hand.json's own "benchmarks" object: BM_HandEval/0 (B=1) and
// BM_HandEvalBatch/0 (B=64) medians, index 0 == D27's exp mode variant 0 (v0). A tiny ad hoc
// scan (not a JSON parser dependency, D12): informational only, never asserted against.
double find_median_us(const std::string& path, const std::string& key) {
  std::ifstream in(path);
  if (!in) return -1.0;
  std::stringstream buf;
  buf << in.rdbuf();
  const std::string text = buf.str();
  const std::size_t k = text.find("\"" + key + "\"");
  if (k == std::string::npos) return -1.0;
  const std::size_t m = text.find("\"median\"", k);
  if (m == std::string::npos) return -1.0;
  const std::size_t colon = text.find(':', m);
  return std::atof(text.c_str() + colon + 1);
}

}  // namespace

TEST(EGraphE1ExtractM1, ExtractedE1PlanWithExpPolyPassesVerificationAtE1Tolerance) {
  const ir::Program& program = m1_program();
  const std::unique_ptr<FullRuleSet> rule_set = make_full_rule_set(/*lane_tile=*/8);

  optimise::EGraph graph(program);
  const optimise::SaturationReport report =
      graph.saturate(rule_set->rules, optimise::SaturationLimits{/*max_iterations=*/24, /*max_plan_nodes=*/200000, /*max_program_nodes=*/2000});
  ASSERT_FALSE(report.bound_hit) << report.bound_reason;

  const optimise::CostModel model{optimise::CostCoefficients::defaults(), "test", false};
  optimise::ExtractOptions options;
  options.max_exactness = rewrite::Exactness::E1;  // fma / r4b now eligible
  options.B = 1;
  options.tile = 256;
  options.lane_tile = 8;
  const optimise::ExtractResult result = optimise::extract(graph, model, options);
  ASSERT_TRUE(result.found);
  std::cout << "[ extract ] E1: program node " << result.program_id << " (" << result.program.domains.size() << " domains), history:";
  for (const std::string& h : result.history) std::cout << ' ' << h;
  std::cout << " -- estimated " << result.estimated_ns << " ns\n";

  const fixtures::Book book = fixtures::make_m1_book();
  rewrite::VerifyOptions vopts;
  vopts.interpreter.exp = exec::ExpMode::poly;
  vopts.adjoint_ball_draws = 4;
  vopts.interpreter_tolerance = verify::Tolerance::e1();
  vopts.adjoint_tolerance = verify::Tolerance::e1();
  const rewrite::VerifyReport verify_report =
      rewrite::verify_annotations(result.program, result.plan, book.z0.data(), fixtures::n_knots, static_cast<int>(program.outputs.size()), vopts);
  EXPECT_TRUE(verify_report.passed()) << verify_report.summary();

  // D57 (alongside, not instead of, verify_annotations above): verify_annotations only ever
  // compares `result.program` against ITSELF, so it is structurally blind to whether the e-graph's
  // OWN structural rewrite chain (`result.history`, which CAN include fma_contraction / R4b here --
  // options.max_exactness = E1 makes both eligible, unlike the E0 sibling test in this same rule
  // set) drifted from the true M1 program. D51/D54 measured 0/0 fires for both on this fixture, so
  // this is currently a bitwise-equivalent check in practice, but it is the composed-history check
  // itself that matters going forward, not today's particular history being empty.
  const rewrite::VerifyReport extraction_check =
      rewrite::verify_extraction(program, result.program, result.plan, result.history, e1_rule_names(rule_set->rules), book.z0.data(),
                                 fixtures::n_knots, static_cast<int>(program.outputs.size()), vopts);
  EXPECT_TRUE(extraction_check.passed()) << "extracted program vs the true original (D57): " << extraction_check.summary();

  // Informational only (D9): the already-committed hand-kernel v0 numbers, for a human reading
  // this test's own stdout to compare against the companion bench file's freshly measured ones.
  const std::string path = "bench/results/d448afd70180/hand_m1_hand.json";
  const double hand_b1_us = find_median_us(path, "BM_HandEval/0");
  const double hand_b64_us = find_median_us(path, "BM_HandEvalBatch/0");
  std::cout << "[ compare ] bench/hand v0 (already committed, informational): B=1 " << hand_b1_us << " us, B=64 " << hand_b64_us
            << " us -- see bench/optimise/egraph_e1_extract_m1_bench.cpp for this package's own freshly measured E1-extracted numbers\n";
}
