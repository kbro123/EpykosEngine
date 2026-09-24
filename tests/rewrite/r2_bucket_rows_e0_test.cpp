// M4/R-a: R2 bucket rows by uniform-column signature (rewrite/r2_bucket_rows.hpp, DESIGN.md §6,
// class E0).
//
// The synthetic domain below has one column, {1,1,2,2,2}: two contiguous runs (rows 0-1, rows
// 2-4), the exact shape r2_bucket_rows.hpp's own header describes. Both of R2's mutants are
// caught by asserting the EXACT bucket structure this produces (r2.wrong_run_boundary changes it
// to three buckets of sizes {2,1,2}; r2.column_slice_uses_wrong_bucket gives bucket 1 a 2-element
// column where 3 are needed, which ir::validate rejects) and, on top of that, a value-level
// differential check via rewrite::verify_rule against the SAME synthetic program.
//
// Finding (reported, not hidden -- run_buckets's own "no singleton run" comment in
// r2_bucket_rows.cpp has the full account): with that safety gate in place, R2 does not fire on
// EITHER real fixture below. Measured WITHOUT the gate (a diagnostic build, not what ships): it
// fires on 5 of the M1 book's 10 domains and passes bit-exact, interpreter and adjoint, at the
// record point AND over the M1 differential ball, despite splitting into thousands of mostly
// singleton buckets (e.g. one 16,103-row domain into 9,152 buckets) -- so singleton buckets are
// not unsound in general. On the Stage A tape it also matches (23 of 67 domains) but crashes
// building one matched domain's adjoint::Adjoint, a fault this package could not chase down to its
// root cause (in src/adjoint/, which R-a does not own) in its own time budget; Stage A has scan
// domains the M1 book has none of, which may be the actual difference. The gate closes off the
// whole shape rather than ship a rule that can crash -- see notes for the reproduction.
#include <gtest/gtest.h>

#include <iostream>

#include "epykos/fixtures/m1_book.hpp"
#include "epykos/fixtures/record_m1.hpp"
#include "epykos/ir/program.hpp"
#include "epykos/ir/signature.hpp"
#include "epykos/rewrite/r2_bucket_rows.hpp"
#include "epykos/rewrite/verifier.hpp"
#include "epykos/tape/tape.hpp"
#include "record_point_check.hpp"
#include "stage_a/stage_a_test_helpers.hpp"

#ifndef EPYKOS_FP_CONTRACT_OFF
#error "an _e0_test.cpp TU must see EPYKOS_FP_CONTRACT_OFF"
#endif

namespace ir = epykos::ir;
namespace rewrite = epykos::rewrite;
namespace fixtures = epykos::fixtures;

namespace {

// domain 0: "input", 5 rows, value ids 0..4. domain 1: 5 rows, value ids 5..9, ONE step
// Add(gather -> domain 0 row r, column {1,1,2,2,2}): two contiguous runs by that column's value.
ir::Program synthetic_program() {
  ir::Program p;
  p.domains.push_back(ir::Domain{"input", 5, 0, 0, false, {}, false, -1});
  p.groups.push_back(ir::Group{0, {ir::Step{epykos::Op::Input, ir::Slot{ir::SlotKind::Input, -1}, {}, {}, {}}}});
  p.inputs = {0, 1, 2, 3, 4};
  p.input_values = {10.0, 20.0, 30.0, 40.0, 50.0};

  p.gathers.push_back(ir::Gather{1, {0, 1, 2, 3, 4}});
  p.columns.push_back(ir::Column{1, {1.0, 1.0, 2.0, 2.0, 2.0}});
  ir::Step step{epykos::Op::Add, ir::Slot{ir::SlotKind::Gather, 0}, ir::Slot{ir::SlotKind::Column, 0}, {}, {}};
  p.groups.push_back(ir::Group{1, {step}});
  p.domains.push_back(ir::Domain{"add(@0,$0)", 5, 5, 0, false, {0}, false, -1});

  p.outputs = {5, 6, 7, 8, 9};
  ir::validate(p);
  return p;
}

}  // namespace

TEST(R2BucketRows, NameAndExactness) {
  const rewrite::R2BucketRows rule;
  EXPECT_EQ(rule.name(), "r2.bucket_rows");
  EXPECT_EQ(rule.exactness_class(), rewrite::Exactness::E0);
}

TEST(R2BucketRows, SyntheticDomainSplitsIntoTwoContiguousRunsAndCatchesBothMutants) {
  const ir::Program program = synthetic_program();
  const rewrite::R2BucketRows rule;

  const std::vector<rewrite::MatchSite> sites = rule.match(program, ir::PlanAnnotations{});
  ASSERT_EQ(sites.size(), 1u);
  EXPECT_EQ(sites.front().domain, 1);

  const rewrite::Proposal p = rule.propose(program, ir::PlanAnnotations{}, sites.front());
  ASSERT_TRUE(p.is_structural());
  const ir::Program& after = *p.program;
  EXPECT_NO_THROW(ir::validate(after));  // r2.column_slice_uses_wrong_bucket: a mis-sized column

  // Exactly one extra domain (2 buckets replacing 1), rows {2, 3} in that order -- pins the exact
  // partition (r2.wrong_run_boundary produces 3 buckets of {2, 1, 2} instead).
  ASSERT_EQ(after.domains.size(), program.domains.size() + 1);
  EXPECT_EQ(after.domains[1].rows, 2);
  EXPECT_EQ(after.domains[1].value_base, 5);
  EXPECT_EQ(after.domains[2].rows, 3);
  EXPECT_EQ(after.domains[2].value_base, 7);
  // Domain 0 (before the split) and whatever follows are otherwise untouched.
  EXPECT_EQ(after.domains[0].rows, 5);
  EXPECT_EQ(after.outputs, program.outputs);  // value ids never change (this landing's own scope note)

  const std::vector<double> state = {10.0, 20.0, 30.0, 40.0, 50.0};
  rewrite::VerifyOptions options;
  options.ball.draws = 8;
  const rewrite::RuleVerifyReport report = rewrite::verify_rule(rule, program, state.data(), 5, 5, ir::PlanAnnotations{}, options);
  EXPECT_GT(report.sites_checked, 0);
  EXPECT_TRUE(report.passed()) << report.report.summary();
}

// Finding (reported, not hidden -- see this file's own header comment): with the "no singleton
// run" safety gate in place, R2 does not currently fire on the M1 book (it would, and correctly,
// without that gate). verify_rule trivially passes with zero sites; this test exists so a future
// change to the gate is checked here too, not just on the synthetic domain above.
TEST(R2BucketRows, MatchesAndVerifiesOnTheM1Book) {
  const fixtures::Book book = fixtures::make_m1_book();
  const epykos::Tape tape = fixtures::record_m1(book);
  const ir::Program program = ir::infer(tape);
  const rewrite::R2BucketRows rule;

  const std::size_t n_sites = rule.match(program, ir::PlanAnnotations{}).size();
  std::cout << "[ r2 ] M1 book: " << n_sites << " of " << program.domains.size() << " domain(s) split into contiguous-run buckets\n";

  rewrite::VerifyOptions options;
  options.ball.draws = 8;
  const rewrite::RuleVerifyReport report =
      rewrite::verify_rule(rule, program, book.z0.data(), static_cast<int>(book.z0.size()), static_cast<int>(program.outputs.size()),
                           ir::PlanAnnotations{}, options);
  EXPECT_TRUE(report.passed()) << report.report.summary();
}

// Finding (reported, not hidden): with the safety gate, R2 does not fire on the Stage A tape
// either -- see this file's own header comment for what it fires on and crashes on without the
// gate. compare_at_record_point (record_point_check.hpp), not rewrite::verify_rule, is used here
// on principle even though sites_checked is 0: this tape's Inputs are quotes an implicit block
// calibrates from, so a ball perturbation (verify_rule's own check) is not a state the tape is
// self-consistent at, only the exact record point is (see record_point_check.hpp's header).
TEST(R2BucketRows, MatchesAndVerifiesOnTheStageATape) {
  const ir::Program program = ir::infer(epykos::test::stage_a_tape().tape);
  const rewrite::R2BucketRows rule;

  const std::vector<rewrite::MatchSite> sites = rule.match(program, ir::PlanAnnotations{});
  std::cout << "[ r2 ] Stage A: " << sites.size() << " of " << program.domains.size() << " domain(s) split into contiguous-run buckets\n";

  const std::vector<double> quotes = epykos::test::stage_a_tape().record_quotes();
  for (const rewrite::MatchSite& site : sites) {
    const rewrite::Proposal p = rule.propose(program, ir::PlanAnnotations{}, site);
    ASSERT_TRUE(p.is_structural());
    ir::validate(*p.program);
    const auto report = epykos::test::compare_at_record_point(program, *p.program, quotes.data(), static_cast<int>(quotes.size()),
                                                               static_cast<int>(program.outputs.size()));
    EXPECT_TRUE(report.passed) << "domain " << site.domain << ": " << report.detail;
  }
}
