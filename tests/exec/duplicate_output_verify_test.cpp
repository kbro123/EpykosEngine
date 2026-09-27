// Every output ordinal is written, including ones that share an IR value.
//
// WHY THIS FILE EXISTS. `exec::Interpreter` used to leave duplicated outputs unwritten: when one
// IR value carried more than one output ordinal, `Impl::build_group` stored a single ordinal per
// row in `g.emit_ordinal`, the last assignment won, and `decide_fusion` then skipped them all
// because it keyed its late copy on `emitted[v]`. The losing ordinals were written NOWHERE and
// the caller read back whatever its buffer held (D81 §6(a)).
//
// It was found on Stage A's MonotoneCubic variant, where `pv(i)` and `pv_usd(i)` are the same
// node for a USD trade, and the mutant `interpreter.duplicate_output_unwritten` pins the fix.
//
// **This file does NOT catch that mutant, and says so rather than implying otherwise.** The
// defect needs a value that is BOTH consumed by a reduction the planner chooses to fuse AND
// carrying two output ordinals; the programs below have the duplicate but the planner does not
// fuse their reduction, so `emitted[v]` is false and the late copy handles them correctly either
// way. Reproducing the fused shape synthetically was tried and not achieved. The mutant's gate is
// `tests/stage_a/variants_verify_test.cpp`, renamed from `variants_test.cpp` for exactly this
// reason: `scripts/mutation_test.sh`'s GATE_REGEX selects the gate set by FILENAME, the old name
// matched none of `roundtrip|differential|verify|_adjoint_test$|_vs_dual_test$|_e0_test$`, and
// the mutant therefore SURVIVED a full harness run. D32 says a survivor is a gap in the gates,
// so the fixture that does expose it became a gate.
//
// What this file is still worth: it asserts the property directly and in a tenth of a second,
// with a NaN sentinel so "never written" is distinguishable from "written zero" — which is what
// made the original defect hard to see, since it surfaced as plausible 0.0 values. It is the
// cheap check that runs everywhere; `variants_verify_test` is the one that reproduces the shape.
#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <vector>

#include "epykos/compile.hpp"
#include "epykos/exec/interpreter.hpp"
#include "epykos/ir/signature.hpp"
#include "epykos/scalar/rec.hpp"
#include "epykos/tape/tape.hpp"

namespace {

using epykos::Rec;
using epykos::Tape;

// A sum over `n` products, registered as several outputs — some of them the SAME value twice.
struct Case {
  Tape tape;
  std::vector<double> inputs;
  int n_outputs = 0;
};

Case build(int n, bool duplicate) {
  Case c;
  Tape::Scope scope(c.tape);
  std::vector<Rec> x;
  x.reserve(static_cast<std::size_t>(n));
  for (int i = 0; i < n; ++i) {
    c.inputs.push_back(1.0 + 0.125 * i);
    x.push_back(epykos::make_input(c.tape, c.inputs.back()));
  }
  // A reduction: the planner's reduction fusion is what makes emit_ordinal load-bearing.
  Rec total = x[0] * x[0];
  for (std::size_t i = 1; i < x.size(); ++i) total = total + x[i] * x[i];
  epykos::register_output(c.tape, total);                       // ordinal 0
  if (duplicate) epykos::register_output(c.tape, total);        // ordinal 1 — the SAME value
  epykos::register_output(c.tape, total * 2.0);                 // a distinct value after it
  c.n_outputs = duplicate ? 3 : 2;
  return c;
}

// Runs the pinned program and returns the outputs, pre-filled with a sentinel so that "never
// written" is distinguishable from "written zero" — which is the whole defect: the original bug
// showed up as plausible 0.0 values, not as anything obviously wrong.
std::vector<double> run(Case& c, int B) {
  epykos::compile(c.tape);
  const epykos::ir::Program program = epykos::ir::infer(c.tape);
  epykos::exec::Interpreter interp(program);
  const int n_out = static_cast<int>(program.outputs.size());
  const int n_in = static_cast<int>(program.inputs.size());
  std::vector<double> state(static_cast<std::size_t>(n_in) * static_cast<std::size_t>(B));
  for (int k = 0; k < n_in; ++k) {
    for (int b = 0; b < B; ++b) {
      state[static_cast<std::size_t>(k) * static_cast<std::size_t>(B) + static_cast<std::size_t>(b)] =
          c.inputs[static_cast<std::size_t>(k)];
    }
  }
  std::vector<double> out(static_cast<std::size_t>(n_out) * static_cast<std::size_t>(B),
                          std::numeric_limits<double>::quiet_NaN());
  interp.run(state.data(), B, out.data());
  return out;
}

}  // namespace

TEST(DuplicateOutputVerify, EveryOrdinalIsWrittenWhenTwoShareAValue) {
  for (const int n : {4, 17, 64}) {
    for (const int B : {1, 8}) {
      Case c = build(n, /*duplicate=*/true);
      const std::vector<double> out = run(c, B);
      ASSERT_EQ(out.size() % static_cast<std::size_t>(B), 0u);
      const std::size_t n_out = out.size() / static_cast<std::size_t>(B);
      for (std::size_t o = 0; o < n_out; ++o) {
        for (int b = 0; b < B; ++b) {
          EXPECT_FALSE(std::isnan(out[o * static_cast<std::size_t>(B) + static_cast<std::size_t>(b)]))
              << "output ordinal " << o << " lane " << b << " was never written (n=" << n << ", B=" << B << ")";
        }
      }
      // The two ordinals that share a value must agree, and the third must be twice it.
      for (int b = 0; b < B; ++b) {
        const double a0 = out[0 * static_cast<std::size_t>(B) + static_cast<std::size_t>(b)];
        const double a1 = out[1 * static_cast<std::size_t>(B) + static_cast<std::size_t>(b)];
        const double a2 = out[2 * static_cast<std::size_t>(B) + static_cast<std::size_t>(b)];
        EXPECT_DOUBLE_EQ(a0, a1) << "the two ordinals of one value disagree (n=" << n << ", B=" << B << ")";
        EXPECT_DOUBLE_EQ(a2, 2.0 * a0);
        double want = 0.0;
        for (int i = 0; i < n; ++i) want += c.inputs[static_cast<std::size_t>(i)] * c.inputs[static_cast<std::size_t>(i)];
        EXPECT_NEAR(a0, want, 1e-9 * want);
      }
    }
  }
}

TEST(DuplicateOutputVerify, TheUnduplicatedProgramIsUnaffected) {
  // The control: the same program without the duplicate must be unchanged, so a fix for the
  // duplicated case cannot be a blanket disabling of in-group emission.
  for (const int B : {1, 8}) {
    Case c = build(17, /*duplicate=*/false);
    const std::vector<double> out = run(c, B);
    for (double v : out) EXPECT_FALSE(std::isnan(v));
    double want = 0.0;
    for (double x : c.inputs) want += x * x;
    for (int b = 0; b < B; ++b) {
      EXPECT_NEAR(out[static_cast<std::size_t>(b)], want, 1e-9 * want);
    }
  }
}
