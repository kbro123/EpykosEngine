// M4/EG "integration" gate: rewrite::ADModePerBlockRule (rewrite/ad_mode_rule.hpp) and its
// consumer adjoint::block_jacobian (adjoint/block_jacobian.hpp) — CLAUDE.md's "every rewrite
// ships with its differential test AND its mutation test", applied to the AD-mode slot D47 left
// unconsumed (see both headers' own comments, and D50 point 5).
//
// A small hand-built two-block program (the same "hand-rolled Program, just enough for
// ir::validate" convention as tests/optimise/egraph_test.cpp / rule_framework_test.cpp):
//   domain 0  3 Inputs x0, x1, x2
//   domain 1  an AFFINE (linmap) domain, 2 rows, reading domain 0 directly:
//               row 0 = 0 + 2*x0 + 3*x1
//               row 1 = 1 + 4*x1 + 5*x2
//             — ClosedFormAffine-eligible (rewrite::is_linmap_domain, members are Inputs)
//   domain 2  ONE nonlinear row: row0 = domain1.row0 * domain1.row1 (a bilinear function of
//             x0, x1, x2) — NOT affine, so ClosedFormAffine must never be chosen for it.
// Outputs: [domain1.row0, domain1.row1, domain2.row0] (ordinals 0, 1, 2).
//
// "affine_block" = {domain 1}, n_inputs = 3, n_outputs = 2: ClosedFormAffine costs one dense
// 3x2 product (2ns/entry x 6 = 12ns at CostCoefficients::defaults()); forward costs one_pass_ns x
// 3, reverse one_pass_ns x 8 x 2 = 16 x one_pass_ns — affine wins by construction whenever
// one_pass_ns is not implausibly tiny (checked, not assumed, below).
// "nonlinear_block" = {domain 2}, n_inputs = 3, n_outputs = 1: not affine-eligible; forward
// (one_pass_ns x 3) is cheaper than reverse (one_pass_ns x 8) — the SAME "few outputs, many
// inputs favours forward" shape RESUME.md's own M3/G5 entry measured on Stage A's real risk
// ladder (6.3x), reproduced here at toy scale so the rule's own arithmetic, not a real machine's
// timing, is what the test pins down.
#include "epykos/rewrite/ad_mode_rule.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

#include "epykos/adjoint/adjoint.hpp"
#include "epykos/adjoint/block_jacobian.hpp"
#include "epykos/exec/interpreter.hpp"
#include "epykos/ir/program.hpp"
#include "epykos/optimise/cost.hpp"
#include "epykos/rewrite/greedy.hpp"
#include "epykos/rewrite/rule.hpp"

namespace ir = epykos::ir;
namespace rewrite = epykos::rewrite;
namespace optimise = epykos::optimise;
namespace adjoint = epykos::adjoint;
namespace exec = epykos::exec;
using epykos::Op;

namespace {

ir::Program mixed_program() {
  ir::Program p;
  p.domains.push_back(ir::Domain{"input", 3, 0, 0, false, {}, false, -1});
  p.groups.push_back(ir::Group{0, {ir::Step{Op::Input, ir::Slot{ir::SlotKind::Input, -1}, {}, {}, {}}}});
  p.inputs = {0, 1, 2};
  p.input_values = {1.0, 2.0, 3.0};

  p.columns.push_back(ir::Column{1, {0.0, 1.0}});
  p.segments.push_back(ir::Segment{1, {0, 2, 4}, {0, 1, 1, 2}, {2.0, 3.0, 4.0, 5.0}});
  p.domains.push_back(ir::Domain{"affine_linmap", 2, 3, 0, false, {0}, false, -1});
  p.groups.push_back(
      ir::Group{1, {ir::Step{Op::Affine, ir::Slot{ir::SlotKind::Segment, 0}, {}, {}, ir::Slot{ir::SlotKind::Column, 0}}}});

  p.gathers.push_back(ir::Gather{2, {3}});
  p.gathers.push_back(ir::Gather{2, {4}});
  p.domains.push_back(ir::Domain{"mul", 1, 5, 0, false, {1}, false, -1});
  p.groups.push_back(ir::Group{2, {ir::Step{Op::Mul, ir::Slot{ir::SlotKind::Gather, 0}, ir::Slot{ir::SlotKind::Gather, 1}, {}, {}}}});

  p.outputs = {3, 4, 5};
  ir::validate(p);
  return p;
}

optimise::CostModel test_model() { return optimise::CostModel{optimise::CostCoefficients::defaults(), "test", false}; }

std::vector<rewrite::JacobianBlockSpec> two_blocks() {
  return {
      rewrite::JacobianBlockSpec{"affine_block", {1}, /*n_inputs=*/3, /*n_outputs=*/2},
      rewrite::JacobianBlockSpec{"nonlinear_block", {2}, /*n_inputs=*/3, /*n_outputs=*/1},
  };
}

TEST(ADModeRule, MatchesOneSitePerUndecidedBlockAndReachesAFixpoint) {
  const ir::Program program = mixed_program();
  const rewrite::ADModePerBlockRule rule(two_blocks(), test_model());
  const std::vector<rewrite::MatchSite> sites = rule.match(program, ir::PlanAnnotations{});
  ASSERT_EQ(sites.size(), 2u);
  EXPECT_EQ(sites[0].kind, rewrite::SiteKind::Domain);
  EXPECT_EQ(sites[0].domain, 1);
  EXPECT_EQ(sites[1].domain, 2);

  ir::Program mutable_program = program;
  ir::PlanAnnotations plan;
  ASSERT_EQ(rewrite::apply_greedy(rule, mutable_program, plan), 2u);
  EXPECT_TRUE(rule.match(mutable_program, plan).empty()) << "both blocks decided: no further site to propose";
}

TEST(ADModeRule, PicksTheCheapestModePerBlock) {
  const ir::Program program = mixed_program();
  const rewrite::ADModePerBlockRule rule(two_blocks(), test_model());
  ir::Program planned = program;
  ir::PlanAnnotations plan;
  rewrite::apply_greedy(rule, planned, plan);

  ASSERT_EQ(plan.jacobian.mode.count("affine_block"), 1u);
  ASSERT_EQ(plan.jacobian.mode.count("nonlinear_block"), 1u);
  EXPECT_EQ(plan.jacobian.mode.at("affine_block"), ir::AdMode::ClosedFormAffine)
      << "a 2-output linmap block should always be cheaper as a dense 3x2 product than any repeated evaluation";
  EXPECT_EQ(plan.jacobian.mode.at("nonlinear_block"), ir::AdMode::Forward)
      << "3 inputs vs 1 output: forward (3 passes) must undercut reverse (8 adjoint lanes) at the default adjoint_multiplier";

  const rewrite::ADModeDecision affine_decision = rewrite::decide_ad_mode(program, two_blocks()[0], /*one_pass_ns=*/1000.0, test_model());
  EXPECT_TRUE(affine_decision.affine_eligible);
  EXPECT_LT(affine_decision.affine_ns, affine_decision.forward_ns);
  EXPECT_LT(affine_decision.affine_ns, affine_decision.reverse_ns);

  const rewrite::ADModeDecision nonlinear_decision = rewrite::decide_ad_mode(program, two_blocks()[1], /*one_pass_ns=*/1000.0, test_model());
  EXPECT_FALSE(nonlinear_decision.affine_eligible);
  EXPECT_LT(nonlinear_decision.forward_ns, nonlinear_decision.reverse_ns);
}

TEST(BlockJacobian, ClosedFormAffineMatchesReverseExactlyOnALinmapBlock) {
  const ir::Program program = mixed_program();
  const adjoint::Adjoint adj(program);
  const exec::Interpreter interp(program);
  const double state[3] = {1.0, 2.0, 3.0};

  adjoint::BlockJacobianRequest req;
  req.output_domains = {1};
  req.input_ordinals = {0, 1, 2};
  req.output_ordinals = {0, 1};  // domain 1's two rows

  ir::Program reverse_program = program;  // no plan entry: defaults to Reverse
  std::vector<double> jz_reverse(2 * 3, 0.0);
  req.block_name = "affine_block";
  adjoint::block_jacobian(reverse_program, interp, adj, state, req, jz_reverse.data());

  ir::Program affine_program = program;
  affine_program.plan.jacobian.mode["affine_block"] = ir::AdMode::ClosedFormAffine;
  std::vector<double> jz_affine(2 * 3, 0.0);
  adjoint::block_jacobian(affine_program, interp, adj, state, req, jz_affine.data());

  // Known by construction: d(row0)/d(x0,x1,x2) = (2, 3, 0); d(row1)/d(x0,x1,x2) = (0, 4, 5).
  const double expected[6] = {2.0, 3.0, 0.0, 0.0, 4.0, 5.0};
  for (int i = 0; i < 6; ++i) {
    EXPECT_NEAR(jz_reverse[static_cast<std::size_t>(i)], expected[i], 1e-12) << i;
    EXPECT_NEAR(jz_affine[static_cast<std::size_t>(i)], expected[i], 1e-12) << i;
  }
}

TEST(BlockJacobian, ForwardFdMatchesReverseOnTheNonlinearBlock) {
  const ir::Program program = mixed_program();
  const adjoint::Adjoint adj(program);
  const exec::Interpreter interp(program);
  const double state[3] = {1.0, 2.0, 3.0};

  adjoint::BlockJacobianRequest req;
  req.output_domains = {2};
  req.input_ordinals = {0, 1, 2};
  req.output_ordinals = {2};  // domain 2's single row
  req.block_name = "nonlinear_block";

  ir::Program reverse_program = program;
  std::vector<double> jz_reverse(3, 0.0);
  adjoint::block_jacobian(reverse_program, interp, adj, state, req, jz_reverse.data());

  ir::Program forward_program = program;
  forward_program.plan.jacobian.mode["nonlinear_block"] = ir::AdMode::Forward;
  std::vector<double> jz_forward(3, 0.0);
  adjoint::block_jacobian(forward_program, interp, adj, state, req, jz_forward.data());

  // value3 = 2 x0 + 3 x1 = 8, value4 = 1 + 4 x1 + 5 x2 = 24 at state (1,2,3); value5 = value3*value4.
  // d/dx0 = 2*value4 = 48; d/dx1 = 3*value4 + value3*4 = 72 + 32 = 104; d/dx2 = value3*5 = 40.
  const double expected[3] = {48.0, 104.0, 40.0};
  for (int i = 0; i < 3; ++i) {
    EXPECT_NEAR(jz_reverse[static_cast<std::size_t>(i)], expected[i], 1e-9) << i;
    EXPECT_NEAR(jz_forward[static_cast<std::size_t>(i)], expected[i], 2e-4) << i << " (one-sided FD, larger tolerance)";
  }
}

TEST(BlockJacobian, ClosedFormAffineRefusesANonAffineBlock) {
  const ir::Program program = mixed_program();
  const adjoint::Adjoint adj(program);
  const exec::Interpreter interp(program);
  const double state[3] = {1.0, 2.0, 3.0};

  ir::Program bad_program = program;
  bad_program.plan.jacobian.mode["nonlinear_block"] = ir::AdMode::ClosedFormAffine;

  adjoint::BlockJacobianRequest req;
  req.block_name = "nonlinear_block";
  req.output_domains = {2};  // domain 2 is NOT a linmap domain
  req.input_ordinals = {0, 1, 2};
  req.output_ordinals = {2};
  std::vector<double> jz(3, 0.0);
  EXPECT_THROW(adjoint::block_jacobian(bad_program, interp, adj, state, req, jz.data()), std::invalid_argument);
}

}  // namespace
