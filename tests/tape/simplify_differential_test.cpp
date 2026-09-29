// The differential and mutation gate for `simplify`, the algebra pass (src/tape/passes.cpp).
//
// CLAUDE.md requires every rewrite to ship with a differential test and a mutation test. This is
// both. The file is named `*_differential_test.cpp` on purpose: `scripts/mutation_test.sh`'s
// GATE_REGEX selects the mutation gate set by filename, and a name that does not match it would
// register two mutants with no catcher.
//
// What "differential" means ABOVE THE PIN is not what it means below (PRINCIPLES.md §5.2). These
// rewrites change the arithmetic deliberately, so the gate is NOT bitwise: it is that the pass
// computes the same real number to within a tolerance far looser than any rounding and far
// tighter than any broken rule. §5.1 is why a bug-detection tolerance need not be principled.
#include <gtest/gtest.h>

#include <cmath>
#include <vector>

#include "epykos/scalar/rec.hpp"
#include "epykos/tape/passes.hpp"
#include "epykos/tape/replay.hpp"
#include "epykos/tape/tape.hpp"

namespace {

using epykos::Rec;
using epykos::Tape;

// A loose bug-detection bound. Legitimate re-rounding of these expressions is at the 1e-16 level;
// every mutant below misses by 1e-2 or more. Anything in between is a rule that is wrong in a way
// worth looking at, which is exactly what should fail.
constexpr double kTol = 1e-9;

struct Case {
  Tape tape;
  std::vector<double> inputs;
};

// Builds a tape from `f`, returning it with the record-point input values.
template <class F>
Case build(F&& f, const std::vector<double>& values) {
  Case c;
  c.inputs = values;
  Tape::Scope scope(c.tape);
  std::vector<Rec> in;
  in.reserve(values.size());
  for (double v : values) in.push_back(epykos::make_input(c.tape, v));
  epykos::register_output(c.tape, f(in));
  return c;
}

// The claim, for one case: simplify does not change the value, and it does remove nodes.
void expect_same_value(Case& c, const char* what, bool expect_smaller = true) {
  const std::vector<double> before = epykos::replay(c.tape, c.inputs);
  const std::size_t n_before = c.tape.size();
  epykos::simplify(c.tape);
  const std::vector<double> after = epykos::replay(c.tape, c.inputs);
  ASSERT_EQ(before.size(), after.size()) << what;
  for (std::size_t k = 0; k < before.size(); ++k) {
    const double scale = std::max({std::fabs(before[k]), std::fabs(after[k]), 1e-300});
    EXPECT_LE(std::fabs(before[k] - after[k]) / scale, kTol)
        << what << ": output " << k << " moved from " << before[k] << " to " << after[k];
  }
  if (expect_smaller) {
    EXPECT_LT(c.tape.size(), n_before) << what << ": simplify removed nothing, so it matched nothing";
  }
}

// ---- one case per rule -------------------------------------------------------------------------

TEST(SimplifyDifferential, CancelsADivideAgainstTheSameConstantMultiply) {
  // (x / c) * c, with c ONE hash-consed Const node. This is the tau cancellation of the
  // compounded coupon, which is what makes the whole telescope reachable.
  Case c = build([](std::vector<Rec>& in) { return (in[0] / 360.0) * 360.0; }, {7.25});
  expect_same_value(c, "mul(div(x,c),c)");
}

TEST(SimplifyDifferential, CancelsAnAddAgainstTheSameSubtract) {
  Case c = build([](std::vector<Rec>& in) { return 1.0 + (in[0] - 1.0); }, {0.37});
  expect_same_value(c, "add(a,sub(x,a))");
}

TEST(SimplifyDifferential, DropsTheMultiplicativeIdentity) {
  Case c = build([](std::vector<Rec>& in) { return Rec(1.0) * in[0]; }, {2.5});
  expect_same_value(c, "mul(one,x)");
}

TEST(SimplifyDifferential, TelescopesAChainOfConsecutiveRatios) {
  // THE rule. A five-day product of consecutive discount-factor ratios must collapse to the
  // ratio of its endpoints, and the tape must end up holding a single Div.
  Case c = build(
      [](std::vector<Rec>& in) {
        Rec acc = 1.0;
        for (std::size_t i = 0; i + 1 < in.size(); ++i) acc = acc * (in[i] / in[i + 1]);
        return acc;
      },
      {1.0, 0.999, 0.998, 0.997, 0.996, 0.995});
  const std::vector<double> before = epykos::replay(c.tape, c.inputs);
  epykos::simplify(c.tape);
  epykos::dce(c.tape);
  const std::vector<double> after = epykos::replay(c.tape, c.inputs);
  EXPECT_LE(std::fabs(before[0] - after[0]) / std::fabs(before[0]), kTol);
  EXPECT_DOUBLE_EQ(after[0], c.inputs.front() / c.inputs.back());
  // Six inputs and one divide is the whole program: nothing of the chain survives.
  std::size_t divs = 0, muls = 0;
  for (const epykos::Node& n : c.tape.nodes()) {
    divs += n.op == epykos::Op::Div ? 1 : 0;
    muls += n.op == epykos::Op::Mul ? 1 : 0;
  }
  EXPECT_EQ(divs, 1u) << "the chain did not telescope to a single ratio";
  EXPECT_EQ(muls, 0u) << "a multiply of the chain survived";
}

TEST(SimplifyDifferential, TheWholeCompoundedCouponCollapses) {
  // The coupon as maths/swap/compounding.hpp records it, end to end: the daily forward from a
  // ratio of discount factors, divided by tau, multiplied by the same tau as a weight, plus one,
  // accumulated. It must reduce to df_first / df_last and nothing else.
  constexpr int kDays = 32;
  Case c = build(
      [](std::vector<Rec>& in) {
        const double tau = 1.0 / 360.0;
        Rec acc = 1.0;
        for (int i = 0; i + 1 < static_cast<int>(in.size()); ++i) {
          const Rec fwd = (in[static_cast<std::size_t>(i)] / in[static_cast<std::size_t>(i + 1)] - 1.0) / tau;
          acc = acc * (1.0 + fwd * tau);
        }
        return acc;
      },
      [] {
        std::vector<double> v;
        for (int i = 0; i <= kDays; ++i) v.push_back(1.0 - 1e-5 * i);
        return v;
      }());
  const std::vector<double> before = epykos::replay(c.tape, c.inputs);
  epykos::simplify(c.tape);
  epykos::dce(c.tape);
  const std::vector<double> after = epykos::replay(c.tape, c.inputs);
  const double truth = c.inputs.front() / c.inputs.back();
  EXPECT_LE(std::fabs(before[0] - after[0]) / std::fabs(truth), kTol);
  EXPECT_DOUBLE_EQ(after[0], truth);
  std::size_t arith = 0;
  for (const epykos::Node& n : c.tape.nodes()) {
    if (n.op != epykos::Op::Input && n.op != epykos::Op::Const) ++arith;
  }
  EXPECT_EQ(arith, 1u) << kDays << " days of compounding did not reduce to one divide";
  std::cout << "[ simplify ] " << kDays << " compounding days -> " << arith << " arithmetic node\n";
}

// ---- what must NOT be rewritten -----------------------------------------------------------------

TEST(SimplifyDifferential, LeavesAMismatchedCancellationAlone) {
  // (x / c) * d with c != d is not a cancellation, and the mutant
  // `simplify.cancel_mismatched_operand` is exactly the rule losing that check.
  Case c = build([](std::vector<Rec>& in) { return (in[0] / 360.0) * 365.0; }, {7.25});
  const std::vector<double> before = epykos::replay(c.tape, c.inputs);
  epykos::simplify(c.tape);
  const std::vector<double> after = epykos::replay(c.tape, c.inputs);
  EXPECT_DOUBLE_EQ(before[0], after[0]) << "a divide by 360 and a multiply by 365 were cancelled";
  EXPECT_NEAR(after[0], 7.25 / 360.0 * 365.0, 1e-12);
}

TEST(SimplifyDifferential, LeavesANonMeetingChainAlone) {
  // The days do not meet: div(a,b) then div(c,d) with b != c, which is what a lookback or
  // observation-shift coupon records. The telescope must not match, and the structure is the
  // whole precondition -- there is no separate predicate to get wrong.
  Case c = build([](std::vector<Rec>& in) { return (in[0] / in[1]) * (in[2] / in[3]); },
                 {1.0, 0.99, 0.98, 0.97});
  const std::vector<double> before = epykos::replay(c.tape, c.inputs);
  epykos::simplify(c.tape);
  const std::vector<double> after = epykos::replay(c.tape, c.inputs);
  EXPECT_DOUBLE_EQ(before[0], after[0]);
  EXPECT_NEAR(after[0], (1.0 / 0.99) * (0.98 / 0.97), 1e-12);
}

TEST(SimplifyDifferential, IsIdempotent) {
  Case c = build(
      [](std::vector<Rec>& in) {
        Rec acc = 1.0;
        for (std::size_t i = 0; i + 1 < in.size(); ++i) acc = acc * (in[i] / in[i + 1]);
        return acc;
      },
      {1.0, 0.99, 0.98, 0.97});
  epykos::simplify(c.tape);
  epykos::dce(c.tape);
  const std::string once = epykos::to_string(c.tape);
  epykos::simplify(c.tape);
  epykos::dce(c.tape);
  EXPECT_EQ(once, epykos::to_string(c.tape)) << "a second sweep changed the tape: the rules do not reach a fixpoint";
}

}  // namespace
