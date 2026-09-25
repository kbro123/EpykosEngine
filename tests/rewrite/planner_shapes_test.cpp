// M4/R0: unit tests of rewrite/planner.hpp's shape-matching pure functions (match_pair,
// match_tail, step_uses, is_whole_segment, row_fusion_pays) against small, hand-built
// ir::Group / ir::Step values — no full ir::Program needed, since these functions read only the
// group's own steps and slot kinds. See tests/rewrite/planner_rules_m1_test.cpp for the
// whole-program decisions (reduction_fusion / inline_producers / emit_outputs) on a real fixture.
#include <gtest/gtest.h>

#include "epykos/ir/program.hpp"
#include "epykos/rewrite/planner.hpp"

namespace ir = epykos::ir;
namespace planner = epykos::rewrite::planner;
using epykos::Op;

namespace {

ir::Slot step(std::int32_t i) { return ir::Slot{ir::SlotKind::Step, i}; }
ir::Slot lit(std::int32_t i) { return ir::Slot{ir::SlotKind::Literal, i}; }
ir::Slot col(std::int32_t i) { return ir::Slot{ir::SlotKind::Column, i}; }
ir::Slot gat(std::int32_t i) { return ir::Slot{ir::SlotKind::Gather, i}; }

}  // namespace

TEST(PlannerShapes, RowFusionPaysAtOneOrAtLeastSixteen) {
  EXPECT_TRUE(planner::row_fusion_pays(1));
  EXPECT_FALSE(planner::row_fusion_pays(2));
  EXPECT_FALSE(planner::row_fusion_pays(4));
  EXPECT_FALSE(planner::row_fusion_pays(8));
  EXPECT_FALSE(planner::row_fusion_pays(15));
  EXPECT_TRUE(planner::row_fusion_pays(16));
  EXPECT_TRUE(planner::row_fusion_pays(32));
  EXPECT_TRUE(planner::row_fusion_pays(64));
}

TEST(PlannerShapes, IsWholeSegmentOnlyForASoleSumOrAffineOverASegment) {
  ir::Group one_sum;
  one_sum.steps = {ir::Step{Op::Sum, ir::Slot{ir::SlotKind::Segment, 0}, {}, {}, {}}};
  EXPECT_TRUE(planner::is_whole_segment(one_sum));

  ir::Group one_affine;
  one_affine.steps = {ir::Step{Op::Affine, ir::Slot{ir::SlotKind::Segment, 0}, {}, {}, col(0)}};
  EXPECT_TRUE(planner::is_whole_segment(one_affine));

  ir::Group two_steps;
  two_steps.steps = {ir::Step{Op::Sum, ir::Slot{ir::SlotKind::Segment, 0}, {}, {}, {}},
                     ir::Step{Op::Mul, step(0), lit(0), {}, {}}};
  EXPECT_FALSE(planner::is_whole_segment(two_steps));

  ir::Group not_a_segment;
  not_a_segment.steps = {ir::Step{Op::Add, gat(0), gat(1), {}, {}}};
  EXPECT_FALSE(planner::is_whole_segment(not_a_segment));
}

TEST(PlannerShapes, StepUsesCountsStepOperandReferences) {
  ir::Group g;
  g.steps = {ir::Step{Op::Sub, gat(0), gat(1), {}, {}},          // step 0
             ir::Step{Op::Mul, step(0), col(0), {}, {}},         // step 1: reads step 0 once
             ir::Step{Op::Add, step(1), step(1), {}, {}}};       // step 2: reads step 1 twice
  const std::vector<int> uses = planner::step_uses(g);
  ASSERT_EQ(uses.size(), 3u);
  EXPECT_EQ(uses[0], 1);
  EXPECT_EQ(uses[1], 2);
  EXPECT_EQ(uses[2], 0);
}

// sub(gather, gather) then mul(step0, literal): a two-gather, non-commutative first op with a
// scalar second operand -- the exact shape DESIGN.md §7 calls a fused pair.
TEST(PlannerShapes, MatchPairAcceptsGatherGatherThenScalar) {
  ir::Group g;
  g.steps = {ir::Step{Op::Sub, gat(0), gat(1), {}, {}}, ir::Step{Op::Mul, step(0), lit(0), {}, {}}};
  const std::vector<int> uses = planner::step_uses(g);
  const std::optional<planner::PairShape> shape = planner::match_pair(g, uses, 0);
  ASSERT_TRUE(shape.has_value());
  EXPECT_EQ(shape->op1, Op::Sub);
  EXPECT_EQ(shape->op2, Op::Mul);
  EXPECT_EQ(shape->ka, planner::OperandKind::Gathered);
  EXPECT_EQ(shape->kb, planner::OperandKind::Gathered);
  EXPECT_EQ(shape->kc, planner::OperandKind::Scalar);
  EXPECT_FALSE(shape->prev_right);
}

// A commutative first op with (gather, scalar) operands is swap-normalised to (scalar, gather).
TEST(PlannerShapes, MatchPairSwapNormalisesCommutativeFirstOp) {
  ir::Group g;
  g.steps = {ir::Step{Op::Add, gat(0), col(0), {}, {}}, ir::Step{Op::Mul, step(0), lit(0), {}, {}}};
  const std::vector<int> uses = planner::step_uses(g);
  const std::optional<planner::PairShape> shape = planner::match_pair(g, uses, 0);
  ASSERT_TRUE(shape.has_value());
  EXPECT_EQ(shape->ka, planner::OperandKind::Scalar);
  EXPECT_EQ(shape->kb, planner::OperandKind::Gathered);
  EXPECT_EQ(shape->a, col(0));
  EXPECT_EQ(shape->b, gat(0));
}

// A non-commutative second op whose "other" operand is on the LEFT sets prev_right.
TEST(PlannerShapes, MatchPairSetsPrevRightForNonCommutativeLeftOperand) {
  ir::Group g;
  g.steps = {ir::Step{Op::Mul, gat(0), gat(1), {}, {}}, ir::Step{Op::Div, col(0), step(0), {}, {}}};
  const std::vector<int> uses = planner::step_uses(g);
  const std::optional<planner::PairShape> shape = planner::match_pair(g, uses, 0);
  ASSERT_TRUE(shape.has_value());
  EXPECT_EQ(shape->op2, Op::Div);
  EXPECT_TRUE(shape->prev_right);
}

TEST(PlannerShapes, MatchPairRejectsWhenTheFirstStepIsReadTwice) {
  ir::Group g;
  g.steps = {ir::Step{Op::Sub, gat(0), gat(1), {}, {}}, ir::Step{Op::Mul, step(0), step(0), {}, {}}};
  // step 1 reads step 0 twice via a and b both -- match_pair's own uses[k]!=1 gate needs the
  // CALLER's uses array (computed over the whole group) to reflect this.
  std::vector<int> uses(2, 0);
  uses[0] = 2;
  EXPECT_FALSE(planner::match_pair(g, uses, 0).has_value());
}

TEST(PlannerShapes, MatchPairRejectsTwoScalarOperands) {
  ir::Group g;
  g.steps = {ir::Step{Op::Add, lit(0), col(0), {}, {}}, ir::Step{Op::Mul, step(0), gat(0), {}, {}}};
  std::vector<int> uses(2, 1);
  EXPECT_FALSE(planner::match_pair(g, uses, 0).has_value());
}

TEST(PlannerShapes, MatchPairRejectsAnExpFirstStep) {
  ir::Group g;
  g.steps = {ir::Step{Op::Exp, gat(0), {}, {}, {}}, ir::Step{Op::Mul, step(0), lit(0), {}, {}}};
  std::vector<int> uses(2, 1);
  EXPECT_FALSE(planner::match_pair(g, uses, 0).has_value());
}

TEST(PlannerShapes, MatchPairRejectsAtTheLastStep) {
  ir::Group g;
  g.steps = {ir::Step{Op::Sub, gat(0), gat(1), {}, {}}};
  std::vector<int> uses(1, 0);
  EXPECT_FALSE(planner::match_pair(g, uses, 0).has_value());
}

TEST(PlannerShapes, MatchTailAcceptsExpOrLogOfThePreviousStepOnly) {
  const ir::Step exp_of_prev{Op::Exp, step(3), {}, {}, {}};
  EXPECT_TRUE(planner::match_tail(exp_of_prev, 3, 0, 2));
  const ir::Step log_of_prev{Op::Log, step(3), {}, {}, {}};
  EXPECT_TRUE(planner::match_tail(log_of_prev, 3, 0, 2));
  const ir::Step exp_of_other{Op::Exp, step(2), {}, {}, {}};
  EXPECT_FALSE(planner::match_tail(exp_of_other, 3, 0, 2));
  const ir::Step add_of_prev{Op::Add, step(3), lit(0), {}, {}};
  EXPECT_FALSE(planner::match_tail(add_of_prev, 3, 0, 2));
  EXPECT_FALSE(planner::match_tail(exp_of_prev, 3, /*n_tail_so_far=*/2, /*max_tail=*/2));
}
