// M3/G5 gate: the Stage A outputs O1 / O2 / O4 from the one tape through solver::ImplicitProgram
// (fixtures/stage_a.hpp). The first scenario lanes as one batch are bitwise the single-lane runs
// (O4: every output, the solved curves, the diagnostics), lane 0 the unshocked base; another
// tile / lane-tile configuration gives the same bits; the knot outputs are bitwise the solved
// state; and the book agrees with the templated double maths priced at the lane's own solved
// knots to a measured tolerance.
//
// RE-ANCHORED for the algebra phase (PRINCIPLES.md §5.2a). Every comparison here against another
// evaluation of the PINNED TAPE — batched against single lane, one tile configuration against
// another, the knot outputs against the solved state — is case 1 and stays bitwise. The three
// comparisons against `price_stage_a_at` / `price_stage_a<double>` are case 2: `epykos::compile`
// collapses the recorded coupon, so the double maths is a different rounding of the same real
// number. They became `book_error` below, measured and printed.
//
// This TU is compiled with -ffp-contract=off in every preset; the fixture (src/fixtures/
// stage_a_e0.cpp) and the interpreter kernels are pinned the same way (D25). A gate of
// scripts/mutation_test.sh (the name must keep its _e0_test.cpp suffix: scripts/mutation_test.sh
// selects the gate set by it).
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <vector>

#include "epykos/solver/implicit_program.hpp"
#include "epykos/tape/replay.hpp"
#include "epykos/verify/differential.hpp"
#include "stage_a/stage_a_test_helpers.hpp"

#ifndef EPYKOS_FP_CONTRACT_OFF
#error "an _e0_test.cpp TU must see EPYKOS_FP_CONTRACT_OFF"
#endif

namespace fixtures = epykos::fixtures;
namespace solver = epykos::solver;
using epykos::test::bits;
using epykos::test::lane_knots;
using epykos::test::stage_a;
using epykos::test::stage_a_tape;

namespace {

constexpr int B = 8;

// The gate for the book against the templated `double` maths (PRINCIPLES.md §5.2a case 2),
// MEASURED before it was set: the worst relative error over the record-point replay, the B = 1
// run and the eight scenario lanes is 4.6354e-11 (lane 7, output 8170, a netting-set aggregate;
// the per-trade PVs are 1e-13 to 1e-11). The gate is about an order of magnitude looser, and
// every call prints what it measured so drift is visible.
constexpr double kBookRel = 5.0e-10;

solver::ImplicitProgram& program() {
  static solver::ImplicitProgram p(stage_a_tape().tape, stage_a_tape().registry, fixtures::stage_a_program_options(stage_a(), 64));
  return p;
}

// The worst relative error of lane b's book outputs (out[o·B + b]) against the double book.
//
// THIS WAS A BITWISE COMPARISON. It is now a measured tolerance, because the algebra phase
// (`epykos::compile`, the `simplify` peepholes of src/tape/passes.cpp) collapses the recorded
// coupon: `price_stage_a<double>` still evaluates the recording as written, the program evaluates
// the pinned tape, and the two are different roundings of the same real number. That is
// PRINCIPLES.md §5.2a case 2 — the comparison crosses the pin — and §5.2a says to measure the
// divergence, gate about an order of magnitude looser and print the measurement. The gate is
// kBookRel below; the measured worst is printed on every call.
//
// A PV is a difference of two legs, so it is measured against the leg scale (D26); everything
// else against its own magnitude.
double book_error(const fixtures::StageALayout& L, const double* out, int Bt, int b, const fixtures::StageABook<double>& book, const char* what) {
  double worst = 0.0;
  int worst_output = -1;
  auto at = [&](int o) { return out[static_cast<std::size_t>(o) * static_cast<std::size_t>(Bt) + static_cast<std::size_t>(b)]; };
  auto check = [&](int o, double ref, double scale) {
    const double e = epykos::verify::relative_error(at(o), ref, scale);
    if (e > worst) {
      worst = e;
      worst_output = o;
    }
  };
  for (int i = 0; i < L.n_trades; ++i) {
    const std::size_t is = static_cast<std::size_t>(i);
    const double legs = std::max(std::fabs(book.leg0[is]), std::fabs(book.leg1[is]));
    check(L.pv(i), book.pv[is], legs);
    check(L.pv_usd(i), book.pv_usd[is], legs);
    check(L.leg0(i), book.leg0[is], 0.0);
    check(L.leg1(i), book.leg1[is], 0.0);
  }
  for (int c = 0; c < L.n_currencies; ++c) check(L.currency(c), book.currency_total[static_cast<std::size_t>(c)], 0.0);
  for (int n = 0; n < L.n_netting_sets; ++n) check(L.netting(n), book.netting_total[static_cast<std::size_t>(n)], 0.0);
  check(L.book, book.book_total, 0.0);
  std::cout << "[  oracle  ] " << what << " lane " << b << ": worst relative error vs the double maths " << worst << " at output " << worst_output
            << " (gate " << kBookRel << ")\n";
  return worst;
}

}  // namespace

TEST(StageAOutputsE0, ReplayAtTheRecordPointMatchesTheDoubleBook) {
  const fixtures::StageATape& t = stage_a_tape();
  const std::vector<double> out = epykos::replay(t.tape, t.tape.input_values());
  ASSERT_EQ(out.size(), t.tape.num_outputs());
  EXPECT_LT(book_error(t.layout, out.data(), 1, 0, t.record_book, "replay"), kBookRel);
  // O1: the knot outputs are the record-point solution.
  for (std::size_t c = 0; c < t.layout.knots.size(); ++c) {
    for (std::size_t k = 0; k < t.layout.knots[c].size(); ++k) {
      EXPECT_EQ(bits(out[static_cast<std::size_t>(t.layout.knots[c][k])]), bits(t.record_knots[c][k])) << "curve " << c << " knot " << k;
    }
  }
  // The residual outputs are at the solver's floor and the diagnostics are what the reports say.
  for (std::size_t k = 0; k < t.registry.blocks.size(); ++k) {
    const solver::ImplicitBlock& b = t.registry.blocks[k];
    for (int o : b.residuals) EXPECT_LT(std::fabs(out[static_cast<std::size_t>(o)]), stage_a().options.solve_tol) << "block " << k;
    EXPECT_EQ(out[static_cast<std::size_t>(b.diag_jtr_output)], t.record_reports[k].jtr_inf);
    EXPECT_EQ(out[static_cast<std::size_t>(b.diag_iterations_output)], static_cast<double>(t.record_reports[k].iterations));
  }
}

TEST(StageAOutputsE0, ProgramRunMatchesTheDoubleBookAtItsSolvedKnots) {
  const fixtures::StageA& s = stage_a();
  const fixtures::StageATape& t = stage_a_tape();
  solver::ImplicitProgram& prog = program();
  std::cout << "[  program ] state " << prog.n_state() << " (quotes), inputs " << prog.n_inputs() << ", outputs " << prog.n_outputs() << ", blocks " << prog.n_blocks()
            << ", program values " << prog.program().num_values() << '\n';
  const epykos::test::clock_type::time_point t0 = epykos::test::clock_type::now();
  const std::vector<double> out = fixtures::run_lanes(prog, {s.quotes});
  const double dt = epykos::test::seconds_since(t0);
  std::cout << "[  run     ] B = 1 at the quotes: " << dt << " s; " << prog.last_run().to_string() << '\n';
  for (int k = 0; k < prog.n_blocks(); ++k) {
    EXPECT_TRUE(prog.report(k, 0).converged) << "block " << k << ": " << solver::to_string(prog.report(k, 0));
    EXPECT_LT(prog.report(k, 0).jtr_inf, 1e-12) << "block " << k;
  }
  // Case 1 (PRINCIPLES.md §5.2a, and §5.2's standing "the slow and fast paths must agree"):
  // the scalar replay of the SAME pinned tape at the state the program solved to. Both sides are
  // below the pin, so this is bitwise. It is asserted because the algebra phase exposed a defect
  // where it does not hold — see the head comment of tests/stage_a/variants_test.cpp.
  std::vector<double> solved(prog.full_state(0), prog.full_state(0) + prog.n_inputs());
  const std::vector<double> replayed = epykos::replay(t.tape, solved);
  std::size_t vs_replay = 0;
  for (int o = 0; o < prog.n_outputs(); ++o) vs_replay += bits(out[static_cast<std::size_t>(o)]) != bits(replayed[static_cast<std::size_t>(o)]);
  EXPECT_EQ(vs_replay, 0u) << "the interpreter and the scalar replay of the same pinned tape disagree at the same state";
  // The same maths on double at the lane's solved knots through the same memoised df.
  const std::vector<std::vector<double>> z = lane_knots(prog, t, 0);
  const fixtures::StageABook<double> book = fixtures::price_stage_a_at(s, *t.set, z);
  EXPECT_LT(book_error(t.layout, out.data(), 1, 0, book, "run"), kBookRel);
  for (std::size_t c = 0; c < t.layout.knots.size(); ++c) {
    for (std::size_t k = 0; k < t.layout.knots[c].size(); ++k) {
      EXPECT_EQ(bits(out[static_cast<std::size_t>(t.layout.knots[c][k])]), bits(z[c][k])) << "curve " << c << " knot " << k;
    }
  }
  // The run-time solution is the record-time one to the solver's tolerance (a different
  // iteration path: chord policy at run time, Gauss-Newton at record time).
  double worst = 0.0;
  for (std::size_t c = 0; c < z.size(); ++c) {
    for (std::size_t k = 0; k < z[c].size(); ++k) worst = std::max(worst, std::fabs(z[c][k] - t.record_knots[c][k]));
  }
  std::cout << "[  solved  ] worst |z_run - z_record| = " << worst << "; book " << out[static_cast<std::size_t>(t.layout.book)] << " vs record "
            << t.record_book.book_total << '\n';
  EXPECT_LT(worst, 1e-10);
}

TEST(StageAOutputsE0, ScenarioLanesAreBitwiseTheSingleRuns) {
  const fixtures::StageA& s = stage_a();
  const fixtures::StageATape& t = stage_a_tape();
  solver::ImplicitProgram& prog = program();
  std::vector<std::vector<double>> lanes(s.scenario_quotes.begin(), s.scenario_quotes.begin() + B);
  ASSERT_EQ(s.scenarios[0].size, 0.0) << "lane 0 is the unshocked base";
  EXPECT_EQ(lanes[0], s.quotes);
  const int n_out = prog.n_outputs();
  const epykos::test::clock_type::time_point t0 = epykos::test::clock_type::now();
  const std::vector<double> batched = fixtures::run_lanes(prog, lanes);
  const double dt = epykos::test::seconds_since(t0);
  std::cout << "[  lanes   ] B = " << B << " scenario lanes: " << dt << " s; " << prog.last_run().to_string() << '\n';
  std::vector<solver::SolveReport> reports;
  std::vector<std::vector<double>> full(B);
  for (int b = 0; b < B; ++b) {
    for (int k = 0; k < prog.n_blocks(); ++k) reports.push_back(prog.report(k, b));
    full[static_cast<std::size_t>(b)].assign(prog.full_state(b), prog.full_state(b) + prog.n_inputs());
  }
  std::size_t mismatches = 0, state_mismatches = 0;
  double worst_book = 0.0;
  for (int b = 0; b < B; ++b) {
    const std::vector<double> single = fixtures::run_lanes(prog, {lanes[static_cast<std::size_t>(b)]});
    for (int k = 0; k < prog.n_blocks(); ++k) {
      const solver::SolveReport& r = prog.report(k, 0);
      EXPECT_TRUE(r.converged) << "lane " << b << " block " << k;
      EXPECT_LT(r.jtr_inf, 1e-12) << "lane " << b << " block " << k;
      EXPECT_EQ(r.iterations, reports[static_cast<std::size_t>(b * prog.n_blocks() + k)].iterations) << "lane " << b << " block " << k;
    }
    for (int o = 0; o < n_out; ++o) {
      if (bits(single[static_cast<std::size_t>(o)]) != bits(batched[static_cast<std::size_t>(o) * B + static_cast<std::size_t>(b)])) ++mismatches;
    }
    for (int i = 0; i < prog.n_inputs(); ++i) {
      if (bits(prog.full_state(0)[i]) != bits(full[static_cast<std::size_t>(b)][static_cast<std::size_t>(i)])) ++state_mismatches;
    }
    // The lane's book against the double maths at its solved knots: §5.2a case 2, measured, and
    // kept OUT of `mismatches` so the case-1 statement above stays a bitwise count of its own.
    const fixtures::StageABook<double> book = fixtures::price_stage_a_at(s, *t.set, lane_knots(prog, t, 0));
    worst_book = std::max(worst_book, book_error(t.layout, batched.data(), B, b, book, "scenario lane"));
  }
  // Case 1 (PRINCIPLES.md §5.2a): batched lanes against single-lane runs of the SAME pinned tape.
  // Below the pin on both sides, so it stays bitwise — a failure here is a defect, not arithmetic.
  EXPECT_EQ(mismatches, 0u) << "batched scenario lanes differ from the single runs";
  EXPECT_EQ(state_mismatches, 0u) << "solved states differ";
  EXPECT_LT(worst_book, kBookRel) << "the lanes' books against the double maths";
  std::cout << "[  grid    ] lane 0 book " << batched[static_cast<std::size_t>(t.layout.book) * B] << "; lanes 1.." << B - 1 << " (" << s.scenarios[1].family
            << " ...): ";
  for (int b = 1; b < B; ++b) std::cout << batched[static_cast<std::size_t>(t.layout.book) * B + static_cast<std::size_t>(b)] << ' ';
  std::cout << '\n';
}

TEST(StageAOutputsE0, AnotherTileConfigurationGivesTheSameBits) {
  const fixtures::StageA& s = stage_a();
  const fixtures::StageATape& t = stage_a_tape();
  solver::ProgramOptions o = fixtures::stage_a_program_options(s, 8);
  o.interpreter.tile = 37;
  o.interpreter.lane_tile = 3;
  solver::ImplicitProgram other(t.tape, t.registry, o);
  std::vector<std::vector<double>> lanes(s.scenario_quotes.begin(), s.scenario_quotes.begin() + 5);
  const std::vector<double> a = fixtures::run_lanes(program(), lanes);
  const std::vector<double> b = fixtures::run_lanes(other, lanes);
  ASSERT_EQ(a.size(), b.size());
  std::size_t mismatches = 0;
  for (std::size_t k = 0; k < a.size(); ++k) mismatches += bits(a[k]) != bits(b[k]);
  EXPECT_EQ(mismatches, 0u);
}
