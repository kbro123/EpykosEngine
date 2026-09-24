// M4/R-b: FmaContractionRule (rewrite/fma_contraction.hpp), class E1 (4 ulps, DESIGN.md §10 "one
// rounding change"). A hand-built program pins the exact rewrite (Mul + fixed-arity 2-member Sum
// -> one Fma step, later steps renumbered); rewrite::verify_rule runs it against the M1 book (no
// scan domains -- DESIGN.md §5.6 "the M1 book has no chain") and the Stage A tape. Measured: 0
// sites on BOTH. The compounding scan's own "1.0 + r*tau" step (DESIGN.md §5.6's own example) is
// NOT this shape once recorded: `tau` (a day-count fraction) is a compile-time constant, so
// `tape::affine_collapse` (DESIGN.md §5.2) already turns "constant + constant*variable" into one
// `Op::Affine` node before the signature pass ever runs -- there is no separate `Mul` step left
// for this rule to find (measured directly: zero fixed-arity 2-member `Sum` steps anywhere in the
// Stage A tape have a `Mul` as either member). Reported, not hidden (the package brief's own
// words): this rule's real target is a genuine product-of-two-Recs `a*b` immediately summed with
// something else in the SAME group, a shape neither fixture happens to record.
#include <gtest/gtest.h>

#include "epykos/fixtures/m1_book.hpp"
#include "epykos/fixtures/record_m1.hpp"
#include "epykos/ir/evaluate.hpp"
#include "epykos/ir/program.hpp"
#include "epykos/ir/signature.hpp"
#include "epykos/rewrite/fma_contraction.hpp"
#include "epykos/rewrite/greedy.hpp"
#include "epykos/rewrite/verifier.hpp"
#include "epykos/tape/tape.hpp"
#include "stage_a/stage_a_test_helpers.hpp"

namespace ir = epykos::ir;
namespace rewrite = epykos::rewrite;
namespace fixtures = epykos::fixtures;

namespace {

// A single domain, one row: value = fma(x0, x1, x2), recorded the UNFUSED way: t0 = Mul(x0,x1);
// t1 = Sum(t0, x2) (fixed-arity, 2 members -- fold_sum's own shape for a 2-term "+", DESIGN.md
// §5.2/§5.6). This is exactly what FmaContractionRule targets.
ir::Program mul_then_sum_program() {
  ir::Program p;
  p.domains.push_back(ir::Domain{"input", 3, 0, 0, false, {}, false, -1});
  p.groups.push_back(ir::Group{0, {ir::Step{epykos::Op::Input, ir::Slot{ir::SlotKind::Input, -1}, {}, {}, {}}}});
  p.inputs = {0, 1, 2};
  p.input_values = {1.25, -0.75, 2.5};

  p.domains.push_back(ir::Domain{"sum(mul(@0,@1),@2)", 1, 3, 0, false, {0}, false, -1});
  p.gathers.push_back(ir::Gather{1, {0}});  // @0 -> x0
  p.gathers.push_back(ir::Gather{1, {1}});  // @1 -> x1
  p.gathers.push_back(ir::Gather{1, {2}});  // @2 -> x2
  ir::Step mul{epykos::Op::Mul, ir::Slot{ir::SlotKind::Gather, 0}, ir::Slot{ir::SlotKind::Gather, 1}, {}, {}};
  ir::Step sum{epykos::Op::Sum, ir::Slot{ir::SlotKind::Step, 0}, ir::Slot{ir::SlotKind::Gather, 2}, {}, {}};
  // An extra, independent step after the Sum (reads nothing from the fused pair) to prove the
  // renumbering pass touches every kept step's slots, not just the fused one.
  ir::Step passthrough{epykos::Op::Neg, ir::Slot{ir::SlotKind::Step, 1}, {}, {}, {}};
  p.groups.push_back(ir::Group{1, {mul, sum, passthrough}});
  // domain 1 has ONE row (value id 3): its value is the group's LAST step (passthrough).
  p.outputs = {3};
  return p;
}

TEST(FmaContraction, MatchesTheMulThenFixedSumShape) {
  const ir::Program program = mul_then_sum_program();
  ir::validate(program);
  const rewrite::FmaContractionRule rule;
  const std::vector<rewrite::MatchSite> sites = rule.match(program, ir::PlanAnnotations{});
  ASSERT_EQ(sites.size(), 1u);
  EXPECT_EQ(sites[0].kind, rewrite::SiteKind::Step);
  EXPECT_EQ(sites[0].domain, 1);
  EXPECT_EQ(sites[0].step, 0);  // the Mul
}

TEST(FmaContraction, RewriteAgreesWithTheUnfusedProgramAndDropsOneStep) {
  const ir::Program before = mul_then_sum_program();
  const rewrite::FmaContractionRule rule;
  const std::vector<rewrite::MatchSite> sites = rule.match(before, ir::PlanAnnotations{});
  ASSERT_EQ(sites.size(), 1u);
  const rewrite::Proposal proposal = rule.propose(before, ir::PlanAnnotations{}, sites[0]);
  ASSERT_TRUE(proposal.is_structural());
  const ir::Program& after = *proposal.program;
  ir::validate(after);

  ASSERT_EQ(after.groups[1].steps.size(), before.groups[1].steps.size() - 1);
  EXPECT_EQ(after.groups[1].steps[0].op, epykos::Op::Fma);
  // No more Mul or fixed-arity Sum left in the fused group.
  for (const ir::Step& s : after.groups[1].steps) EXPECT_NE(s.op, epykos::Op::Mul);

  const std::vector<double> inputs = before.input_values;
  const std::vector<double> out_before = ir::evaluate(before, inputs);
  const std::vector<double> out_after = ir::evaluate(after, inputs);
  ASSERT_EQ(out_before.size(), out_after.size());
  // fma(1.25, -0.75, 2.5) = -0.9375 + 2.5 = 1.5625, then Neg -> -1.5625; agrees with the unfused
  // path (double's own fma vs mul-then-add happen to agree exactly at these operands, so the
  // check is exact here rather than merely within tolerance).
  EXPECT_DOUBLE_EQ(out_after[0], -1.5625);
  EXPECT_DOUBLE_EQ(out_before[0], out_after[0]);

  // The rule no longer matches its own output (fixpoint after one application).
  EXPECT_TRUE(rule.match(after, ir::PlanAnnotations{}).empty());
}

TEST(FmaContraction, DoesNotFireOnTheM1Book) {
  // DESIGN.md §5.6: "The M1 book has no chain and its program is unchanged" -- no scan domains,
  // hence no fixed-arity Sum steps, hence nothing for this rule to fuse. Documented here as the
  // correct outcome, not silently skipped (CLAUDE.md rule 10 / the package brief: "a rule that
  // never fires is a finding to report, not to hide").
  const fixtures::Book book = fixtures::make_m1_book();
  const epykos::Tape tape = fixtures::record_m1(book);
  const ir::Program program = ir::infer(tape);
  const rewrite::FmaContractionRule rule;
  const std::vector<rewrite::MatchSite> sites = rule.match(program, ir::PlanAnnotations{});
  EXPECT_TRUE(sites.empty()) << "the M1 book has " << sites.size() << " fma-fusable sites (unexpected: no scan on this book)";
}

TEST(FmaContraction, DoesNotFireOnTheStageATapeAndVerifierAgreesTrivially) {
  const fixtures::StageATape& tape = epykos::test::stage_a_tape();
  const ir::Program program = ir::infer(tape.tape);
  // The FULL recorded input vector (see r4a_push_unary_e0_test.cpp's own comment on this: NOT
  // StageATape::record_quotes(), a shorter subset).
  const std::vector<double>& state = program.input_values;
  const int n_inputs = static_cast<int>(state.size());
  const int n_outputs = static_cast<int>(program.outputs.size());

  const rewrite::FmaContractionRule rule;
  const std::vector<rewrite::MatchSite> sites = rule.match(program, ir::PlanAnnotations{});
  std::cout << "[ fma      ] " << sites.size() << " Mul+fixed-Sum sites on the Stage A tape (0 expected: see file header)\n";
  EXPECT_TRUE(sites.empty());

  rewrite::VerifyOptions options;
  options.interpreter_tolerance = epykos::verify::Tolerance::e1();
  options.adjoint_tolerance = epykos::verify::Tolerance::e1();
  const rewrite::RuleVerifyReport rule_report = rewrite::verify_rule(rule, program, state.data(), n_inputs, n_outputs, ir::PlanAnnotations{}, options);
  EXPECT_EQ(rule_report.sites_checked, 0);
  EXPECT_TRUE(rule_report.passed());
}

}  // namespace
