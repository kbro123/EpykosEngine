// ‖JᵀF‖∞ is optional, and switching it off leaves NaN rather than a plausible number.
//
// WHY. Since D89 removed the eager exit Jacobian, the optimality diagnostic is the LARGEST single
// fixed cost of a warm solve: 5.5 us of a 19.9 us re-quote (28%), and more than half the 9.79 us
// floor at the record point where nothing iterates (D91 §2). It is already computed the cheap way —
// one matrix-free reverse lane, 4.91 us against 52.07 us for the materialised Jacobian (D87 §3) —
// so the only remaining saving is not computing it at all. A caller re-quoting a curve thousands of
// times and never reading the diagnostic should not pay for it.
//
// WHAT THIS GATES, and the second clause is the one that matters:
//
//   1. Default ON is today's behaviour. The knots and the diagnostic are BITWISE what they were.
//   2. OFF leaves NaN, and specifically NOT ZERO. This repository keeps finding the same defect
//      shape — D81 §6(a)'s silently unwritten outputs read back as plausible 0.0, D89 §3's
//      all-zero risk ladder from an unbuilt factorisation. Both were numbers that looked fine.
//      Zero is the worst value available here, because ‖JᵀF‖∞ = 0 is the signature of a perfectly
//      converged solve: a switched-off diagnostic reporting it would announce success. So the
//      mutant `solver.diagnostic_returns_zero` reports 0.0 and this file catches it.
//   3. OFF changes nothing else. The solved knots are bitwise identical with it on and off, because
//      the diagnostic is computed AFTER convergence and feeds nothing.
//
// `PROBLEM.md` §6's O1 gate reads this output, so the gated builds keep it on. This is a knob for
// the hot path, not a weakening of what the engine reports by default.
#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <vector>

#include "epykos/fixtures/compare_ois.hpp"
#include "epykos/solver/implicit_program.hpp"

namespace {

using namespace epykos;

fixtures::CompareOisOptions book() {
  fixtures::CompareOisOptions o;
  o.trades = 4;
  o.tenors = {"1Y", "2Y", "3Y", "5Y", "7Y", "10Y", "20Y", "30Y"};
  return o;
}

// Solves at `quotes` and returns (the solved knots, the diagnostic).
struct Solved {
  std::vector<double> knots;
  double jtr = 0.0;
  bool converged = false;
};

Solved solve_at(const fixtures::CompareOis& s, fixtures::CompareOisTape& t, bool diagnostic,
             bool warm, int jac, double bp) {
  solver::ProgramOptions po = fixtures::compare_ois_program_options(s);
  po.warm_start = warm;
  po.jacobian = jac;
  // The block's own options carry the switch; ProgramOptions has no override for it by design.
  fixtures::CompareOisTape local = fixtures::record_compare_ois(s);
  for (solver::ImplicitBlock& b : local.registry.blocks) b.options.optimality_diagnostic = diagnostic;
  solver::ImplicitProgram prog(local.tape, local.registry, po);
  const int n_q = prog.n_state();
  std::vector<double> st(static_cast<std::size_t>(n_q)), out(static_cast<std::size_t>(prog.n_outputs()));
  for (int j = 0; j < n_q; ++j) st[static_cast<std::size_t>(j)] = s.quotes[static_cast<std::size_t>(j)] + bp;
  prog.run(st.data(), 1, out.data());
  Solved r;
  for (int k : local.knot_outputs) r.knots.push_back(out[static_cast<std::size_t>(k)]);
  r.jtr = prog.report(0, 0).jtr_inf;
  r.converged = prog.report(0, 0).converged;
  (void)t;
  return r;
}

}  // namespace

TEST(OptionalDiagnosticVerify, OffLeavesNaNNotAZeroValue) {
  // Clause 2, and the mutant's catcher. Three market moves, including +0bp where the solve
  // converges immediately and a zero would be most convincing.
  const fixtures::CompareOis s = fixtures::make_compare_ois(book());
  fixtures::CompareOisTape t = fixtures::record_compare_ois(s);
  const int CH = static_cast<int>(solver::JacobianPolicy::chord);

  for (const double bp : {0.0, 1e-4, 25e-4}) {
    const Solved off = solve_at(s, t, /*diagnostic=*/false, /*warm=*/true, CH, bp);
    ASSERT_TRUE(off.converged) << "at " << bp * 1e4 << "bp";
    EXPECT_TRUE(std::isnan(off.jtr))
        << "at " << bp * 1e4 << "bp the diagnostic is switched off but reported " << off.jtr
        << ". A switched-off diagnostic must be NaN: zero is the signature of a converged solve, "
        << "so it is a plausible number where there should be an unmistakable one (D81 §6(a), D89 §3)";
  }
}

TEST(OptionalDiagnosticVerify, OnIsUnchangedAndOffDoesNotMoveTheAnswer) {
  // Clauses 1 and 3. The knots must be BITWISE identical with the diagnostic on and off, because it
  // is computed after convergence and feeds nothing; and with it on it must still be a real, tiny
  // number at a converged solve.
  const fixtures::CompareOis s = fixtures::make_compare_ois(book());
  fixtures::CompareOisTape t = fixtures::record_compare_ois(s);
  const int CH = static_cast<int>(solver::JacobianPolicy::chord);

  for (const int jac : {-1, CH}) {
    for (const double bp : {0.0, 1e-4, 100e-4}) {
      const Solved on = solve_at(s, t, /*diagnostic=*/true, /*warm=*/true, jac, bp);
      const Solved off = solve_at(s, t, /*diagnostic=*/false, /*warm=*/true, jac, bp);
      ASSERT_TRUE(on.converged && off.converged) << "jac=" << jac << " bp=" << bp * 1e4;
      ASSERT_EQ(on.knots.size(), off.knots.size());
      ASSERT_FALSE(on.knots.empty());
      EXPECT_EQ(std::memcmp(on.knots.data(), off.knots.data(), on.knots.size() * sizeof(double)), 0)
          << "jac=" << jac << " at " << bp * 1e4 << "bp: switching the diagnostic off moved the "
          << "solved knots. It is computed after convergence and feeds nothing, so it must not";
      EXPECT_FALSE(std::isnan(on.jtr)) << "jac=" << jac << " bp=" << bp * 1e4;
      EXPECT_LT(on.jtr, 1e-10)
          << "jac=" << jac << " at " << bp * 1e4 << "bp: ‖JᵀF‖∞ = " << on.jtr << " at a converged solve";
    }
  }
}
