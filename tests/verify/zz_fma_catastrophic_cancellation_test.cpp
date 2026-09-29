// The D80 finding, asserted: it pins PRINCIPLES.md §5.3's justification for making contraction a
// declared build policy rather than an E1 peephole. Moved here from tests/rewrite/ when the M4
// plan-level search was quarantined (PRINCIPLES.md §7.1, §10 step 3): tests/rewrite/ is dropped
// from the default build, this assertion is not. rewrite::FmaContractionRule is one of the six
// rewrite/ stems the default build therefore keeps (root CMakeLists.txt, EPYKOS_REWRITE_KEEP).
//
// Demonstrates that FmaContractionRule's declared Exactness::E1 (<= 4 ulps,
// verify::Tolerance::e1()) is NOT an upper bound in general: for inputs where the subsequent Sum
// suffers catastrophic cancellation against the Mul's own rounding error, "a*b (rounded) + c"
// and "fma(a,b,c)" (correctly rounded) diverge by an UNBOUNDED number of ulps of the true result,
// not just <= 4. a=2^27+1, b=2^27-1: exact a*b = 2^54-1, which double rounds (ties-to-even) UP to
// 2^54. c = -2^54 exactly. True value a*b+c = -1 exactly (fma computes this exactly, since fma
// evaluates a*b at full/extended precision before the single rounding). The unfused (mul-then-sum)
// path computes round(a*b) + c = 2^54 + (-2^54) = 0.0 exactly: 0.0 vs -1.0 is not "within 4 ulps
// of -1.0" by any stretch (ulp(-1.0) ~= 2.22e-16) -- it is off by 1.0, i.e. ~4.5e15 ulps.
#include <gtest/gtest.h>

#include <cmath>
#include <iostream>

#include "epykos/ir/evaluate.hpp"
#include "epykos/ir/program.hpp"
#include "epykos/rewrite/fma_contraction.hpp"
#include "epykos/verify/differential.hpp"

namespace ir = epykos::ir;
namespace rewrite = epykos::rewrite;

namespace {

// Same shape as fma_contraction_verify_test.cpp's mul_then_sum_program(), catastrophic-cancellation
// operands instead of arbitrary ones.
ir::Program mul_then_sum_program_catastrophic() {
  ir::Program p;
  p.domains.push_back(ir::Domain{"input", 3, 0, 0, false, {}, false, -1});
  p.groups.push_back(ir::Group{0, {ir::Step{epykos::Op::Input, ir::Slot{ir::SlotKind::Input, -1}, {}, {}, {}}}});
  p.inputs = {0, 1, 2};
  // a = 2^27+1, b = 2^27-1, c = -(2^54) exactly.
  p.input_values = {134217729.0, 134217727.0, -18014398509481984.0};

  p.domains.push_back(ir::Domain{"sum(mul(@0,@1),@2)", 1, 3, 0, false, {0}, false, -1});
  p.gathers.push_back(ir::Gather{1, {0}});
  p.gathers.push_back(ir::Gather{1, {1}});
  p.gathers.push_back(ir::Gather{1, {2}});
  ir::Step mul{epykos::Op::Mul, ir::Slot{ir::SlotKind::Gather, 0}, ir::Slot{ir::SlotKind::Gather, 1}, {}, {}};
  ir::Step sum{epykos::Op::Sum, ir::Slot{ir::SlotKind::Step, 0}, ir::Slot{ir::SlotKind::Gather, 2}, {}, {}};
  p.groups.push_back(ir::Group{1, {mul, sum}});
  p.outputs = {3};
  return p;
}

TEST(FmaContractionAdversarial, CatastrophicCancellationExceedsTheDeclaredE1Tolerance) {
  const ir::Program before = mul_then_sum_program_catastrophic();
  ir::validate(before);

  const rewrite::FmaContractionRule rule;
  const std::vector<rewrite::MatchSite> sites = rule.match(before, ir::PlanAnnotations{});
  ASSERT_EQ(sites.size(), 1u);
  const rewrite::Proposal proposal = rule.propose(before, ir::PlanAnnotations{}, sites[0]);
  ASSERT_TRUE(proposal.is_structural());
  const ir::Program& after = *proposal.program;
  ir::validate(after);

  const std::vector<double> out_before = ir::evaluate(before, before.input_values);
  const std::vector<double> out_after = ir::evaluate(after, before.input_values);
  ASSERT_EQ(out_before.size(), 1u);
  ASSERT_EQ(out_after.size(), 1u);

  std::cout << "[ adversarial ] unfused (a*b)+c = " << out_before[0] << "  fma(a,b,c) = " << out_after[0]
            << "  abs diff = " << std::fabs(out_before[0] - out_after[0]) << "\n";

  // The rule's own declared bound.
  const bool within_declared_tolerance = epykos::verify::within_ulps(out_before[0], out_after[0], /*ulps=*/4.0);
  std::cout << "[ adversarial ] within_ulps(before, after, 4.0) = " << (within_declared_tolerance ? "true" : "false") << "\n";

  // The true mathematical result is -1.0 exactly (fma computes it exactly here); the unfused path
  // gives 0.0 -- confirming the direction of the error is the UNFUSED (pre-existing, "more E0-like")
  // side that is wrong relative to ground truth, not fma introducing a spurious error of its own.
  EXPECT_DOUBLE_EQ(out_after[0], -1.0);
  EXPECT_DOUBLE_EQ(out_before[0], 0.0);

  // THE FINDING, now asserted rather than demonstrated by failing (D80). Contracting a*b+c to
  // fma(a,b,c) has NO bound in ulps when the Sum cancels against the Mul's own rounding error:
  // here the two forms differ by 1.0, about 4.5e15 ulps, against a declared bound of 4.
  //
  // This is why PRINCIPLES.md 5.3 makes contraction a DECLARED BUILD POLICY, fixed above the pin
  // and constant below it, rather than an E1 peephole carrying a ulp bound it cannot honour. The
  // test passes when the bound is violated, because that violation is the reason for the rule.
  EXPECT_FALSE(within_declared_tolerance)
      << "abs diff " << std::fabs(out_before[0] - out_after[0])
      << ": if this now holds within 4 ulps, either the rule changed or this input stopped "
         "cancelling -- re-derive PRINCIPLES.md 5.3's justification before relaxing it.";
}

}  // namespace
