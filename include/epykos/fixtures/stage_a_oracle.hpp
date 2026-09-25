// EpykosEngine — the Stage A book against ground truth (a test-only fixture; PRINCIPLES.md §4, D72).
//
// WHAT IS MEASURED, AND WHY IT IS THE RIGHT SCOPE.
//
// The Stage A problem is quotes -> calibration solve -> knots -> book. The oracle covers the
// second arrow: given the solved knots, the book. That is not a shortcut, it is what the contract
// asks for.
//
//   * D7 makes a solver an implicit node that is never unrolled, and `record_stage_a` records the
//     solved knots as tape INPUTS (the 78 non-quote inputs of D65). The recorded expression
//     downstream of the solve therefore takes the knots as given, and "the recorded expression
//     evaluated without rounding" (PRINCIPLES.md §4) means exactly: price the book from those
//     knots with no rounding.
//   * PRINCIPLES.md §3 puts the solver out of scope for now ("a solver change can alter
//     convergence rather than a value"). The optimiser cannot see it, so its error is not what
//     this instrument is for.
//   * It is also what is POSSIBLE without touching the engine, and the reason is a real finding
//     rather than a convenience: `solver::CurveSet` type-erases every instrument residual to
//     exactly two instantiations, `Rec` and `double`
//     (include/epykos/solver/curve_set.hpp:144-145 and :164-165), and the block solve itself is
//     Eigen-on-`double` by D12 (include/epykos/solver/residual.hpp). A third Scalar cannot reach
//     the calibration. D72 records this; nothing here works around it.
//
// The `double` side of the comparison is `price_stage_a_at`, which is the reference every existing
// Stage A gate already uses, and `tests/stage_a/gate_differential_e0_test.cpp` holds the compiled
// program bitwise equal to it. So the naive path's error measured here is also the compiled
// program's error, by transfer from a gate that is already green — measured, not assumed.
//
// Header-only for the same reason as fixtures/m1_oracle.hpp.
#pragma once

#include <cmath>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "epykos/fixtures/stage_a.hpp"
#include "epykos/scalar/dual.hpp"
#include "epykos/scalar/wide.hpp"
#include "epykos/solver/curve_set.hpp"
#include "epykos/verify/oracle.hpp"

namespace epykos::fixtures {

// `solver::CurveSet::states_at` at any Scalar. The engine ships only the `double` one
// (curve_set.hpp:206); every piece this needs — `CurveStates<Scalar>`, `CurveSpec`,
// `curve::Composite::prepare<Scalar>` — is already templated, so this is an assembly of existing
// parts and not new maths. `z[c]` is curve c's knots in that curve's own variable.
template <class Scalar>
solver::CurveStates<Scalar> stage_a_states_at(const solver::CurveSet& set, const std::vector<std::vector<Scalar>>& z) {
  solver::CurveStates<Scalar> st;
  st.specs = &set.curves();
  st.resize(static_cast<std::size_t>(set.n_curves()));
  for (int c = 0; c < set.n_curves(); ++c) {
    const std::vector<Scalar>& zc = z.at(static_cast<std::size_t>(c));
    st.set(c, zc.data(), zc.size());
  }
  return st;
}

// The number of book outputs this fixture reports: 4 per trade, then the currency totals, the
// netting totals and the book. (8,043 on the Stage A problem of PROBLEM.md §4.)
inline int stage_a_oracle_outputs(const StageA& s) {
  return 4 * s.n_trades() + s.n_currencies() + s.n_netting_sets() + 1;
}

// The flat output order this fixture uses, for both sides of the comparison. It is deliberately
// the fixture's own order rather than the tape's `StageALayout`: the tape's order interleaves the
// residual and diagnostic outputs of the solve, which the oracle does not compute, and a
// comparison should not carry ordinals it leaves empty.
//
//   [0,            n)  pv        per trade, trade currency
//   [n,           2n)  pv_usd    per trade, reporting currency
//   [2n,          3n)  leg0      per trade
//   [3n,          4n)  leg1      per trade
//   [4n,     4n+C   )  currency totals
//   [4n+C,   4n+C+N )  netting totals
//    4n+C+N            the book total
template <class Scalar>
void stage_a_flatten(const StageA& s, const StageABook<Scalar>& b, Scalar* out) {
  const std::size_t n = static_cast<std::size_t>(s.n_trades());
  for (std::size_t i = 0; i < n; ++i) {
    out[i] = b.pv[i];
    out[n + i] = b.pv_usd[i];
    out[2 * n + i] = b.leg0[i];
    out[3 * n + i] = b.leg1[i];
  }
  std::size_t at = 4 * n;
  for (std::size_t c = 0; c < b.currency_total.size(); ++c) out[at++] = b.currency_total[c];
  for (std::size_t k = 0; k < b.netting_total.size(); ++k) out[at++] = b.netting_total[k];
  out[at] = b.book_total;
}

// Curve c's knots sliced out of a flat state of `set.n_knots_total()` values, curve order.
//
// TEMPLATED, and that is load-bearing rather than tidiness. An earlier draft of this fixture took
// `const double*` and reached it from the Wide side with `z[k].value()`. For the VALUATION channel
// that is harmless, because a ball's states are exactly representable doubles. For the JACOBIAN it
// is fatal and silent: `oracle_jacobian` perturbs a knot by h = 3.3e-11, so the perturbed state is
// a Wide with a nonzero low word, and rounding it back to double quantises the step to double's
// own resolution. The measured symptom was an oracle finite difference that got BETTER as h grew
// (4.5e-05 at h = 1e-13 down to 4.2e-13 at h = 1e-5) -- the signature of a roundoff-limited double
// difference, not of a 106-bit one -- and a "naive path error" of 1.3e-07 that was the
// instrument's error, not the naive path's. Nothing in the harness could have caught it: the
// values were finite, normal and at full precision, and the loss was in the INPUT.
// tests/stage_a/oracle_error_test.cpp now pins the h-stability that would have exposed it.
template <class Scalar>
std::vector<std::vector<Scalar>> stage_a_unflatten_knots(const solver::CurveSet& set, const Scalar* z) {
  std::vector<std::vector<Scalar>> knots;
  knots.reserve(static_cast<std::size_t>(set.n_curves()));
  std::size_t at = 0;
  for (int c = 0; c < set.n_curves(); ++c) {
    const std::size_t n = static_cast<std::size_t>(set.n_knots(c));
    knots.emplace_back(z + at, z + at + n);
    at += n;
  }
  return knots;
}

// Ground truth: the Stage A book priced from the knot state `z` at 106 significand bits, in the
// flat order above. `s` and `set` must outlive the function.
inline verify::OracleFn stage_a_oracle_fn(const StageA& s, const solver::CurveSet& set) {
  return [&s, &set](const Wide* z, Wide* out) {
    // The state stays in Wide from end to end. It must: see stage_a_unflatten_knots.
    const std::vector<std::vector<Wide>> knots = stage_a_unflatten_knots<Wide>(set, z);
    solver::CurveStates<Wide> st = stage_a_states_at<Wide>(set, knots);
    DfMemo<Wide, std::function<Wide(int, double)>> memo(
        [&st](int slot, double t) -> Wide { return st.df(slot, t); });
    const StageABook<Wide> b = price_stage_a<Wide>(s, memo);
    stage_a_flatten<Wide>(s, b, out);
  };
}

// The naive path: the same book from the same knots on `double`, through the same memoised df the
// recording used. This is `price_stage_a_at`'s own composition, re-expressed here so that both
// sides of the comparison read the state the same way; `tests/stage_a/oracle_truth_test.cpp`
// checks it agrees with `price_stage_a_at` bitwise before measuring anything with it.
inline verify::ScalarFn stage_a_naive_fn(const StageA& s, const solver::CurveSet& set) {
  return [&s, &set](const double* z, double* out) {
    const std::vector<std::vector<double>> knots = stage_a_unflatten_knots<double>(set, z);
    solver::CurveStates<double> st = stage_a_states_at<double>(set, knots);
    DfMemo<double, std::function<double(int, double)>> memo(
        [&st](int slot, double t) -> double { return st.df(slot, t); });
    const StageABook<double> b = price_stage_a<double>(s, memo);
    stage_a_flatten<double>(s, b, out);
  };
}

// PRINCIPLES.md §4's output classes for the Stage A book.
inline std::vector<verify::OutputClass> stage_a_output_classes(const StageA& s) {
  const int n = s.n_trades();
  return {verify::OutputClass{"valuation: trade PV", 0, n},
          verify::OutputClass{"valuation: trade PV (rpt)", n, n},
          verify::OutputClass{"valuation: leg PV", 2 * n, 2 * n},
          verify::OutputClass{"valuation: aggregates", 4 * n, s.n_currencies() + s.n_netting_sets() + 1}};
}

// The D26 scale of every output, in the flat order above, at the given knot state. Without it a
// trade PV's error reads as the CONDITIONING of a cancelling difference rather than as the quality
// of the arithmetic, and the two differ by five decades on this fixture: measured unscaled, the
// trade-PV class maxes at 2.4e-08 while the leg-PV class, which does not cancel, maxes at 1.6e-13.
// Reporting the first as "the naive path's error" would be reporting the book's conditioning.
//
// Mirrors fixtures::stage_a_scales (src/fixtures/stage_a_e0.cpp): |leg0| + |leg1| for a trade
// (|notional| for a future), Sigma |pv| over an aggregate's trades. A leg PV is a same-signed sum
// of coupons and does not cancel, so its scale is 0 -- differential.hpp's convention for
// "relative to the value itself".
inline verify::ScaleFn stage_a_scale_fn(const StageA& s, const solver::CurveSet& set) {
  return [&s, &set](const double* z, double* scale) {
    const std::vector<std::vector<double>> knots = stage_a_unflatten_knots<double>(set, z);
    solver::CurveStates<double> st = stage_a_states_at<double>(set, knots);
    DfMemo<double, std::function<double(int, double)>> memo(
        [&st](int slot, double t) -> double { return st.df(slot, t); });
    const StageABook<double> b = price_stage_a<double>(s, memo);
    const StageAScales sc = stage_a_scales(s, b);

    const std::size_t n = static_cast<std::size_t>(s.n_trades());
    for (std::size_t i = 0; i < n; ++i) {
      const instrument::Instrument& in = s.instruments[i];
      const double fx = in.currency == s.def.reporting_currency ? 1.0 : s.def.fx_placeholder(in.currency);
      scale[i] = sc.trade[i];
      scale[n + i] = sc.trade[i] * std::fabs(fx);
      scale[2 * n + i] = 0.0;  // a leg does not cancel: relative to itself
      scale[3 * n + i] = 0.0;
    }
    std::size_t at = 4 * n;
    for (std::size_t c = 0; c < sc.currency.size(); ++c) scale[at++] = sc.currency[c];
    for (std::size_t k = 0; k < sc.netting.size(); ++k) scale[at++] = sc.netting[k];
    scale[at] = sc.book;
  };
}

// The NAIVE derivative path: the same templated maths on Dual<1> (scalar/dual.hpp), seeded along
// knot `k`. This is the analytic derivative of the recorded expression computed in double -- what
// a risk number on this engine is made of -- as distinct from a bumped finite difference, which is
// a different algorithm with a truncation error of its own. `out` receives one directional
// derivative per output, in the flat order above.
inline void stage_a_tangent(const StageA& s, const solver::CurveSet& set, const double* z, int k, double* out) {
  using D = Dual<1>;
  const std::vector<std::vector<double>> knots = stage_a_unflatten_knots<double>(set, z);
  solver::CurveStates<D> st;
  st.specs = &set.curves();
  st.resize(static_cast<std::size_t>(set.n_curves()));
  std::vector<D> v;
  int flat = 0;
  for (int c = 0; c < set.n_curves(); ++c) {
    const std::vector<double>& zc = knots.at(static_cast<std::size_t>(c));
    v.clear();
    for (double x : zc) {
      v.push_back(flat == k ? D::variable(x, 0) : D(x));
      ++flat;
    }
    st.set(c, v.data(), v.size());
  }
  DfMemo<D, std::function<D(int, double)>> memo([&st](int slot, double t) -> D { return st.df(slot, t); });
  const StageABook<D> b = price_stage_a<D>(s, memo);
  std::vector<D> flatb(static_cast<std::size_t>(stage_a_oracle_outputs(s)));
  stage_a_flatten<D>(s, b, flatb.data());
  for (std::size_t o = 0; o < flatb.size(); ++o) out[o] = flatb[o].tangent(0);
}
// The record-point knot state, flattened in curve order: the centre of the oracle's ball.
inline std::vector<double> stage_a_flat_knots(const std::vector<std::vector<double>>& knots) {
  std::vector<double> z;
  for (const std::vector<double>& v : knots) z.insert(z.end(), v.begin(), v.end());
  return z;
}

}  // namespace epykos::fixtures
