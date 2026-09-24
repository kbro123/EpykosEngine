// M4/R-b: R5GroupFormation (rewrite/r5_group_formation.hpp), class E0.
//
// A hand-built program pins the exact target shape (a single-reader elementwise producer read by
// a plain identity gather, whose reader feeds a segment-sum) and is checked with
// rewrite::verify_rule. The rule is then run on the M1 book (expected: 0 -- M1's own domains are
// already fused by hand, DESIGN.md §1) and on the Stage A tape, where it is asserted to fire
// (measured: three producer/consumer pairs collapse, the largest one 15,358 rows, reported in the
// landing notes) and checked directly against exec::Interpreter / adjoint::Adjoint rather than
// through rewrite::compare_programs -- see the comment at that check's call site for why.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>

#include "epykos/adjoint/adjoint.hpp"
#include "epykos/exec/interpreter.hpp"
#include "epykos/fixtures/m1_book.hpp"
#include "epykos/fixtures/record_m1.hpp"
#include "epykos/ir/evaluate.hpp"
#include "epykos/ir/program.hpp"
#include "epykos/ir/signature.hpp"
#include "epykos/rewrite/greedy.hpp"
#include "epykos/rewrite/r5_group_formation.hpp"
#include "epykos/rewrite/verifier.hpp"
#include "epykos/tape/tape.hpp"
#include "stage_a/stage_a_test_helpers.hpp"

namespace ir = epykos::ir;
namespace rewrite = epykos::rewrite;
namespace fixtures = epykos::fixtures;
namespace exec = epykos::exec;
namespace adjoint = epykos::adjoint;

namespace {

std::uint64_t bits(double v) noexcept {
  std::uint64_t u;
  std::memcpy(&u, &v, sizeof u);
  return u;
}

// input(3 rows) -> P = mul(neg(@0),@1) [1 row, TWO steps: Neg then Mul -- deliberately more than
// one, so a mutant that points a consumer's replacement slot at the producer's FIRST step instead
// of its LAST (rewrite.r5.wrong_step_index) is actually distinguishable: with a single-step
// producer "first" and "last" name the same step] -> D = mul(@P,@in2) [1 row] -> Leg =
// sum(segment{D's row, a constant}) [1 row, a genuine segment-sum epilogue]. P has exactly one
// reader (D, via a bijective identity gather) and D feeds the Leg's segment: R5's exact target
// shape.
ir::Program producer_feeds_reduction_program() {
  ir::Program p;
  p.domains.push_back(ir::Domain{"input", 3, 0, 0, false, {}, false, -1});
  p.groups.push_back(ir::Group{0, {ir::Step{epykos::Op::Input, ir::Slot{ir::SlotKind::Input, -1}, {}, {}, {}}}});
  p.inputs = {0, 1, 2};
  p.input_values = {2.0, 3.0, 5.0};

  p.domains.push_back(ir::Domain{"mul(neg(@0),@1)", 1, 3, 0, false, {0}, false, -1});  // P: -in0*in1 = -6
  p.gathers.push_back(ir::Gather{1, {0}});  // @0 -> in0
  p.gathers.push_back(ir::Gather{1, {1}});  // @1 -> in1
  p.groups.push_back(ir::Group{
      1, {ir::Step{epykos::Op::Neg, ir::Slot{ir::SlotKind::Gather, 0}, {}, {}, {}},
          ir::Step{epykos::Op::Mul, ir::Slot{ir::SlotKind::Step, 0}, ir::Slot{ir::SlotKind::Gather, 1}, {}, {}}}});

  p.domains.push_back(ir::Domain{"mul(@0,@1)", 1, 4, 0, false, {0, 1}, false, -1});  // D: P*in2 = -30
  p.gathers.push_back(ir::Gather{2, {3}});  // identity into P's one row
  p.gathers.push_back(ir::Gather{2, {2}});  // in2
  p.groups.push_back(ir::Group{2, {ir::Step{epykos::Op::Mul, ir::Slot{ir::SlotKind::Gather, 2}, ir::Slot{ir::SlotKind::Gather, 3}, {}, {}}}});

  p.domains.push_back(ir::Domain{"sum(%0)", 1, 5, 0, false, {2}, false, -1});  // Leg: sum(D's row) = -30
  p.segments.push_back(ir::Segment{3, {0, 1}, {4}, {}});
  p.groups.push_back(ir::Group{3, {ir::Step{epykos::Op::Sum, ir::Slot{ir::SlotKind::Segment, 0}, {}, {}, {}}}});
  p.outputs = {5};
  return p;
}

TEST(R5GroupFormation, MatchesTheSingleReaderProducerFeedingAReduction) {
  const ir::Program program = producer_feeds_reduction_program();
  ir::validate(program);
  const rewrite::R5GroupFormation rule;
  const std::vector<rewrite::MatchSite> sites = rule.match(program, ir::PlanAnnotations{});
  ASSERT_EQ(sites.size(), 1u);
  EXPECT_EQ(sites[0].domain, 1);  // P
}

TEST(R5GroupFormation, RewriteAgreesAndMergesTheTwoDomains) {
  const ir::Program before = producer_feeds_reduction_program();
  const rewrite::R5GroupFormation rule;
  const std::vector<rewrite::MatchSite> sites = rule.match(before, ir::PlanAnnotations{});
  ASSERT_EQ(sites.size(), 1u);
  const rewrite::Proposal proposal = rule.propose(before, ir::PlanAnnotations{}, sites[0]);
  ASSERT_TRUE(proposal.is_structural());
  const ir::Program& after = *proposal.program;
  ir::validate(after);

  ASSERT_EQ(after.domains.size(), 3u);  // P absorbed into D
  ASSERT_EQ(after.groups[1].steps.size(), 3u);  // P's Neg + Mul, then D's own Mul
  // The merged group's Mul (D's own step) must read P's LAST step (the Mul, index 1) directly,
  // never its first (the Neg, index 0) -- exactly what rewrite.r5.wrong_step_index gets wrong.
  EXPECT_EQ(after.groups[1].steps[2].a.kind, ir::SlotKind::Step);
  EXPECT_EQ(after.groups[1].steps[2].a.index, 1);

  const std::vector<double> out_before = ir::evaluate(before, before.input_values);
  const std::vector<double> out_after = ir::evaluate(after, before.input_values);
  ASSERT_EQ(out_before.size(), 1u);
  EXPECT_EQ(out_before, out_after);
  EXPECT_DOUBLE_EQ(out_after[0], -30.0);

  EXPECT_TRUE(rule.match(after, ir::PlanAnnotations{}).empty());
}

TEST(R5GroupFormation, DoesNotFireOnTheM1Book) {
  const fixtures::Book book = fixtures::make_m1_book();
  const epykos::Tape tape = fixtures::record_m1(book);
  const ir::Program program = ir::infer(tape);
  const rewrite::R5GroupFormation rule;
  const std::vector<rewrite::MatchSite> sites = rule.match(program, ir::PlanAnnotations{});
  EXPECT_TRUE(sites.empty()) << sites.size() << " unexpected site(s) on the M1 book";
}

TEST(R5GroupFormation, FiresOnTheStageATapeAndVerifiesE0) {
  const fixtures::StageATape& tape = epykos::test::stage_a_tape();
  const ir::Program program = ir::infer(tape.tape);
  // The FULL recorded input vector (every Input-domain leaf: the 70 calibration quotes AND the
  // realised fixings recorded alongside them, Program::inputs order), NOT
  // StageATape::record_quotes() (a 70-entry SUBSET, the quotes alone) -- exec::Interpreter::run /
  // adjoint::Adjoint::run take exactly `program.inputs.size()` entries and, per their own
  // contract, allocate nothing to check the length: passing the 70-entry subset here silently
  // read/wrote past the end of that shorter buffer (a real bug this test's own development
  // caught, as a SIGABRT from heap-corruption well after the last out-of-bounds write, in the
  // reverse-mode loop below).
  const std::vector<double>& state = program.input_values;
  const int n_inputs = static_cast<int>(program.inputs.size());
  const int n_outputs = static_cast<int>(program.outputs.size());

  const rewrite::R5GroupFormation rule;
  ir::Program working = program;
  ir::PlanAnnotations plan;
  int applications = 0;
  std::size_t applied = 0;
  do {
    applied = rewrite::apply_greedy(rule, working, plan);
    applications += static_cast<int>(applied);
  } while (applied > 0);

  std::cout << "[ r5       ] " << applications << " producer/consumer pairs merged on the Stage A tape ("
            << program.domains.size() << " -> " << working.domains.size() << " domains, "
            << program.num_values() << " -> " << working.num_values() << " recorded values)\n";
  EXPECT_GT(applications, 0) << "expected at least the measured single-reader-into-a-reduction pairs to fire";

  // Not rewrite::compare_programs / verify_rule here: Stage A's own tape embeds an implicit
  // calibration solve, and comparing a SINGLE-state reference run (B=1) against a BATCHED
  // "compiled" run (verify::differential's own convention, B = min(ball draws, max_batch)) makes
  // the calibration's Newton iteration count -- and so almost every one of the 8,191 outputs --
  // sensitive to which batch width evaluated it, for the UNCHANGED program as much as the
  // rewritten one (measured: comparing `program` against itself this way reports 8,147/8,191
  // "mismatches"; not a property of this rule). The gate that actually isolates this rule's own
  // effect is a same-batch-width (B=1 both sides) bitwise comparison at the record point plus a
  // seeded reverse-mode check, run below directly against exec::Interpreter / adjoint::Adjoint.
  {
    exec::Interpreter interp_before(program, {});
    exec::Interpreter interp_after(working, {});
    std::vector<double> out_before(static_cast<std::size_t>(n_outputs)), out_after(static_cast<std::size_t>(n_outputs));
    interp_before.run(state.data(), 1, out_before.data());
    interp_after.run(state.data(), 1, out_after.data());
    long mismatches = 0;
    for (int i = 0; i < n_outputs; ++i) {
      if (bits(out_before[static_cast<std::size_t>(i)]) != bits(out_after[static_cast<std::size_t>(i)])) ++mismatches;
    }
    EXPECT_EQ(mismatches, 0) << mismatches << " / " << n_outputs << " outputs differ at the record point (B=1 both sides)";

    adjoint::Adjoint adj_before(program, {});
    adjoint::Adjoint adj_after(working, {});
    std::vector<double> out_bar(static_cast<std::size_t>(n_outputs), 0.0), sb_before(static_cast<std::size_t>(n_inputs)),
        sb_after(static_cast<std::size_t>(n_inputs)), tmp_out(static_cast<std::size_t>(n_outputs));
    // A handful of evenly-spaced seeded output directions rather than every one of the 8,191 (the
    // reverse pass is the expensive half of this check; the forward comparison above already
    // covers every output).
    const int n_seeds = std::min(n_outputs, 16);
    for (int k = 0; k < n_seeds; ++k) {
      const int o = static_cast<int>((static_cast<long>(k) * n_outputs) / n_seeds);
      std::fill(out_bar.begin(), out_bar.end(), 0.0);
      out_bar[static_cast<std::size_t>(o)] = 1.0;
      adj_before.run(state.data(), 1, out_bar.data(), tmp_out.data(), sb_before.data());
      adj_after.run(state.data(), 1, out_bar.data(), tmp_out.data(), sb_after.data());
      for (int i = 0; i < n_inputs; ++i) {
        EXPECT_EQ(bits(sb_before[static_cast<std::size_t>(i)]), bits(sb_after[static_cast<std::size_t>(i)]))
            << "output " << o << " input " << i;
      }
    }
  }
}

}  // namespace
