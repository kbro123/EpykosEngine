// PRINCIPLES.md §1b invariant I2, the gate that was missing: GAMMA OF A CALIBRATED BOOK WITH
// RESPECT TO PAR QUOTES. tests/maths/second_order_test.cpp gates second order over the pricing
// maths; this gates it one stage further back, through solver::implicit_dual, which is where the
// number a desk actually asks for lives.
//
// Nothing in the engine writes a second-order implicit-function rule down. `implicit_dual` builds
// F_z, its LU and the triangular solves on the scalar S; instantiate it at S = Dual<M> and the
// first-order rule differentiates once more by itself (solver/tangent.hpp's header comment,
// CLAUDE.md "no hand-written derivatives"). This file is the evidence that what comes out is the
// second derivative.
//
// ---------------------------------------------------------------------------------------------
// THE ERROR CONTRACT (§5.2a case 2: a tolerance stated once MEASURED, never guessed)
// ---------------------------------------------------------------------------------------------
// The reference is central differences of the EXACT first-order IFT sensitivities — the quantity
// the engine already computes to machine precision — not of the prices. Measured first,
// 2026-10-07, by sweeping the step over six decades (the [ SWEEP ] lines this file prints are
// that measurement, re-run on every ctest):
//
//   2-point central differences, O(h^2): a clean 100x per decade from h = 1e-3 (1.507e-3) down to
//   a floor at h = 2e-6 and rising again below it. Worst over all 1,013 M1 outputs and all 23
//   two-curve outputs, at h = 2e-6:
//        max |H_dual − H_cd3| / max|H_dual|                      worst 6.020e-9
//        the same entry by entry over |H_ij| >= 0.1 max|H|       worst 2.089e-8
//
//   5-point central differences, O(h^4), at h = 3e-5 — fifteen times the step and an order of
//   magnitude tighter, which is only possible if the NESTED pass is the accurate side:
//        max |H_dual − H_cd5| / max|H_dual|                      worst 4.628e-10
//        the same over the dominant entries                      worst 4.628e-10
//
//   max |H_ij − H_ji| / max|H| of the nested pass alone          worst 1.569e-15
//
// So the gates are 5e-8 / 2e-7, 5e-9 / 5e-9 and 2e-14 — about an order of magnitude above what
// was measured — and every case PRINTS its measured value so drift is visible rather than
// absorbed. Four of the five are tolerances on a REFERENCE's accuracy, not the dual's. The
// symmetry figure is different in kind: it is a property of the nested pass alone (Clairaut), it
// needs no reference, and it is held to roundoff.
//
// The floor is the differencing's and not the solver's: driving every Newton to stagnation
// (tol = 0, 8 accepted steps, ‖F‖∞ 6.9e-18 instead of 4 steps and 7.6e-17) moved no figure above
// at all, and the h-independent part of the residual disagreement sits on Hessian entries three
// decades below their row's maximum, where the difference quotient has no digits left.
//
// ---------------------------------------------------------------------------------------------
// CAN A DEFECT REACH THE OBSERVABLE? — per fixture, because three stages this week found a gate
// that could not observe what it claimed to gate.
// ---------------------------------------------------------------------------------------------
//   * M1 calibration (12 quotes, 1,000 swaps): YES. The calibrated state's own second derivative
//     is 6.36e+1 against a first derivative of order 1, and it is gated directly; and
//     SecondOrderIftReachability injects the single most plausible defect — F_z factorised on its
//     value channel, so it does not carry dF_z/dw — and measures 100% of d2z and 218% of the book
//     gamma changed, with the first-order answer EXACTLY untouched. That last clause is the point:
//     no first-order gate in this repository could ever have caught it.
//   * two-curve chain, the trades and states that read both curves: YES, the same way, with the
//     chain rule across two blocks on top.
//   * two-curve chain, the basis quotes against the disc-only trades and the disc state: NO. Those
//     derivatives are exactly zero by construction, so no arithmetic defect can move them. That
//     case is a STRUCTURAL CHECK and is labelled one below; it is not banked as coverage of the
//     second-order arithmetic.
//
// That the nesting leaves the first-order answer bitwise alone, and that the LU's pivot sequence
// is still the plain-`double` factorisation's, is tests/solver/second_order_ift_e0_test.cpp.
#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <string>
#include <type_traits>
#include <vector>

#include "epykos/fixtures/m1_book.hpp"
#include "epykos/fixtures/m1_calibration.hpp"
#include "epykos/fixtures/m1_price.hpp"
#include "epykos/fixtures/second_order_ift.hpp"
#include "epykos/scalar/dual.hpp"
#include "epykos/solver/tangent.hpp"

namespace fx = epykos::fixtures;
namespace solver = epykos::solver;
using epykos::Dual;

namespace {

// The steps the sweeps picked, and the tolerances measured at them. Never move one of these to
// make a change pass: a failure here says the second derivative through the calibration is wrong,
// or the step is no longer at the floor, and both are findings.
constexpr double kStep3 = 2.0e-6;   // 2-point stencil
constexpr double kStep5 = 3.0e-5;   // 5-point stencil
constexpr double kTol3RelNorm = 5e-8;
constexpr double kTol3RelTop = 2e-7;
constexpr double kTol5RelNorm = 5e-9;
constexpr double kTol5RelTop = 5e-9;
constexpr double kTolAsymmetry = 2e-14;

constexpr int Nq = fx::so_m1_quotes;   // 12
constexpr int Nt = fx::so_2c_quotes;   // 18

const fx::Book& book() {
  static const fx::Book b = fx::make_m1_book();
  return b;
}
const std::vector<fx::CalSwap>& disc_swaps() {
  static const std::vector<fx::CalSwap> s = fx::m1_calibration_swaps();
  return s;
}
const std::vector<fx::CalSwap>& proj_swaps() {
  static const std::vector<fx::CalSwap> s = fx::proj_calibration_swaps();
  return s;
}
// The quotes whose calibration recovers the M1 record state.
const std::vector<double>& m1_quotes() {
  static const std::vector<double> q = fx::m1_par_quotes(book().z0.data());
  return q;
}
// The 18 quotes (12 par rates, 6 basis spreads) at the two generating curves.
const std::vector<double>& two_curve_quotes() {
  static const std::vector<double> q = fx::two_curve_quotes(fx::record_state.data(), fx::proj_record_state.data());
  return q;
}

// fixture 1: quotes -> implicit_dual -> price_book, plus the calibrated state.
struct M1 {
  template <class Scalar>
  void operator()(const Scalar* q, Scalar* out) const {
    fx::calibrated_m1<Scalar>(book(), disc_swaps(), q, out);
  }
};
// fixture 2: 18 quotes -> two chained implicit nodes -> 5 trades, plus both states.
struct TwoCurve {
  template <class Scalar>
  void operator()(const Scalar* q, Scalar* out) const {
    fx::two_curve_chain<Scalar>(disc_swaps(), proj_swaps(), two_curve_quotes().data(), q, out);
  }
};

// One slice of outputs, gated and reported as a unit.
struct Slice {
  const char* name;
  int lo;
  int hi;
};

// Gate a run against both references and print what was measured.
template <int N, class F>
void check(const char* fixture, F&& f, const double* q, int n_out, const fx::NestedRun<N>& run,
           const std::vector<Slice>& slices) {
  const std::vector<double> cd3 = fx::hessian_cd3<N>(f, q, kStep3, n_out);
  const std::vector<double> cd5 = fx::hessian_cd5<N>(f, q, kStep5, n_out);
  for (const Slice& s : slices) {
    int a3 = -1, a5 = -1, aa = -1;
    const fx::Agreement g3 = fx::worst_agreement<N>(run, cd3, s.lo, s.hi, &a3);
    const fx::Agreement g5 = fx::worst_agreement<N>(run, cd5, s.lo, s.hi, &a5);
    const double asym = fx::worst_asymmetry<N>(run, s.lo, s.hi, &aa);
    std::printf(
        "[ MEASURED ] %-26s %-22s cd3 rel %-10.3e (o%-4d) top %-10.3e | cd5 rel %-10.3e (o%-4d) top %-10.3e | asym "
        "%-10.3e (o%d)\n",
        fixture, s.name, g3.rel_norm, a3, g3.rel_top, g5.rel_norm, a5, g5.rel_top, asym, aa);
    const std::string where = std::string(fixture) + " / " + s.name;
    EXPECT_GT(g3.n_top, 0) << where << ": no dominant Hessian entry anywhere in this slice — the gate would be vacuous";
    EXPECT_LE(g3.rel_norm, kTol3RelNorm) << where << ": gamma disagrees with 2-point central differences of the exact "
                                                     "first-order IFT sensitivities (worst output "
                                         << a3 << ")";
    EXPECT_LE(g3.rel_top, kTol3RelTop) << where << ": a dominant gamma entry disagrees with its 2-point difference";
    EXPECT_LE(g5.rel_norm, kTol5RelNorm) << where << ": gamma disagrees with 5-point central differences (worst output "
                                         << a5 << ")";
    EXPECT_LE(g5.rel_top, kTol5RelTop) << where << ": a dominant gamma entry disagrees with its 5-point difference";
    EXPECT_LE(asym, kTolAsymmetry) << where << ": the nested pass's own Hessian is not symmetric (output " << aa << ")";
  }
}

// The sweep, printed so the floor is visible rather than asserted.
template <int N, class F>
void sweep(const char* fixture, F&& f, const double* q, int n_out, const fx::NestedRun<N>& run,
           const std::vector<Slice>& slices) {
  for (double h : {1e-3, 1e-4, 1e-5, 3e-6, 2e-6, 1e-6, 3e-7}) {
    const std::vector<double> ref = fx::hessian_cd3<N>(f, q, h, n_out);
    std::printf("[  SWEEP   ] %-16s cd3 h %-9.1e", fixture, h);
    for (const Slice& s : slices) {
      int arg = -1;
      const fx::Agreement a = fx::worst_agreement<N>(run, ref, s.lo, s.hi, &arg);
      std::printf("  %s rel %-11.4e (o%-4d)", s.name, a.rel_norm, arg);
    }
    std::printf("\n");
    std::fflush(stdout);
  }
  for (double h : {1e-3, 3e-4, 1e-4, 3e-5, 1e-5, 3e-6}) {
    const std::vector<double> ref = fx::hessian_cd5<N>(f, q, h, n_out);
    std::printf("[  SWEEP   ] %-16s cd5 h %-9.1e", fixture, h);
    for (const Slice& s : slices) {
      int arg = -1;
      const fx::Agreement a = fx::worst_agreement<N>(run, ref, s.lo, s.hi, &arg);
      std::printf("  %s rel %-11.4e (o%-4d)", s.name, a.rel_norm, arg);
    }
    std::printf("\n");
    std::fflush(stdout);
  }
}

std::vector<Slice> m1_slices() {
  return {{"1001 book PVs", 0, fx::so_m1_book_pv + 1}, {"12 calibrated knots", fx::so_m1_state0, fx::so_m1_state0 + Nq}};
}
std::vector<Slice> two_curve_slices() {
  return {{"5 trades", 0, fx::n_two_curve_book_outputs}, {"18 calibrated knots", fx::so_2c_disc_state0, fx::so_2c_outputs}};
}

const fx::NestedRun<Nq>& m1_run() {
  static const fx::NestedRun<Nq> r = fx::nested_run<Nq>(M1{}, m1_quotes().data(), fx::so_m1_outputs());
  return r;
}
const fx::NestedRun<Nt>& two_curve_run() {
  static const fx::NestedRun<Nt> r = fx::nested_run<Nt>(TwoCurve{}, two_curve_quotes().data(), fx::so_2c_outputs);
  return r;
}

}  // namespace

// ---- the M1 calibration: gamma of 1,000 swaps and a book with respect to 12 par quotes ---------

TEST(SecondOrderIft, M1CalibratedBookGammaAgreesWithCentralDifferences) {
  const fx::NestedRun<Nq>& run = m1_run();
  // The engine could not compute any of this before (PRINCIPLES.md §1b: "no gamma, cross-gamma,
  // vanna or volga anywhere ... by any route"), so first: there IS something here to be wrong.
  double hz = 0.0, gz = 0.0;
  for (int k = 0; k < Nq; ++k) {
    const int o = fx::so_m1_state0 + k;
    hz = std::fmax(hz, run.norm(o));
    for (int i = 0; i < Nq; ++i) gz = std::fmax(gz, std::fabs(run.g(o, i)));
  }
  std::printf(
      "[  REACH   ] M1 calibration: book PV %.6e, max|gamma| %.4e ; the CALIBRATED STATE's own "
      "max|d2z/dq2| = %.4e against max|dz/dq| = %.4e, and both slices below are gated — so a defect in the "
      "second-order IFT reaches the observable. Quantified by SecondOrderIftReachability.\n",
      run.value[static_cast<std::size_t>(fx::so_m1_book_pv)], run.norm(fx::so_m1_book_pv), hz, gz);
  EXPECT_GT(run.norm(fx::so_m1_book_pv), 0.0) << "the book's gamma with respect to par quotes is identically zero";
  EXPECT_GT(hz, 0.0) << "d2z/dq2 is identically zero: the second-order IFT term contributes nothing and this "
                        "fixture cannot observe a defect in it";

  check<Nq>("M1 calibration", M1{}, m1_quotes().data(), fx::so_m1_outputs(), run, m1_slices());
}

TEST(SecondOrderIft, M1StepSweep) {
  sweep<Nq>("M1 calibration", M1{}, m1_quotes().data(), fx::so_m1_outputs(), m1_run(), m1_slices());
}

// ---- the two-curve chain: two implicit nodes, 18 quotes ----------------------------------------

TEST(SecondOrderIft, TwoCurveChainGammaAgreesWithCentralDifferences) {
  const fx::NestedRun<Nt>& run = two_curve_run();
  for (int i : fx::so_2c_both_trades) {
    EXPECT_GT(run.norm(i), 0.0) << "two-curve trade " << i << " reads both curves and has a zero Hessian";
  }
  // The proj state's gamma with respect to the DISC quotes can only come through the chain: the
  // basis residual reads the disc state, so d2zp/dq_disc2 carries d2zd/dq_disc2 with it.
  double chain = 0.0;
  for (int k = 0; k < fx::n_proj_knots; ++k) {
    const int o = fx::so_2c_proj_state0 + k;
    for (int i = 0; i < Nq; ++i) {
      for (int j = 0; j < Nq; ++j) chain = std::fmax(chain, std::fabs(run.h(o, i, j)));
    }
  }
  std::printf(
      "[  REACH   ] two-curve chain: trades reading both curves have max|gamma| %.4e, and the PROJ state's gamma "
      "w.r.t. the 12 DISC quotes is %.4e — nonzero only because the second block's parameters carry the first "
      "block's second derivative. Gated below.\n",
      run.norm(fx::so_2c_both_trades[2]), chain);
  EXPECT_GT(chain, 0.0) << "the second block's Hessian does not see the first block's: the chain carries nothing "
                           "and this fixture cannot observe a defect in it";

  check<Nt>("two-curve chain", TwoCurve{}, two_curve_quotes().data(), fx::so_2c_outputs, run, two_curve_slices());
}

TEST(SecondOrderIft, TwoCurveStepSweep) {
  sweep<Nt>("two-curve chain", TwoCurve{}, two_curve_quotes().data(), fx::so_2c_outputs, two_curve_run(),
            two_curve_slices());
}

// ---- the exact zero ---------------------------------------------------------------------------

// A trade that does not read a curve must give EXACTLY 0.0 for that curve's quotes, at BOTH
// orders — not a small plausible number. In the sequential chain the disc block's residual never
// reads the 6 basis quotes, so F_p's columns there are exactly zero, the triangular solve returns
// exact zeros, and nothing downstream of the disc state can manufacture a dependence. This is a
// STRUCTURAL CHECK: no arithmetic defect in the second-order path can move a zero that is zero by
// construction, so it is not coverage of the second-order arithmetic. It is here because this
// repository has twice shipped a plausible number where there should have been an unmistakable
// one (D81 §6(a), D89 §3, D94).
TEST(SecondOrderIft, TwoCurveStructuralZeroIsExactlyZero) {
  const fx::NestedRun<Nt>& run = two_curve_run();
  std::printf(
      "[  REACH   ] two-curve chain, basis quotes 12..17 against the disc-only trades and the disc state: "
      "STRUCTURAL CHECK ONLY — these derivatives are exactly zero by construction, so no second-order defect can "
      "reach them. Not counted as coverage.\n");
  std::vector<int> disc_only;
  for (int i : fx::so_2c_disc_only_trades) disc_only.push_back(i);
  for (int k = 0; k < fx::n_knots; ++k) disc_only.push_back(fx::so_2c_disc_state0 + k);
  int checked = 0;
  for (int o : disc_only) {
    for (int i = fx::so_2c_proj_quote0; i < Nt; ++i) {
      // The sign of a zero is not pinned: the solve divides an exact zero by a pivot that may be
      // negative, so −0.0 reaches here. −0.0 == 0.0, and that is the claim being made.
      EXPECT_EQ(run.g(o, i), 0.0) << "output " << o << " d/dq_" << i << " must be exactly zero";
      EXPECT_EQ(run.gi(o, i), 0.0) << "output " << o << " d/dq_" << i << " (inner channel) must be exactly zero";
      for (int j = 0; j < Nt; ++j) {
        EXPECT_EQ(run.h(o, i, j), 0.0) << "output " << o << " d2/dq_" << i << "dq_" << j << " must be exactly zero";
        EXPECT_EQ(run.h(o, j, i), 0.0) << "output " << o << " d2/dq_" << j << "dq_" << i << " must be exactly zero";
      }
      ++checked;
    }
  }
  EXPECT_EQ(checked, static_cast<int>(disc_only.size()) * fx::n_proj_knots);
  // ... and the same outputs are NOT zero where they should not be, or the check above would pass
  // on a fixture that computes nothing at all.
  for (int o : disc_only) EXPECT_GT(run.norm(o), 0.0) << "output " << o << " has an identically zero Hessian";
}

// ---- reachability, quantified ------------------------------------------------------------------

// The single most plausible defect in a nesting implicit rule is to factorise F_z on its value
// channel — to do the LU "in doubles" and lift the result — so that the entries no longer carry
// dF_z/dw. The first-order answer is then still exactly right, which is why no first-order gate
// could catch it. This injects exactly that and measures what it changes. It is also the honest
// substitute for a registered mutant: solver/tangent.hpp is header-only under include/, and
// tests/mutation/registry_test.cpp requires every registered mutant to have exactly one use site
// under src/, so a mutant cannot be registered for it without moving the templated LU out of the
// header — which D14 and the Scalar-templated design forbid.
TEST(SecondOrderIftReachability, FreezingFzChangesTheGammaAndNotTheDelta) {
  using Inner = Dual<Nq>;
  using Outer = Dual<Nq, Inner>;
  const std::vector<double>& q = m1_quotes();
  auto R = [&](const auto* z, const auto* p, auto* F) { fx::m1_residual(disc_swaps(), z, p, F); };

  // The solution on Inner, with its first derivatives: exactly what implicit_dual builds.
  std::vector<Inner> qv(Nq), zv(Nq);
  for (int k = 0; k < Nq; ++k) {
    qv[static_cast<std::size_t>(k)] = Inner::variable(q[static_cast<std::size_t>(k)], k);
    zv[static_cast<std::size_t>(k)] = Inner(fx::so_ift_start);
  }
  const solver::SolveReport rep =
      solver::implicit_dual<Nq>(R, Nq, Nq, Nq, qv.data(), zv.data(), fx::so_ift_options());
  ASSERT_TRUE(rep.converged);

  std::vector<Inner> F(Nq), Jz(static_cast<std::size_t>(Nq) * Nq);
  solver::jacobian_dual(R, Nq, Nq, Nq, qv.data(), zv.data(), F.data(), Jz.data());
  std::vector<Inner> frozen = Jz;
  for (Inner& x : frozen) x.d.fill(0.0);  // the defect
  std::vector<int> perm_full, perm_frozen;
  std::vector<Inner> lu_full = Jz, lu_frozen = frozen;
  solver::detail::lu_factor(Nq, lu_full, perm_full);
  solver::detail::lu_factor(Nq, lu_frozen, perm_frozen);
  EXPECT_EQ(perm_full, perm_frozen) << "the pivot is decided on the value channel, so freezing the tangents must "
                                       "not change the elimination order";

  // The rhs is the same on both sides: only F_z differs.
  std::vector<Outer> qd(Nq), zc(Nq), Fd(Nq);
  for (int k = 0; k < Nq; ++k) {
    qd[static_cast<std::size_t>(k)] = Outer::variable(Inner::variable(q[static_cast<std::size_t>(k)], k), k);
    zc[static_cast<std::size_t>(k)] = Outer(zv[static_cast<std::size_t>(k)]);
  }
  R(zc.data(), qd.data(), Fd.data());
  std::vector<Outer> z_full(Nq), z_frozen(Nq);
  for (int j = 0; j < Nq; ++j) {
    z_full[static_cast<std::size_t>(j)].v = zv[static_cast<std::size_t>(j)];
    z_frozen[static_cast<std::size_t>(j)].v = zv[static_cast<std::size_t>(j)];
  }
  std::vector<Inner> rhs(Nq), da(Nq), db(Nq);
  for (int dir = 0; dir < Nq; ++dir) {
    for (int i = 0; i < Nq; ++i) rhs[static_cast<std::size_t>(i)] = -Fd[static_cast<std::size_t>(i)].d[static_cast<std::size_t>(dir)];
    solver::detail::lu_solve(Nq, lu_full, perm_full, rhs.data(), da.data());
    solver::detail::lu_solve(Nq, lu_frozen, perm_frozen, rhs.data(), db.data());
    for (int j = 0; j < Nq; ++j) {
      z_full[static_cast<std::size_t>(j)].d[static_cast<std::size_t>(dir)] = da[static_cast<std::size_t>(j)];
      z_frozen[static_cast<std::size_t>(j)].d[static_cast<std::size_t>(dir)] = db[static_cast<std::size_t>(j)];
    }
  }

  // d2z: 100% of it comes from F_z carrying its own derivative. dz: none of it does.
  double dz_diff = 0.0, d2z_diff = 0.0, d2z_norm = 0.0;
  for (int j = 0; j < Nq; ++j) {
    for (int i = 0; i < Nq; ++i) {
      dz_diff = std::fmax(dz_diff, std::fabs(z_full[static_cast<std::size_t>(j)].d[static_cast<std::size_t>(i)].v -
                                             z_frozen[static_cast<std::size_t>(j)].d[static_cast<std::size_t>(i)].v));
      for (int l = 0; l < Nq; ++l) {
        const double a = z_full[static_cast<std::size_t>(j)].d[static_cast<std::size_t>(i)].d[static_cast<std::size_t>(l)];
        const double b = z_frozen[static_cast<std::size_t>(j)].d[static_cast<std::size_t>(i)].d[static_cast<std::size_t>(l)];
        d2z_norm = std::fmax(d2z_norm, std::fabs(a));
        d2z_diff = std::fmax(d2z_diff, std::fabs(a - b));
      }
    }
  }
  // And through the book.
  const std::size_t n_pv = static_cast<std::size_t>(book().n_swaps) + 1;
  std::vector<Outer> pv_full(n_pv), pv_frozen(n_pv);
  fx::price_book<Outer>(book(), z_full.data(), pv_full.data(), pv_full.data() + book().n_swaps);
  fx::price_book<Outer>(book(), z_frozen.data(), pv_frozen.data(), pv_frozen.data() + book().n_swaps);
  const std::size_t bp = static_cast<std::size_t>(book().n_swaps);
  double delta_diff = 0.0, gamma_diff = 0.0, gamma_norm = 0.0;
  for (int i = 0; i < Nq; ++i) {
    delta_diff = std::fmax(delta_diff, std::fabs(pv_full[bp].d[static_cast<std::size_t>(i)].v -
                                                 pv_frozen[bp].d[static_cast<std::size_t>(i)].v));
    for (int l = 0; l < Nq; ++l) {
      const double a = pv_full[bp].d[static_cast<std::size_t>(i)].d[static_cast<std::size_t>(l)];
      const double b = pv_frozen[bp].d[static_cast<std::size_t>(i)].d[static_cast<std::size_t>(l)];
      gamma_norm = std::fmax(gamma_norm, std::fabs(a));
      gamma_diff = std::fmax(gamma_diff, std::fabs(a - b));
    }
  }
  std::printf(
      "[ MEASURED ] reachability, F_z frozen to its value channel: dz unchanged by %.3e, d2z changed by %.3e of "
      "%.3e (%.1f%%); book delta unchanged by %.3e, book gamma changed by %.3e of %.3e (%.1f%%)\n",
      dz_diff, d2z_diff, d2z_norm, 100.0 * d2z_diff / d2z_norm, delta_diff, gamma_diff, gamma_norm,
      100.0 * gamma_diff / gamma_norm);
  // The defect leaves first order EXACTLY alone — so no first-order gate could ever see it.
  EXPECT_EQ(dz_diff, 0.0);
  EXPECT_EQ(delta_diff, 0.0);
  // ... and moves second order by the whole of it, so the gates above DO see it.
  EXPECT_GT(d2z_diff, 0.5 * d2z_norm);
  EXPECT_GT(gamma_diff, 0.5 * gamma_norm);
}

// ---- third order, for free ----------------------------------------------------------------------
//
// The honest test that depth 2 is not special-cased: `Dual<3, Dual<3, Dual<3>>>` through the same
// calibration, with no further code anywhere. Three of the twelve quotes vary (a depth-3 scalar is
// (1+3)^3 = 64 doubles), and the third derivative is checked two ways —
//
//   * full permutation symmetry over all six orderings of (i, j, l), which needs no reference at
//     all and is held to roundoff: measured worst 4.533e-17, gated 1e-15;
//   * against central differences of the EXACT second derivatives the depth-2 pass already
//     produces. The sweep falls a clean 100x per decade from h = 1e-3 to a floor at h = 3e-6:
//     3.156e-6, 2.840e-7, 3.156e-8, 2.837e-9 (h = 3e-5), 3.348e-10 (1e-5), 1.906e-10 (3e-6),
//     5.165e-10 (1e-6). Worst at the floor 1.906e-10, gated 2e-9.
TEST(SecondOrderIft, ThirdOrderThroughTheCalibrationNeedsNoFurtherCode) {
  constexpr int M = 3;
  constexpr double kStepT = 3.0e-6;
  constexpr double kTolThird = 2e-9;
  constexpr double kTolThirdSymmetry = 1e-15;
  constexpr int kVary[M] = {1, 3, 5};  // which quotes move
  using D1 = Dual<M>;
  using D2 = Dual<M, D1>;
  using D3 = Dual<M, D2>;

  // out[0] = the book PV, out[1..3] = the three calibrated knots at the varied tenors.
  auto f = [](const auto* x, auto* out) {
    using Scalar = std::decay_t<decltype(*x)>;
    std::vector<Scalar> q(static_cast<std::size_t>(Nq));
    for (int k = 0; k < Nq; ++k) q[static_cast<std::size_t>(k)] = Scalar(m1_quotes()[static_cast<std::size_t>(k)]);
    for (int k = 0; k < M; ++k) q[static_cast<std::size_t>(kVary[k])] = x[k];
    std::vector<Scalar> all(static_cast<std::size_t>(fx::so_m1_outputs()));
    fx::calibrated_m1<Scalar>(book(), disc_swaps(), q.data(), all.data());
    out[0] = all[static_cast<std::size_t>(fx::so_m1_book_pv)];
    for (int k = 0; k < M; ++k) out[1 + k] = all[static_cast<std::size_t>(fx::so_m1_state0 + kVary[k])];
  };
  double x0[M];
  for (int k = 0; k < M; ++k) x0[k] = m1_quotes()[static_cast<std::size_t>(kVary[k])];

  std::vector<D3> xd(M);
  for (int k = 0; k < M; ++k) xd[static_cast<std::size_t>(k)] = D3::variable(D2::variable(D1::variable(x0[k], k), k), k);
  std::vector<D3> out(M + 1);
  f(xd.data(), out.data());

  // The depth-2 references at the two shifted points, once for every direction.
  std::vector<fx::NestedRun<M>> plus, minus;
  for (int m = 0; m < M; ++m) {
    double xp[M], xm[M];
    for (int k = 0; k < M; ++k) {
      xp[k] = x0[k];
      xm[k] = x0[k];
    }
    xp[m] += kStepT;
    xm[m] -= kStepT;
    plus.push_back(fx::nested_run<M>(f, xp, M + 1));
    minus.push_back(fx::nested_run<M>(f, xm, M + 1));
  }

  for (int o = 0; o <= M; ++o) {
    const D3& r = out[static_cast<std::size_t>(o)];
    auto T = [&](int i, int j, int l) {
      return r.d[static_cast<std::size_t>(i)].d[static_cast<std::size_t>(j)].d[static_cast<std::size_t>(l)];
    };
    double tn = 0.0, sym = 0.0, worst = 0.0;
    for (int i = 0; i < M; ++i) {
      for (int j = 0; j < M; ++j) {
        for (int l = 0; l < M; ++l) {
          tn = std::fmax(tn, std::fabs(T(i, j, l)));
          const double p[6] = {T(i, j, l), T(i, l, j), T(j, i, l), T(j, l, i), T(l, i, j), T(l, j, i)};
          for (int a = 1; a < 6; ++a) sym = std::fmax(sym, std::fabs(p[a] - p[0]));
          const double ref =
              (plus[static_cast<std::size_t>(l)].h(o, i, j) - minus[static_cast<std::size_t>(l)].h(o, i, j)) /
              (2.0 * kStepT);
          worst = std::fmax(worst, std::fabs(T(i, j, l) - ref));
        }
      }
    }
    ASSERT_GT(tn, 0.0) << "output " << o << ": the third derivative through the calibration is identically zero";
    std::printf("[ MEASURED ] third order, output %d: max|T| %-12.4e perm-sym rel %-11.3e vs cd(H) rel %.3e\n", o, tn,
                sym / tn, worst / tn);
    EXPECT_LE(sym / tn, kTolThirdSymmetry) << "output " << o << ": the third derivative is not permutation-symmetric";
    EXPECT_LE(worst / tn, kTolThird) << "output " << o
                                     << ": the third derivative disagrees with central differences of the exact "
                                        "second derivatives";
  }
}
