// M1/P3 E0 gate: the tape expanded from the domain IR replays bit-identically to the recorded
// tape (and to the double oracle) at the record point and at all 64 batch states; the IR's own
// row-by-row evaluator (ir/evaluate.hpp, the contract P4 implements) agrees bitwise too; and the
// text serialisation is exact.
//
// This TU is compiled with -ffp-contract=off in every preset (the _e0_test.cpp convention), so
// the oracle, the record-point values, the replays and the IR evaluator run without fused
// multiply-adds. The book and the batch come from libepykos.
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "epykos/ir/evaluate.hpp"
#include "epykos/ir/expand.hpp"
#include "epykos/ir/program.hpp"
#include "epykos/ir/signature.hpp"
#include "epykos/maths/m1/book.hpp"
#include "epykos/maths/m1/reference.hpp"
#include "epykos/tape/record_m1.hpp"
#include "epykos/tape/replay.hpp"
#include "epykos/tape/tape.hpp"
#include "ir/ir_test_helpers.hpp"

#ifndef EPYKOS_FP_CONTRACT_OFF
#error "an _e0_test.cpp TU must see EPYKOS_FP_CONTRACT_OFF"
#endif

namespace m1 = epykos::m1;
namespace ir = epykos::ir;
using epykos::Replayer;
using epykos::Tape;

namespace {

std::uint64_t bits(double v) {
  std::uint64_t u;
  std::memcpy(&u, &v, sizeof u);
  return u;
}

struct Fixture {
  m1::Book book;
  m1::Batch batch;
  m1::ReferenceTable oracle;
};

const Fixture& fixture() {
  static const Fixture f = [] {
    Fixture x;
    x.book = m1::make_m1_book();
    x.batch = m1::make_m1_batch();
    x.oracle = m1::m1_reference_values(x.book, x.batch);
    return x;
  }();
  return f;
}

// Every state the gate evaluates: the record point (the tape's own input values) then the
// 64 batch states.
std::vector<std::vector<double>> gate_states(const Tape& tape) {
  const Fixture& f = fixture();
  std::vector<std::vector<double>> states;
  states.push_back(tape.input_values());
  for (int b = 0; b < f.batch.n_states; ++b) {
    std::vector<double> z(static_cast<std::size_t>(m1::n_knots));
    f.batch.state(b, z.data());
    states.push_back(z);
  }
  return states;
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

}  // namespace

TEST(IrExpandE0, ExpandedTapeReplaysBitIdenticallyAtAllStates) {
  const Fixture& f = fixture();
  const Tape tape = m1::record_m1(f.book);
  const ir::Program program = ir::infer(tape);
  const Tape expanded = ir::expand(program);
  std::string diff;
  ASSERT_TRUE(ir::roundtrip_identical(tape, expanded, &diff)) << diff;

  Replayer original(tape), regenerated(expanded);
  ir::Evaluator evaluator(program);
  std::vector<double> out_o(tape.num_outputs()), out_r(expanded.num_outputs()), out_e(program.outputs.size());
  const std::vector<std::vector<double>> states = gate_states(tape);
  std::size_t mismatches = 0;
  for (std::size_t s = 0; s < states.size(); ++s) {
    const std::string where = s == 0 ? "record point" : "state " + std::to_string(s - 1);
    original.run(states[s].data(), out_o.data());
    regenerated.run(states[s].data(), out_r.data());
    evaluator.run(states[s].data(), out_e.data());
    mismatches += count_mismatches(out_o, out_r, "expanded replay vs original replay", where);
    mismatches += count_mismatches(out_o, out_e, "IR evaluator vs original replay", where);
    // And the oracle: the original tape is E0 (P2's gate), so the regenerated one must be too.
    const double* expect = s == 0 ? f.oracle.record.data() : f.oracle.state(static_cast<int>(s - 1));
    const std::vector<double> oracle(expect, expect + static_cast<std::ptrdiff_t>(tape.num_outputs()));
    mismatches += count_mismatches(out_r, oracle, "expanded replay vs double oracle", where);
    mismatches += count_mismatches(out_e, oracle, "IR evaluator vs double oracle", where);
  }
  EXPECT_EQ(mismatches, 0u);
  std::cout << "[  states  ] " << states.size() << " states x " << tape.num_outputs() << " outputs, mismatches " << mismatches
            << '\n';
}

TEST(IrExpandE0, RawRecordingExpandsAndReplaysBitIdentically) {
  const Fixture& f = fixture();
  const Tape tape = m1::record_m1_raw(f.book);
  const ir::Program program = ir::infer(tape);
  const Tape expanded = ir::expand(program);
  std::string diff;
  ASSERT_TRUE(ir::roundtrip_identical(tape, expanded, &diff)) << diff;
  Replayer original(tape), regenerated(expanded);
  ir::Evaluator evaluator(program);
  std::vector<double> out_o(tape.num_outputs()), out_r(expanded.num_outputs()), out_e(program.outputs.size());
  const std::vector<std::vector<double>> states = gate_states(tape);
  std::size_t mismatches = 0;
  for (std::size_t s = 0; s < states.size(); ++s) {
    const std::string where = s == 0 ? "record point" : "state " + std::to_string(s - 1);
    original.run(states[s].data(), out_o.data());
    regenerated.run(states[s].data(), out_r.data());
    evaluator.run(states[s].data(), out_e.data());
    mismatches += count_mismatches(out_o, out_r, "raw: expanded replay vs original replay", where);
    mismatches += count_mismatches(out_o, out_e, "raw: IR evaluator vs original replay", where);
  }
  EXPECT_EQ(mismatches, 0u);
}

TEST(IrExpandE0, SmallerBooksAndSerialisedProgramAgree) {
  const Fixture& f = fixture();
  const std::vector<std::vector<int>> cases = {{300}, {3}, {0, 1, 2, 3, 4, 200, 201, 202, 203, 204}};
  for (const std::vector<int>& swaps : cases) {
    const m1::Book sub = epykos::test::sub_book(f.book, swaps);
    const Tape tape = m1::record_m1(sub);
    const ir::Program program = ir::infer(tape);
    const ir::Program parsed = ir::deserialize(ir::serialize(program));
    ASSERT_TRUE(parsed == program) << "serialisation is exact";
    const Tape expanded = ir::expand(parsed);
    std::string diff;
    ASSERT_TRUE(ir::roundtrip_identical(tape, expanded, &diff)) << diff;
    Replayer original(tape), regenerated(expanded);
    ir::Evaluator evaluator(parsed);
    std::vector<double> out_o(tape.num_outputs()), out_r(expanded.num_outputs()), out_e(parsed.outputs.size());
    // The double oracle for the sub-book, in this TU.
    std::vector<double> oracle(static_cast<std::size_t>(sub.n_swaps) + 1);
    std::size_t mismatches = 0;
    for (const std::vector<double>& z : gate_states(tape)) {
      original.run(z.data(), out_o.data());
      regenerated.run(z.data(), out_r.data());
      evaluator.run(z.data(), out_e.data());
      m1::m1_reference_state(sub, z.data(), oracle.data());
      mismatches += count_mismatches(out_o, out_r, "sub-book expanded replay", "");
      mismatches += count_mismatches(out_o, out_e, "sub-book IR evaluator", "");
      mismatches += count_mismatches(out_r, oracle, "sub-book vs oracle", "");
    }
    EXPECT_EQ(mismatches, 0u) << swaps.size() << " swaps";
  }
}
