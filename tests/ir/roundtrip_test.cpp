// M1/P3 gate: round-trip identity (DESIGN.md §5.7, D10) — infer the domain IR from a recorded
// tape, expand it back to a scalar tape, and compare node-for-node up to a canonical renumbering.
// On the M1 book (record_m1: raw recording and after the E0 passes), on three smaller seeded
// books cut from it, and on P2's mini book (which records Select / comparisons).
#include <gtest/gtest.h>

#include <cstddef>
#include <iostream>
#include <string>
#include <vector>

#include "epykos/ir/expand.hpp"
#include "epykos/ir/program.hpp"
#include "epykos/ir/signature.hpp"
#include "epykos/maths/m1/book.hpp"
#include "epykos/tape/passes.hpp"
#include "epykos/tape/record_m1.hpp"
#include "epykos/tape/tape.hpp"
#include "ir/ir_test_helpers.hpp"
#include "tape/mini_book.hpp"

namespace m1 = epykos::m1;
namespace ir = epykos::ir;
using epykos::Tape;

namespace {

void expect_roundtrip(const Tape& tape, const char* what, bool print_program) {
  ir::InferStats stats;
  const ir::Program program = ir::infer(tape, &stats);
  ASSERT_NO_THROW(ir::validate(program)) << what;
  if (print_program) std::cout << "[ program  ] " << what << "\n" << ir::to_string(program);
  std::cout << "[  infer   ] " << what << ": nodes " << stats.nodes << ", rows " << stats.boundaries << ", inner "
            << stats.inner << ", const leaves " << stats.const_leaves << ", classes " << stats.classes << ", domains "
            << stats.domains << ", fan-out rounds " << stats.boundary_rounds << ", promoted " << stats.class_promoted
            << '\n';
  const Tape expanded = ir::expand(program);
  EXPECT_EQ(expanded.size(), tape.size()) << what;
  EXPECT_EQ(expanded.num_inputs(), tape.num_inputs()) << what;
  EXPECT_EQ(expanded.num_outputs(), tape.num_outputs()) << what;
  std::string diff;
  const bool identical = ir::roundtrip_identical(tape, expanded, &diff);
  EXPECT_TRUE(identical) << what << ": expanded tape differs from the recording:\n" << diff;
  // The canonical form is a function of the graph: identical to itself, and the op histograms
  // of the two tapes agree.
  EXPECT_TRUE(ir::roundtrip_identical(tape, tape));
  EXPECT_EQ(expanded.op_histogram(), tape.op_histogram()) << what;
}

}  // namespace

TEST(IrRoundtrip, M1BookAfterPasses) {
  const m1::Book book = m1::make_m1_book();
  m1::RecordM1Stats rs;
  const Tape tape = m1::record_m1(book, &rs);
  expect_roundtrip(tape, "M1 book (record_m1)", true);
}

TEST(IrRoundtrip, M1BookRawRecording) {
  const m1::Book book = m1::make_m1_book();
  const Tape tape = m1::record_m1_raw(book);
  expect_roundtrip(tape, "M1 book (raw recording)", false);
}

TEST(IrRoundtrip, SmallerBooks) {
  const m1::Book full = m1::make_m1_book();
  const std::vector<std::pair<const char*, std::vector<int>>> cases = {
      {"1 swap, unseasoned (swap 300)", {300}},
      {"1 swap, seasoned (swap 3)", {3}},
      {"10 swaps (0..4, 200..204)", {0, 1, 2, 3, 4, 200, 201, 202, 203, 204}},
  };
  for (const auto& [name, swaps] : cases) {
    const m1::Book sub = epykos::test::sub_book(full, swaps);
    ASSERT_EQ(sub.n_swaps, static_cast<int>(swaps.size()));
    expect_roundtrip(m1::record_m1(sub), name, true);
    expect_roundtrip(m1::record_m1_raw(sub), (std::string(name) + " raw").c_str(), false);
  }
}

TEST(IrRoundtrip, MiniBookWithSelect) {
  const std::vector<epykos::test::MiniSwap> book = epykos::test::mini_book(25);
  Tape tape = epykos::test::record_mini_book(book);
  expect_roundtrip(tape, "mini book (raw)", false);
  epykos::standard_passes(tape);
  expect_roundtrip(tape, "mini book (passes)", true);
}

// The comparison is a real check: a changed constant, a swapped non-commutative operand, a
// dropped node or a changed output is reported with the offending nodes.
TEST(IrRoundtrip, ComparisonDetectsDifferences) {
  const m1::Book full = m1::make_m1_book();
  const Tape tape = m1::record_m1(epykos::test::sub_book(full, {0, 300}));
  const ir::Program program = ir::infer(tape);
  {
    ir::Program bad = program;
    bool touched = false;
    for (ir::Column& c : bad.columns) {
      if (!c.values.empty()) {
        c.values[0] = c.values[0] * 1.5 + 1.0;
        touched = true;
        break;
      }
    }
    ASSERT_TRUE(touched);
    std::string diff;
    EXPECT_FALSE(ir::roundtrip_identical(tape, ir::expand(bad), &diff));
    EXPECT_NE(diff.find("konst="), std::string::npos) << diff;
  }
  {
    ir::Program bad = program;
    bool touched = false;
    for (ir::Group& g : bad.groups) {
      for (ir::Step& s : g.steps) {
        if (s.op == epykos::Op::Sub || s.op == epykos::Op::Div) {
          std::swap(s.a, s.b);
          touched = true;
          break;
        }
      }
      if (touched) break;
    }
    ASSERT_TRUE(touched);
    std::string diff;
    EXPECT_FALSE(ir::roundtrip_identical(tape, ir::expand(bad), &diff));
    EXPECT_FALSE(diff.empty());
  }
  {
    ir::Program bad = program;
    std::swap(bad.outputs[0], bad.outputs[1]);
    std::string diff;
    EXPECT_FALSE(ir::roundtrip_identical(tape, ir::expand(bad), &diff));
    EXPECT_FALSE(diff.empty());
  }
  {
    // A commuted commutative pair is the same node.
    Tape t;
    {
      Tape::Scope scope(t);
      epykos::Rec x = epykos::make_input(t, 1.0);
      epykos::Rec y = epykos::make_input(t, 2.0);
      epykos::register_output(t, x * y + 3.0);
    }
    Tape u;
    {
      Tape::Scope scope(u);
      epykos::Rec x = epykos::make_input(u, 1.0);
      epykos::Rec y = epykos::make_input(u, 2.0);
      epykos::register_output(u, 3.0 + y * x);
    }
    std::string diff;
    EXPECT_TRUE(ir::roundtrip_identical(t, u, &diff)) << diff;
  }
}
