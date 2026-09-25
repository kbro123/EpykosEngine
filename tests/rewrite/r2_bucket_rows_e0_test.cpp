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
// D65 replaced R2's gate. The old one ("every bucket has at least 2 rows") existed because a
// singleton-heavy Stage A split reproducibly aborted with heap corruption; the fault was in THIS
// FILE's own call into compare_at_record_point, not in src/adjoint/ (record_point_check.hpp's
// header has the account). The new one is two structural floors (run_buckets' own comment): the
// split must consolidate something, and it must be a PER-KIND partition -- every distinct
// signature in exactly one contiguous run. Measured fire-counts: 0 of the M1 book's 10 domains
// (all five of its bucketable domains have their kinds interleaved) and 3 of the Stage A tape's
// 67, every one of those verified bit-exact, interpreter and adjoint.
//
// Measured on the way there, and recorded in D65 because it is the more useful finding: with the
// per-kind floor OFF -- i.e. the pure soundness gate D52 proposed -- R2 fires on 5 of 10 and 23 of
// 67, every site still bit-exact, but what it produces is fragmentation, not batching (M1 domain
// 4: 2,432 rows, 2 kinds, 1,243 runs), and the e-graph cannot absorb it.
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

// The same two domains, but every row of the signature column is distinct: maximal contiguous
// runs are 5 singletons, so the split would consolidate nothing -- one one-row domain per row,
// strictly more IR describing the same maths. run_buckets' floor (1) rejects it and match()
// reports no site (mutant r2.accepts_full_singleton_split drops that floor). The split is
// value-preserving, so no differential check can see this: the match count is the only witness.
ir::Program all_distinct_program() {
  ir::Program p = synthetic_program();
  p.columns[0].values = {1.0, 2.0, 3.0, 4.0, 5.0};
  ir::validate(p);
  return p;
}

// Six rows, TWO kinds, INTERLEAVED: {1,1,2,2,1,1} is 3 contiguous runs over 2 distinct signatures,
// so a contiguous-run split fragments kind 1 across two domains instead of batching it. That is
// not the per-kind partition DESIGN.md §6 asks R2 to form; run_buckets' floor (2) rejects it
// (mutant r2.accepts_fragmented_split drops that floor). Also value-preserving: match count only.
ir::Program interleaved_kinds_program() {
  ir::Program p;
  p.domains.push_back(ir::Domain{"input", 6, 0, 0, false, {}, false, -1});
  p.groups.push_back(ir::Group{0, {ir::Step{epykos::Op::Input, ir::Slot{ir::SlotKind::Input, -1}, {}, {}, {}}}});
  p.inputs = {0, 1, 2, 3, 4, 5};
  p.input_values = {10.0, 20.0, 30.0, 40.0, 50.0, 60.0};

  p.gathers.push_back(ir::Gather{1, {0, 1, 2, 3, 4, 5}});
  p.columns.push_back(ir::Column{1, {1.0, 1.0, 2.0, 2.0, 1.0, 1.0}});
  p.groups.push_back(
      ir::Group{1, {ir::Step{epykos::Op::Add, ir::Slot{ir::SlotKind::Gather, 0}, ir::Slot{ir::SlotKind::Column, 0}, {}, {}}}});
  p.domains.push_back(ir::Domain{"add(@0,$0)", 6, 6, 0, false, {0}, false, -1});

  p.outputs = {6, 7, 8, 9, 10, 11};
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

TEST(R2BucketRows, AllDistinctRowsConsolidateNothingSoTheRuleReportsNoSite) {
  const ir::Program program = all_distinct_program();
  const rewrite::R2BucketRows rule;
  EXPECT_TRUE(rule.match(program, ir::PlanAnnotations{}).empty())
      << "a domain whose every row has a distinct signature would split into one one-row domain "
         "per row: more IR, no batch formed (mutant r2.accepts_full_singleton_split)";
}

TEST(R2BucketRows, InterleavedKindsAreNotAPerKindPartitionSoTheRuleReportsNoSite) {
  const ir::Program program = interleaved_kinds_program();
  const rewrite::R2BucketRows rule;
  EXPECT_TRUE(rule.match(program, ir::PlanAnnotations{}).empty())
      << "{1,1,2,2,1,1} is 3 contiguous runs over 2 kinds: a contiguous-run split fragments kind 1 "
         "rather than batching it (mutant r2.accepts_fragmented_split)";
}

// The M1 book: R2 fires on 0 of its 10 domains. Five of them ARE bucketable by contiguous run,
// but none of those runs is a per-kind partition -- measured (D65) rows / runs / kinds: domain 3
// 16,103 / 9,152 / 2,169, domain 4 2,432 / 1,243 / 2, domain 5 15,703 / 8,752 / 1,953, domain 7
// 969 / 454 / 2, domain 8 31 / 18 / 2. The kinds are there and the recording order interleaves
// them, which is R2's own documented scope limit (it buckets contiguous runs, it does not sort).
// verify_rule below is therefore a checked no-op; it stays so that a future R2 that DOES fire here
// is checked by this gate rather than a new one. Note also that the M1 book's own state vector
// (book.z0) has exactly as many entries as the program has Inputs, which is why the buffer-sizing
// defect D65 found on the Stage A side was never reachable from here.
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

// R2 fires on the Stage A tape and every site it reports is bit-exact, forward AND adjoint, at
// the record point. This is also the regression gate for the crash D52 point 4 walled the whole
// singleton shape off for: EVERY site's split program has its adjoint::Adjoint built and run here,
// and site 0 is the exact 177-row -> 168-bucket (159 singleton) domain that reproduction named.
// The fault was never in src/adjoint/ -- it was this test handing compare_at_record_point the
// 70-quote subset as a 148-Input state and state_bar buffer (D65, record_point_check.hpp's header,
// which now derives both from the Program so no caller can repeat it).
//
// compare_at_record_point, not rewrite::verify_rule: this tape's Inputs feed an implicit block the
// book's knots are calibrated from, so a ball perturbation is not a state the tape is
// self-consistent at, only the exact record point is (see record_point_check.hpp's header).
TEST(R2BucketRows, MatchesAndVerifiesOnTheStageATape) {
  const ir::Program program = ir::infer(epykos::test::stage_a_tape().tape);
  const rewrite::R2BucketRows rule;

  const std::vector<rewrite::MatchSite> sites = rule.match(program, ir::PlanAnnotations{});
  std::cout << "[ r2 ] Stage A: " << sites.size() << " of " << program.domains.size() << " domain(s) split into contiguous-run buckets\n";
  EXPECT_GT(sites.size(), 0u) << "R2 is expected to fire on the Stage A tape (D65); a zero here is a finding, not a pass";

  bool saw_singleton_bucket_split = false;
  for (const rewrite::MatchSite& site : sites) {
    const rewrite::Proposal p = rule.propose(program, ir::PlanAnnotations{}, site);
    ASSERT_TRUE(p.is_structural());
    ir::validate(*p.program);
    // The crash shape itself: a split whose buckets are mostly single rows. Keyed on the IR the
    // rule produced (buckets > rows/2), never on a fixture's domain id or row count.
    const std::int32_t rows = program.domains[static_cast<std::size_t>(site.domain)].rows;
    const std::int32_t buckets = static_cast<std::int32_t>(p.program->domains.size() - program.domains.size()) + 1;
    if (buckets * 2 > rows) saw_singleton_bucket_split = true;
    const auto report = epykos::test::compare_at_record_point(program, *p.program);
    EXPECT_TRUE(report.passed) << "domain " << site.domain << " (" << rows << " rows -> " << buckets << " buckets): " << report.detail;
  }
  EXPECT_TRUE(saw_singleton_bucket_split) << "no mostly-singleton split among the sites: the crash shape is no longer covered here";
}
