// M4/R-b: R4bSharedReciprocal (rewrite/r4b_shared_reciprocal.hpp), class E1 (4 ulps, D26).
//
// A hand-built program pins the exact target shape (several `Div`s by the SAME gathered domain);
// the rewrite is checked against the unfused program within 4 ulps, not bit-identically (DESIGN.md
// §12: `a/b` and `a*(1/b)` round differently in general). rewrite::verify_rule then runs the real
// rule on the M1 book and the Stage A tape: measured, every `Div`-by-gather site on Stage A reads
// from a gather whose rows come from MORE THAN ONE source domain (a per-slot "whichever curve
// applies" lookup), so this rule's own homogeneous-source requirement correctly declines all of
// them -- reported here, not hidden, same as r4a_push_unary_e0_test.cpp.
#include <gtest/gtest.h>

#include "epykos/fixtures/m1_book.hpp"
#include "epykos/fixtures/record_m1.hpp"
#include "epykos/ir/evaluate.hpp"
#include "epykos/ir/program.hpp"
#include "epykos/ir/signature.hpp"
#include "epykos/rewrite/r4b_shared_reciprocal.hpp"
#include "epykos/rewrite/verifier.hpp"
#include "epykos/tape/tape.hpp"
#include "stage_a/stage_a_test_helpers.hpp"

namespace ir = epykos::ir;
namespace rewrite = epykos::rewrite;
namespace fixtures = epykos::fixtures;

namespace {

// input (2 rows: two distinct denominators) -> D1 (3 rows) = div(@in2, gather(input)), D2 (2 rows)
// = div(@in3, gather(input)): two SEPARATE domains sharing the same source, one of them also with
// an in-gather repeat -- both the "self-redundant" and the "shared across sites" profitability
// paths fire on this one fixture.
ir::Program shared_divisor_program() {
  ir::Program p;
  p.domains.push_back(ir::Domain{"input", 4, 0, 0, false, {}, false, -1});
  p.groups.push_back(ir::Group{0, {ir::Step{epykos::Op::Input, ir::Slot{ir::SlotKind::Input, -1}, {}, {}, {}}}});
  p.inputs = {0, 1, 2, 3};
  p.input_values = {2.0, 4.0, 10.0, 3.0};  // in0, in1 are the two "curve" denominators

  p.domains.push_back(ir::Domain{"div(@0,@1)", 3, 4, 0, false, {0}, false, -1});
  p.gathers.push_back(ir::Gather{1, {2, 3, 2}});   // numerators: in2, in3, in2 (repeat)
  p.gathers.push_back(ir::Gather{1, {0, 0, 1}});   // denominators: in0, in0 (repeat!), in1
  p.groups.push_back(
      ir::Group{1, {ir::Step{epykos::Op::Div, ir::Slot{ir::SlotKind::Gather, 0}, ir::Slot{ir::SlotKind::Gather, 1}, {}, {}}}});

  p.domains.push_back(ir::Domain{"div(@0,@1)", 2, 7, 0, false, {0}, false, -1});
  p.gathers.push_back(ir::Gather{2, {2, 3}});  // numerators: in2, in3
  p.gathers.push_back(ir::Gather{2, {0, 1}});  // denominators: in0, in1 (no repeat within THIS gather)
  p.groups.push_back(
      ir::Group{2, {ir::Step{epykos::Op::Div, ir::Slot{ir::SlotKind::Gather, 2}, ir::Slot{ir::SlotKind::Gather, 3}, {}, {}}}});

  p.outputs = {4, 5, 6, 7, 8};
  return p;
}

TEST(R4bSharedReciprocal, MatchesTheSharedDenominatorDomain) {
  const ir::Program program = shared_divisor_program();
  ir::validate(program);
  const rewrite::R4bSharedReciprocal rule;
  const std::vector<rewrite::MatchSite> sites = rule.match(program, ir::PlanAnnotations{});
  ASSERT_EQ(sites.size(), 1u);
  EXPECT_EQ(sites[0].domain, 0);  // the "input" domain both Divs read their denominator from
}

TEST(R4bSharedReciprocal, RewriteAgreesWithin4UlpsAndSharesOneReciprocalDomain) {
  const ir::Program before = shared_divisor_program();
  const rewrite::R4bSharedReciprocal rule;
  const std::vector<rewrite::MatchSite> sites = rule.match(before, ir::PlanAnnotations{});
  ASSERT_EQ(sites.size(), 1u);
  const rewrite::Proposal proposal = rule.propose(before, ir::PlanAnnotations{}, sites[0]);
  ASSERT_TRUE(proposal.is_structural());
  const ir::Program& after = *proposal.program;
  ir::validate(after);

  ASSERT_EQ(after.domains.size(), 4u);
  EXPECT_EQ(after.domains[1].rows, 4);  // one shared "recip(@0)" domain, rows == the input domain's
  EXPECT_EQ(after.groups[1].steps[0].op, epykos::Op::Recip);
  EXPECT_EQ(after.groups[2].steps[0].op, epykos::Op::Mul);  // was Div
  EXPECT_EQ(after.groups[3].steps[0].op, epykos::Op::Mul);  // was Div

  const std::vector<double> out_before = ir::evaluate(before, before.input_values);
  const std::vector<double> out_after = ir::evaluate(after, before.input_values);
  ASSERT_EQ(out_before.size(), out_after.size());
  for (std::size_t i = 0; i < out_before.size(); ++i) {
    EXPECT_TRUE(epykos::verify::within_ulps(out_before[i], out_after[i], 4.0)) << i << ": " << out_before[i] << " vs " << out_after[i];
  }

  EXPECT_TRUE(rule.match(after, ir::PlanAnnotations{}).empty());
}

TEST(R4bSharedReciprocal, DoesNotFireOnTheM1Book) {
  const fixtures::Book book = fixtures::make_m1_book();
  const epykos::Tape tape = fixtures::record_m1(book);
  const ir::Program program = ir::infer(tape);
  const rewrite::R4bSharedReciprocal rule;
  const std::vector<rewrite::MatchSite> sites = rule.match(program, ir::PlanAnnotations{});
  EXPECT_TRUE(sites.empty()) << sites.size() << " unexpected site(s) on the M1 book";
}

TEST(R4bSharedReciprocal, DoesNotFireOnTheStageATapeAndVerifierAgreesTrivially) {
  const fixtures::StageATape& tape = epykos::test::stage_a_tape();
  const ir::Program program = ir::infer(tape.tape);
  // The FULL recorded input vector (see r4a_push_unary_e0_test.cpp's own comment on this).
  const std::vector<double>& state = program.input_values;
  const rewrite::R4bSharedReciprocal rule;
  const std::vector<rewrite::MatchSite> sites = rule.match(program, ir::PlanAnnotations{});
  std::cout << "[ r4b      ] " << sites.size() << " shared-denominator sites on the Stage A tape (0 expected: see file header)\n";
  EXPECT_TRUE(sites.empty());

  rewrite::VerifyOptions options;
  options.interpreter_tolerance = epykos::verify::Tolerance::e1();
  options.adjoint_tolerance = epykos::verify::Tolerance::e1();
  const rewrite::RuleVerifyReport report = rewrite::verify_rule(rule, program, state.data(), static_cast<int>(state.size()),
                                                                 static_cast<int>(program.outputs.size()), ir::PlanAnnotations{}, options);
  EXPECT_EQ(report.sites_checked, 0);
  EXPECT_TRUE(report.passed());
}

}  // namespace
