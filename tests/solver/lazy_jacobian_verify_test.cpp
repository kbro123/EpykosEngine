// The exit Jacobian is built only when something will use it — and deferring it changes nothing.
//
// WHAT CHANGED (D85, D87). `BlockSolver::solve` used to build the full n_z-column Jacobian at the
// solution on EVERY call, for two purposes with very different costs: the ‖JᵀF‖∞ diagnostic, and
// the factorisation the IFT needs. The diagnostic is a VECTOR, JᵀF, and one reverse lane seeded
// with F computes it 10.6x cheaper than materialising the matrix to contract it back down
// (52.07 us against 4.91 us, D87 §3). So the diagnostic now always takes the matrix-free path,
// and the factorisation is built only when `want_factors` says a caller is going to use it —
// `ImplicitProgram::adjoint` passes true, `run` passes false.
//
// WHY THIS FILE EXISTS. `PRINCIPLES.md` §4a states the gate for anything that touches the solver,
// and D85 restated it for this shape. Clauses 1 and 3 (an uncached default; `refreshed` reported)
// are vacuous here — there is no cache and nothing to refresh. The two that are not:
//
//   * CLAUSE 2, and it is STRONGER than the original. Laziness is not an approximation: when the
//     build happens it is the same call at the same point, so the ladder must be BITWISE what the
//     eager path produced — not within a tolerance of it. The sharpest form of that, and the one
//     a defect would actually break, is HISTORY INDEPENDENCE: an `adjoint` must not care what
//     `run` calls came before it. That is exactly the property D85 measured a solve cache
//     destroying, which is why it is asserted bitwise here.
//   * CLAUSE 4, the mutant `solver.lazy_jacobian_never_builds`, which honours the deferral even
//     when the caller asked for the factorisation.
//
// AND THE TRAP THIS FILE IS SHAPED AROUND. D85's first version of the ladder check ran at the
// RECORD POINT, where a stale Jacobian IS the Jacobian at the solution. It returned 0.00e+00 for
// every configuration and would have shipped the wrong conclusion. Every check below therefore
// exercises a MOVED market. A check whose control case is the only case it runs is not a check.
#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <iostream>
#include <vector>

#include "epykos/fixtures/compare_ois.hpp"
#include "epykos/solver/implicit_program.hpp"

namespace {

using namespace epykos;

fixtures::CompareOisOptions book() {
  fixtures::CompareOisOptions o;
  o.trades = 8;
  o.tenors = {"1Y", "2Y", "3Y", "5Y", "7Y", "10Y", "20Y", "30Y"};
  return o;
}

// The IFT ladder d(book)/d(quote) at `quotes`.
std::vector<double> ladder_at(solver::ImplicitProgram& prog, const fixtures::CompareOisTape& t,
                              const std::vector<double>& quotes) {
  const std::vector<int> ordinals = {t.book_output};
  return fixtures::compare_ois_ladder(prog, quotes, ordinals, nullptr);
}

std::vector<double> shifted(const std::vector<double>& q, double bp) {
  std::vector<double> out = q;
  for (double& v : out) v += bp;
  return out;
}

}  // namespace

TEST(LazyJacobianVerify, TheLadderDoesNotDependOnWhatRanBefore) {
  // Clause 2, bitwise. `run` now leaves each lane's Factors invalid; if `adjoint` ever read a
  // stale one instead of building its own, the ladder would depend on the call history — and at
  // the record point it would still look perfect, which is why every market below is moved.
  const fixtures::CompareOis s = fixtures::make_compare_ois(book());
  fixtures::CompareOisTape t = fixtures::record_compare_ois(s);
  const int n_q = s.n_quotes(), n_out = 0;
  (void)n_out;

  for (const double bp : {1e-4, 25e-4, 100e-4}) {
    const std::vector<double> q = shifted(s.quotes, bp);

    // (a) a program that has done nothing else
    solver::ImplicitProgram fresh(t.tape, t.registry, fixtures::compare_ois_program_options(s));
    const std::vector<double> clean = ladder_at(fresh, t, q);

    // (b) the same program after a run of OTHER markets, which is what would leave stale factors
    solver::ImplicitProgram used(t.tape, t.registry, fixtures::compare_ois_program_options(s));
    std::vector<double> st(static_cast<std::size_t>(n_q)), out(static_cast<std::size_t>(used.n_outputs()));
    for (const double other : {-50e-4, 200e-4, 0.0}) {
      for (int j = 0; j < n_q; ++j) st[static_cast<std::size_t>(j)] = s.quotes[static_cast<std::size_t>(j)] + other;
      used.run(st.data(), 1, out.data());
    }
    const std::vector<double> after = ladder_at(used, t, q);

    ASSERT_EQ(clean.size(), after.size());
    ASSERT_FALSE(clean.empty());
    EXPECT_EQ(std::memcmp(clean.data(), after.data(), clean.size() * sizeof(double)), 0)
        << "at " << bp * 1e4 << "bp the ladder depends on the preceding run() calls: deferring the "
        << "exit Jacobian must not make the answer a function of history (PRINCIPLES.md §5.2)";
  }
}

TEST(LazyJacobianVerify, TheLadderStillAgreesWithBumpOnAMovedMarket) {
  // Clause 4's catcher. With `solver.lazy_jacobian_never_builds` the IFT runs on a stale or absent
  // Jacobian; on a MOVED market that is a visibly wrong ladder, and at the record point it is not.
  const fixtures::CompareOis s = fixtures::make_compare_ois(book());
  fixtures::CompareOisTape t = fixtures::record_compare_ois(s);
  solver::ImplicitProgram prog(t.tape, t.registry, fixtures::compare_ois_program_options(s));
  const int n_q = s.n_quotes(), n_out = prog.n_outputs();
  const double h = 1e-6;

  for (const double bp : {25e-4, 100e-4}) {
    const std::vector<double> q = shifted(s.quotes, bp);
    const std::vector<double> rows = ladder_at(prog, t, q);
    ASSERT_EQ(rows.size(), static_cast<std::size_t>(n_q));
    double ladder_inf = 0.0;
    for (double v : rows) ladder_inf = std::max(ladder_inf, std::fabs(v));
    ASSERT_GT(ladder_inf, 0.0)
        << "the whole ladder is zero at " << bp * 1e4 << "bp, which is what an IFT that never "
        << "received a factorised Jacobian produces";

    std::vector<double> st(static_cast<std::size_t>(n_q) * 2), out(static_cast<std::size_t>(n_out) * 2);
    double worst = 0.0;
    for (int k = 0; k < n_q; ++k) {
      for (int j = 0; j < n_q; ++j) {
        const std::size_t jj = static_cast<std::size_t>(j);
        st[jj * 2 + 0] = q[jj] + (j == k ? h : 0.0);
        st[jj * 2 + 1] = q[jj] - (j == k ? h : 0.0);
      }
      prog.run(st.data(), 2, out.data());
      const std::size_t b = static_cast<std::size_t>(t.book_output) * 2;
      const double fd = (out[b + 0] - out[b + 1]) / (2.0 * h);
      worst = std::max(worst, std::fabs(fd - rows[static_cast<std::size_t>(k)]) / ladder_inf);
    }
    std::cout << "[lazy_jacobian] at " << bp * 1e4 << "bp: IFT vs bump worst " << worst
              << " relative to ‖ladder‖∞ " << ladder_inf << "\n";
    EXPECT_LT(worst, 1e-6) << "at " << bp * 1e4 << "bp the IFT ladder disagrees with "
                           << "bump-and-recalibrate: the deferred Jacobian was not built, or was "
                           << "built somewhere other than at this market's solution";
  }
}

TEST(LazyJacobianVerify, TheDiagnosticSurvivesTheDeferral) {
  // ‖JᵀF‖∞ no longer comes from the materialised J. It must still be a real number on every path
  // (it was NaN under chord-without-refresh-without-final_jacobian before), and still tiny at a
  // converged solution — including on a market the record point never saw.
  const fixtures::CompareOis s = fixtures::make_compare_ois(book());
  fixtures::CompareOisTape t = fixtures::record_compare_ois(s);
  const int n_q = s.n_quotes();

  for (const int jac : {-1, static_cast<int>(solver::JacobianPolicy::chord)}) {
    for (const bool warm : {false, true}) {
      solver::ProgramOptions po = fixtures::compare_ois_program_options(s);
      po.jacobian = jac;
      po.warm_start = warm;
      solver::ImplicitProgram prog(t.tape, t.registry, po);
      std::vector<double> st(static_cast<std::size_t>(n_q)), out(static_cast<std::size_t>(prog.n_outputs()));
      for (int j = 0; j < n_q; ++j) st[static_cast<std::size_t>(j)] = s.quotes[static_cast<std::size_t>(j)] + 25e-4;
      prog.run(st.data(), 1, out.data());
      const solver::SolveReport& r = prog.report(0, 0);
      EXPECT_TRUE(r.converged) << "jac=" << jac << " warm=" << warm;
      EXPECT_FALSE(std::isnan(r.jtr_inf))
          << "jac=" << jac << " warm=" << warm
          << ": the optimality diagnostic is an OUTPUT of the tape and must exist on every path";
      EXPECT_LT(r.jtr_inf, 1e-10)
          << "jac=" << jac << " warm=" << warm << ": ‖JᵀF‖∞ = " << r.jtr_inf << " at a converged solve";
    }
  }
}
