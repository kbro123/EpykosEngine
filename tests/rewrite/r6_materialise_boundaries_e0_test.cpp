// M4/R-c gate: R6MaterialiseBoundaries (rewrite/r6_materialise_boundaries.hpp) — the differential
// test CLAUDE.md/D8 requires for every rewrite, run on BOTH the M1 book and the Stage A tape.
// E0: an InlineIntoConsumer choice never changes an interpreter's or adjoint's output (DESIGN.md
// §7 "a structure-only decision; the arithmetic is the same either way"), so this file's job is
// to show the rule proposes it only where that is actually true (rewrite::verify_rule).
//
// Measured finding (development note, HARD RULE 9: not a hard-coded expectation): R6 finds ZERO
// candidates on the M1 book (checked below) -- every intermediate domain is either itself a
// whole-domain reduction (Times/affine, legs, book), a direct member of one (the DF, fixed- and
// float-coupon domains all feed a Sum/Affine segment one step downstream), or read far more than
// once per row by its single consumer (`forwards`, reused ~6.5x per row by the float coupons that
// share a curve's small forward grid -- R6's own "fan-out above a threshold" boundary, working as
// intended). The Stage A tape (four curves, a much wider pipeline) does have candidates.
//
// scripts/mutation_test.sh selects this file as a gate by its `_e0_test` suffix: r6.
// ignore_reduction_boundary / r6.ignore_fanout_boundary (both guarded in
// src/rewrite/r6_materialise_boundaries.cpp) make match()/propose() accept a site the checks
// above would otherwise reject; the Stage A test below is what actually exercises them (the M1
// book has no real site left to corrupt once the checks are correct, so it cannot catch these
// mutants by itself -- a finding this header states rather than hides).
#include <gtest/gtest.h>

#include <algorithm>
#include <iostream>

#include "epykos/exec/interpreter.hpp"
#include "epykos/fixtures/m1_book.hpp"
#include "epykos/fixtures/record_m1.hpp"
#include "epykos/fixtures/stage_a.hpp"
#include "epykos/ir/program.hpp"
#include "epykos/ir/signature.hpp"
#include "epykos/rewrite/r6_materialise_boundaries.hpp"
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

// A small (fast) Stage A tape: enough curves and trades for real inlining candidates to exist
// without paying the full 2,000-trade record + a differential ball's worth of Interpreter /
// Adjoint constructions over it.
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

// A hand-built near-miss shape (the same convention as fixtures::nearmiss_shapes for the M2
// adjoint mutants not exercisable on the M1 book: docs/WORKLOADS.md §M2, "not exercisable on the
// M1 book ... the near-miss ... shapes"): domain 1 ("P") is read exactly once per row, but by TWO
// DIFFERENT consumer domains (2 and 3) -- neither the M1 book nor the Stage A tape happens to
// contain a single-use-per-row producer with more than one distinct consumer, so r6.
// ignore_fanout_boundary needs this fixture to have anything to corrupt.
//   domain 0 "in"          Input, 2 rows: z0, z1
//   domain 1 "P = neg(@in)" 2 rows: P0 = -z0, P1 = -z1
//   domain 2 "A = neg(@P0)" 1 row: A0 = -P0 = z0
//   domain 3 "B = neg(@P1)" 1 row: B0 = -P1 = z1
// outputs = {A0, B0} = {z0, z1}: the identity, so any wrong number the interpreter comparison
// sees is definitely this rule's doing, not an oracle mismatch.
ir::Program two_consumer_program() {
  ir::Program p;
  p.domains.push_back(ir::Domain{"input", 2, 0, 0, false, {}, false, -1});
  p.groups.push_back(ir::Group{0, {ir::Step{epykos::Op::Input, ir::Slot{ir::SlotKind::Input, -1}, {}, {}, {}}}});
  p.inputs = {0, 1};
  p.input_values = {1.0, 2.0};

  ir::Gather gather_in{1, {0, 1}};  // P's own gather: row r reads Input row r
  p.gathers.push_back(gather_in);
  p.domains.push_back(ir::Domain{"neg(@0)", 2, 2, 0, false, {0}, false, -1});
  p.groups.push_back(ir::Group{1, {ir::Step{epykos::Op::Neg, ir::Slot{ir::SlotKind::Gather, 0}, {}, {}, {}}}});

  ir::Gather gather_p0{2, {2}};  // A's gather: reads P's row 0 (value id 2)
  ir::Gather gather_p1{3, {3}};  // B's gather: reads P's row 1 (value id 3)
  p.gathers.push_back(gather_p0);
  p.gathers.push_back(gather_p1);
  p.domains.push_back(ir::Domain{"neg(@0)", 1, 4, 0, false, {1}, false, -1});
  p.groups.push_back(ir::Group{2, {ir::Step{epykos::Op::Neg, ir::Slot{ir::SlotKind::Gather, 1}, {}, {}, {}}}});
  p.domains.push_back(ir::Domain{"neg(@0)", 1, 5, 0, false, {1}, false, -1});
  p.groups.push_back(ir::Group{3, {ir::Step{epykos::Op::Neg, ir::Slot{ir::SlotKind::Gather, 2}, {}, {}, {}}}});

  p.outputs = {4, 5};
  return p;
}

}  // namespace

// r6.ignore_fanout_boundary's own gate: neither the M1 book nor the Stage A tape has a single-
// use-per-row producer read by more than one distinct consumer (every such case in both real
// fixtures also fails a DIFFERENT check first -- multiple reads per row, not multiple consumers
// each reading once), so this hand-built near-miss shape is what actually exercises it.
TEST(R6MaterialiseBoundaries, DeclinesASingleUseProducerReadByTwoDistinctConsumers) {
  const ir::Program program = two_consumer_program();
  ASSERT_NO_THROW(ir::validate(program));
  const rewrite::R6MaterialiseBoundaries rule;
  const std::vector<rewrite::MatchSite> sites = rule.match(program, ir::PlanAnnotations{});
  for (const rewrite::MatchSite& site : sites) EXPECT_NE(site.domain, 1) << "P is read once each by two DIFFERENT consumers: R6 must decline it";
  const std::vector<double> z = {3.0, -2.0};
  const rewrite::RuleVerifyReport report = rewrite::verify_rule(rule, program, z.data(), 2, 2);
  EXPECT_TRUE(report.passed()) << report.report.summary();
}

TEST(R6MaterialiseBoundaries, RealRulePassesTheDifferentialCheckOnM1Book) {
  const ir::Program& program = m1_program();
  const fixtures::Book book = fixtures::make_m1_book();
  const rewrite::R6MaterialiseBoundaries rule;
  rewrite::VerifyOptions options;
  options.ball.draws = 8;
  const rewrite::RuleVerifyReport report = rewrite::verify_rule(
      rule, program, book.z0.data(), static_cast<int>(book.z0.size()), static_cast<int>(program.outputs.size()), {}, options);
  EXPECT_TRUE(report.passed()) << report.report.summary();
  std::cout << "[ r6 ] M1 book: " << report.sites_checked << " site(s) checked\n";
}

TEST(R6MaterialiseBoundaries, RealRulePassesTheDifferentialCheckOnStageA) {
  const ir::Program& program = small_stage_a().program;
  const rewrite::R6MaterialiseBoundaries rule;
  const std::vector<rewrite::MatchSite> sites = rule.match(program, ir::PlanAnnotations{});
  ASSERT_GT(sites.size(), 0u) << "R6 found no candidate on the (small) Stage A tape -- adjust the fixture, not the rule";
  rewrite::VerifyOptions options;
  options.ball.draws = 3;  // keep the ball small: this program has ~60-trade scale, not M1's
  const rewrite::RuleVerifyReport report = rewrite::verify_rule(rule, program, program.input_values.data(),
                                                                static_cast<int>(program.inputs.size()),
                                                                static_cast<int>(program.outputs.size()), {}, options);
  EXPECT_TRUE(report.passed()) << report.report.summary();
  EXPECT_EQ(report.sites_checked, static_cast<int>(sites.size()));
  std::cout << "[ r6 ] small Stage A: " << sites.size() << " site(s) inlined, " << report.sites_checked << " checked\n";
}

// PROBLEM.md §7 / this package's brief: "a rule that never fires is a finding to report, not to
// hide." Reports, on the full Stage A tape, how many domains R6 actually proposes inlining, and
// into how many distinct consumers -- the number docs/RESUME.md's landing entry cites in notes.
TEST(R6MaterialiseBoundaries, FiresOnTheFullStageATapeCountsReported) {
  fixtures::StageA s = fixtures::make_stage_a();
  const fixtures::StageATape tape = fixtures::record_stage_a(s);
  const ir::Program program = ir::infer(tape.tape);
  const rewrite::R6MaterialiseBoundaries rule;
  const std::vector<rewrite::MatchSite> sites = rule.match(program, ir::PlanAnnotations{});
  std::size_t total_rows_inlined = 0;
  std::vector<ir::domain_id> consumers;
  for (const rewrite::MatchSite& site : sites) {
    const rewrite::Proposal p = rule.propose(program, ir::PlanAnnotations{}, site);
    ASSERT_TRUE(p.is_annotation());
    ASSERT_LT(static_cast<std::size_t>(site.domain), p.annotations->domain.size());
    const ir::DomainPlan& dp = p.annotations->domain[static_cast<std::size_t>(site.domain)];
    ASSERT_EQ(dp.choice, ir::Materialise::InlineIntoConsumer);
    total_rows_inlined += static_cast<std::size_t>(program.domains[static_cast<std::size_t>(site.domain)].rows);
    consumers.push_back(dp.inline_consumer);
  }
  std::sort(consumers.begin(), consumers.end());
  consumers.erase(std::unique(consumers.begin(), consumers.end()), consumers.end());
  std::cout << "[ r6 ] Stage A (full, " << program.domains.size() << " domains): " << sites.size()
            << " domain(s) would inline (" << total_rows_inlined << " rows) into " << consumers.size() << " distinct consumer(s)\n";
  // Not a hard pass/fail count (HARD RULE 9: never a magic instance number keyed to this fixture)
  // -- just make the finding visible in the test log for the landing notes either way.
}
