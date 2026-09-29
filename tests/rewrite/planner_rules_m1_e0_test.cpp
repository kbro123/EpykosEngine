// M4/R0 gate: "the default plan must be bit-identical and time-identical to M1's" (PROBLEM.md
// §7). This checks the bit-identical half directly against rewrite::planner on the M1 book: an
// exec::Interpreter that derives its own plan (a caller who never touches rewrite::, exactly as
// before M4/R0) must produce the SAME bits as one built over a Program carrying an EXPLICIT
// rewrite::planner::default_plan(...) annotation for the very same Options -- across every
// Options combination the M1 gates already cover (tile x lane_tile, and each fusion flag off in
// turn). Time-identical is the M1 bench's job (bench/run.sh + scripts/perf_gate.py, unchanged by
// this package); ctest --preset release / reference re-running exec_m1_interp_e0_test bitwise is
// the existing proof that this refactor changed no bits (docs/RESUME.md's R0 landing entry cites
// both).
#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <tuple>
#include <vector>

#include "epykos/exec/interpreter.hpp"
#include "epykos/fixtures/m1_book.hpp"
#include "epykos/fixtures/m1_reference.hpp"
#include "epykos/fixtures/record_m1.hpp"
#include "epykos/ir/program.hpp"
#include "epykos/ir/signature.hpp"
#include "epykos/rewrite/planner_rules.hpp"
#include "epykos/tape/tape.hpp"

#ifndef EPYKOS_FP_CONTRACT_OFF
#error "an _e0_test.cpp TU must see EPYKOS_FP_CONTRACT_OFF"
#endif

namespace ir = epykos::ir;
namespace exec = epykos::exec;
namespace fixtures = epykos::fixtures;
namespace planner = epykos::rewrite::planner;

namespace {

std::uint64_t bits(double v) {
  std::uint64_t u;
  std::memcpy(&u, &v, sizeof u);
  return u;
}

const ir::Program& m1_program() {
  static const ir::Program program = [] {
    const fixtures::Book book = fixtures::make_m1_book();
    const epykos::Tape tape = fixtures::record_m1(book);
    return ir::infer(tape);
  }();
  return program;
}

// Runs `in` at every one of `states` (each n_inputs long) at B = 1 and returns the flattened
// n_outputs x states.size() bits.
std::vector<std::uint64_t> run_all_b1(const exec::Interpreter& in, const std::vector<std::vector<double>>& states) {
  std::vector<std::uint64_t> out;
  std::vector<double> row(static_cast<std::size_t>(in.n_outputs()));
  for (const std::vector<double>& z : states) {
    in.run(z.data(), 1, row.data());
    for (double v : row) out.push_back(bits(v));
  }
  return out;
}

std::vector<std::vector<double>> some_states(const ir::Program& program, int n) {
  const fixtures::Book book = fixtures::make_m1_book();
  const fixtures::Batch batch = fixtures::make_m1_batch();
  std::vector<std::vector<double>> states;
  states.emplace_back(book.z0.begin(), book.z0.end());
  (void)program;
  for (int b = 0; b < n && b < batch.n_states; ++b) {
    std::vector<double> z(static_cast<std::size_t>(fixtures::n_knots));
    batch.state(b, z.data());
    states.push_back(z);
  }
  return states;
}

struct FlagCombo {
  bool fuse_reductions, fuse_pairs, inline_producers;
};

}  // namespace

class DefaultPlanMatchesM1 : public ::testing::TestWithParam<std::tuple<int, int>> {};

TEST_P(DefaultPlanMatchesM1, ExplicitAnnotationBitwiseTheInternalFallback) {
  const auto [tile, lane_tile] = GetParam();
  const ir::Program& program = m1_program();
  const std::vector<std::vector<double>> states = some_states(program, 6);

  exec::Options opt;
  opt.tile = tile;
  opt.lane_tile = lane_tile;

  const exec::Interpreter fallback(program, opt);  // program.plan empty: derives its own default

  ir::Program annotated = program;
  planner::DefaultPlanOptions dopt;
  dopt.lane_tile = std::min(lane_tile, opt.max_batch);
  annotated.plan = planner::default_plan(program, dopt);
  const exec::Interpreter explicit_plan(annotated, opt);

  EXPECT_EQ(fallback.describe(), explicit_plan.describe()) << "tile " << tile << " lane_tile " << lane_tile;
  EXPECT_EQ(run_all_b1(fallback, states), run_all_b1(explicit_plan, states)) << "tile " << tile << " lane_tile " << lane_tile;
}

INSTANTIATE_TEST_SUITE_P(TileLaneTileGrid, DefaultPlanMatchesM1,
                        ::testing::Combine(::testing::Values(1, 7, 128, 256), ::testing::Values(1, 4, 8, 16, 32)));

class OptionsFlagSelectsTheGreedyPass : public ::testing::TestWithParam<FlagCombo> {};

// PROBLEM.md §7: "Options flags keep working by selecting the greedy pass" -- with each flag off
// in turn, the interpreter still agrees bitwise with the M1 book's own oracle (fixtures'
// reference table), proving the DefaultPlanner really does omit the corresponding rule rather
// than silently ignoring the flag now that the decision lives in rewrite::.
TEST_P(OptionsFlagSelectsTheGreedyPass, StillBitwiseCorrectWithARuleTurnedOff) {
  const FlagCombo combo = GetParam();
  const ir::Program& program = m1_program();
  const fixtures::Book book = fixtures::make_m1_book();
  const fixtures::ReferenceTable oracle = fixtures::m1_reference_values(book, fixtures::make_m1_batch());

  exec::Options opt;
  opt.fuse_reductions = combo.fuse_reductions;
  opt.fuse_pairs = combo.fuse_pairs;
  opt.inline_producers = combo.inline_producers;
  const exec::Interpreter in(program, opt);

  std::vector<double> out(static_cast<std::size_t>(in.n_outputs()));
  in.run(book.z0.data(), 1, out.data());
  ASSERT_EQ(out.size(), oracle.record.size());
  for (std::size_t i = 0; i < oracle.record.size(); ++i) {
    EXPECT_EQ(bits(out[i]), bits(oracle.record[i])) << "output " << i;
  }
}

INSTANTIATE_TEST_SUITE_P(EachFlagOffInTurn, OptionsFlagSelectsTheGreedyPass,
                        ::testing::Values(FlagCombo{false, true, true}, FlagCombo{true, false, true},
                                          FlagCombo{true, true, false}, FlagCombo{false, false, false}));

// The M1 book's own plan, described: at least one domain fuses into a reduction and at least one
// fused-pair grouping exists at the M1 gate's own tile/lane_tile (RESUME.md §5 "M1 result" /
// DESIGN.md §7) -- so this test would fail loudly if a future change made the default rule
// pipeline a no-op rather than reproducing decide_fusion / decide_inline's old decisions.
TEST(DefaultPlan, M1BookFusesAndPairsAtTheGateLaneTile) {
  const ir::Program& program = m1_program();
  planner::DefaultPlanOptions dopt;
  dopt.lane_tile = 32;  // the M1 batched gate point
  const ir::PlanAnnotations plan = planner::default_plan(program, dopt);
  ASSERT_EQ(plan.domain.size(), program.domains.size());
  bool any_fused = false, any_inlined = false, any_paired = false;
  for (const ir::DomainPlan& dp : plan.domain) {
    any_fused |= dp.choice == ir::Materialise::FuseIntoReduction;
    any_inlined |= dp.choice == ir::Materialise::InlineIntoConsumer;
  }
  for (const ir::GroupPlan& gp : plan.group) any_paired |= !gp.pairings.empty();
  EXPECT_TRUE(any_fused);
  EXPECT_TRUE(any_inlined);
  EXPECT_TRUE(any_paired);
}
