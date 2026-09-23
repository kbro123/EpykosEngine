// M2/Q4 gate: round-trip identity and E0 replay on the near-miss shapes fixture
// (fixtures/nearmiss_shapes.hpp) — op trees that differ from one another in exactly one respect a
// signature may overlook (where a constant sits, which way round a non-commutative op is, which arm
// of a select is a constant, how many members a sum has, whether an affine chain has a leading
// constant), each a class of several rows.
//
// The M1 book's classes are far apart, so a signature pass that merged near-miss shapes, or a
// tape pass that mishandled a leading constant, would pass every M1 gate; this gate closes that
// gap (found by the mutation harness, D31). Round-trip identity on the raw recording and after
// the passes; the expanded tape, the IR evaluator and the tape after every pass replay bitwise
// the double instantiation of the same shapes at the record point and a 64-draw state ball.
//
// This TU is compiled with -ffp-contract=off in every preset (the _e0_test.cpp convention): the
// oracle, the record-point values, the replays and the IR evaluator carry no fused multiply-adds.
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "epykos/fixtures/nearmiss_shapes.hpp"
#include "epykos/ir/evaluate.hpp"
#include "epykos/ir/expand.hpp"
#include "epykos/ir/program.hpp"
#include "epykos/ir/signature.hpp"
#include "epykos/tape/op.hpp"
#include "epykos/tape/passes.hpp"
#include "epykos/tape/replay.hpp"
#include "epykos/tape/tape.hpp"

#ifndef EPYKOS_FP_CONTRACT_OFF
#error "an _e0_test.cpp TU must see EPYKOS_FP_CONTRACT_OFF"
#endif

namespace fixtures = epykos::fixtures;
namespace ir = epykos::ir;
using epykos::Op;
using epykos::PassResult;
using epykos::Replayer;
using epykos::Tape;

namespace {

std::uint64_t bits(double v) {
  std::uint64_t u;
  std::memcpy(&u, &v, sizeof u);
  return u;
}

std::size_t count_op(const Tape& t, Op op) {
  std::size_t c = 0;
  for (const epykos::Node& n : t.nodes()) c += (n.op == op);
  return c;
}

std::size_t count_mismatches(const std::vector<double>& a, const std::vector<double>& b, const char* what,
                             const std::string& where) {
  EXPECT_EQ(a.size(), b.size()) << what;
  std::size_t n = 0;
  for (std::size_t k = 0; k < a.size() && k < b.size(); ++k) {
    if (bits(a[k]) != bits(b[k])) {
      if (n < 5) ADD_FAILURE() << what << ", " << where << ", output " << k << ": " << a[k] << " vs " << b[k];
      ++n;
    }
  }
  return n;
}

constexpr int n_ball = 64;

// Infer, expand, compare node for node; returns the program.
ir::Program expect_roundtrip(const Tape& tape, const char* what) {
  ir::InferStats stats;
  const ir::Program program = ir::infer(tape, &stats);
  EXPECT_NO_THROW(ir::validate(program)) << what;
  std::cout << "[  infer   ] " << what << ": nodes " << stats.nodes << ", rows " << stats.boundaries << ", classes "
            << stats.classes << ", domains " << stats.domains << '\n';
  const Tape expanded = ir::expand(program);
  EXPECT_EQ(expanded.size(), tape.size()) << what;
  EXPECT_EQ(expanded.op_histogram(), tape.op_histogram()) << what;
  std::string diff;
  EXPECT_TRUE(ir::roundtrip_identical(tape, expanded, &diff)) << what << ": expanded tape differs from the recording:\n" << diff;
  return program;
}

// The original replay, the expanded tape's replay and the IR evaluator against the double oracle
// at every state of the ball.
std::size_t expect_bitwise_at_ball(const Tape& tape, const ir::Program& program, const char* what) {
  const Tape expanded = ir::expand(program);
  Replayer original(tape), regenerated(expanded);
  ir::Evaluator evaluator(program);
  std::vector<double> out_o(tape.num_outputs()), out_r(expanded.num_outputs()), out_e(program.outputs.size());
  std::size_t mismatches = 0;
  const std::vector<std::vector<double>> states = fixtures::nearmiss_states(n_ball);
  for (std::size_t s = 0; s < states.size(); ++s) {
    const std::string where = std::string(what) + ", " + (s == 0 ? "record point" : "draw " + std::to_string(s));
    const std::vector<double> oracle = fixtures::nearmiss_oracle(states[s]);
    original.run(states[s].data(), out_o.data());
    regenerated.run(states[s].data(), out_r.data());
    evaluator.run(states[s].data(), out_e.data());
    mismatches += count_mismatches(out_o, oracle, "replay vs double", where);
    mismatches += count_mismatches(out_r, oracle, "expanded replay vs double", where);
    mismatches += count_mismatches(out_e, oracle, "IR evaluator vs double", where);
  }
  return mismatches;
}

}  // namespace

TEST(NearmissRoundtrip, FixtureHasManyDistinctClassesAndRoundTripsRawAndAfterPasses) {
  Tape tape = fixtures::record_nearmiss();
  ASSERT_EQ(tape.num_inputs(), static_cast<std::size_t>(fixtures::nearmiss_inputs));
  const std::size_t n_shapes = fixtures::NearmissShapes<double>::all().size();
  ASSERT_EQ(tape.num_outputs(), n_shapes * static_cast<std::size_t>(fixtures::nearmiss_instances) + 1);
  EXPECT_GT(count_op(tape, Op::Select), 0u);
  EXPECT_GT(count_op(tape, Op::Fma), 0u);
  EXPECT_GT(count_op(tape, Op::Recip), 0u);

  const ir::Program raw = expect_roundtrip(tape, "near-miss shapes (raw)");
  // Distinct near-miss shapes are distinct classes: sub(c, x) and sub(x, c), c / x and x / c, the
  // select arms, ... Shapes that are the same tree on different references legitimately share a
  // class, so the count is below the number of shapes but well above a handful.
  EXPECT_GE(raw.domains.size(), 24u) << ir::to_string(raw);
  // Every shape row is its own boundary with the recorded operand order: a row of the domain
  // holding "c - x" must not be emitted as "x - c" (the expansion above checks it node by node).

  epykos::standard_passes(tape);
  EXPECT_EQ(count_op(tape, Op::Add), 0u);
  EXPECT_GT(count_op(tape, Op::Sum), 0u);
  EXPECT_GT(count_op(tape, Op::Affine), 0u) << "the affine chains (leading, trailing and no constant) collapse";
  const ir::Program passed = expect_roundtrip(tape, "near-miss shapes (after passes)");
  EXPECT_GE(passed.domains.size(), 20u) << ir::to_string(passed);
  std::cout << "[ program  ] near-miss shapes (after passes)\n" << ir::to_string(passed);
}

TEST(NearmissRoundtripE0, ExpandedTapeAndEvaluatorReplayBitwiseAtTheStateBall) {
  Tape tape = fixtures::record_nearmiss();
  std::size_t mismatches = expect_bitwise_at_ball(tape, ir::infer(tape), "raw");
  epykos::standard_passes(tape);
  mismatches += expect_bitwise_at_ball(tape, ir::infer(tape), "after passes");
  EXPECT_EQ(mismatches, 0u);
  std::cout << "[  states  ] " << n_ball << " states x " << tape.num_outputs() << " outputs, raw and after passes, mismatches "
            << mismatches << '\n';
}

TEST(NearmissRoundtripE0, EveryPassPreservesTheOracleBitwise) {
  Tape tape = fixtures::record_nearmiss();
  const std::vector<std::vector<double>> states = fixtures::nearmiss_states(n_ball);
  std::vector<std::vector<double>> oracle;
  for (const std::vector<double>& s : states) oracle.push_back(fixtures::nearmiss_oracle(s));
  auto check = [&](const char* stage) {
    Replayer rp(tape);
    std::vector<double> out(tape.num_outputs());
    std::size_t mismatches = 0;
    for (std::size_t s = 0; s < states.size(); ++s) {
      rp.run(states[s].data(), out.data());
      mismatches += count_mismatches(out, oracle[s], stage, s == 0 ? "record point" : "draw " + std::to_string(s));
    }
    EXPECT_EQ(mismatches, 0u) << stage;
    return mismatches;
  };
  std::size_t total = check("raw recording");
  struct Step {
    const char* name;
    PassResult (*run)(Tape&);
  };
  const Step steps[] = {
      {"cse", [](Tape& x) { return epykos::cse(x); }},
      {"dce", [](Tape& x) { return epykos::dce(x); }},
      {"fold_sum", [](Tape& x) { return epykos::fold_sum(x, {}); }},
      {"affine_collapse", [](Tape& x) { return epykos::affine_collapse(x); }},
      {"final_dce", [](Tape& x) { return epykos::dce(x); }},
  };
  for (const Step& step : steps) {
    const PassResult r = step.run(tape);
    ASSERT_NO_THROW(tape.validate()) << step.name;
    EXPECT_EQ(r.nodes_after, tape.size()) << step.name;
    total += check(step.name);
  }
  EXPECT_EQ(total, 0u);
  // The affine chains with a leading constant keep it as c_0; the others have the exact identity.
  std::size_t with_offset = 0, without = 0;
  for (const epykos::Node& n : tape.nodes()) {
    if (n.op != Op::Affine) continue;
    if (bits(n.konst) == bits(-0.0)) ++without; else ++with_offset;
  }
  EXPECT_GT(with_offset, 0u) << "affine_lead / affine_neg_sub carry a leading constant";
  EXPECT_GT(without, 0u) << "affine_plain / affine_trail have none";
}
