// EpykosEngine — second order THROUGH THE CALIBRATION (PRINCIPLES.md §1b invariant I2; a
// test-only fixture, D28, the sibling of fixtures/second_order.hpp).
//
// fixtures/second_order.hpp nests `Dual` over the pricing maths and gets a gamma with respect to
// CURVE STATE. This header does the same thing one stage further back, over
// solver::implicit_dual, and gets the number a desk actually asks for: **gamma with respect to
// PAR QUOTES**, through the calibration. Nothing here writes a second derivative down — the
// second-order implicit-function term is what differentiating the existing first-order rule
// produces when the scalar under it nests (solver/tangent.hpp's header comment).
//
//   calibrated_m1<Scalar>(q, out)    quotes -> implicit_dual -> price_book: 1,000 swap PVs, the
//                                    book PV, and the 12 calibrated knots (1,013 outputs)
//   two_curve_chain<Scalar>(q, out)  18 quotes -> TWO chained implicit nodes (12 disc unknowns,
//                                    then 6 proj unknowns whose parameters include the disc
//                                    state) -> 5 trades and the 18 knots (23 outputs). The two
//                                    disc-only trades and the 12 disc knots cannot depend on the
//                                    6 basis quotes: that is the exact-zero fixture.
//
//   nested_run<N>(f, z, n_out)       ONE pass on Dual<N, Dual<N>>: value, both gradient channels
//                                    and the full N x N Hessian of every output
//   gradient_exact<N>(f, z, n_out)   the EXACT first derivatives, one Dual<N> pass (no differencing)
//   hessian_cd3<N>(f, z, h, n_out)   the reference: 2-point central differences of those exact
//                                    first derivatives, O(h^2) truncation      (2N passes)
//   hessian_cd5<N>(f, z, h, n_out)   the sharper reference: the 5-point stencil, O(h^4)  (4N passes)
//   agreement<N>(run, ref, o)        the statistics the gate is stated in, per output
//
// `f` is a generic callable `void f(const Scalar* x, Scalar* out)` — the maths with its book,
// schedules and solver options bound. It is instantiated here on double, on Dual<N> and on
// Dual<N, Dual<N>>. Header-only, so the including TU's contraction setting governs the
// arithmetic: under -ffp-contract=off the value channel is bitwise the double evaluation and both
// first-derivative channels are bitwise the Dual<N> pass
// (tests/solver/second_order_ift_e0_test.cpp).
#pragma once

#include <cmath>
#include <cstddef>
#include <type_traits>
#include <vector>

#include "epykos/fixtures/m1_book.hpp"
#include "epykos/fixtures/m1_calibration.hpp"
#include "epykos/fixtures/m1_price.hpp"
#include "epykos/scalar/dual.hpp"
#include "epykos/solver/implicit.hpp"
#include "epykos/solver/tangent.hpp"

namespace epykos::fixtures {

// Every solve in this header starts from a flat 3% curve and is driven to ‖F‖∞ < 1e-14. Measured
// (tests/solver/second_order_ift_test.cpp): 4 accepted Newton steps and ‖F‖∞ ~ 7.6e-17, and
// driving it further (tol = 0, 8 steps, ‖F‖∞ ~ 6.9e-18) moves no agreement figure at all — so the
// reference's floor is the differencing's, not the solver's.
inline constexpr double so_ift_start = 0.03;
inline solver::SolveOptions so_ift_options() {
  solver::SolveOptions o;
  o.tol = 1e-14;
  return o;
}

// Solve one block on whatever Scalar the maths is instantiated on. `double` has no derivative to
// carry, so it is the Newton iteration alone; a Dual gets the implicit-function rule on top, which
// at a nesting scalar is the second-order rule as well (solver/tangent.hpp). This exists so that
// ONE templated callable can serve as the double oracle, the first-order pass and the nested pass,
// which is what makes the bitwise E0 gate possible.
template <int N, class Scalar, class Residual>
solver::SolveReport solve_block(Residual& R, int n_z, int n_p, int n_r, const Scalar* p, Scalar* z,
                                const solver::SolveOptions& options) {
  if constexpr (std::is_same_v<Scalar, double>) {
    return solver::solve_newton(R, n_z, n_p, n_r, p, z, options);
  } else {
    return solver::implicit_dual<N>(R, n_z, n_p, n_r, p, z, options);
  }
}

// ---- fixture 1: the M1 single-curve calibration -----------------------------------------------

inline constexpr int so_m1_quotes = n_knots;                 // 12
inline int so_m1_outputs() { return n_swaps + 1 + n_knots; }  // 1,000 swap PVs + book PV + 12 knots
inline constexpr int so_m1_book_pv = n_swaps;                 // index of the book PV in out[]
inline constexpr int so_m1_state0 = n_swaps + 1;              // index of calibrated knot 0

// quotes -> implicit_dual -> price_book, then the calibrated state itself. The par quotes come
// from m1_par_quotes (the quotes whose calibration recovers the record state), so the solve lands
// on the record point and the book is the M1 book.
template <class Scalar>
solver::SolveReport calibrated_m1(const Book& book, const std::vector<CalSwap>& swaps, const Scalar* q, Scalar* out) {
  auto R = [&](const auto* z, const auto* p, auto* F) { m1_residual(swaps, z, p, F); };
  std::vector<Scalar> z(static_cast<std::size_t>(so_m1_quotes));
  for (int k = 0; k < so_m1_quotes; ++k) z[static_cast<std::size_t>(k)] = Scalar(so_ift_start);
  const solver::SolveReport rep =
      solve_block<so_m1_quotes>(R, so_m1_quotes, so_m1_quotes, so_m1_quotes, q, z.data(), so_ift_options());
  price_book<Scalar>(book, z.data(), out, out + book.n_swaps);
  for (int k = 0; k < so_m1_quotes; ++k) out[static_cast<std::size_t>(so_m1_state0 + k)] = z[static_cast<std::size_t>(k)];
  return rep;
}

// ---- fixture 2: two chained implicit nodes, and the structural zero ---------------------------
//
// disc (12 unknowns) is calibrated to the 12 par swaps, which read quotes 0..11 only; proj (6
// unknowns) is calibrated to the 6 basis swaps, which read the basis quotes AND the disc state —
// so proj's PARAMETER vector is [quote_12..17, zdisc_0..11] and the chain rule carries first and
// second derivatives with respect to all 18 quotes through it. Both blocks are passed the whole
// 18-direction seed, so the disc block's parameter columns 12..17 are exactly untouched by its
// residual: that is what makes the zeros below structural rather than small.
//
// The trades' own contract terms (the fixed rate, the spread) are taken from the BASE quotes and
// stay fixed when a quote is shifted — they are terms of a trade, not market inputs.

inline constexpr int so_2c_quotes = n_knots + n_proj_knots;  // 18
inline constexpr int so_2c_outputs = n_two_curve_book_outputs + n_knots + n_proj_knots;  // 23
inline constexpr int so_2c_disc_only_trades[2] = {3, 4};  // the two disc swaps of two_curve_book
inline constexpr int so_2c_both_trades[3] = {0, 1, 2};    // the three basis swaps
inline constexpr int so_2c_disc_state0 = n_two_curve_book_outputs;        // 5
inline constexpr int so_2c_proj_state0 = n_two_curve_book_outputs + n_knots;  // 17
inline constexpr int so_2c_proj_quote0 = n_knots;         // quote index of the first basis quote

template <class Scalar>
void two_curve_chain(const std::vector<CalSwap>& dswaps, const std::vector<CalSwap>& pswaps,
                     const double* base_quotes, const Scalar* q, Scalar* out) {
  auto Rd = [&](const auto* z, const auto* p, auto* F) {
    using S = std::decay_t<decltype(*z)>;
    for (int i = 0; i < n_knots; ++i) {
      F[i] = cal_par_rate<S>(knot_times.data(), z, n_knots, dswaps[static_cast<std::size_t>(i)]) - p[i];
    }
  };
  auto Rp = [&](const auto* z, const auto* p, auto* F) {
    using S = std::decay_t<decltype(*z)>;
    const S* zdisc = p + n_proj_knots;
    for (int k = 0; k < n_proj_knots; ++k) {
      F[k] = basis_spread<S>(knot_times.data(), zdisc, n_knots, proj_knot_times.data(), z, n_proj_knots,
                             pswaps[static_cast<std::size_t>(k)]) -
             p[k];
    }
  };
  std::vector<Scalar> zd(static_cast<std::size_t>(n_knots)), zp(static_cast<std::size_t>(n_proj_knots));
  for (int k = 0; k < n_knots; ++k) zd[static_cast<std::size_t>(k)] = Scalar(so_ift_start);
  for (int k = 0; k < n_proj_knots; ++k) zp[static_cast<std::size_t>(k)] = Scalar(so_ift_start);
  solve_block<so_2c_quotes>(Rd, n_knots, so_2c_quotes, n_knots, q, zd.data(), so_ift_options());
  std::vector<Scalar> p2(static_cast<std::size_t>(so_2c_quotes));
  for (int k = 0; k < n_proj_knots; ++k) p2[static_cast<std::size_t>(k)] = q[n_knots + k];
  for (int k = 0; k < n_knots; ++k) p2[static_cast<std::size_t>(n_proj_knots + k)] = zd[static_cast<std::size_t>(k)];
  solve_block<so_2c_quotes>(Rp, n_proj_knots, so_2c_quotes, n_proj_knots, p2.data(), zp.data(), so_ift_options());
  two_curve_book<Scalar>(dswaps, pswaps, base_quotes, zd.data(), zp.data(), out);
  for (int k = 0; k < n_knots; ++k) out[static_cast<std::size_t>(so_2c_disc_state0 + k)] = zd[static_cast<std::size_t>(k)];
  for (int k = 0; k < n_proj_knots; ++k) out[static_cast<std::size_t>(so_2c_proj_state0 + k)] = zp[static_cast<std::size_t>(k)];
}

// ---- one nested pass, the references, and the statistics --------------------------------------

// The result of one Dual<N, Dual<N>> pass of a vector-valued f over n inputs.
template <int N>
struct NestedRun {
  static constexpr int n = N;
  int n_out = 0;
  std::vector<double> value;       // [o]
  std::vector<double> grad;        // [o][i]   outer tangent channel: d out_o / d x_i
  std::vector<double> grad_inner;  // [o][i]   the same gradient by the inner route
  std::vector<double> hess;        // [o][i][j] = d2 out_o / d x_i d x_j

  double g(int o, int i) const { return grad[static_cast<std::size_t>(o) * N + static_cast<std::size_t>(i)]; }
  double gi(int o, int i) const { return grad_inner[static_cast<std::size_t>(o) * N + static_cast<std::size_t>(i)]; }
  double h(int o, int i, int j) const {
    return hess[(static_cast<std::size_t>(o) * N + static_cast<std::size_t>(i)) * N + static_cast<std::size_t>(j)];
  }
  // max |H_ij| of output o — the only scale a Hessian has.
  double norm(int o) const {
    double m = 0.0;
    for (int i = 0; i < N; ++i) {
      for (int j = 0; j < N; ++j) m = std::fmax(m, std::fabs(h(o, i, j)));
    }
    return m;
  }
  // max |H_ij − H_ji| / max |H| of output o: Clairaut, which the nested pass satisfies to
  // roundoff and no further (the two mixed partials are different operation orders). It needs no
  // reference at all, which is why it is held to roundoff and not to a measured tolerance.
  double asymmetry(int o) const {
    const double s = norm(o);
    if (!(s > 0.0)) return 0.0;
    double m = 0.0;
    for (int i = 0; i < N; ++i) {
      for (int j = 0; j < N; ++j) m = std::fmax(m, std::fabs(h(o, i, j) - h(o, j, i)));
    }
    return m / s;
  }
};

// ONE pass of f on Dual<N, Dual<N>> at x. Seeding (scalar/dual.hpp): input k gets the inner seed
// e_k in its VALUE channel and the outer seed e_k in its TANGENT channel, so r.d[i].d[j] is
// d2/dx_i dx_j and r.d[i].v / r.v.d[j] are both the gradient.
template <int N, class F>
NestedRun<N> nested_run(F&& f, const double* x, int n_out) {
  using Inner = Dual<N>;
  using Outer = Dual<N, Inner>;
  std::vector<Outer> xd(static_cast<std::size_t>(N));
  for (int k = 0; k < N; ++k) xd[static_cast<std::size_t>(k)] = Outer::variable(Inner::variable(x[k], k), k);
  std::vector<Outer> out(static_cast<std::size_t>(n_out));
  f(xd.data(), out.data());
  NestedRun<N> r;
  r.n_out = n_out;
  r.value.resize(static_cast<std::size_t>(n_out));
  r.grad.resize(static_cast<std::size_t>(n_out) * N);
  r.grad_inner.resize(static_cast<std::size_t>(n_out) * N);
  r.hess.resize(static_cast<std::size_t>(n_out) * N * N);
  for (int o = 0; o < n_out; ++o) {
    const Outer& y = out[static_cast<std::size_t>(o)];
    r.value[static_cast<std::size_t>(o)] = y.v.v;
    for (int i = 0; i < N; ++i) {
      const std::size_t si = static_cast<std::size_t>(i);
      r.grad[static_cast<std::size_t>(o) * N + si] = y.d[si].v;
      r.grad_inner[static_cast<std::size_t>(o) * N + si] = y.v.d[si];
      for (int j = 0; j < N; ++j) {
        r.hess[(static_cast<std::size_t>(o) * N + si) * N + static_cast<std::size_t>(j)] = y.d[si].d[static_cast<std::size_t>(j)];
      }
    }
  }
  return r;
}

// The EXACT first derivatives at x by one Dual<N> pass: [o][i]. No differencing — this is the
// quantity the engine already computes, and it is what the references below difference.
template <int N, class F>
std::vector<double> gradient_exact(F&& f, const double* x, int n_out) {
  using W = Dual<N>;
  std::vector<W> xd(static_cast<std::size_t>(N));
  for (int k = 0; k < N; ++k) xd[static_cast<std::size_t>(k)] = W::variable(x[k], k);
  std::vector<W> out(static_cast<std::size_t>(n_out));
  f(xd.data(), out.data());
  std::vector<double> g(static_cast<std::size_t>(n_out) * N);
  for (int o = 0; o < n_out; ++o) {
    for (int i = 0; i < N; ++i) {
      g[static_cast<std::size_t>(o) * N + static_cast<std::size_t>(i)] = out[static_cast<std::size_t>(o)].d[static_cast<std::size_t>(i)];
    }
  }
  return g;
}

// H[o][i][j] ~ (g_i(x + h e_j) − g_i(x − h e_j)) / 2h: 2-point central differences of the EXACT
// first derivatives, 2N forward passes. O(h^2) truncation against O(eps/h) cancellation; its own
// error, not the nested dual's, is what limits the agreement.
template <int N, class F>
std::vector<double> hessian_cd3(F&& f, const double* x, double h, int n_out) {
  std::vector<double> H(static_cast<std::size_t>(n_out) * N * N);
  std::vector<double> xx(x, x + N);
  for (int j = 0; j < N; ++j) {
    const std::size_t sj = static_cast<std::size_t>(j);
    xx[sj] = x[j] + h;
    const std::vector<double> gp = gradient_exact<N>(f, xx.data(), n_out);
    xx[sj] = x[j] - h;
    const std::vector<double> gm = gradient_exact<N>(f, xx.data(), n_out);
    xx[sj] = x[j];
    for (int o = 0; o < n_out; ++o) {
      for (int i = 0; i < N; ++i) {
        const std::size_t t = static_cast<std::size_t>(o) * N + static_cast<std::size_t>(i);
        H[t * N + sj] = (gp[t] - gm[t]) / (2.0 * h);
      }
    }
  }
  return H;
}

// The same with the 5-point stencil: (−g(+2h) + 8g(+h) − 8g(−h) + g(−2h)) / 12h, 4N passes,
// O(h^4) truncation. It exists to answer WHOSE floor the 2-point agreement is: this reference
// reaches a tolerance an order of magnitude tighter at a step twenty times larger, which can only
// be true if the nested Hessian is the accurate side.
template <int N, class F>
std::vector<double> hessian_cd5(F&& f, const double* x, double h, int n_out) {
  std::vector<double> H(static_cast<std::size_t>(n_out) * N * N);
  std::vector<double> xx(x, x + N);
  for (int j = 0; j < N; ++j) {
    const std::size_t sj = static_cast<std::size_t>(j);
    xx[sj] = x[j] + h;
    const std::vector<double> gp1 = gradient_exact<N>(f, xx.data(), n_out);
    xx[sj] = x[j] - h;
    const std::vector<double> gm1 = gradient_exact<N>(f, xx.data(), n_out);
    xx[sj] = x[j] + 2.0 * h;
    const std::vector<double> gp2 = gradient_exact<N>(f, xx.data(), n_out);
    xx[sj] = x[j] - 2.0 * h;
    const std::vector<double> gm2 = gradient_exact<N>(f, xx.data(), n_out);
    xx[sj] = x[j];
    for (int o = 0; o < n_out; ++o) {
      for (int i = 0; i < N; ++i) {
        const std::size_t t = static_cast<std::size_t>(o) * N + static_cast<std::size_t>(i);
        H[t * N + sj] = (-gp2[t] + 8.0 * gp1[t] - 8.0 * gm1[t] + gm2[t]) / (12.0 * h);
      }
    }
  }
  return H;
}

// How far apart one output's two Hessians are. `rel_norm` is the figure the gate is stated in:
// the largest deviation measured against the largest entry. `rel_top` repeats it entry by entry
// over the DOMINANT entries (|H_ij| >= 0.1 max|H|), where the difference quotient still has
// digits to spare; below that cut the reference, not the dual, runs out.
struct Agreement {
  double norm = 0.0;      // max |H_ij| of the nested Hessian
  double max_abs = 0.0;   // max |H_ij − Href_ij|
  double rel_norm = 0.0;  // max_abs / norm
  double rel_top = 0.0;   // max |H_ij − Href_ij| / |H_ij| over |H_ij| >= 0.1 max|H|
  int n_top = 0;
};

template <int N>
Agreement agreement(const NestedRun<N>& run, const std::vector<double>& ref, int o) {
  Agreement a;
  a.norm = run.norm(o);
  for (int i = 0; i < N; ++i) {
    for (int j = 0; j < N; ++j) {
      const double y = ref[(static_cast<std::size_t>(o) * N + static_cast<std::size_t>(i)) * N + static_cast<std::size_t>(j)];
      const double xv = run.h(o, i, j);
      const double d = std::fabs(xv - y);
      a.max_abs = std::fmax(a.max_abs, d);
      if (a.norm > 0.0 && std::fabs(xv) >= 0.1 * a.norm) {
        a.rel_top = std::fmax(a.rel_top, d / std::fabs(xv));
        ++a.n_top;
      }
    }
  }
  if (a.norm > 0.0) a.rel_norm = a.max_abs / a.norm;
  return a;
}

// The worst Agreement over outputs [lo, hi), skipping outputs with an identically zero Hessian
// (nothing to state a relative deviation against); `arg` receives where the worst rel_norm was.
template <int N>
Agreement worst_agreement(const NestedRun<N>& run, const std::vector<double>& ref, int lo, int hi, int* arg) {
  Agreement w;
  if (arg) *arg = -1;
  for (int o = lo; o < hi; ++o) {
    if (!(run.norm(o) > 0.0)) continue;
    const Agreement a = agreement<N>(run, ref, o);
    if (a.rel_norm > w.rel_norm) {
      w.rel_norm = a.rel_norm;
      w.max_abs = a.max_abs;
      w.norm = a.norm;
      if (arg) *arg = o;
    }
    w.rel_top = std::fmax(w.rel_top, a.rel_top);
    w.n_top += a.n_top;
  }
  return w;
}

// The worst relative asymmetry over outputs [lo, hi); `arg` receives where.
template <int N>
double worst_asymmetry(const NestedRun<N>& run, int lo, int hi, int* arg) {
  double w = 0.0;
  if (arg) *arg = -1;
  for (int o = lo; o < hi; ++o) {
    const double a = run.asymmetry(o);
    if (a > w) {
      w = a;
      if (arg) *arg = o;
    }
  }
  return w;
}

}  // namespace epykos::fixtures
