// M3/G4: multi-curve dependencies through the implicit node (PROBLEM.md §5 "curves depend on
// each other through the implicit node"): the two-curve fixture of fixtures/m1_calibration.hpp
// — "disc" (the M1 curve, 12 par swaps) and "proj" (6 knots, 6 basis swaps whose residuals read
// both curves: projection on discounting).
//
//   (1) the dependency graph is discovered from the maths (no declaration): proj -> disc;
//       sequential mode gives the blocks {disc}, {proj}; joint mode one block of both;
//   (2) both modes recover the generating curves from quotes that are their par rates and
//       spreads, with the residuals on double below 1e-13 at the solution; iterations,
//       Jacobians and residual evaluations of each mode are reported (the measurement the
//       "solver order or joint system" choice rests on, with bench/solver/m1_implicit_bench);
//   (3) the IFT adjoint through two chained blocks (the proj block's parameters include the
//       disc block's unknowns, so proj's rule feeds disc's) of a 5-output book priced off both
//       curves vs bump-and-recalibrate (Richardson of 1 bp / 0.5 bp central) at 1e-6 relative
//       above the FD noise floor, and the joint tape's adjoint equals the sequential one's;
//   (4) the sharing gate over both blocks: every DF domain feeds the residuals and the book, no
//       DF computed twice.
//
// Named *_adjoint_test: it is in the mutation harness's gate set (D33).
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

#include "epykos/fixtures/m1_book.hpp"
#include "epykos/fixtures/m1_calibration.hpp"
#include "epykos/ir/sharing.hpp"
#include "epykos/ir/signature.hpp"
#include "epykos/solver/curve_set.hpp"
#include "epykos/solver/implicit_program.hpp"

namespace fixtures = epykos::fixtures;
namespace ir = epykos::ir;
namespace solver = epykos::solver;
using Mode = solver::CurveSet::Mode;

namespace {

constexpr int n_d = fixtures::n_knots;
constexpr int n_p = fixtures::n_proj_knots;
constexpr int n_q = n_d + n_p;

struct Fixture {
  fixtures::Book book;
  std::vector<double> zd0, zp0, quotes, disc_start, proj_start;
  fixtures::CalibratedTwoCurves seq, joint;
};

const Fixture& fixture() {
  static const Fixture f = [] {
    Fixture x;
    x.book = fixtures::make_m1_book();
    x.zd0.assign(x.book.z0.begin(), x.book.z0.end());
    x.zp0.assign(fixtures::proj_record_state.begin(), fixtures::proj_record_state.end());
    x.quotes = fixtures::two_curve_quotes(x.zd0.data(), x.zp0.data());
    x.disc_start.assign(n_d, 0.03);
    x.proj_start.assign(n_p, 0.04);
    x.seq = fixtures::record_two_curves(x.quotes, x.disc_start, x.proj_start, Mode::sequential);
    x.joint = fixtures::record_two_curves(x.quotes, x.disc_start, x.proj_start, Mode::joint);
    return x;
  }();
  return f;
}

void print_reports(const char* what, const fixtures::CalibratedTwoCurves& c) {
  for (std::size_t k = 0; k < c.record_reports.size(); ++k) {
    std::cout << "[  " << what << " ] block " << k << " curves";
    for (int cv : c.block_curves[k]) std::cout << ' ' << cv;
    std::cout << ": " << solver::to_string(c.record_reports[k]) << '\n';
  }
}

}  // namespace

TEST(CurveSet, DependenciesAreDiscoveredFromTheMaths) {
  const Fixture& f = fixture();
  const solver::CurveSet set = fixtures::make_two_curve_set(f.disc_start, f.proj_start);
  EXPECT_EQ(set.n_curves(), 2);
  EXPECT_EQ(set.n_instruments(), n_q);
  const std::vector<std::vector<int>> reads = set.instrument_reads();
  for (int i = 0; i < n_d; ++i) EXPECT_EQ(reads[static_cast<std::size_t>(i)], (std::vector<int>{0})) << "instrument " << i;
  for (int i = n_d; i < n_q; ++i) EXPECT_EQ(reads[static_cast<std::size_t>(i)], (std::vector<int>{0, 1})) << "instrument " << i;
  EXPECT_EQ(set.dependencies(), (std::vector<std::vector<int>>{{}, {0}}));
  EXPECT_EQ(set.blocks(Mode::sequential), (std::vector<std::vector<int>>{{0}, {1}}));
  EXPECT_EQ(set.blocks(Mode::joint), (std::vector<std::vector<int>>{{0, 1}}));
  EXPECT_EQ(set.find("proj"), 1);
  EXPECT_EQ(set.find("none"), -1);
}

TEST(CurveSet, SequentialAndJointModesRecoverBothCurves) {
  const Fixture& f = fixture();
  print_reports("seq  ", f.seq);
  print_reports("joint", f.joint);
  ASSERT_EQ(f.seq.record_reports.size(), 2u);
  ASSERT_EQ(f.joint.record_reports.size(), 1u);
  for (const fixtures::CalibratedTwoCurves* c : {&f.seq, &f.joint}) {
    for (const solver::SolveReport& r : c->record_reports) {
      EXPECT_TRUE(r.converged);
      EXPECT_LT(r.residual_inf, 1e-14);
      EXPECT_LT(r.jtr_inf, 1e-12);
    }
    double worst = 0.0;
    for (int k = 0; k < n_d; ++k) worst = std::max(worst, std::fabs(c->zd_record[static_cast<std::size_t>(k)] - f.zd0[static_cast<std::size_t>(k)]));
    for (int k = 0; k < n_p; ++k) worst = std::max(worst, std::fabs(c->zp_record[static_cast<std::size_t>(k)] - f.zp0[static_cast<std::size_t>(k)]));
    EXPECT_LE(worst, 1e-11);
    // The residuals of the same maths on double at the solution.
    const solver::CurveSet set = fixtures::make_two_curve_set(f.disc_start, f.proj_start);
    std::vector<double> z_all = c->zd_record;
    z_all.insert(z_all.end(), c->zp_record.begin(), c->zp_record.end());
    const std::vector<double> F = set.residuals(set.states_at(z_all), f.quotes);
    double r_inf = 0.0;
    for (double v : F) r_inf = std::max(r_inf, std::fabs(v));
    EXPECT_LT(r_inf, 1e-13);
    std::cout << "[  recover ] " << (c == &f.seq ? "sequential" : "joint") << ": worst |z* - z0| " << worst << ", |F|_inf on double " << r_inf
              << ", tape " << c->tape.size() << " nodes\n";
  }
  // The same recording either way but for the diagnostics: two blocks carry two pairs of
  // diagnostic inputs, one block one pair.
  EXPECT_EQ(f.seq.tape.size(), f.joint.tape.size() + 2);
}

namespace {

struct Grad {
  std::vector<double> adj;   // n_out × n_q, row-major
  std::vector<double> out;   // n_out
};

Grad adjoint_all(solver::ImplicitProgram& prog, const fixtures::CalibratedTwoCurves& c, const std::vector<double>& q) {
  const int B = static_cast<int>(c.book_outputs.size());
  const std::size_t Bs = static_cast<std::size_t>(B);
  const int n_out = prog.n_outputs();
  std::vector<double> state(static_cast<std::size_t>(n_q) * Bs), out_bar(static_cast<std::size_t>(n_out) * Bs, 0.0),
      out(static_cast<std::size_t>(n_out) * Bs), q_bar(static_cast<std::size_t>(n_q) * Bs);
  for (std::size_t k = 0; k < static_cast<std::size_t>(n_q); ++k) {
    for (std::size_t b = 0; b < Bs; ++b) state[k * Bs + b] = q[k];
  }
  for (std::size_t b = 0; b < Bs; ++b) out_bar[static_cast<std::size_t>(c.book_outputs[b]) * Bs + b] = 1.0;
  prog.adjoint(state.data(), B, out_bar.data(), out.data(), q_bar.data());
  Grad g;
  g.adj.resize(Bs * static_cast<std::size_t>(n_q));
  for (std::size_t b = 0; b < Bs; ++b) {
    g.out.push_back(out[static_cast<std::size_t>(c.book_outputs[b]) * Bs + b]);
    for (std::size_t k = 0; k < static_cast<std::size_t>(n_q); ++k) g.adj[b * static_cast<std::size_t>(n_q) + k] = q_bar[k * Bs + b];
  }
  return g;
}

}  // namespace

TEST(CurveSet, IftAdjointThroughTwoChainedBlocksMatchesBumpAndRecalibrate) {
  const Fixture& f = fixture();
  solver::ImplicitProgram prog(f.seq.tape, f.seq.registry);
  std::cout << prog.describe();
  EXPECT_EQ(prog.n_blocks(), 2);
  EXPECT_EQ(prog.n_state(), n_q);
  // Block 1 (proj) reads, besides its 6 quotes, the disc unknowns its instruments reach: the
  // basis swaps mature at 10 years at most, so knots 0 (DF(0)), 4 (t = 1) and 5..10 (the
  // interpolated coupon times up to 10.003 years, which overshoots the 10-year knot): 8 of 12.
  EXPECT_EQ(prog.residual_program(1).n_params(), n_p + 8);
  {
    std::vector<int> expected = {0, 4, 5, 6, 7, 8, 9, 10};
    const std::vector<int>& po = prog.residual_program(1).param_ordinals();
    std::vector<int> disc_params;
    for (int o : po) {
      if (!std::binary_search(f.seq.quote_inputs.begin(), f.seq.quote_inputs.end(), o)) disc_params.push_back(o - f.seq.registry.blocks[0].unknowns[0]);
    }
    EXPECT_EQ(disc_params, expected);
  }
  const Grad g = adjoint_all(prog, f.seq, f.quotes);
  std::cout << "[  lanes   ] " << prog.last_run().to_string() << '\n';
  constexpr double eps = std::numeric_limits<double>::epsilon();
  const int n_out = prog.n_outputs();
  std::vector<double> out(static_cast<std::size_t>(n_out));
  double worst_rich = 0.0, worst_1bp = 0.0, worst_floor = 0.0;
  std::size_t below = 0, fails = 0;
  for (std::size_t o = 0; o < f.seq.book_outputs.size(); ++o) {
    const int ord = f.seq.book_outputs[o];
    const double floor = 8.0 * eps * 1.0e6 * 30.0 / (2.0 * 1e-4);  // notional 1e6, up to 30 coupon legs of it
    for (int k = 0; k < n_q; ++k) {
      double d[2];
      for (int s = 0; s < 2; ++s) {
        const double h = s == 0 ? 1e-4 : 0.5e-4;
        std::vector<double> qp = f.quotes, qm = f.quotes;
        qp[static_cast<std::size_t>(k)] += h;
        qm[static_cast<std::size_t>(k)] -= h;
        prog.run(qp.data(), 1, out.data());
        const double vp = out[static_cast<std::size_t>(ord)];
        prog.run(qm.data(), 1, out.data());
        d[s] = (vp - out[static_cast<std::size_t>(ord)]) / (2.0 * h);
      }
      const double rich = (4.0 * d[1] - d[0]) / 3.0;
      const double adj = g.adj[o * static_cast<std::size_t>(n_q) + static_cast<std::size_t>(k)];
      const double diff = std::fabs(adj - rich);
      worst_floor = std::max(worst_floor, diff / floor);
      if (std::fabs(rich) <= floor / 1e-6) {
        ++below;
        if (diff > floor) {
          ++fails;
          ADD_FAILURE() << "output " << o << ", quote " << k << ": adjoint " << adj << " vs bump " << rich << " above the floor " << floor;
        }
        continue;
      }
      worst_rich = std::max(worst_rich, diff / std::fabs(rich));
      worst_1bp = std::max(worst_1bp, std::fabs(adj - d[0]) / std::fabs(d[0]));
      if (diff > 1e-6 * std::fabs(rich)) {
        ++fails;
        ADD_FAILURE() << "output " << o << ", quote " << k << ": adjoint " << adj << " vs Richardson bump-and-recalibrate " << rich
                      << " (1 bp " << d[0] << ", 0.5 bp " << d[1] << ")";
      }
    }
  }
  std::cout << "[  fd      ] 5 outputs x 18 quotes: worst |adj - fd| / |fd| vs Richardson " << worst_rich << ", vs 1 bp " << worst_1bp
            << "; " << below << " components below the FD noise floor, worst |adj - fd| / floor " << worst_floor << ", " << fails << " failures\n";
  EXPECT_EQ(fails, 0u);
  EXPECT_LE(worst_floor, 1.0);
}

TEST(CurveSet, JointAndSequentialAdjointsAgree) {
  const Fixture& f = fixture();
  solver::ImplicitProgram ps(f.seq.tape, f.seq.registry), pj(f.joint.tape, f.joint.registry);
  const Grad gs = adjoint_all(ps, f.seq, f.quotes);
  const Grad gj = adjoint_all(pj, f.joint, f.quotes);
  std::cout << "[  joint   ] " << pj.last_run().to_string() << "; " << solver::to_string(pj.report(0, 0)) << '\n';
  std::cout << "[  seq     ] " << ps.last_run().to_string() << "; " << solver::to_string(ps.report(0, 0)) << " | " << solver::to_string(ps.report(1, 0)) << '\n';
  // Per output, against the row's largest component (an entry that is a rounding-level residue
  // of cancelling leg derivatives is compared at the scale of those derivatives, D26).
  double worst = 0.0;
  std::size_t wo = 0, wk = 0;
  for (std::size_t o = 0; o < gs.out.size(); ++o) {
    double row = 0.0;
    for (std::size_t k = 0; k < static_cast<std::size_t>(n_q); ++k) row = std::max(row, std::fabs(gs.adj[o * static_cast<std::size_t>(n_q) + k]));
    for (std::size_t k = 0; k < static_cast<std::size_t>(n_q); ++k) {
      const std::size_t i = o * static_cast<std::size_t>(n_q) + k;
      const double r = std::fabs(gs.adj[i] - gj.adj[i]) / row;
      if (r > worst) {
        worst = r;
        wo = o;
        wk = k;
      }
    }
    // The two modes converge to the same solution to rounding; the outputs agree to their scale.
    EXPECT_NEAR(gs.out[o], gj.out[o], 1e-12 * 1.0e6 * 30.0) << "output " << o;
  }
  std::cout << "[  agree   ] worst |seq - joint| / row max = " << worst << " at output " << wo << ", quote " << wk << " (seq "
            << gs.adj[wo * static_cast<std::size_t>(n_q) + wk] << ", joint " << gj.adj[wo * static_cast<std::size_t>(n_q) + wk] << ")\n";
  EXPECT_LE(worst, 1e-10);
}

TEST(CurveSet, SharingGateOverBothBlocks) {
  const Fixture& f = fixture();
  const ir::Program p = ir::infer(f.seq.tape);
  std::vector<int> residuals;
  for (const solver::ImplicitBlock& b : f.seq.registry.blocks) residuals.insert(residuals.end(), b.residuals.begin(), b.residuals.end());
  const ir::SharingReport rep = ir::sharing(p, {residuals, f.seq.book_outputs}, epykos::Op::Exp);
  std::cout << rep.to_string();
  std::string why;
  EXPECT_TRUE(ir::assert_all_shared(rep, &why)) << why;
  EXPECT_EQ(ir::duplicates(p, epykos::Op::Exp), 0u);
}
