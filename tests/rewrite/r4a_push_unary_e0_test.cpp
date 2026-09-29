// M4/R-b: R4aPushUnaryThroughGathers (rewrite/r4a_push_unary_through_gathers.hpp), class E0.
//
// A hand-built program pins the exact target shape (DESIGN.md §6's own example: several rows
// gathering the SAME underlying time and re-computing `exp` for each one) and checks the rewrite
// is bit-identical and genuinely shrinks the number of `exp` evaluations. rewrite::verify_rule
// then runs the real rule against the M1 book and the Stage A tape (CLAUDE.md D8 "run on both"):
// both are asserted NOT to fire, with the reasoning printed rather than hidden (the package brief
// / RESUME.md's own words, "a rule that never fires is a finding to report, not to hide") --
// Stage A's own discount-factor memo (fixtures/stage_a.hpp's own header comment: "a discount
// factor is recorded once per distinct time rather than once per coupon that reads it") already
// gives its two Exp/Neg-of-gather sites zero duplication (measured: gather size == distinct
// sources for both), so there is nothing left for this rule's own, independent CSE to remove on
// the CURRENT recording -- not a defect in the rule, a property of the fixture it is asked to
// improve.
#include <gtest/gtest.h>

#include <set>

#include "epykos/fixtures/m1_book.hpp"
#include "epykos/fixtures/record_m1.hpp"
#include "epykos/ir/evaluate.hpp"
#include "epykos/ir/program.hpp"
#include "epykos/ir/signature.hpp"
#include "epykos/rewrite/r4a_push_unary_through_gathers.hpp"
#include "epykos/rewrite/verifier.hpp"
#include "epykos/tape/tape.hpp"
#include "stage_a/stage_a_test_helpers.hpp"

namespace ir = epykos::ir;
namespace rewrite = epykos::rewrite;
namespace fixtures = epykos::fixtures;

namespace {

// input (4 rows: 4 distinct "times") -> Times = identity columns (the M1-style "gather straight
// off Input", so this program needs no separate linmap domain) -> D (6 rows: 6 READERS, several
// repeating the SAME time) = exp(gather(Times)). 6 > 4: real redundancy for R4a to remove.
ir::Program repeated_exp_program() {
  ir::Program p;
  p.domains.push_back(ir::Domain{"input", 4, 0, 0, false, {}, false, -1});
  p.groups.push_back(ir::Group{0, {ir::Step{epykos::Op::Input, ir::Slot{ir::SlotKind::Input, -1}, {}, {}, {}}}});
  p.inputs = {0, 1, 2, 3};
  p.input_values = {0.25, 0.5, 1.0, 2.0};

  p.domains.push_back(ir::Domain{"exp(@0)", 6, 4, 0, false, {0}, false, -1});
  // Reader rows 0..5 gather times [0, 1, 0, 2, 1, 3] -- times 0 and 1 are each read twice.
  p.gathers.push_back(ir::Gather{1, {0, 1, 0, 2, 1, 3}});
  p.groups.push_back(ir::Group{1, {ir::Step{epykos::Op::Exp, ir::Slot{ir::SlotKind::Gather, 0}, {}, {}, {}}}});
  p.outputs = {4, 5, 6, 7, 8, 9};
  return p;
}

TEST(R4aPushUnary, MatchesAndProposesOneSiteForTheRepeatedGather) {
  const ir::Program program = repeated_exp_program();
  ir::validate(program);
  const rewrite::R4aPushUnaryThroughGathers rule;
  const std::vector<rewrite::MatchSite> sites = rule.match(program, ir::PlanAnnotations{});
  ASSERT_EQ(sites.size(), 1u);
  EXPECT_EQ(sites[0].kind, rewrite::SiteKind::Domain);
  EXPECT_EQ(sites[0].domain, 0);  // the "Times"/input domain
  EXPECT_EQ(static_cast<epykos::Op>(sites[0].index), epykos::Op::Exp);
}

TEST(R4aPushUnary, RewriteIsBitIdenticalAndComputesExpOnceThenReusesIt) {
  const ir::Program before = repeated_exp_program();
  const rewrite::R4aPushUnaryThroughGathers rule;
  const std::vector<rewrite::MatchSite> sites = rule.match(before, ir::PlanAnnotations{});
  ASSERT_EQ(sites.size(), 1u);
  const rewrite::Proposal proposal = rule.propose(before, ir::PlanAnnotations{}, sites[0]);
  ASSERT_TRUE(proposal.is_structural());
  const ir::Program& after = *proposal.program;
  ir::validate(after);

  // A new 4-row "exp(@0)" domain exists (rows == the 4 distinct times, not the 6 readers), and
  // the original reader domain's step is now a plain multiply-by-one.
  ASSERT_EQ(after.domains.size(), 3u);
  EXPECT_EQ(after.domains[1].rows, 4);
  EXPECT_EQ(after.groups[1].steps[0].op, epykos::Op::Exp);
  EXPECT_EQ(after.groups[2].steps[0].op, epykos::Op::Mul);

  const std::vector<double> out_before = ir::evaluate(before, before.input_values);
  const std::vector<double> out_after = ir::evaluate(after, before.input_values);
  ASSERT_EQ(out_before.size(), out_after.size());
  for (std::size_t i = 0; i < out_before.size(); ++i) EXPECT_EQ(out_before[i], out_after[i]) << i;  // E0: bit-identical

  // The rule no longer matches its own output.
  EXPECT_TRUE(rule.match(after, ir::PlanAnnotations{}).empty());
}

TEST(R4aPushUnary, DoesNotFireOnTheM1Book) {
  const fixtures::Book book = fixtures::make_m1_book();
  const epykos::Tape tape = fixtures::record_m1(book);
  const ir::Program program = ir::infer(tape);
  const rewrite::R4aPushUnaryThroughGathers rule;
  const std::vector<rewrite::MatchSite> sites = rule.match(program, ir::PlanAnnotations{});
  EXPECT_TRUE(sites.empty()) << sites.size() << " unexpected site(s) on the M1 book";
}

TEST(R4aPushUnary, DoesNotFireOnTheStageATapeAndVerifierAgreesTrivially) {
  const fixtures::StageATape& tape = epykos::test::stage_a_tape();
  const ir::Program program = ir::infer(tape.tape);
  // The FULL recorded input vector (Program::inputs order: the calibration quotes AND the
  // realised fixings recorded alongside them), not StageATape::record_quotes() (the quotes
  // alone, a shorter subset -- exec::Interpreter / adjoint::Adjoint take exactly
  // `program.inputs.size()` entries and allocate nothing to check the length).
  const std::vector<double>& state = program.input_values;
  const rewrite::R4aPushUnaryThroughGathers rule;
  const std::vector<rewrite::MatchSite> sites = rule.match(program, ir::PlanAnnotations{});
  std::cout << "[ r4a      ] " << sites.size() << " (source, op) sites on the Stage A tape (0 expected: see file header)\n";
  EXPECT_TRUE(sites.empty());

  // verify_rule on 0 sites is a defined, checked no-op (RuleVerifyReport::passed(): "sites_checked
  // == 0 || report.passed()") -- still exercised so a future change that makes the rule fire here
  // is checked by the SAME gate this test already builds, not a new one.
  const rewrite::RuleVerifyReport report =
      rewrite::verify_rule(rule, program, state.data(), static_cast<int>(state.size()), static_cast<int>(program.outputs.size()));
  EXPECT_EQ(report.sites_checked, 0);
  EXPECT_TRUE(report.passed());
}

}  // namespace
