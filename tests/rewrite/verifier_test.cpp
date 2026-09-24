// M4/R0 gate: "a per-rule verifier ... catches a deliberately wrong rule" (PROBLEM.md §7). Uses
// the M1 book (a real, gated fixture) so a wrong rule's failure is a genuine wrong NUMBER, not a
// contrived one: WrongReductionFusionRule reproduces the exact defect this package's own
// refactor introduced and caught during development (a fused domain's output row dropped from
// its keep set below the emit byte-size threshold, i.e. written nowhere) -- see
// rewrite/planner.hpp's ReductionFusionParams::emit_min_bytes comment.
#include <gtest/gtest.h>

#include "epykos/exec/interpreter.hpp"
#include "epykos/fixtures/m1_book.hpp"
#include "epykos/fixtures/record_m1.hpp"
#include "epykos/ir/program.hpp"
#include "epykos/ir/signature.hpp"
#include "epykos/rewrite/planner_rules.hpp"
#include "epykos/rewrite/verifier.hpp"
#include "epykos/tape/tape.hpp"

namespace ir = epykos::ir;
namespace rewrite = epykos::rewrite;
namespace planner = epykos::rewrite::planner;
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

// Fuses every eligible domain (by planner.reduction_fusion's own member-refs test) but always
// with an EMPTY keep set -- an output row that cannot be emitted (small domains, well under
// emit_min_bytes) is then written nowhere. Everything else about the rule (name, exactness,
// match) is copied from the real one so the only difference under test is this one defect.
class WrongReductionFusionRule : public rewrite::Rule {
 public:
  const std::string& name() const noexcept override {
    static const std::string n = "test.wrong_reduction_fusion";
    return n;
  }
  rewrite::Exactness exactness_class() const noexcept override { return rewrite::Exactness::E0; }
  std::vector<rewrite::MatchSite> match(const ir::Program& program, const ir::PlanAnnotations&) const override {
    return program.domains.empty() ? std::vector<rewrite::MatchSite>{} : std::vector<rewrite::MatchSite>{rewrite::MatchSite::whole_program()};
  }
  rewrite::Proposal propose(const ir::Program& program, const ir::PlanAnnotations&, const rewrite::MatchSite&) const override {
    const planner::FusionAnalysis analysis = planner::analyze_fusion(program);
    ir::PlanAnnotations a;
    a.domain.assign(program.domains.size(), ir::DomainPlan{});
    for (std::size_t d = 0; d < program.domains.size(); ++d) {
      if (analysis.member_refs[d] > 0) a.domain[d].choice = ir::Materialise::FuseIntoReduction;  // keep_rows left empty: the defect
    }
    rewrite::Proposal p;
    p.annotations = a;
    return p;
  }
};

// D57 (a review finding on the M4-gate-1 landing): a minimal, hand-built stand-in for "an e-graph
// ExtractResult whose PROGRAM already carries a wrong structural rewrite" -- domain 1's one step
// turned from Add into Sub, a stand-in for what a wrong composition of more-than-one E1 rewrite
// could do to a real extraction, with none of fma_contraction's own genuine-cancellation caveat
// (D57's OTHER finding, fma_contraction_verify_test.cpp) muddying which gap this test is about:
// 3 + 4 = 7 vs 3 - 4 = -1 is wrong by any tolerance, not merely "outside an unscaled bound at a
// pathological input".
ir::Program tiny_add_program() {
  ir::Program p;
  p.domains.push_back(ir::Domain{"input", 2, 0, 0, false, {}, false, -1});
  p.groups.push_back(ir::Group{0, {ir::Step{epykos::Op::Input, ir::Slot{ir::SlotKind::Input, -1}, {}, {}, {}}}});
  p.inputs = {0, 1};
  p.input_values = {3.0, 4.0};
  p.domains.push_back(ir::Domain{"add(@0,@1)", 1, 2, 0, false, {0}, false, -1});
  p.gathers.push_back(ir::Gather{1, {0}});
  p.gathers.push_back(ir::Gather{1, {1}});
  p.groups.push_back(
      ir::Group{1, {ir::Step{epykos::Op::Add, ir::Slot{ir::SlotKind::Gather, 0}, ir::Slot{ir::SlotKind::Gather, 1}, {}, {}}}});
  p.outputs = {2};
  return p;
}

}  // namespace

TEST(Verifier, VerifyAnnotationsAloneCannotCatchAWrongStructuralHistoryBakedIntoTheProgram) {
  const ir::Program original = tiny_add_program();
  ir::Program extracted = original;  // stands in for ExtractResult::program after a wrong rewrite
  extracted.groups[1].steps[0].op = epykos::Op::Sub;  // the (simulated) composed-rewrite defect

  // THE GAP, demonstrated directly: verify_annotations compares `extracted` against ITSELF (plan
  // cleared vs the same plan) -- src/rewrite/verifier.cpp's own documented bare/annotated pair,
  // correct for what it checks -- so it is structurally blind to how `extracted` came to be and
  // reports PASS no matter what a prior structural rewrite chain did to it. This is finding 2 of
  // D57: every shipped egraph_*_test.cpp's ONLY correctness check on an extracted program was
  // exactly this call, never one against the true, unrewritten original.
  const rewrite::VerifyReport annotations_only =
      rewrite::verify_annotations(extracted, ir::PlanAnnotations{}, original.input_values.data(), 2, 1);
  EXPECT_TRUE(annotations_only.passed()) << "demonstrates the gap: verify_annotations cannot see a "
                                             "wrong composed history at all -- "
                                          << annotations_only.summary();

  // THE FIX: verify_extraction compares against the TRUE original and correctly reports FAIL --
  // 7.0 (3+4) vs -1.0 (3-4) is nowhere near even the generously ulps-scaled E1 bound a two-entry
  // E1 history would get.
  const rewrite::VerifyReport extraction_check =
      rewrite::verify_extraction(original, extracted, ir::PlanAnnotations{}, {"test.wrong_rewrite", "test.wrong_rewrite"},
                                 {"test.wrong_rewrite"}, original.input_values.data(), 2, 1);
  EXPECT_FALSE(extraction_check.passed()) << extraction_check.summary();
  EXPECT_GT(extraction_check.interpreter.mismatches, 0u);

  // Sanity: verify_extraction on the UNCHANGED program (empty history, matching the common "no E1
  // rule fired" case, D51's own 0/0 measurements) is bitwise and passes, exactly as
  // verify_annotations already does for a real plan on an untouched program -- this function adds
  // a check, it does not replace or loosen the existing one.
  const rewrite::VerifyReport identity_check =
      rewrite::verify_extraction(original, original, ir::PlanAnnotations{}, {}, {}, original.input_values.data(), 2, 1);
  EXPECT_TRUE(identity_check.passed()) << identity_check.summary();
}

TEST(Verifier, RealReductionFusionRulePassesTheDifferentialCheck) {
  const ir::Program& program = m1_program();
  const fixtures::Book book = fixtures::make_m1_book();
  const int n_inputs = static_cast<int>(book.z0.size());
  const int n_outputs = static_cast<int>(program.outputs.size());
  const planner::ReductionFusionRule rule(/*lane_tile=*/8);
  rewrite::VerifyOptions options;
  options.ball.draws = 8;
  const rewrite::RuleVerifyReport report =
      rewrite::verify_rule(rule, program, book.z0.data(), n_inputs, n_outputs, ir::PlanAnnotations{}, options);
  EXPECT_GT(report.sites_checked, 0);
  EXPECT_TRUE(report.passed()) << report.report.summary();
}

TEST(Verifier, CatchesADeliberatelyWrongReductionFusionRule) {
  const ir::Program& program = m1_program();
  const fixtures::Book book = fixtures::make_m1_book();
  const int n_inputs = static_cast<int>(book.z0.size());
  const int n_outputs = static_cast<int>(program.outputs.size());
  const WrongReductionFusionRule rule;
  rewrite::VerifyOptions options;
  options.ball.draws = 8;
  options.check_adjoint = false;  // the defect is a value dropped from the FORWARD pass
  const rewrite::RuleVerifyReport report =
      rewrite::verify_rule(rule, program, book.z0.data(), n_inputs, n_outputs, ir::PlanAnnotations{}, options);
  EXPECT_GT(report.sites_checked, 0);
  EXPECT_FALSE(report.passed());
  EXPECT_FALSE(report.report.interpreter.passed);
  EXPECT_GT(report.report.interpreter.mismatches, 0u) << report.report.summary();
}

TEST(Verifier, VerifyAnnotationsAgreesWithTheDefaultPlanOnTheM1Book) {
  const ir::Program& program = m1_program();
  const fixtures::Book book = fixtures::make_m1_book();
  const int n_inputs = static_cast<int>(book.z0.size());
  const int n_outputs = static_cast<int>(program.outputs.size());
  planner::DefaultPlanOptions dopt;
  dopt.lane_tile = 8;
  const ir::PlanAnnotations plan = planner::default_plan(program, dopt);
  rewrite::VerifyOptions options;
  options.ball.draws = 8;
  const rewrite::VerifyReport report = rewrite::verify_annotations(program, plan, book.z0.data(), n_inputs, n_outputs, options);
  EXPECT_TRUE(report.passed()) << report.summary();
}
