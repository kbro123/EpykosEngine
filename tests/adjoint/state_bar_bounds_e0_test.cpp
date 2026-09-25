// EpykosEngine — adjoint::Adjoint::run's state_bar write bounds (D65).
//
// Adjoint::run takes raw pointers with no lengths (adjoint.hpp: "Zero allocations in run(): every
// buffer is sized in the constructor"), so the caller owns the sizing contract. The clause this
// test pins is the one a caller got wrong for a whole milestone: run() writes
// `state_bar[k * B + b]` for EVERY input ordinal k in [0, program.inputs.size()) — the reverse
// pass zeroes the whole span before it accumulates anything, so the write happens whether or not
// that ordinal ends up carrying an adjoint, and whether or not the seed reaches it.
//
// Handing it a buffer sized from a SUBSET of the inputs overflows the heap by the difference and
// aborts later, far away from the write. That is exactly what happened on the Stage A tape:
// `StageATape::record_quotes()` is the 70 calibration quotes, the tape has 148 Inputs, and
// tests/rewrite/record_point_check.hpp was handed the former as both the state and the state_bar
// buffer — 78 doubles (624 bytes) past the end, every call. D52 point 4 read the resulting
// "heap-corruption-shaped abort some distance past the actual fault" as a defect inside
// src/adjoint/ and walled R2's entire singleton-bucket shape off rather than ship a rule that
// could crash; there is no such defect, and D65 relaxes the gate.
//
// A guard region after the buffer, compared bit-for-bit, catches a regression of this clause
// deterministically in an ordinary build — no sanitiser, no allocator luck.
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "epykos/adjoint/adjoint.hpp"
#include "epykos/ir/program.hpp"

#ifndef EPYKOS_FP_CONTRACT_OFF
#error "an _e0_test.cpp TU must see EPYKOS_FP_CONTRACT_OFF"
#endif

namespace ir = epykos::ir;

namespace {

constexpr int kInputs = 6;

// A NaN payload no arithmetic below can produce: "untouched" is then a bit-pattern comparison,
// not a value comparison.
constexpr std::uint64_t kSentinel = 0x7ff8badc0ffee111ULL;

double sentinel_value() {
  double d;
  std::memcpy(&d, &kSentinel, sizeof d);
  return d;
}

std::uint64_t bits_of(double v) {
  std::uint64_t u;
  std::memcpy(&u, &v, sizeof u);
  return u;
}

// domain 0: Input, kInputs rows (value ids 0 .. kInputs-1).
// domain 1: kInputs rows, Mul(gather -> domain 0 row r, column) — every input ordinal reaches an
// output, so a correct run writes every state_bar entry with a non-zero adjoint, and the
// "everything in range was written" half of the check below is meaningful.
ir::Program tiny_program() {
  ir::Program p;
  p.domains.push_back(ir::Domain{"input", kInputs, 0, 0, false, {}, false, -1});
  p.groups.push_back(ir::Group{0, {ir::Step{epykos::Op::Input, ir::Slot{ir::SlotKind::Input, -1}, {}, {}, {}}}});
  for (int k = 0; k < kInputs; ++k) {
    p.inputs.push_back(k);
    p.input_values.push_back(1.0 + k);
  }

  ir::Gather g;
  g.domain = 1;
  ir::Column c;
  c.domain = 1;
  for (int r = 0; r < kInputs; ++r) {
    g.index.push_back(r);
    c.values.push_back(2.0 + r);
  }
  p.gathers.push_back(g);
  p.columns.push_back(c);
  p.groups.push_back(ir::Group{1, {ir::Step{epykos::Op::Mul, ir::Slot{ir::SlotKind::Gather, 0}, ir::Slot{ir::SlotKind::Column, 0}, {}, {}}}});
  p.domains.push_back(ir::Domain{"mul(@0,$0)", kInputs, kInputs, 0, false, {0}, false, -1});

  for (int r = 0; r < kInputs; ++r) p.outputs.push_back(kInputs + r);
  ir::validate(p);
  return p;
}

}  // namespace

TEST(AdjointStateBarBoundsE0, WritesEveryInputOrdinalAndNothingBeyondIt) {
  const ir::Program program = tiny_program();
  const epykos::adjoint::Adjoint adj(program, {});

  const std::size_t n_inputs = program.inputs.size();
  const std::size_t n_outputs = program.outputs.size();
  ASSERT_EQ(n_inputs, static_cast<std::size_t>(kInputs));

  // One lane and several, so the b0 / B striding of the write is covered too.
  for (const int B : {1, 4}) {
    const std::size_t in_range = n_inputs * static_cast<std::size_t>(B);
    const std::size_t guard = 64;  // generous: the historical overflow was 78 doubles
    std::vector<double> state_bar(in_range + guard, sentinel_value());
    std::vector<double> out(n_outputs * static_cast<std::size_t>(B), 0.0);
    std::vector<double> out_bar(n_outputs * static_cast<std::size_t>(B), 0.0);
    std::vector<double> state(n_inputs * static_cast<std::size_t>(B), 0.0);
    for (std::size_t k = 0; k < n_inputs; ++k) {
      for (int b = 0; b < B; ++b) state[k * static_cast<std::size_t>(B) + static_cast<std::size_t>(b)] = program.input_values[k];
    }
    for (std::size_t o = 0; o < n_outputs; ++o) {
      for (int b = 0; b < B; ++b) out_bar[o * static_cast<std::size_t>(B) + static_cast<std::size_t>(b)] = 1.0;
    }

    adj.run(state.data(), B, out_bar.data(), out.data(), state_bar.data());

    for (std::size_t i = 0; i < in_range; ++i) {
      EXPECT_NE(bits_of(state_bar[i]), kSentinel)
          << "B=" << B << ": state_bar entry " << i << " of " << in_range << " was never written; a caller sizing the buffer from the "
          << "ordinals it expects an adjoint on would still be handed one of these";
    }
    for (std::size_t i = in_range; i < state_bar.size(); ++i) {
      ASSERT_EQ(bits_of(state_bar[i]), kSentinel)
          << "B=" << B << ": run() wrote " << (i - in_range + 1) << " double(s) past state_bar's " << in_range
          << " in-range entries (program.inputs.size() * B)";
    }
  }
}
