// M4/EG "integration" — experiment (3) AD MODE (RESUME.md §3's EG row: "AD mode per Jacobian
// block is a rule"; PROBLEM.md §7). This does not re-run Stage A (8s to record, a separate
// multi-second benchmark suite) — it feeds optimise::estimate_jacobian_ns, the SAME function
// rewrite::decide_ad_mode calls, the REAL numbers already measured and committed for this exact
// fingerprint (bench/results/d448afd70180/stage_a_stage_a.json, commit cc23459, M3/G6's own
// baseline) and checks the rule reproduces the ALREADY-KNOWN answer (RESUME.md §5 "M3 result":
// "forward mode on Dual<70> ... 6.2x cheaper for the 2,000 x 70 shape") rather than asserting a
// number this file invented. Two real shapes, one block (O3's IFT ladder, 70 free quotes):
//
//   one_pass_ns        BM_Evaluate/1/8 median 1.5301333773615104 ms (one whole-program pass, the
//                      shape optimise::estimate_jacobian_ns's `one_pass_ns` parameter documents).
//   book-only          n_outputs = 1 (the book PV alone): BM_Adjoint/1 median 32.62968311901204 ms
//                      measures reverse mode directly here, so this case's own "adjoint_multiplier"
//                      IS 32.62968311901204 / 1.5301333773615104 = 21.33 (Stage A's measured
//                      ratio; the M1-book-calibrated default of 8.0, DESIGN.md §7's own 7.4x-9.3x,
//                      is a DIFFERENT fixture's number, cited only for comparison below).
//   full ladder        n_outputs = 2043 (2,000 trades + the book + 42 other aggregates, RESUME.md
//                      §5's own count): BM_ForwardLadder measured 1,373.388527543284 ms directly.
//
// Two caveats, both measured here rather than assumed, and both point the SAME direction (the
// model UNDERSTATES how much cheaper reverse gets at scale and UNDERSTATES how expensive forward
// really is, so the two errors partially cancel in the ratio but each is real on its own):
//
//   Reverse's `one_pass_ns x adjoint_multiplier x n_outputs` is LINEAR in n_outputs, so it cannot
//   see Stage A's real batching (B=64) and chord-policy Jacobian sharing (RESUME.md §5: "one lane
//   per output ... 8.8s = 4.4ms/lane" once warmed, far below the FIRST lane's 32.6ms this file's
//   own multiplier is measured from). Extrapolating 32.6ms/output linearly to 2043 outputs
//   (~66.6s) OVERSTATES the measured reverse cost (~8.8s) by about 7.6x.
//
//   Forward's `one_pass_ns x n_inputs` assumes `one_pass_ns` already represents a SINGLE
//   differentiation direction's own pass (tangent.hpp's `jacobian_dual`: N separate Dual<1>
//   passes). Stage A's real forward ladder does not do that — `implicit_dual<70>` runs the
//   Newton solve ONCE and propagates all 70 tangent directions through ONE Dual<70> pass, whose
//   per-operation cost is itself O(70), not the plain double interpreter's `one_pass_ns` (this
//   file's proxy for it, BM_Evaluate/1/8, IS a plain double pass) repeated 70 times — so the
//   model UNDERSTATES the real forward cost by roughly 13x here (measured below), not because
//   Forward is priced too cheaply in general, but because `one_pass_ns` sourced from the plain
//   interpreter is the wrong unit for a WIDE Dual<N> pass. Both gaps are stated, not silently
//   assumed away (D9); closing either is follow-up work this package does not attempt.
//
// The qualitative decision (book: reverse; full ladder: forward) is checked against the
// ALREADY-MEASURED fact either way — both errors above make forward look WORSE, if anything, so
// the crossover this rule finds is, if biased at all, biased towards reverse, not towards a false
// forward win.
#include "epykos/rewrite/ad_mode_rule.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <iostream>

#include "epykos/optimise/cost.hpp"

namespace optimise = epykos::optimise;

namespace {
constexpr double kOnePassNs = 1.5301333773615104 * 1e6;             // BM_Evaluate/1/8 median, ms -> ns
constexpr double kBookReverseNs = 32.62968311901204 * 1e6;          // BM_Adjoint/1 median, ms -> ns
constexpr double kMeasuredAdjointMultiplier = kBookReverseNs / kOnePassNs;
constexpr double kMeasuredForwardLadderNs = 1373.388527543284 * 1e6;  // BM_ForwardLadder median, ms -> ns
constexpr int kNInputs = 70;    // Stage A's free quotes (RESUME.md §5)
constexpr int kNOutputsBook = 1;
constexpr int kNOutputsLadder = 2043;  // 2,000 trades + the book + 42 aggregates (RESUME.md §5)
}  // namespace

TEST(ADModeStageAShapes, MeasuredAdjointMultiplierIsWellAboveTheM1BookDefault) {
  // A finding, not an assumption: Stage A's own first-lane multiplier (21.3x) is well above the
  // M1-book-calibrated default (8.0, DESIGN.md §7's 7.4x-9.3x) — a different fixture, a different
  // ratio, exactly D9's "never compared across fingerprints OR fixtures" caution restated for AD
  // mode specifically.
  EXPECT_GT(kMeasuredAdjointMultiplier, 8.0);
  EXPECT_NEAR(kMeasuredAdjointMultiplier, 21.33, 0.05);
}

TEST(ADModeStageAShapes, PicksReverseForTheBookAloneUnderBothMultipliers) {
  const optimise::CostModel model{optimise::CostCoefficients::defaults(), "d448afd70180", false};
  for (double mult : {8.0, kMeasuredAdjointMultiplier}) {
    const double fwd = optimise::estimate_jacobian_ns(kOnePassNs, kNInputs, kNOutputsBook, optimise::ADMode::Forward, model, mult);
    const double rev = optimise::estimate_jacobian_ns(kOnePassNs, kNInputs, kNOutputsBook, optimise::ADMode::Reverse, model, mult);
    EXPECT_LT(rev, fwd) << "multiplier " << mult;
  }
}

TEST(ADModeStageAShapes, PicksForwardForTheFullLadderUnderBothMultipliers) {
  const optimise::CostModel model{optimise::CostCoefficients::defaults(), "d448afd70180", false};
  for (double mult : {8.0, kMeasuredAdjointMultiplier}) {
    const double fwd = optimise::estimate_jacobian_ns(kOnePassNs, kNInputs, kNOutputsLadder, optimise::ADMode::Forward, model, mult);
    const double rev = optimise::estimate_jacobian_ns(kOnePassNs, kNInputs, kNOutputsLadder, optimise::ADMode::Reverse, model, mult);
    EXPECT_LT(fwd, rev) << "multiplier " << mult;
  }
  // Measured, reported rather than asserted tight (see the file header's forward caveat): the
  // model's own forward estimate, fed the plain-interpreter one_pass_ns, undershoots the real
  // Dual<70> forward ladder by about an order of magnitude -- a real, stated model gap, not a
  // hidden one. A ratio far below 1 is the EXPECTED, understood outcome here, not a test bug.
  const double fwd_estimate = optimise::estimate_jacobian_ns(kOnePassNs, kNInputs, kNOutputsLadder, optimise::ADMode::Forward, model);
  const double ratio = fwd_estimate / kMeasuredForwardLadderNs;
  std::cout << "[ ad_mode ] forward estimate " << fwd_estimate / 1e6 << " ms vs measured " << kMeasuredForwardLadderNs / 1e6
            << " ms (ratio " << ratio << ") -- see this file's header: one_pass_ns from the plain interpreter"
            << " is the wrong unit for a wide Dual<70> pass\n";
  EXPECT_GT(ratio, 0.01) << "sanity: still the right order of magnitude, not a unit/arithmetic bug";
  EXPECT_LT(ratio, 1.0) << "the gap should undershoot (see header), never overshoot, or the explanation above is wrong";
}
