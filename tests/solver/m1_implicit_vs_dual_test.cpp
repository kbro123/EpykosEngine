// M3/G4: the IFT adjoint vs forward mode through the calibration (the M2/Q3b pattern at 1e-12).
//
// The forward-mode side is the same templated maths instantiated on Dual<12>: the 12 quotes are
// the seeded directions, solver/tangent.hpp's implicit_dual applies the forward rule
// dz = −F_z⁻¹ (F_p dp) at the solution (F_z from 12 passes of the residual on Dual<1>, F_p dp
// from one pass on Dual<12>), and price_book<Dual<12>> carries the tangents through the book:
// every output's d out / dq_k. The adjoint side is ImplicitProgram::adjoint over all 1,001 book
// outputs, one lane per output (16 batched calls of up to 64 lanes, the same state in every
// lane: one solve per call), compared per entry at
//   |adj − dual| <= 1e-12 · max(|dual|, scale),  scale = |d fixed_i/dq_k| + |d float_i/dq_k|
// (D26 read for a derivative: a swap PV's derivative is a difference of two legs' derivatives);
// the book's scale is the sum over swaps. Both sides differentiate at the SAME solution (the
// Dual pass starts at the adjoint's z*, where ‖F‖∞ is already below its tolerance), so the
// comparison is of the derivative rules, not of two solvers' last bits. The Dual value solve
// from the flat start is checked against the recorded solve as well (an independent O1 oracle).
//
// Named *_vs_dual_test: it is in the mutation harness's gate set (D33).
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <vector>

#include "epykos/fixtures/m1_book.hpp"
#include "epykos/fixtures/m1_calibration.hpp"
#include "epykos/fixtures/m1_price.hpp"
#include "epykos/scalar/dual.hpp"
#include "epykos/solver/implicit_program.hpp"
#include "epykos/solver/tangent.hpp"

namespace fixtures = epykos::fixtures;
namespace solver = epykos::solver;
using epykos::Dual;

namespace {

constexpr int n_knots = fixtures::n_knots;
constexpr int n_out = fixtures::n_swaps + 1;
using Wide = Dual<n_knots>;

struct Fixture {
  fixtures::Book book;
  std::vector<double> quotes;
  fixtures::CalibratedM1 cal;
  std::vector<fixtures::CalSwap> swaps;
};

const Fixture& fixture() {
  static const Fixture f = [] {
    Fixture x;
    x.book = fixtures::make_m1_book();
    x.quotes = fixtures::m1_par_quotes(x.book.z0.data());
    std::vector<double> start(n_knots, 0.03);
    x.cal = fixtures::record_m1_calibrated(x.book, x.quotes, start);
    x.swaps = fixtures::m1_calibration_swaps();
    return x;
  }();
  return f;
}

}  // namespace

TEST(M1ImplicitVsDual, DualValueSolveFromTheFlatStartAgreesWithTheRecordedSolve) {
  const Fixture& f = fixture();
  auto R = [&](const auto* z, const auto* q, auto* F) { fixtures::m1_residual(f.swaps, z, q, F); };
  std::vector<double> z(n_knots, 0.03);
  solver::SolveOptions o;
  const solver::SolveReport rep = solver::solve_newton(R, n_knots, n_knots, n_knots, f.quotes.data(), z.data(), o);
  std::cout << "[  dual    ] " << solver::to_string(rep) << '\n';
  EXPECT_TRUE(rep.converged);
  EXPECT_LT(rep.jtr_inf, 1e-12);
  double worst = 0.0;
  for (int k = 0; k < n_knots; ++k) worst = std::max(worst, std::fabs(z[static_cast<std::size_t>(k)] - f.cal.z_record[static_cast<std::size_t>(k)]));
  std::cout << "[  dual    ] worst |z_dual - z_recorded| = " << worst << '\n';
  EXPECT_LE(worst, 1e-13);
}

TEST(M1ImplicitVsDual, FullJacobianOfEveryOutputMatchesForwardModeThroughTheCalibration) {
  const Fixture& f = fixture();
  solver::ImplicitProgram prog(f.cal.tape, f.cal.registry);
  const int n_tape_out = prog.n_outputs();
  // The adjoint's solution.
  std::vector<double> out1(static_cast<std::size_t>(n_tape_out));
  prog.run(f.quotes.data(), 1, out1.data());
  std::vector<double> zstar(prog.full_state(0) + f.cal.registry.blocks[0].unknowns[0], prog.full_state(0) + f.cal.registry.blocks[0].unknowns[0] + n_knots);
  // Forward mode at that solution: q as Dual<12> variables, z = implicit_dual (0 Newton steps:
  // the residual at z* is below the tolerance), then the book on Dual<12>.
  auto R = [&](const auto* z, const auto* q, auto* F) { fixtures::m1_residual(f.swaps, z, q, F); };
  std::vector<Wide> qd(n_knots), zd(n_knots);
  for (int k = 0; k < n_knots; ++k) {
    qd[static_cast<std::size_t>(k)] = Wide::variable(f.quotes[static_cast<std::size_t>(k)], k);
    zd[static_cast<std::size_t>(k)] = Wide(zstar[static_cast<std::size_t>(k)]);
  }
  solver::SolveOptions o;
  o.tol = 1e-12;
  const solver::SolveReport rep = solver::implicit_dual<n_knots>(R, n_knots, n_knots, n_knots, qd.data(), zd.data(), o);
  EXPECT_EQ(rep.iterations, 0) << "the Dual pass must differentiate at the adjoint's own solution";
  for (int k = 0; k < n_knots; ++k) EXPECT_EQ(zd[static_cast<std::size_t>(k)].v, zstar[static_cast<std::size_t>(k)]);
  std::vector<Wide> pv(static_cast<std::size_t>(n_out));
  fixtures::price_book<Wide>(f.book, zd.data(), pv.data(), pv.data() + f.book.n_swaps);
  std::vector<Wide> fixed(static_cast<std::size_t>(f.book.n_swaps)), flt(static_cast<std::size_t>(f.book.n_swaps));
  for (int i = 0; i < f.book.n_swaps; ++i) {
    fixed[static_cast<std::size_t>(i)] = fixtures::fixed_leg_pv<Wide>(f.book, i, zd.data());
    flt[static_cast<std::size_t>(i)] = fixtures::float_leg_pv<Wide>(f.book, i, zd.data());
  }
  // The adjoint of every output, one lane per output.
  std::vector<double> jac(static_cast<std::size_t>(n_out) * n_knots);
  const int chunk = prog.max_batch();
  std::size_t solves = 0;
  for (int o0 = 0; o0 < n_out; o0 += chunk) {
    const int B = std::min(chunk, n_out - o0);
    const std::size_t Bs = static_cast<std::size_t>(B);
    std::vector<double> state(static_cast<std::size_t>(n_knots) * Bs), out_bar(static_cast<std::size_t>(n_tape_out) * Bs, 0.0),
        out(static_cast<std::size_t>(n_tape_out) * Bs), q_bar(static_cast<std::size_t>(n_knots) * Bs);
    for (std::size_t k = 0; k < static_cast<std::size_t>(n_knots); ++k) {
      for (std::size_t b = 0; b < Bs; ++b) state[k * Bs + b] = f.quotes[k];
    }
    for (std::size_t b = 0; b < Bs; ++b) {
      const int o = o0 + static_cast<int>(b);
      const int ord = o == f.book.n_swaps ? f.cal.book_pv : f.cal.swap_pv_begin + o;
      out_bar[static_cast<std::size_t>(ord) * Bs + b] = 1.0;
    }
    prog.adjoint(state.data(), B, out_bar.data(), out.data(), q_bar.data());
    solves += static_cast<std::size_t>(prog.last_run().solves);
    for (std::size_t b = 0; b < Bs; ++b) {
      const int o = o0 + static_cast<int>(b);
      const int ord = o == f.book.n_swaps ? f.cal.book_pv : f.cal.swap_pv_begin + o;
      EXPECT_EQ(out[static_cast<std::size_t>(ord) * Bs + b], pv[static_cast<std::size_t>(o)].v) << "output " << o << ": the values differ";
      for (std::size_t k = 0; k < static_cast<std::size_t>(n_knots); ++k) jac[static_cast<std::size_t>(o) * n_knots + k] = q_bar[k * Bs + b];
    }
  }
  EXPECT_EQ(solves, 16u) << "one solve per batched call (identical lanes)";
  // Compare. The scale of entry (o, k) is the largest leg-derivative magnitude of output o over
  // every quote, S_o = max_k (|∂fixed_o/∂q_k| + |∂float_o/∂q_k|): through the calibration every
  // quote reaches every knot (the interpolation overshoots a knot by three days, so the Jacobian
  // has no exact zeros), and an entry of 1e-23 beside a row of 1e7 is a rounding residue of
  // those terms, not a value to compare relatively.
  double worst = 0.0, worst_scaled = 0.0, worst_entry = 0.0;
  std::size_t failures = 0, zeros = 0, significant = 0;
  for (int o = 0; o < n_out; ++o) {
    double S = 0.0;
    for (int k = 0; k < n_knots; ++k) {
      const std::size_t s = static_cast<std::size_t>(k);
      double scale;
      if (o < f.book.n_swaps) {
        scale = std::fabs(fixed[static_cast<std::size_t>(o)].d[s]) + std::fabs(flt[static_cast<std::size_t>(o)].d[s]);
      } else {
        scale = 0.0;
        for (int i = 0; i < f.book.n_swaps; ++i) scale += std::fabs(fixed[static_cast<std::size_t>(i)].d[s]) + std::fabs(flt[static_cast<std::size_t>(i)].d[s]);
      }
      S = std::max(S, scale);
    }
    for (int k = 0; k < n_knots; ++k) {
      const std::size_t s = static_cast<std::size_t>(k);
      const double a = jac[static_cast<std::size_t>(o) * n_knots + s];
      const double d = pv[static_cast<std::size_t>(o)].d[s];
      if (a == 0.0 && d == 0.0) {
        ++zeros;
        continue;
      }
      const double diff = std::fabs(a - d);
      const double m = std::max(std::fabs(d), S);
      worst_scaled = std::max(worst_scaled, diff / m);
      if (std::fabs(d) >= 1e-3 * S) {  // a significant entry: its own relative error is meaningful
        ++significant;
        worst = std::max(worst, diff / std::fabs(d));
      }
      worst_entry = std::max(worst_entry, diff / std::max(std::fabs(d), 1e-300));
      if (diff > 1e-12 * m) {
        ++failures;
        if (failures <= 10) ADD_FAILURE() << "output " << o << ", quote " << k << ": adjoint " << a << " vs dual " << d << " (row scale " << S << ")";
      }
    }
  }
  std::cout << "[  vs dual ] " << n_out * n_knots << " entries, " << zeros << " exact zeros on both sides, " << significant
            << " significant (>= 1e-3 of the row scale): worst relative " << worst << "; worst against max(|dual|, row scale) " << worst_scaled
            << "; worst unscaled relative over every entry " << worst_entry << "; " << failures << " outside 1e-12\n";
  EXPECT_EQ(failures, 0u);
  EXPECT_LE(worst_scaled, 1e-12);
  EXPECT_LE(worst, 1e-10) << "a significant entry differs beyond rounding";
}
