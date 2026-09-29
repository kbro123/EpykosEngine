// M3/G1 gate: the adjoint of the M4 composite (Linear zero / MonotoneCubic zero / Linear logdf on
// the M1 knots) recorded on the 10-swap M1-style book — the full Jacobian of every output (swap
// PVs, book PV, DFs on a time grid) with respect to the 12 knots from adjoint::Adjoint (one lane
// per output) against one pass of the same maths on Dual<12>, at the record point and on a state
// ball, AWAY FROM PREDICATE BOUNDARIES: a state where any exported select margin is within 1e-9
// of zero is a kink of the Hyman limiter (the record point is one: |d_4| == |d_5|), where the two
// sides may legitimately pick different arms; such states are counted and skipped, the rest are
// asserted at 1e-12 of the row's gradient scale. A gate of scripts/mutation_test.sh (the
// *_vs_dual_test name): the adjoint's select rule routes to the chosen arm.
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <vector>

#include "epykos/adjoint/adjoint.hpp"
#include "epykos/ir/program.hpp"
#include "epykos/ir/signature.hpp"
#include "epykos/verify/differential.hpp"
#include "curve/curve_test_helpers.hpp"

namespace curve = epykos::curve;
namespace ir = epykos::ir;
namespace fixtures = epykos::fixtures;

namespace {

const std::vector<double> df_times = {0.3, 1.5, 2.5, 4.0, 6.0, 8.5, 9.99, 15.0, 25.0};
constexpr double margin_floor = 1e-9;
constexpr double rel_tol = 1e-12;

}  // namespace

TEST(CurveCompositeVsDual, AdjointJacobianMatchesForwardModeAwayFromPredicateBoundaries) {
  const curve::Composite c = epykos::test::m4_composite();
  const fixtures::Book& book = epykos::test::curve_book();
  const epykos::test::Recording rec = epykos::test::record_on(c, book, df_times, true, true);
  const int n_out = rec.n_outputs;  // the pricing and DF outputs; the exports follow
  ASSERT_GT(rec.exports.n(), 0);
  const ir::Program program = ir::infer(rec.tape);
  epykos::adjoint::Options opt;
  opt.max_batch = 64;
  opt.lane_tile = 16;
  const epykos::adjoint::Adjoint ad(program, opt);
  const int n_all = ad.n_outputs();
  ASSERT_EQ(n_all, n_out + rec.exports.n_outputs());
  constexpr int n_in = fixtures::n_knots;

  epykos::verify::BallOptions bopt;
  bopt.draws = 24;
  const epykos::verify::StateBall ball = epykos::verify::make_state_ball(book.z0.data(), n_in, bopt);
  std::vector<std::vector<double>> states;
  states.emplace_back(book.z0.begin(), book.z0.end());
  for (int r = 0; r < ball.n_draws; ++r) states.emplace_back(ball.state(r), ball.state(r) + n_in);

  int skipped = 0, compared_states = 0;
  std::size_t entries = 0, failures = 0;
  double worst = 0.0;
  for (std::size_t s = 0; s < states.size(); ++s) {
    const std::vector<double>& z = states[s];
    // The Jacobian by the adjoint: one lane per output, out_bar = e_o, batches of max_batch.
    std::vector<std::vector<double>> adj(static_cast<std::size_t>(n_out), std::vector<double>(static_cast<std::size_t>(n_in), 0.0));
    std::vector<double> fwd_all;
    for (int o0 = 0; o0 < n_out; o0 += ad.max_batch()) {
      const int B = std::min(ad.max_batch(), n_out - o0);
      const std::size_t Bs = static_cast<std::size_t>(B);
      std::vector<double> st(static_cast<std::size_t>(n_in) * Bs), ob(static_cast<std::size_t>(n_all) * Bs, 0.0);
      std::vector<double> fwd(static_cast<std::size_t>(n_all) * Bs), sb(static_cast<std::size_t>(n_in) * Bs);
      for (std::size_t k = 0; k < static_cast<std::size_t>(n_in); ++k) {
        for (std::size_t b = 0; b < Bs; ++b) st[k * Bs + b] = z[k];
      }
      for (std::size_t b = 0; b < Bs; ++b) ob[(static_cast<std::size_t>(o0) + b) * Bs + b] = 1.0;
      ad.run(st.data(), B, ob.data(), fwd.data(), sb.data());
      for (std::size_t b = 0; b < Bs; ++b) {
        for (std::size_t k = 0; k < static_cast<std::size_t>(n_in); ++k) adj[static_cast<std::size_t>(o0) + b][k] = sb[k * Bs + b];
      }
      if (o0 == 0) {
        fwd_all.resize(static_cast<std::size_t>(n_all));
        for (int o = 0; o < n_all; ++o) fwd_all[static_cast<std::size_t>(o)] = fwd[static_cast<std::size_t>(o) * Bs];
      }
    }
    // Near a predicate boundary?
    double min_margin = INFINITY;
    for (int e = 0; e < rec.exports.n(); ++e) min_margin = std::fmin(min_margin, std::fabs(rec.exports.margin(e, fwd_all.data(), 1, 0)));
    if (min_margin < margin_floor) {
      ++skipped;
      std::cout << "[  skip    ] state " << s << ": a select margin of " << min_margin << " (a kink of the limiter)\n";
      continue;
    }
    ++compared_states;
    const epykos::test::DualJacobian dj = epykos::test::dual_jacobian_on(c, book, z.data(), df_times);
    ASSERT_EQ(dj.value.size(), static_cast<std::size_t>(n_out));
    for (int o = 0; o < n_out; ++o) {
      const std::size_t os = static_cast<std::size_t>(o);
      EXPECT_NEAR(fwd_all[os], dj.value[os], 1e-12 * std::fabs(dj.value[os]) + 1e-14) << "state " << s << " output " << o;
      double row_scale = 0.0;
      for (int k = 0; k < n_in; ++k) row_scale = std::fmax(row_scale, std::fabs(dj.jac[os][static_cast<std::size_t>(k)]));
      const double tol = rel_tol * std::fmax(row_scale, 1e-14);
      for (int k = 0; k < n_in; ++k) {
        const double a = adj[os][static_cast<std::size_t>(k)], d = dj.jac[os][static_cast<std::size_t>(k)];
        const double diff = std::fabs(a - d);
        ++entries;
        worst = std::fmax(worst, diff / tol * rel_tol);
        if (diff <= tol) continue;
        ++failures;
        if (failures <= 8) ADD_FAILURE() << "state " << s << ", output " << o << ", knot " << k << ": adjoint " << a << " vs dual " << d << " (row scale " << row_scale << ")";
      }
    }
  }
  std::cout << "[  dual    ] " << compared_states << " states compared (" << skipped << " skipped at a predicate boundary), " << entries
            << " Jacobian entries, worst |adj - dual| / row scale = " << worst << ", " << failures << " outside " << rel_tol << '\n';
  EXPECT_EQ(failures, 0u);
  EXPECT_GE(compared_states, 12) << "too few states away from the boundaries to be a gate";
  EXPECT_GE(skipped, 1) << "the record point sits on a tie of the limiter and must be skipped";
}
