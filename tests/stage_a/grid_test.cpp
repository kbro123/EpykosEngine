// M3/G5: the whole scenario grid O4 — the 1,000 seeded quote scenarios of blueprints/problems/
// stage_a.json as batch lanes of the one tape (chunks of 64), every lane a full recalibration of
// the four curves and a repricing of the 2,000 trades: every lane converges with ‖Jᵀr‖∞ < 1e-12
// on every block, lane 0 reproduces the record point, a parallel shock moves the book against
// the sign of the book's DV01, and the per-curve family leaves the other currency's trades
// untouched. Timed (informational). Not a mutation gate (the E0 lane gate is outputs_e0_test).
#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <iostream>
#include <map>
#include <vector>

#include "epykos/solver/implicit_program.hpp"
#include "stage_a/stage_a_test_helpers.hpp"

namespace fixtures = epykos::fixtures;
namespace solver = epykos::solver;
using epykos::test::bits;
using epykos::test::stage_a;
using epykos::test::stage_a_tape;

TEST(StageAGrid, EveryLaneRecalibratesAndReprices) {
  const fixtures::StageA& s = stage_a();
  const fixtures::StageATape& t = stage_a_tape();
  solver::ImplicitProgram prog(t.tape, t.registry, fixtures::stage_a_program_options(s, 64));
  const int S = s.n_scenarios();
  const std::size_t Ss = static_cast<std::size_t>(S);
  ASSERT_EQ(S, 1000);
  // The grid, chunk by chunk (run_lanes does the chunking); the per-lane reports are read per chunk.
  const int chunk = prog.max_batch();
  std::vector<double> out(static_cast<std::size_t>(prog.n_outputs()) * Ss);
  int not_converged = 0, refreshed = 0;
  double worst_jtr = 0.0;
  std::size_t solves = 0, residual_evals = 0;
  const epykos::test::clock_type::time_point t0 = epykos::test::clock_type::now();
  for (int b0 = 0; b0 < S; b0 += chunk) {
    const int B = std::min(chunk, S - b0);
    const std::vector<std::vector<double>> lanes(s.scenario_quotes.begin() + b0, s.scenario_quotes.begin() + b0 + B);
    const std::vector<double> o = fixtures::run_lanes(prog, lanes);
    for (int oo = 0; oo < prog.n_outputs(); ++oo) {
      for (int b = 0; b < B; ++b) out[static_cast<std::size_t>(oo) * Ss + static_cast<std::size_t>(b0 + b)] = o[static_cast<std::size_t>(oo) * static_cast<std::size_t>(B) + static_cast<std::size_t>(b)];
    }
    for (int b = 0; b < B; ++b) {
      for (int k = 0; k < prog.n_blocks(); ++k) {
        const solver::SolveReport& r = prog.report(k, b);
        not_converged += !r.converged;
        worst_jtr = std::max(worst_jtr, r.jtr_inf);
      }
    }
    refreshed += prog.last_run().refreshed;
    solves += static_cast<std::size_t>(prog.last_run().solves);
    residual_evals += prog.last_run().residual_evaluations;
  }
  const double dt = epykos::test::seconds_since(t0);
  std::cout << "[  grid    ] " << S << " lanes in " << dt << " s (" << dt / S * 1e3 << " ms per lane): " << solves << " block solves, " << residual_evals
            << " residual evaluations, " << refreshed << " chord refreshes, " << not_converged << " not converged, worst |J^T r|_inf " << worst_jtr << '\n';
  EXPECT_EQ(not_converged, 0);
  EXPECT_LT(worst_jtr, 1e-12);
  auto at = [&](int o, int lane) { return out[static_cast<std::size_t>(o) * Ss + static_cast<std::size_t>(lane)]; };
  // Lane 0 is the base: its knots are the run-time solution of the quotes (the record point to
  // the solver's tolerance) and its book is the same to the PV's rounding.
  for (std::size_t c = 0; c < t.layout.knots.size(); ++c) {
    for (std::size_t k = 0; k < t.layout.knots[c].size(); ++k) EXPECT_NEAR(at(t.layout.knots[c][k], 0), t.record_knots[c][k], 1e-10);
  }
  EXPECT_NEAR(at(t.layout.book, 0), t.record_book.book_total, 1e-6 * std::fabs(t.record_book.book_total) + 1.0);
  // Per family: the book under the shocks; a parallel shock of size s moves the book by about
  // s × (the book's rate DV01 per unit rate) — the sign agrees for the small shocks.
  std::map<std::string, int> counted;
  std::map<std::string, double> mean_abs_move;
  double dv01 = 0.0;   // d book / d(parallel rate shift), from the first-order lanes: use the smallest parallel shock
  int best = -1;
  for (int l = 1; l < S; ++l) {
    if (s.scenarios[static_cast<std::size_t>(l)].family != "parallel") continue;
    if (best < 0 || std::fabs(s.scenarios[static_cast<std::size_t>(l)].size) < std::fabs(s.scenarios[static_cast<std::size_t>(best)].size)) best = l;
  }
  ASSERT_GE(best, 1);
  dv01 = (at(t.layout.book, best) - at(t.layout.book, 0)) / s.scenarios[static_cast<std::size_t>(best)].size;
  int sign_disagreements = 0, parallel_lanes = 0;
  for (int l = 1; l < S; ++l) {
    const fixtures::StageAScenario& sc = s.scenarios[static_cast<std::size_t>(l)];
    const double move = at(t.layout.book, l) - at(t.layout.book, 0);
    ++counted[sc.family];
    mean_abs_move[sc.family] += std::fabs(move);
    if (sc.family == "parallel" && std::fabs(sc.size) <= 0.0025) {
      ++parallel_lanes;
      sign_disagreements += (move * (dv01 * sc.size) < 0.0);
    }
    if (sc.family == "per_curve") {
      // A shock of one curve leaves the trades of the other currency exactly as in lane 0 (the
      // same quotes calibrate the same curves bitwise, lane by lane).
      const std::string ccy = s.sets[static_cast<std::size_t>(sc.curve)].instruments.front().instrument.currency;
      for (int i = 0; i < s.n_trades(); ++i) {
        if (s.instruments[static_cast<std::size_t>(i)].currency == ccy) continue;
        EXPECT_EQ(bits(at(t.layout.pv(i), l)), bits(at(t.layout.pv(i), 0))) << "lane " << l << " (" << ccy << " shocked) moved trade " << i;
      }
    }
  }
  std::cout << "[  families] base book " << at(t.layout.book, 0) << "; d book / d parallel shift " << dv01 << " per unit rate;";
  for (const auto& [name, n] : counted) std::cout << ' ' << name << ' ' << n << " lanes, mean |move| " << mean_abs_move[name] / n;
  std::cout << "; sign disagreements among the " << parallel_lanes << " parallel lanes within 25 bp: " << sign_disagreements << '\n';
  EXPECT_EQ(sign_disagreements, 0);
}
