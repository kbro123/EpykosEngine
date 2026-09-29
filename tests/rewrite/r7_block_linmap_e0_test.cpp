// M4/R-c gate: R7BlockLinmap (rewrite/r7_block_linmap.hpp) — the differential test CLAUDE.md/D8
// requires for every rewrite, run on BOTH the M1 book and the Stage A tape. E0: splitting a
// linmap domain into column-span blocks is per-row order unchanged bookkeeping, never a change
// to any arithmetic (rewrite::verify_rule's interpreter + adjoint comparison is the proof).
//
// Measured finding (development note, not a hard-coded expectation: HARD RULE 9): R7 fires on
// BOTH fixtures, though for different structural reasons. The M1 book's own one-curve "Knots
// --linmap--> Times" domain (D22) still splits into 2 blocks -- one row (t = 0, D22: "three rows
// read a knot directly") whose Affine reads a narrower / disjoint knot span from the other 2,560
// rows' advancing one. The Stage A tape's four curves share ONE Input domain
// (ir/program.hpp: "the Input domain"), so its own linmap domain reads four (plus the same kind
// of single-row outlier) non-overlapping column spans and splits further (measured on a small
// Stage A tape during development: 5 blocks of [7456, 7668, 48, 3, 117] rows). Both are asserted
// as "R7 matches, and the split it proposes passes the differential/adjoint check" -- never a
// magic block count.
//
// scripts/mutation_test.sh selects this file as a gate by its `_e0_test` suffix:
// r7.no_offset_rebase / r7.wrong_block_value_base (both guarded in
// src/rewrite/r7_block_linmap.cpp) corrupt every block after the first the moment ANY multi-block
// split runs, which the Stage A test below always exercises.
#include <gtest/gtest.h>

#include <iostream>

#include "epykos/exec/interpreter.hpp"
#include "epykos/fixtures/m1_book.hpp"
#include "epykos/fixtures/record_m1.hpp"
#include "epykos/fixtures/stage_a.hpp"
#include "epykos/ir/program.hpp"
#include "epykos/ir/signature.hpp"
#include "epykos/rewrite/r7_block_linmap.hpp"
#include "epykos/rewrite/verifier.hpp"
#include "epykos/tape/tape.hpp"

namespace ir = epykos::ir;
namespace rewrite = epykos::rewrite;
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

}  // namespace

TEST(R7BlockLinmap, RealRulePassesTheDifferentialCheckOnM1Book) {
  const ir::Program& program = m1_program();
  const rewrite::R7BlockLinmap rule;
  const std::vector<rewrite::MatchSite> sites = rule.match(program, ir::PlanAnnotations{});
  ASSERT_FALSE(sites.empty()) << "R7 found nothing to block on the M1 book -- see this file's header for the finding this used to be (a 1-row outlier block) before revising this assertion";
  const std::vector<rewrite::LinmapBlock> blocks = rewrite::linmap_blocks(program, sites.front().domain);
  std::cout << "[ r7 ] M1 book: domain " << sites.front().domain << " splits into " << blocks.size() << " block(s): [";
  for (const rewrite::LinmapBlock& b : blocks) std::cout << (b.hi - b.lo) << ' ';
  std::cout << "]\n";
  const fixtures::Book book = fixtures::make_m1_book();
  const rewrite::RuleVerifyReport report = rewrite::verify_rule(
      rule, program, book.z0.data(), static_cast<int>(book.z0.size()), static_cast<int>(program.outputs.size()));
  EXPECT_GT(report.sites_checked, 0);
  EXPECT_TRUE(report.passed()) << report.report.summary();
}

TEST(R7BlockLinmap, RealRulePassesTheDifferentialCheckOnStageA) {
  const ir::Program& program = small_stage_a().program;
  const rewrite::R7BlockLinmap rule;
  const std::vector<rewrite::MatchSite> sites = rule.match(program, ir::PlanAnnotations{});
  ASSERT_GT(sites.size(), 0u) << "R7 found no linmap to block on the (small) Stage A tape -- adjust the fixture, not the rule";
  const std::vector<rewrite::LinmapBlock> blocks = rewrite::linmap_blocks(program, sites.front().domain);
  ASSERT_GE(blocks.size(), 2u);
  std::cout << "[ r7 ] small Stage A: domain " << sites.front().domain << " splits into " << blocks.size() << " block(s): [";
  for (const rewrite::LinmapBlock& b : blocks) std::cout << (b.hi - b.lo) << ' ';
  std::cout << "]\n";

  rewrite::VerifyOptions options;
  options.ball.draws = 3;
  const rewrite::RuleVerifyReport report = rewrite::verify_rule(rule, program, program.input_values.data(),
                                                                static_cast<int>(program.inputs.size()),
                                                                static_cast<int>(program.outputs.size()), {}, options);
  EXPECT_TRUE(report.passed()) << report.report.summary();
  EXPECT_GT(report.sites_checked, 0);
}

// Splitting must be idempotent to repeat application (rewrite/greedy.hpp: a structural rule
// applies one site per apply_greedy call, so a caller wanting every block split calls it
// repeatedly) and must eventually reach a fixed point where match() is empty again -- every
// resulting block really is one column span, not a domain R7 would immediately want to re-split.
TEST(R7BlockLinmap, RepeatedApplicationReachesAFixedPointAndKeepsValidating) {
  ir::Program program = small_stage_a().program;
  const rewrite::R7BlockLinmap rule;
  int rounds = 0;
  for (; rounds < 16; ++rounds) {
    const std::vector<rewrite::MatchSite> sites = rule.match(program, ir::PlanAnnotations{});
    if (sites.empty()) break;
    const rewrite::Proposal p = rule.propose(program, ir::PlanAnnotations{}, sites.front());
    ASSERT_TRUE(p.is_structural());
    program = *p.program;
    ASSERT_NO_THROW(ir::validate(program));
  }
  EXPECT_LT(rounds, 16) << "R7 did not reach a fixed point in 16 rounds";
  EXPECT_TRUE(rule.match(program, ir::PlanAnnotations{}).empty());
}

// PROBLEM.md §7 / this package's brief: "a rule that never fires is a finding to report, not to
// hide." Reports, on the full Stage A tape, how many blocks R7 exposes on its linmap domain(s).
TEST(R7BlockLinmap, FiresOnTheFullStageATapeCountsReported) {
  fixtures::StageA s = fixtures::make_stage_a();
  const fixtures::StageATape tape = fixtures::record_stage_a(s);
  const ir::Program program = ir::infer(tape.tape);
  const rewrite::R7BlockLinmap rule;
  const std::vector<rewrite::MatchSite> sites = rule.match(program, ir::PlanAnnotations{});
  for (const rewrite::MatchSite& site : sites) {
    const std::vector<rewrite::LinmapBlock> blocks = rewrite::linmap_blocks(program, site.domain);
    std::cout << "[ r7 ] Stage A (full): domain " << site.domain << " (" << program.domains[static_cast<std::size_t>(site.domain)].rows
              << " rows) splits into " << blocks.size() << " block(s): [";
    for (const rewrite::LinmapBlock& b : blocks) std::cout << (b.hi - b.lo) << ' ';
    std::cout << "]\n";
  }
  std::cout << "[ r7 ] Stage A (full, " << program.domains.size() << " domains): " << sites.size() << " linmap domain(s) matched\n";
}
