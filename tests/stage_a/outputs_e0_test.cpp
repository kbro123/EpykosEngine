// M3/G5 E0 gate: the Stage A outputs O1 / O2 / O4 from the one tape through solver::ImplicitProgram
// (fixtures/stage_a.hpp). The replay of the recording at its record point is bitwise the double
// maths (the record-point book); a run of the whole program at the quotes is bitwise the double
// maths priced at the lane's own solved knots (O2 per trade, per leg, the aggregates; O1 the
// knot outputs are the solved state); the first scenario lanes as one batch are bitwise the
// single-lane runs (O4: every output, the solved curves, the diagnostics), lane 0 the unshocked
// base; another tile / lane-tile configuration gives the same bits.
//
// This TU is compiled with -ffp-contract=off in every preset; the fixture (src/fixtures/
// stage_a_e0.cpp) and the interpreter kernels are pinned the same way (D25). A gate of
// scripts/mutation_test.sh.
#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <iostream>
#include <vector>

#include "epykos/solver/implicit_program.hpp"
#include "epykos/tape/replay.hpp"
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

solver::ImplicitProgram& program() {
  static solver::ImplicitProgram p(stage_a_tape().tape, stage_a_tape().registry, fixtures::stage_a_program_options(stage_a(), 64));
  return p;
}

// out[o·B + b] of the book outputs of lane b vs the double book: mismatching bit patterns.
std::size_t compare_book(const fixtures::StageALayout& L, const double* out, int Bt, int b, const fixtures::StageABook<double>& book, const char* what) {
  std::size_t mismatches = 0;
  auto at = [&](int o) { return out[static_cast<std::size_t>(o) * static_cast<std::size_t>(Bt) + static_cast<std::size_t>(b)]; };
  auto check = [&](int o, double ref, const char* name, int i) {
    if (bits(at(o)) != bits(ref)) {
      if (mismatches < 5) ADD_FAILURE() << what << ": lane " << b << ' ' << name << ' ' << i << ": program " << at(o) << " vs double " << ref;
      ++mismatches;
    }
  };
  for (int i = 0; i < L.n_trades; ++i) {
    const std::size_t is = static_cast<std::size_t>(i);
    check(L.pv(i), book.pv[is], "pv", i);
    check(L.pv_usd(i), book.pv_usd[is], "pv_usd", i);
    check(L.leg0(i), book.leg0[is], "leg0", i);
    check(L.leg1(i), book.leg1[is], "leg1", i);
  }
  for (int c = 0; c < L.n_currencies; ++c) check(L.currency(c), book.currency_total[static_cast<std::size_t>(c)], "currency", c);
  for (int n = 0; n < L.n_netting_sets; ++n) check(L.netting(n), book.netting_total[static_cast<std::size_t>(n)], "netting", n);
  check(L.book, book.book_total, "book", 0);
  return mismatches;
}

}  // namespace

TEST(StageAOutputsE0, ReplayAtTheRecordPointIsBitwiseTheDoubleBook) {
  const fixtures::StageATape& t = stage_a_tape();
  const std::vector<double> out = epykos::replay(t.tape, t.tape.input_values());
  ASSERT_EQ(out.size(), t.tape.num_outputs());
  EXPECT_EQ(compare_book(t.layout, out.data(), 1, 0, t.record_book, "replay"), 0u);
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

TEST(StageAOutputsE0, ProgramRunIsBitwiseTheDoubleBookAtItsSolvedKnots) {
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
  // The same maths on double at the lane's solved knots through the same memoised df.
  const std::vector<std::vector<double>> z = lane_knots(prog, t, 0);
  const fixtures::StageABook<double> book = fixtures::price_stage_a_at(s, *t.set, z);
  EXPECT_EQ(compare_book(t.layout, out.data(), 1, 0, book, "run"), 0u);
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
    // The lane's book is the double maths at its solved knots.
    const fixtures::StageABook<double> book = fixtures::price_stage_a_at(s, *t.set, lane_knots(prog, t, 0));
    mismatches += compare_book(t.layout, batched.data(), B, b, book, "scenario lane");
  }
  EXPECT_EQ(mismatches, 0u) << "batched scenario lanes differ from the single runs";
  EXPECT_EQ(state_mismatches, 0u) << "solved states differ";
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
