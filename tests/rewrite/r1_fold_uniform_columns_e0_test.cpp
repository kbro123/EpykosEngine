// M4/R-a: R1 fold uniform columns (rewrite/r1_fold_uniform_columns.hpp, DESIGN.md §6, class E0).
//
// The synthetic test below is this rule's OWN mutation gate: a hand-built domain with one column
// that IS uniform (in an operand position other than `a`, so a defect that always overwrites `a`
// shows up) and one that is uniform in every row but its last (so a defect that stops checking one
// row early shows up), verified with rewrite::verify_rule -- the same differential harness R0's own
// rules use (rewrite/verifier.hpp), never a check written to know about either mutant's defect.
// Then the M1 book and the Stage A tape: whichever domains the rule actually finds there.
#include <gtest/gtest.h>

#include <cstdint>
#include <iostream>

#include "epykos/fixtures/m1_book.hpp"
#include "epykos/fixtures/record_m1.hpp"
#include "epykos/ir/program.hpp"
#include "epykos/ir/signature.hpp"
#include "epykos/rewrite/r1_fold_uniform_columns.hpp"
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

// domain 0: "input", 4 rows, value ids 0..3 (mirrors tests/rewrite/rule_framework_test.cpp's own
// tiny_program shape). domain 1: 4 rows, value ids 4..7, group
//   step0 = Mul(gather -> domain 0 row r, column0 = {3,3,3,3})       -- uniform, read from `b`
//   step1 = Add(step0, column1 = {5,5,5,7})                          -- NOT uniform (last row: 7)
// so the domain's own row value is step1's, and only column0 should ever fold.
ir::Program synthetic_program() {
  ir::Program p;
  p.domains.push_back(ir::Domain{"input", 4, 0, 0, false, {}, false, -1});
  p.groups.push_back(ir::Group{0, {ir::Step{epykos::Op::Input, ir::Slot{ir::SlotKind::Input, -1}, {}, {}, {}}}});
  p.inputs = {0, 1, 2, 3};
  p.input_values = {10.0, 20.0, 30.0, 40.0};

  p.gathers.push_back(ir::Gather{1, {0, 1, 2, 3}});         // gather 0: row r reads domain 0 row r
  p.columns.push_back(ir::Column{1, {3.0, 3.0, 3.0, 3.0}}); // column 0: uniform
  p.columns.push_back(ir::Column{1, {5.0, 5.0, 5.0, 7.0}}); // column 1: uniform except the last row
  ir::Step step0{epykos::Op::Mul, ir::Slot{ir::SlotKind::Gather, 0}, ir::Slot{ir::SlotKind::Column, 0}, {}, {}};
  ir::Step step1{epykos::Op::Add, ir::Slot{ir::SlotKind::Step, 0}, ir::Slot{ir::SlotKind::Column, 1}, {}, {}};
  p.groups.push_back(ir::Group{1, {step0, step1}});
  p.domains.push_back(ir::Domain{"add(mul(@0,$0),$1)", 4, 4, 0, false, {0}, false, -1});

  p.outputs = {4, 5, 6, 7};
  ir::validate(p);
  return p;
}

}  // namespace

TEST(R1FoldUniformColumns, NameAndExactness) {
  const rewrite::R1FoldUniformColumns rule;
  EXPECT_EQ(rule.name(), "r1.fold_uniform_columns");
  EXPECT_EQ(rule.exactness_class(), rewrite::Exactness::E0);
}

TEST(R1FoldUniformColumns, SyntheticDomainFoldsOnlyTheUniformColumnAndCatchesBothMutants) {
  const ir::Program program = synthetic_program();
  const rewrite::R1FoldUniformColumns rule;

  const std::vector<rewrite::MatchSite> sites = rule.match(program, ir::PlanAnnotations{});
  ASSERT_EQ(sites.size(), 1u);
  EXPECT_EQ(sites.front().kind, rewrite::SiteKind::Domain);
  EXPECT_EQ(sites.front().domain, 1);

  const rewrite::Proposal p = rule.propose(program, ir::PlanAnnotations{}, sites.front());
  ASSERT_TRUE(p.is_structural());
  const ir::Program& after = *p.program;
  ir::validate(after);
  // Exactly one new literal (column 0's value); column 1 (not uniform) is untouched, still a
  // Column slot on step1 -- this is the direct, structural half of the check (r1.wrong_slot leaves
  // a WRONG operand replaced, not merely an unfolded one, so this alone would not catch it, but it
  // pins the intended shape of a correct fold before the differential check below does the rest).
  EXPECT_EQ(after.literals.size(), program.literals.size() + 1);
  const ir::Step& after_step1 = after.groups[1].steps[1];
  EXPECT_EQ(after_step1.b.kind, ir::SlotKind::Column);
  EXPECT_EQ(after_step1.b.index, 1);

  const std::vector<double> state = {10.0, 20.0, 30.0, 40.0};
  rewrite::VerifyOptions options;
  options.ball.draws = 8;
  const rewrite::RuleVerifyReport report = rewrite::verify_rule(rule, program, state.data(), 4, 4, ir::PlanAnnotations{}, options);
  EXPECT_GT(report.sites_checked, 0);
  EXPECT_TRUE(report.passed()) << report.report.summary();
}

// Finding (reported, not hidden -- rule.hpp point 4 / stub_rule.hpp's own framing): R1 never fires
// on a program fresh out of ir::infer. DESIGN.md §5.5's own column classification ("identical
// across instances -> literal (folded); varying -> data column") is already done by the signature
// pass itself (src/ir/signature.cpp's `const_slot`, bit-pattern uniform -> Literal else Column) --
// so no domain `ir::infer` ever produces can have a uniform Column left for R1 to find. R1 is not
// dead code: it exists for a Program a REWRITE has since changed underneath the signature pass's
// own classification -- concretely, R2 bucket rows, whose own point is that every column of a
// freshly-split bucket domain is uniform by construction (r2_bucket_rows.hpp's own header; this
// file's synthetic test above exercises R1's own fold directly, not that pairing -- R2's own
// safety gate, D49, currently keeps it from firing on either real fixture too, so the pairing is
// not currently observable end to end on either).
TEST(R1FoldUniformColumns, MatchesAndVerifiesOnTheM1Book) {
  const fixtures::Book book = fixtures::make_m1_book();
  const epykos::Tape tape = fixtures::record_m1(book);
  const ir::Program program = ir::infer(tape);
  const rewrite::R1FoldUniformColumns rule;

  const std::size_t n_sites = rule.match(program, ir::PlanAnnotations{}).size();
  std::cout << "[ r1 ] M1 book: " << n_sites << " of " << program.domains.size() << " domain(s) fold at least one column\n";
  EXPECT_EQ(n_sites, 0u) << "a domain straight out of ir::infer should never have a foldable column (see the test's own comment)";

  rewrite::VerifyOptions options;
  options.ball.draws = 8;
  const rewrite::RuleVerifyReport report =
      rewrite::verify_rule(rule, program, book.z0.data(), static_cast<int>(book.z0.size()), static_cast<int>(program.outputs.size()),
                           ir::PlanAnnotations{}, options);
  EXPECT_TRUE(report.passed()) << report.report.summary();
}

TEST(R1FoldUniformColumns, MatchesAndVerifiesOnTheStageATape) {
  const ir::Program program = ir::infer(epykos::test::stage_a_tape().tape);
  const rewrite::R1FoldUniformColumns rule;

  const std::vector<rewrite::MatchSite> sites = rule.match(program, ir::PlanAnnotations{});
  std::cout << "[ r1 ] Stage A: " << sites.size() << " of " << program.domains.size() << " domain(s) fold at least one column\n";
  EXPECT_EQ(sites.size(), 0u) << "a domain straight out of ir::infer should never have a foldable column (see the test's own comment)";

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
