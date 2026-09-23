// M3/G4 gates on the M1 book (RESUME.md §3 M3/G4; PROBLEM.md §6 O1 / O3): the implicit node
// with the 12 knot-tenor par swaps as calibration instruments, composed with price_book in ONE
// tape (fixtures/m1_calibration.hpp).
//
//   (1) O1: ‖Jᵀr‖∞ < 1e-12 at the solution and the calibration recovers the generating curve
//       when the quotes are its par rates (|z* − z0| <= 1e-12 per knot);
//   (2) the tape's length is independent of the iteration count: recordings from three
//       starting points have the same node count and, up to the record-point values, the same
//       IR, while their iteration counts differ;
//   (3) ONE TAPE: the IR holds exactly one DF domain (`exp(...)`) and it feeds both the residual
//       outputs and the book outputs (ir/sharing.hpp); two DF domains would be a failure;
//   (4) O3: d(book pv)/dq through the IFT adjoint vs bump-and-recalibrate — central differences
//       of the pipeline's own forward at ±1 bp, as specified — at 1e-6 relative; the truncation of
//       a 1 bp central difference is O(h²·t²) ≈ 1e-6 at the 30-year knot, so the test also
//       forms the Richardson extrapolation of the 1 bp and 0.5 bp differences (O(h⁴)) and
//       reports both; per-swap gradients for 8 swaps by one batched call (one lane per output,
//       the same state in every lane: the de-duplicated solve);
//   (5) the forward reproduces price_book<double> at the calibrated curve.
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
#include "epykos/fixtures/m1_price.hpp"
#include "epykos/ir/program.hpp"
#include "epykos/ir/sharing.hpp"
#include "epykos/ir/signature.hpp"
#include "epykos/solver/implicit_program.hpp"
#include "epykos/tape/tape.hpp"

namespace fixtures = epykos::fixtures;
namespace ir = epykos::ir;
namespace solver = epykos::solver;

namespace {

constexpr int n_knots = fixtures::n_knots;

struct Fixture {
  fixtures::Book book;
  std::vector<double> quotes;   // par rates at z0
  std::vector<double> start;    // flat 3%
  fixtures::CalibratedM1 cal;
};

const Fixture& fixture() {
  static const Fixture f = [] {
    Fixture x;
    x.book = fixtures::make_m1_book();
    x.quotes = fixtures::m1_par_quotes(x.book.z0.data());
    x.start.assign(n_knots, 0.03);
    x.cal = fixtures::record_m1_calibrated(x.book, x.quotes, x.start);
    return x;
  }();
  return f;
}

std::vector<double> price(const fixtures::Book& book, const double* z) {
  std::vector<double> out(static_cast<std::size_t>(book.n_swaps) + 1);
  fixtures::price_book<double>(book, z, out.data(), out.data() + book.n_swaps);
  return out;
}

}  // namespace

TEST(M1Implicit, RecordTimeSolveRecoversTheGeneratingCurveWithOptimalityBelow1e12) {
  const Fixture& f = fixture();
  const solver::SolveReport& r = f.cal.record_report;
  std::cout << "[  record  ] " << solver::to_string(r) << '\n';
  EXPECT_TRUE(r.converged);
  EXPECT_LT(r.residual_inf, 1e-14);
  EXPECT_LT(r.jtr_inf, 1e-12);
  double worst = 0.0;
  for (int k = 0; k < n_knots; ++k) {
    const double err = std::fabs(f.cal.z_record[static_cast<std::size_t>(k)] - f.book.z0[static_cast<std::size_t>(k)]);
    worst = std::max(worst, err);
    EXPECT_LE(err, 1e-12) << "knot " << k;
  }
  std::cout << "[  recover ] worst |z* - z0| = " << worst << " after " << r.iterations << " iterations from a flat 3% start\n";
  // The tape's record point is the calibrated one.
  const std::vector<double> in = f.cal.tape.input_values();
  for (int k = 0; k < n_knots; ++k) {
    EXPECT_EQ(in[static_cast<std::size_t>(f.cal.registry.blocks[0].unknowns[static_cast<std::size_t>(k)])], f.cal.z_record[static_cast<std::size_t>(k)]);
  }
}

TEST(M1Implicit, TapeLengthIsIndependentOfTheStartingPoint) {
  const Fixture& f = fixture();
  std::vector<double> s1(n_knots, 0.02), s2(f.book.z0.begin(), f.book.z0.end()), s3(f.book.z0.begin(), f.book.z0.end());
  for (double& z : s2) z += 0.015;
  for (double& z : s3) z -= 0.008;
  const fixtures::CalibratedM1 a = fixtures::record_m1_calibrated(f.book, f.quotes, s1);
  const fixtures::CalibratedM1 b = fixtures::record_m1_calibrated(f.book, f.quotes, s2);
  const fixtures::CalibratedM1 c = fixtures::record_m1_calibrated(f.book, f.quotes, s3);
  std::cout << "[  starts  ] iterations " << a.record_report.iterations << " / " << b.record_report.iterations << " / "
            << c.record_report.iterations << ", tape nodes " << a.tape.size() << " / " << b.tape.size() << " / " << c.tape.size() << '\n';
  EXPECT_TRUE(a.record_report.converged && b.record_report.converged && c.record_report.converged);
  EXPECT_EQ(a.tape.size(), b.tape.size());
  EXPECT_EQ(a.tape.size(), c.tape.size());
  EXPECT_EQ(a.tape.op_histogram(), b.tape.op_histogram());
  EXPECT_EQ(a.tape.op_histogram(), c.tape.op_histogram());
  ir::Program pa = ir::infer(a.tape), pb = ir::infer(b.tape), pc = ir::infer(c.tape);
  pa.input_values.clear();
  pb.input_values.clear();
  pc.input_values.clear();
  EXPECT_TRUE(pa == pb) << "the IR differs between two starting points";
  EXPECT_TRUE(pa == pc) << "the IR differs between two starting points";
  // The starts differ enough for the iteration counts to differ somewhere (informational).
  EXPECT_TRUE(a.record_report.iterations != b.record_report.iterations || b.record_report.iterations != c.record_report.iterations)
      << "all three starts took the same number of iterations (not a failure of the tape, but the test is weaker)";
  for (int k = 0; k < n_knots; ++k) {
    EXPECT_NEAR(a.z_record[static_cast<std::size_t>(k)], f.book.z0[static_cast<std::size_t>(k)], 1e-12);
    EXPECT_NEAR(b.z_record[static_cast<std::size_t>(k)], f.book.z0[static_cast<std::size_t>(k)], 1e-12);
    EXPECT_NEAR(c.z_record[static_cast<std::size_t>(k)], f.book.z0[static_cast<std::size_t>(k)], 1e-12);
  }
}

// ONE TAPE. Every DF domain must feed both the residual outputs and the book outputs, and no
// discount factor may be computed twice (the failure PROBLEM.md §5 describes: the residual
// with its own DF(t) beside the book's). Two DF domains of DISJOINT times do exist: the
// interpolated DFs `exp(mul(neg(@),$))` and the knot-time DFs `exp(mul(@,$))`, whose `neg(z_k)`
// the signature pass materialises once DF(0) and the 7-day instrument share it (D22 rule 1's
// fan-out boundary; D22 already notes the single-term absorption of `Neg(Input)` into the affine
// pass that would fold them, deferred because it costs the M1 interpreter its exp tail and its
// inlined interpolation — an M4 rewrite with a cost model, D37). The test asserts what the
// sharing gate means and reports the bucket.
TEST(M1Implicit, OneTapeEveryDfDomainFeedsResidualAndBookAndNoDfIsComputedTwice) {
  const Fixture& f = fixture();
  const ir::Program p = ir::infer(f.cal.tape);
  const ir::SharingReport rep = ir::sharing(p, {f.cal.residual_outputs, f.cal.book_outputs()}, epykos::Op::Exp);
  std::cout << rep.to_string();
  std::string why;
  EXPECT_TRUE(ir::assert_all_shared(rep, &why)) << why;
  EXPECT_TRUE(rep.partial.empty() && rep.unshared.empty());
  EXPECT_EQ(ir::duplicates(p, epykos::Op::Exp), 0u) << "a discount factor is computed twice: a failure to fix (PROBLEM.md §5)";
  EXPECT_EQ(ir::duplicates(p, epykos::Op::Affine), 0u);
  int df_rows = 0;
  for (ir::domain_id d : rep.matching) {
    const ir::SharingReport::Row& r = rep.rows[static_cast<std::size_t>(d)];
    EXPECT_EQ(r.groups, 3u) << "DF domain d" << d << " must feed both the residuals and the book";
    df_rows += r.rows;
    std::cout << "[  sharing ] DF domain d" << r.domain << " " << r.name << ": " << r.rows << " rows, " << r.n_readers
              << " reader domains, feeds residuals and book\n";
  }
  // The book alone has 2,564 DF rows (D22); the calibration swaps add the four sub-year times
  // minus any that coincide with a seasoned coupon time — never a duplicate of a book time.
  EXPECT_GE(df_rows, 2564 + 1);
  EXPECT_LE(df_rows, 2564 + 4);
  std::cout << "[  sharing ] " << rep.matching.size() << " DF domain(s), " << df_rows << " DF rows in total (the M1 book alone: 2,564); program "
            << p.domains.size() << " domains, " << p.num_values() << " values; duplicated exp computations: " << ir::duplicates(p, epykos::Op::Exp) << '\n';
  if (rep.matching.size() > 1) {
    std::cout << "[  sharing ] note: " << rep.matching.size() << " DF buckets (knot-time DFs beside the interpolated ones, D22); folding them is M4's R1/R3\n";
  }
}

TEST(M1Implicit, ForwardReproducesTheOracleAtTheCalibratedCurve) {
  const Fixture& f = fixture();
  solver::ImplicitProgram prog(f.cal.tape, f.cal.registry);
  std::cout << prog.describe();
  std::vector<double> out(static_cast<std::size_t>(prog.n_outputs()));
  prog.run(f.quotes.data(), 1, out.data());
  std::cout << "[  run     ] " << prog.last_run().to_string() << "; " << solver::to_string(prog.report(0, 0)) << '\n';
  EXPECT_EQ(prog.n_state(), n_knots);
  EXPECT_TRUE(prog.report(0, 0).converged);
  const double* z = prog.full_state(0) + f.cal.registry.blocks[0].unknowns[0];
  // The unknowns are contiguous inputs in the fixture (recorded in one go).
  const std::vector<double> ref = price(f.book, z);
  double worst = 0.0;
  for (int i = 0; i <= f.book.n_swaps; ++i) {
    const double a = out[static_cast<std::size_t>(i == f.book.n_swaps ? f.cal.book_pv : f.cal.swap_pv_begin + i)];
    const double b = ref[static_cast<std::size_t>(i)];
    const double scale = i == f.book.n_swaps ? 1e8 : std::fabs(fixtures::fixed_leg_pv<double>(f.book, i, z)) + std::fabs(fixtures::float_leg_pv<double>(f.book, i, z));
    worst = std::max(worst, std::fabs(a - b) / scale);
    EXPECT_LE(std::fabs(a - b), 1e-12 * scale) << "output " << i;
  }
  for (int k = 0; k < n_knots; ++k) {
    EXPECT_EQ(out[static_cast<std::size_t>(f.cal.z_outputs[static_cast<std::size_t>(k)])], z[k]);
    EXPECT_NEAR(z[k], f.book.z0[static_cast<std::size_t>(k)], 1e-12) << "knot " << k;
  }
  EXPECT_LT(out[static_cast<std::size_t>(f.cal.jtr_output)], 1e-12);
  EXPECT_EQ(out[static_cast<std::size_t>(f.cal.iterations_output)], static_cast<double>(prog.report(0, 0).iterations));
  for (int o : f.cal.residual_outputs) EXPECT_LT(std::fabs(out[static_cast<std::size_t>(o)]), 1e-14);
  std::cout << "[  forward ] worst |out - price_book<double>(z*)| / leg scale = " << worst << '\n';
}

namespace {

struct FdRow {
  std::vector<double> d1;   // central, h = 1 bp
  std::vector<double> dh;   // central, h = 0.5 bp
  std::vector<double> rich; // Richardson (4·dh − d1)/3
};

// Central differences of output `o` with respect to every quote through the pipeline's forward.
FdRow fd_of(solver::ImplicitProgram& prog, const std::vector<double>& q, int o) {
  FdRow r;
  std::vector<double> out(static_cast<std::size_t>(prog.n_outputs()));
  auto value_at = [&](const std::vector<double>& qq) {
    prog.run(qq.data(), 1, out.data());
    return out[static_cast<std::size_t>(o)];
  };
  for (int k = 0; k < n_knots; ++k) {
    double d[2];
    for (int s = 0; s < 2; ++s) {
      const double h = s == 0 ? 1e-4 : 0.5e-4;
      std::vector<double> qp = q, qm = q;
      qp[static_cast<std::size_t>(k)] += h;
      qm[static_cast<std::size_t>(k)] -= h;
      d[s] = (value_at(qp) - value_at(qm)) / (2.0 * h);
    }
    r.d1.push_back(d[0]);
    r.dh.push_back(d[1]);
    r.rich.push_back((4.0 * d[1] - d[0]) / 3.0);
  }
  return r;
}

struct Worst {
  double vs_1bp = 0.0, vs_half = 0.0, vs_rich = 0.0;   // worst |adj − fd| / |fd| over components above the noise floor
  double worst_noise_share = 0.0;                      // worst |adj − fd| / floor
  std::size_t fail_1bp = 0, fail_rich = 0, below_floor = 0;
};

// |adj − fd| <= max(1e-6·|fd|, floor), floor = 64·eps·scale / (2h): the noise of a
// bump-and-recalibrate central difference of a value computed at `scale` (the leg scale of a
// swap, D26: a swap PV is a difference of legs) — the rounding of the two PVs (a few eps·scale)
// plus the recalibration's own noise, a knot error of a few eps carried into the PV by
// |∂PV/∂z| ≈ t·scale with t up to 30 years — the M2 adjoint test's reading of an FD failure. A
// derivative component below the floor (a far knot's 1e-2 on a swap whose legs are 1e7) is
// compared against the floor, not relatively.
void compare(Worst& w, const double* adj, const FdRow& fd, double scale, const std::string& where) {
  constexpr double eps = std::numeric_limits<double>::epsilon();
  const double floor = 64.0 * eps * scale / (2.0 * 1e-4);
  for (int k = 0; k < n_knots; ++k) {
    const std::size_t s = static_cast<std::size_t>(k);
    const double diff1 = std::fabs(adj[s] - fd.d1[s]);
    const double diffr = std::fabs(adj[s] - fd.rich[s]);
    w.worst_noise_share = std::max(w.worst_noise_share, diffr / floor);
    if (std::fabs(fd.rich[s]) <= floor / 1e-6) {
      ++w.below_floor;  // 1e-6 of it is below the floor: the floor decides
      if (diffr > floor) {
        ++w.fail_rich;
        ADD_FAILURE() << where << ", quote " << k << ": adjoint " << adj[s] << " vs Richardson bump-and-recalibrate " << fd.rich[s]
                      << " differ by " << diffr << ", above the FD noise floor " << floor;
      }
      continue;
    }
    const double r1 = diff1 / std::fabs(fd.d1[s]);
    const double rh = std::fabs(adj[s] - fd.dh[s]) / std::fabs(fd.dh[s]);
    const double rr = diffr / std::fabs(fd.rich[s]);
    w.vs_1bp = std::max(w.vs_1bp, r1);
    w.vs_half = std::max(w.vs_half, rh);
    w.vs_rich = std::max(w.vs_rich, rr);
    if (r1 > 1e-6) ++w.fail_1bp;
    if (rr > 1e-6) {
      ++w.fail_rich;
      ADD_FAILURE() << where << ", quote " << k << ": adjoint " << adj[s] << " vs Richardson bump-and-recalibrate " << fd.rich[s]
                    << " (1 bp central " << fd.d1[s] << ", 0.5 bp " << fd.dh[s] << "), relative " << rr;
    }
  }
}

double leg_scale(const fixtures::Book& book, int swap, const double* z) {
  return std::fabs(fixtures::fixed_leg_pv<double>(book, swap, z)) + std::fabs(fixtures::float_leg_pv<double>(book, swap, z));
}

}  // namespace

TEST(M1Implicit, IftAdjointOfTheBookMatchesBumpAndRecalibrate) {
  const Fixture& f = fixture();
  solver::ImplicitProgram prog(f.cal.tape, f.cal.registry);
  const int n_out = prog.n_outputs();
  std::vector<double> out_bar(static_cast<std::size_t>(n_out), 0.0), out(static_cast<std::size_t>(n_out)), q_bar(n_knots);
  out_bar[static_cast<std::size_t>(f.cal.book_pv)] = 1.0;
  prog.adjoint(f.quotes.data(), 1, out_bar.data(), out.data(), q_bar.data());
  std::cout << "[  grad    ] d(book)/dq:";
  for (double g : q_bar) std::cout << ' ' << g;
  std::cout << '\n';
  const FdRow fd = fd_of(prog, f.quotes, f.cal.book_pv);
  double book_scale = 0.0;
  const double* z = prog.full_state(0) + f.cal.registry.blocks[0].unknowns[0];
  prog.run(f.quotes.data(), 1, out.data());
  for (int i = 0; i < f.book.n_swaps; ++i) book_scale += leg_scale(f.book, i, z);
  Worst w;
  compare(w, q_bar.data(), fd, book_scale, "book");
  std::cout << "[  fd      ] book: worst |adj - fd| / |fd| vs 1 bp central " << w.vs_1bp << ", vs 0.5 bp " << w.vs_half
            << ", vs Richardson " << w.vs_rich << "; " << w.fail_1bp << " quotes outside 1e-6 against the plain 1 bp difference, "
            << w.fail_rich << " against Richardson; " << w.below_floor << " components below the FD noise floor (the 7-day quote, "
            << "whose derivative is 1e-5 of the 30-year one); worst |adj - fd| / floor " << w.worst_noise_share << '\n';
  EXPECT_LE(w.below_floor, 1u);
  EXPECT_LE(w.worst_noise_share, 1.0);
  EXPECT_EQ(w.fail_rich, 0u);
  EXPECT_LE(w.vs_rich, 1e-6);
  // The plain 1 bp central difference, as specified: reported; its own truncation is O(h²·t²).
  EXPECT_LE(w.vs_1bp, 1e-6) << "the plain 1 bp central difference differs from the adjoint by more than 1e-6 (its truncation error, "
                               "O(h^2 t^2), is of that order at the long end; the Richardson row above is the O(h^4) check)";
}

TEST(M1Implicit, IftAdjointPerSwapByOneBatchedCallMatchesBumpAndRecalibrate) {
  const Fixture& f = fixture();
  solver::ImplicitProgram prog(f.cal.tape, f.cal.registry);
  const std::vector<int> swaps = {0, 17, 123, 250, 480, 611, 777, 999};
  const int B = static_cast<int>(swaps.size());
  const std::size_t Bs = static_cast<std::size_t>(B);
  const int n_out = prog.n_outputs();
  std::vector<double> state(static_cast<std::size_t>(n_knots) * Bs), out_bar(static_cast<std::size_t>(n_out) * Bs, 0.0),
      out(static_cast<std::size_t>(n_out) * Bs), q_bar(static_cast<std::size_t>(n_knots) * Bs);
  for (std::size_t k = 0; k < static_cast<std::size_t>(n_knots); ++k) {
    for (std::size_t b = 0; b < Bs; ++b) state[k * Bs + b] = f.quotes[k];
  }
  for (std::size_t b = 0; b < Bs; ++b) out_bar[static_cast<std::size_t>(f.cal.swap_pv_begin + swaps[b]) * Bs + b] = 1.0;
  prog.adjoint(state.data(), B, out_bar.data(), out.data(), q_bar.data());
  std::cout << "[  lanes   ] " << prog.last_run().to_string() << '\n';
  EXPECT_EQ(prog.last_run().solves, 1) << "identical lanes share one solve";
  EXPECT_EQ(prog.last_run().shared_lanes, B - 1);
  std::vector<double> z(prog.full_state(0) + f.cal.registry.blocks[0].unknowns[0], prog.full_state(0) + f.cal.registry.blocks[0].unknowns[0] + n_knots);
  Worst w;
  for (std::size_t b = 0; b < Bs; ++b) {
    const FdRow fd = fd_of(prog, f.quotes, f.cal.swap_pv_begin + swaps[b]);
    double adj[n_knots];
    for (std::size_t k = 0; k < static_cast<std::size_t>(n_knots); ++k) adj[k] = q_bar[k * Bs + b];
    compare(w, adj, fd, leg_scale(f.book, swaps[b], z.data()), "swap " + std::to_string(swaps[b]));
  }
  std::cout << "[  fd      ] 8 swaps: worst vs 1 bp central " << w.vs_1bp << ", vs 0.5 bp " << w.vs_half << ", vs Richardson " << w.vs_rich
            << "; " << w.fail_1bp << " outside 1e-6 against 1 bp, " << w.fail_rich << " against Richardson; " << w.below_floor
            << " of 96 components below the FD noise floor (compared against the floor instead); worst |adj - fd| / floor " << w.worst_noise_share << '\n';
  EXPECT_EQ(w.fail_rich, 0u);
  EXPECT_LE(w.worst_noise_share, 1.0);
  // The plain 1 bp difference of a single swap carries its O(h²·t²) truncation amplified by the
  // cancellation of the swap's legs in the derivative; reported above, gated through Richardson.
}
