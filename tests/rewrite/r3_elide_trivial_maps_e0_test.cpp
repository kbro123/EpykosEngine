// M4/R-a: R3 elide trivial maps (rewrite/r3_elide_trivial_maps.hpp, DESIGN.md §6, class E0).
//
// Two synthetic domains: one whose ENTIRE group is a length-1 Sum (a pure relabelling of an
// earlier value -- R3's own target) catches r3.off_by_one_member (the eliminated domain is
// followed by a THIRD domain that reads it, so a wrong substitution shows up as a wrong output);
// one with a genuine two-member Sum catches r3.treats_length_two_as_trivial (which would drop the
// second term). Then the M1 book and the Stage A tape.
#include <gtest/gtest.h>

#include <iostream>

#include "epykos/fixtures/m1_book.hpp"
#include "epykos/fixtures/record_m1.hpp"
#include "epykos/ir/program.hpp"
#include "epykos/ir/signature.hpp"
#include "epykos/rewrite/r3_elide_trivial_maps.hpp"
#include "epykos/rewrite/verifier.hpp"
#include "epykos/tape/tape.hpp"
#include "stage_a/stage_a_test_helpers.hpp"

#ifndef EPYKOS_FP_CONTRACT_OFF
#error "an _e0_test.cpp TU must see EPYKOS_FP_CONTRACT_OFF"
#endif

namespace ir = epykos::ir;
namespace rewrite = epykos::rewrite;
namespace fixtures = epykos::fixtures;

namespace {

// domain 0: "input", 3 rows (ids 0..2). domain 1: 3 rows (ids 3..5), ONE step Sum over a length-1
// segment whose row r's sole member is domain 0's row r -- a pure relabelling. domain 2: 3 rows
// (ids 6..8), Mul(gather -> domain 1 row r, literal 2.0): reads domain 1, so a wrong substitution
// in domain 1's elimination shows up here as a wrong output.
ir::Program synthetic_single_member_program() {
  ir::Program p;
  p.domains.push_back(ir::Domain{"input", 3, 0, 0, false, {}, false, -1});
  p.groups.push_back(ir::Group{0, {ir::Step{epykos::Op::Input, ir::Slot{ir::SlotKind::Input, -1}, {}, {}, {}}}});
  p.inputs = {0, 1, 2};
  p.input_values = {10.0, 20.0, 30.0};

  p.segments.push_back(ir::Segment{1, {0, 1, 2, 3}, {0, 1, 2}, {}});  // row r's one member is value id r
  p.groups.push_back(ir::Group{1, {ir::Step{epykos::Op::Sum, ir::Slot{ir::SlotKind::Segment, 0}, {}, {}, {}}}});
  p.domains.push_back(ir::Domain{"sum(%0)", 3, 3, 0, false, {0}, false, -1});

  p.literals.push_back(2.0);
  p.gathers.push_back(ir::Gather{2, {3, 4, 5}});  // reads domain 1's rows 1:1
  p.groups.push_back(ir::Group{2, {ir::Step{epykos::Op::Mul, ir::Slot{ir::SlotKind::Gather, 0}, ir::Slot{ir::SlotKind::Literal, 0}, {}, {}}}});
  p.domains.push_back(ir::Domain{"mul(@0,#0)", 3, 6, 0, false, {1}, false, -1});

  p.outputs = {6, 7, 8};
  ir::validate(p);
  return p;
}

// domain 0: "input", 2 rows. domain 1: 1 row, ONE step Sum over a genuine two-member segment
// (member 0 and member 1 of domain 0) -- NOT trivial: its value is their SUM, not either one alone.
ir::Program synthetic_two_member_program() {
  ir::Program p;
  p.domains.push_back(ir::Domain{"input", 2, 0, 0, false, {}, false, -1});
  p.groups.push_back(ir::Group{0, {ir::Step{epykos::Op::Input, ir::Slot{ir::SlotKind::Input, -1}, {}, {}, {}}}});
  p.inputs = {0, 1};
  p.input_values = {10.0, 20.0};

  p.segments.push_back(ir::Segment{1, {0, 2}, {0, 1}, {}});  // one row, two members: value ids 0 and 1
  p.groups.push_back(ir::Group{1, {ir::Step{epykos::Op::Sum, ir::Slot{ir::SlotKind::Segment, 0}, {}, {}, {}}}});
  p.domains.push_back(ir::Domain{"sum(%0)", 1, 2, 0, false, {0}, false, -1});

  p.outputs = {2};
  ir::validate(p);
  return p;
}

}  // namespace

TEST(R3ElideTrivialMaps, NameAndExactness) {
  const rewrite::R3ElideTrivialMaps rule;
  EXPECT_EQ(rule.name(), "r3.elide_trivial_maps");
  EXPECT_EQ(rule.exactness_class(), rewrite::Exactness::E0);
}

TEST(R3ElideTrivialMaps, SyntheticSingleMemberDomainIsEliminatedAndCatchesOffByOneMember) {
  const ir::Program program = synthetic_single_member_program();
  const rewrite::R3ElideTrivialMaps rule;

  const std::vector<rewrite::MatchSite> sites = rule.match(program, ir::PlanAnnotations{});
  ASSERT_EQ(sites.size(), 1u);
  EXPECT_EQ(sites.front().domain, 1);

  const rewrite::Proposal p = rule.propose(program, ir::PlanAnnotations{}, sites.front());
  ASSERT_TRUE(p.is_structural());
  const ir::Program& after = *p.program;
  EXPECT_NO_THROW(ir::validate(after));
  ASSERT_EQ(after.domains.size(), program.domains.size() - 1);  // domain 1 is gone entirely
  EXPECT_EQ(after.domains[0].rows, 3);                          // "input" untouched
  EXPECT_EQ(after.domains[1].rows, 3);                          // the old domain 2, now at position 1
  EXPECT_EQ(after.gathers[0].domain, 1);                        // its own gather's domain id shifted down by one
  EXPECT_EQ(after.gathers[0].index, (std::vector<ir::value_id>{0, 1, 2}));  // reads domain 0 directly now

  const std::vector<double> state = {10.0, 20.0, 30.0};
  rewrite::VerifyOptions options;
  options.ball.draws = 8;
  const rewrite::RuleVerifyReport report = rewrite::verify_rule(rule, program, state.data(), 3, 3, ir::PlanAnnotations{}, options);
  EXPECT_GT(report.sites_checked, 0);
  EXPECT_TRUE(report.passed()) << report.report.summary();
}

TEST(R3ElideTrivialMaps, SyntheticTwoMemberDomainNeverMatchesAndCatchesTheLengthTwoMutant) {
  const ir::Program program = synthetic_two_member_program();
  const rewrite::R3ElideTrivialMaps rule;

  const std::vector<rewrite::MatchSite> sites = rule.match(program, ir::PlanAnnotations{});
  // Under a correct build, a genuine two-member row is NOT trivial and match() reports nothing --
  // there is nothing further to check. r3.treats_length_two_as_trivial wrongly reports a site here;
  // the block below exists to catch exactly that mutant, by showing its propose() drops the second
  // term (`sum(10,20)=30` folded to `10`), a mismatch compare_programs sees.
  if (sites.empty()) {
    SUCCEED();
    return;
  }
  ASSERT_EQ(sites.size(), 1u);
  const rewrite::Proposal p = rule.propose(program, ir::PlanAnnotations{}, sites.front());
  ASSERT_TRUE(p.is_structural());

  const std::vector<double> state = {10.0, 20.0};
  rewrite::VerifyOptions options;
  options.ball.draws = 8;
  options.check_adjoint = false;  // a 1-output, order-losing program: the value check alone suffices
  const rewrite::VerifyReport report = rewrite::compare_programs(program, *p.program, state.data(), 2, 1, options);
  EXPECT_TRUE(report.passed()) << report.summary();  // reached only under the mutant: this fails
}

TEST(R3ElideTrivialMaps, MatchesAndVerifiesOnTheM1Book) {
  const fixtures::Book book = fixtures::make_m1_book();
  const epykos::Tape tape = fixtures::record_m1(book);
  const ir::Program program = ir::infer(tape);
  const rewrite::R3ElideTrivialMaps rule;

  const std::size_t n_sites = rule.match(program, ir::PlanAnnotations{}).size();
  std::cout << "[ r3 ] M1 book: " << n_sites << " of " << program.domains.size() << " domain(s) are pure length-1 sums\n";

  rewrite::VerifyOptions options;
  options.ball.draws = 8;
  const rewrite::RuleVerifyReport report =
      rewrite::verify_rule(rule, program, book.z0.data(), static_cast<int>(book.z0.size()), static_cast<int>(program.outputs.size()),
                           ir::PlanAnnotations{}, options);
  EXPECT_TRUE(report.passed()) << report.report.summary();
}

TEST(R3ElideTrivialMaps, MatchesAndVerifiesOnTheStageATape) {
  const ir::Program program = ir::infer(epykos::test::stage_a_tape().tape);
  const rewrite::R3ElideTrivialMaps rule;

  const std::vector<rewrite::MatchSite> sites = rule.match(program, ir::PlanAnnotations{});
  std::cout << "[ r3 ] Stage A: " << sites.size() << " of " << program.domains.size() << " domain(s) are pure length-1 sums\n";

  const std::vector<double> quotes = epykos::test::stage_a_tape().record_quotes();
  rewrite::VerifyOptions options;
  options.ball.rho = 0.0005;
  options.ball.draws = 3;
  options.adjoint_ball_draws = 2;
  options.max_outputs_checked = 24;
  const rewrite::RuleVerifyReport report =
      rewrite::verify_rule(rule, program, quotes.data(), static_cast<int>(quotes.size()), static_cast<int>(program.outputs.size()),
                           ir::PlanAnnotations{}, options);
  EXPECT_TRUE(report.passed()) << report.report.summary();
}
