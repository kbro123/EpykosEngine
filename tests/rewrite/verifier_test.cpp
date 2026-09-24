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

}  // namespace

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
