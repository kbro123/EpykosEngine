// M3/G6 gate: the differential ball over the quotes of the Stage A tape (PROBLEM.md section 6
// "differential ball (E0 under the reference preset)"; DESIGN.md section 11; the M2 harness of
// verify/differential.hpp). The state is the 70 free inputs (the quotes, StageA::quotes order);
// the ball is z + rho·u with rho = 0.005 (the WORKLOADS section M2 radius: 50 bp on a rate
// quote, 0.005 on a futures price), R draws from Philox sub-stream 200000 + r.
//
// Compiled side: solver::ImplicitProgram over the one tape, B lanes at a time — every lane
// recalibrates the four curves (chord policy, the scenario options) and the whole-program
// interpreter prices the book. Reference side, per draw: a SEPARATE single-lane ImplicitProgram
// solves the same quotes and the templated maths on double is evaluated at the knots it found —
// price_stage_a_at (O2: every trade, both legs, the aggregates, all in src/fixtures/
// stage_a_e0.cpp, pinned to -ffp-contract=off), CurveSet::residuals (the instrument residuals on
// double, instantiated in the same pinned TU), the knots themselves (O1) and the solve's
// diagnostics. So a pass says: at every draw, every one of the 8,191 outputs of the batched
// program is bitwise the double maths at the knots the single-lane solve finds (which includes
// "the batched solve is bitwise the single solve", O4's lane statement, at random quotes).
//
// The linear algebra of the solves (Eigen, src/solver/residual.cpp) is not pinned, so the knots
// may differ in their last bits between presets; within one preset both sides run the same
// code. The gate is stated for the reference preset (CLAUDE.md Build: an E0 gate that crosses
// into unpinned src/ code runs there); it is run under release too and reported.
//
// A gate of scripts/mutation_test.sh (the name ends in _e0_test).
#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <iostream>
#include <memory>
#include <vector>

#include "epykos/solver/implicit_program.hpp"
#include "epykos/verify/differential.hpp"
#include "stage_a/stage_a_test_helpers.hpp"

#ifndef EPYKOS_FP_CONTRACT_OFF
#error "an _e0_test.cpp TU must see EPYKOS_FP_CONTRACT_OFF"
#endif

namespace fixtures = epykos::fixtures;
namespace solver = epykos::solver;
namespace verify = epykos::verify;
using epykos::test::lane_knots;
using epykos::test::stage_a;
using epykos::test::stage_a_tape;

namespace {

constexpr int R = 64;   // draws (stated: fewer than the M1 ball's 256; every draw is a full recalibration)

struct Reference {
  std::unique_ptr<solver::ImplicitProgram> single;   // the reference's own solve, one lane
  std::vector<int> first;                            // CurveSet instrument index of each slot's first instrument
  std::vector<double> tmp;
  double seconds = 0.0;
  int calls = 0;

  Reference() {
    const fixtures::StageA& s = stage_a();
    const fixtures::StageATape& t = stage_a_tape();
    single = std::make_unique<solver::ImplicitProgram>(t.tape, t.registry, fixtures::stage_a_program_options(s, 1));
    first.assign(static_cast<std::size_t>(s.n_curves()) + 1, 0);
    for (int c = 0; c < s.n_curves(); ++c) first[static_cast<std::size_t>(c) + 1] = first[static_cast<std::size_t>(c)] + s.sets[static_cast<std::size_t>(c)].n_instruments();
    tmp.assign(static_cast<std::size_t>(t.tape.num_outputs()), 0.0);
  }

  // The reference for one draw: out[0..n_outputs) in the tape's output order.
  void operator()(const double* z, double* out) {
    const fixtures::StageA& s = stage_a();
    const fixtures::StageATape& t = stage_a_tape();
    const epykos::test::clock_type::time_point t0 = epykos::test::clock_type::now();
    single->run(z, 1, tmp.data());   // the single-lane solve; tmp is not read
    const std::vector<std::vector<double>> knots = lane_knots(*single, t, 0);
    // O1: the knots, and the solve's diagnostics.
    for (std::size_t c = 0; c < knots.size(); ++c) {
      for (std::size_t k = 0; k < knots[c].size(); ++k) out[static_cast<std::size_t>(t.layout.knots[c][k])] = knots[c][k];
    }
    // The residual outputs: the instrument residuals on double at those knots (block k holds one
    // curve, its residuals in the set's instrument order = the quote order of that slot).
    std::vector<double> all;
    for (const std::vector<double>& v : knots) all.insert(all.end(), v.begin(), v.end());
    const std::vector<double> q(z, z + s.n_quotes());
    const std::vector<double> F = t.set->residuals(t.set->states_at(all), q);
    for (std::size_t k = 0; k < t.registry.blocks.size(); ++k) {
      const solver::ImplicitBlock& b = t.registry.blocks[k];
      const solver::SolveReport& rep = single->report(static_cast<int>(k), 0);
      out[static_cast<std::size_t>(b.diag_jtr_output)] = rep.jtr_inf;
      out[static_cast<std::size_t>(b.diag_iterations_output)] = static_cast<double>(rep.iterations);
      std::size_t m = 0;
      for (int c : t.block_curves[k]) {
        for (int j = 0; j < s.sets[static_cast<std::size_t>(c)].n_instruments(); ++j, ++m) {
          out[static_cast<std::size_t>(b.residuals[m])] = F[static_cast<std::size_t>(first[static_cast<std::size_t>(c)] + j)];
        }
      }
    }
    // O2: the book on double at the knots, through the same memoised df as the recording.
    const fixtures::StageABook<double> book = fixtures::price_stage_a_at(s, *t.set, knots);
    const fixtures::StageALayout& L = t.layout;
    for (int i = 0; i < L.n_trades; ++i) {
      const std::size_t is = static_cast<std::size_t>(i);
      out[static_cast<std::size_t>(L.pv(i))] = book.pv[is];
      out[static_cast<std::size_t>(L.pv_usd(i))] = book.pv_usd[is];
      out[static_cast<std::size_t>(L.leg0(i))] = book.leg0[is];
      out[static_cast<std::size_t>(L.leg1(i))] = book.leg1[is];
    }
    for (int c = 0; c < L.n_currencies; ++c) out[static_cast<std::size_t>(L.currency(c))] = book.currency_total[static_cast<std::size_t>(c)];
    for (int n = 0; n < L.n_netting_sets; ++n) out[static_cast<std::size_t>(L.netting(n))] = book.netting_total[static_cast<std::size_t>(n)];
    out[static_cast<std::size_t>(L.book)] = book.book_total;
    seconds += epykos::test::seconds_since(t0);
    ++calls;
  }
};

Reference& reference() {
  static Reference r;
  return r;
}

verify::StateBall ball(int draws) {
  const fixtures::StageA& s = stage_a();
  verify::BallOptions o;
  o.rho = 0.0050;
  o.draws = draws;
  return verify::make_state_ball(s.quotes.data(), s.n_quotes(), o);
}

void print(const char* what, const verify::Report& rep) { std::cout << "[  " << what << " ] " << rep.summary() << '\n'; }

}  // namespace

TEST(StageAGateDifferentialE0, TheBallIsOverTheQuotes) {
  const fixtures::StageA& s = stage_a();
  const verify::StateBall b = ball(R);
  ASSERT_EQ(b.n_inputs, s.n_quotes());
  ASSERT_EQ(b.n_draws, R);
  double max_dev = 0.0;
  for (int r = 0; r < b.n_draws; ++r) {
    for (int k = 0; k < b.n_inputs; ++k) max_dev = std::max(max_dev, std::fabs(b.state(r)[k] - s.quotes[static_cast<std::size_t>(k)]));
  }
  EXPECT_LE(max_dev, 0.0050);
  EXPECT_GT(max_dev, 0.004);
  std::cout << "[  ball    ] " << R << " draws in [q - 0.005, q + 0.005]^" << s.n_quotes() << ", max |q_k - q0_k| = " << max_dev << " (rate units; a futures price moves by the same amount)\n";
}

TEST(StageAGateDifferentialE0, BatchedProgramIsBitwiseTheDoubleMathsAtTheSingleSolvesKnots) {
  const fixtures::StageA& s = stage_a();
  const fixtures::StageATape& t = stage_a_tape();
  constexpr int B = 8;
  solver::ImplicitProgram prog(t.tape, t.registry, fixtures::stage_a_program_options(s, B));
  const int n_out = prog.n_outputs();
  Reference& ref = reference();
  ref.seconds = 0.0;
  ref.calls = 0;
  verify::DifferentialOptions o;
  o.tolerance = verify::Tolerance::e0();
  o.batch = B;
  const epykos::test::clock_type::time_point t0 = epykos::test::clock_type::now();
  const verify::Report rep = verify::differential(
      ball(R), n_out, [&ref](const double* z, double* out) { ref(z, out); }, [&prog](const double* state, int Bn, double* out) { prog.run(state, Bn, out); }, o);
  const double dt = epykos::test::seconds_since(t0);
  print("B=8 ", rep);
  std::cout << "[  cost    ] " << R << " draws in " << dt << " s: reference (single solve + double maths) " << ref.seconds << " s over " << ref.calls
            << " calls; compiled " << dt - ref.seconds << " s\n";
  EXPECT_TRUE(rep.passed) << rep.summary();
  EXPECT_TRUE(rep.bitwise_equal);
  EXPECT_EQ(rep.mismatches, 0u);
  EXPECT_EQ(rep.n_draws, R);
  EXPECT_EQ(rep.n_outputs, n_out);
  std::cout << "[  GATE    ] differential_e0 ok=" << (rep.passed ? 1 : 0) << " draws=" << R << " batch=" << B << " outputs=" << n_out << " mismatches=" << rep.mismatches
            << " max_ulps=" << rep.max_ulps << '\n';
}

TEST(StageAGateDifferentialE0, SingleLaneProgramIsBitwiseTooOnTheFirstDraws) {
  const fixtures::StageA& s = stage_a();
  const fixtures::StageATape& t = stage_a_tape();
  constexpr int R1 = 16;
  solver::ImplicitProgram prog(t.tape, t.registry, fixtures::stage_a_program_options(s, 1));
  Reference& ref = reference();
  verify::DifferentialOptions o;
  o.tolerance = verify::Tolerance::e0();
  o.batch = 1;
  const verify::Report rep = verify::differential(
      ball(R1), prog.n_outputs(), [&ref](const double* z, double* out) { ref(z, out); }, [&prog](const double* state, int Bn, double* out) { prog.run(state, Bn, out); }, o);
  print("B=1 ", rep);
  EXPECT_TRUE(rep.passed) << rep.summary();
  EXPECT_TRUE(rep.bitwise_equal);
  EXPECT_EQ(rep.n_draws, R1);
}
