// EpykosEngine — calibrating the M1 curve through the implicit node (M3/G4; a test-only
// fixture, not engine API): the 12 knot-tenor par swaps as calibration instruments, the one-tape
// composition quotes -> implicit -> price_book, and a synthetic two-curve set (projection on
// discounting) for the multi-curve dependency.
//
// Calibration swaps (synthetic, from the M1 calendar): knot k's instrument is a par swap of
// notional 1 maturing at the knot's tenor. Tenors of a year or more take the book's annual
// schedule (period ends round(365.25·j) days, j = 1..T; τ = days/360; t = days/365), so their
// discount factors are the book's own nodes and cse merges them (the ONE TAPE sharing of
// PROBLEM.md §5); the four sub-year tenors are single-period swaps over round(365·T) days. The
// par rate is the float leg over the annuity, both written the natural way (no telescoping:
// CLAUDE.md recording discipline) with the engine's coupon formulas. The residual of a par
// quote q is par(z) − q, so ∂F/∂q = −I.
//
// Two curves: "disc" is the M1 curve with the 12 par swaps; "proj" is a 6-knot projection curve
// (1, 2, 3, 5, 7, 10 years) calibrated to 6 synthetic basis swaps whose par spread is
// Σ τ_j (fwd_proj_j − fwd_disc_j)·DF_disc(t_j) / Σ τ_j DF_disc(t_j): every proj instrument reads
// the disc curve too (projection on discounting), which CurveSet discovers from the maths.
//
// Everything is templated on Scalar so the same maths runs on double (quotes, oracles), Rec
// (recording) and Dual (the forward-mode oracle of the IFT, solver/tangent.hpp). Header-only:
// the including TU's flags govern the arithmetic.
#pragma once

#include <cmath>
#include <cstddef>
#include <span>
#include <string>
#include <vector>

#include "epykos/fixtures/m1_book.hpp"
#include "epykos/fixtures/m1_price.hpp"
#include "epykos/maths/calendar.hpp"
#include "epykos/maths/curve/linear.hpp"
#include "epykos/maths/swap/ois.hpp"
#include "epykos/scalar/rec.hpp"
#include "epykos/solver/curve_set.hpp"
#include "epykos/solver/implicit.hpp"
#include "epykos/tape/passes.hpp"
#include "epykos/tape/tape.hpp"

namespace epykos::fixtures {

// A calibration swap's schedule: period j runs day[j] -> day[j+1], j in [0, n_periods).
struct CalSwap {
  double tenor = 0.0;
  int n_periods = 0;
  std::vector<int> days;    // n_periods + 1, days[0] = 0
  std::vector<double> tau;  // per period, ACT/360
  std::vector<double> t;    // n_periods + 1, discount times d/365
};

// Knot k's instrument (tenor knot_times[k]).
inline CalSwap m1_calibration_swap(int k) {
  CalSwap s;
  s.tenor = knot_times[static_cast<std::size_t>(k)];
  if (s.tenor >= 1.0) {
    s.n_periods = static_cast<int>(std::lround(s.tenor));
    s.days.resize(static_cast<std::size_t>(s.n_periods) + 1);
    calendar::annual_schedule(0, s.n_periods, s.days.data());
  } else {
    s.n_periods = 1;
    s.days = {0, static_cast<int>(std::lround(365.0 * s.tenor))};
  }
  for (int j = 0; j < s.n_periods; ++j) {
    s.tau.push_back(calendar::year_fraction(s.days[static_cast<std::size_t>(j)], s.days[static_cast<std::size_t>(j) + 1]));
  }
  for (int d : s.days) s.t.push_back(calendar::time_of_day(d));
  return s;
}

inline std::vector<CalSwap> m1_calibration_swaps() {
  std::vector<CalSwap> v;
  for (int k = 0; k < n_knots; ++k) v.push_back(m1_calibration_swap(k));
  return v;
}

// Σ_j τ_j·DF(t_j) and Σ_j τ_j·fwd_j·DF(t_j), fwd_j = (DF(t_{j−1})/DF(t_j) − 1)/τ_j, on the curve
// (knot_t, z, n); left folds, the engine's coupon formulas with notional 1.
template <class Scalar>
Scalar cal_annuity(const double* knot_t, const Scalar* z, int n, const CalSwap& s) {
  auto term = [&](int j) {
    const Scalar df_e = curve::linear::df(knot_t, z, n, s.t[static_cast<std::size_t>(j) + 1]);
    return ois::fixed_coupon_pv(1.0, s.tau[static_cast<std::size_t>(j)], 1.0, df_e);
  };
  Scalar acc = term(0);
  for (int j = 1; j < s.n_periods; ++j) acc = acc + term(j);
  return acc;
}

template <class Scalar>
Scalar cal_float_leg(const double* knot_t, const Scalar* z, int n, const CalSwap& s) {
  auto term = [&](int j) {
    const Scalar df_s = curve::linear::df(knot_t, z, n, s.t[static_cast<std::size_t>(j)]);
    const Scalar df_e = curve::linear::df(knot_t, z, n, s.t[static_cast<std::size_t>(j) + 1]);
    return ois::float_coupon_pv(1.0, s.tau[static_cast<std::size_t>(j)], df_s, df_e);
  };
  Scalar acc = term(0);
  for (int j = 1; j < s.n_periods; ++j) acc = acc + term(j);
  return acc;
}

// par = float leg / annuity.
template <class Scalar>
Scalar cal_par_rate(const double* knot_t, const Scalar* z, int n, const CalSwap& s) {
  return cal_float_leg(knot_t, z, n, s) / cal_annuity(knot_t, z, n, s);
}

// The M1 residual as a templated callable: F[i] = par_i(z) − q[i] over the 12 swaps.
template <class Scalar>
void m1_residual(const std::vector<CalSwap>& swaps, const Scalar* z, const Scalar* q, Scalar* F) {
  for (std::size_t i = 0; i < swaps.size(); ++i) {
    F[i] = cal_par_rate(knot_times.data(), z, n_knots, swaps[i]) - q[i];
  }
}

// The par quotes at z (the quotes whose calibration recovers z).
inline std::vector<double> m1_par_quotes(const double* z) {
  const std::vector<CalSwap> swaps = m1_calibration_swaps();
  std::vector<double> q(static_cast<std::size_t>(n_knots));
  for (int k = 0; k < n_knots; ++k) q[static_cast<std::size_t>(k)] = cal_par_rate(knot_times.data(), z, n_knots, swaps[static_cast<std::size_t>(k)]);
  return q;
}

// The M1 curve as a CurveSet with its 12 instruments; `start` is every solve's start point.
inline solver::CurveSet make_m1_curve_set(std::span<const double> start) {
  solver::CurveSet set;
  solver::CurveSpec spec;
  spec.name = "usd-ois";
  spec.knot_t.assign(knot_times.begin(), knot_times.end());
  spec.start.assign(start.begin(), start.end());
  const int c = set.add_curve(spec);
  const std::vector<CalSwap> swaps = m1_calibration_swaps();
  for (int k = 0; k < n_knots; ++k) {
    const CalSwap s = swaps[static_cast<std::size_t>(k)];
    set.add_instrument(c, "par-swap-" + std::to_string(k), [s](const auto& states, const auto& quote) {
      using Scalar = std::decay_t<decltype(quote)>;
      const auto& z = states.curve(0);
      return cal_par_rate<Scalar>(states.spec(0).knot_t.data(), z.data(), n_knots, s) - quote;
    });
  }
  return set;
}

// The one-tape composition quotes -> implicit -> price_book, with the passes.
struct CalibratedM1 {
  Tape tape;
  solver::ImplicitRegistry registry;
  std::vector<int> quote_inputs;      // tape input ordinals of the 12 quotes (the free state)
  int block = 0;
  std::vector<int> z_outputs;         // O1: the calibrated knots, tape output ordinals
  int jtr_output = -1;                // O1 diagnostics
  int iterations_output = -1;
  std::vector<int> residual_outputs;  // the 12 residuals
  int swap_pv_begin = -1;             // O2: swap PV i at swap_pv_begin + i
  int book_pv = -1;
  std::vector<double> z_record;       // the record-time solution
  solver::SolveReport record_report;
  std::vector<int> book_outputs() const {
    std::vector<int> v;
    for (int i = 0; i < n_swaps; ++i) v.push_back(swap_pv_begin + i);
    v.push_back(book_pv);
    return v;
  }
};

inline CalibratedM1 record_m1_calibrated(const Book& book, std::span<const double> quotes, std::span<const double> start,
                                         const solver::SolveOptions& options = {}, bool run_passes = true) {
  CalibratedM1 c;
  {
    Tape::Scope scope(c.tape);
    std::vector<Rec> q;
    for (double v : quotes) {
      c.quote_inputs.push_back(static_cast<int>(c.tape.num_inputs()));
      q.push_back(make_input(c.tape, v));
    }
    const solver::CurveSet set = make_m1_curve_set(start);
    const solver::CurveSet::Calibration cal = set.calibrate(c.tape, c.registry, q, solver::CurveSet::Mode::sequential, options);
    c.block = cal.results[0].block;
    c.record_report = cal.results[0].report;
    const std::vector<Rec>& z = cal.states.curve(0);
    for (const Rec& zk : z) {
      c.z_outputs.push_back(register_output(c.tape, zk));
      c.z_record.push_back(zk.v);
    }
    std::vector<Rec> swap_pv(static_cast<std::size_t>(book.n_swaps));
    Rec book_pv;
    price_book<Rec>(book, z.data(), swap_pv.data(), &book_pv);
    c.swap_pv_begin = static_cast<int>(c.tape.num_outputs());
    for (const Rec& pv : swap_pv) register_output(c.tape, pv);
    c.book_pv = register_output(c.tape, book_pv);
  }
  const solver::ImplicitBlock& b = c.registry.blocks[static_cast<std::size_t>(c.block)];
  c.residual_outputs = b.residuals;
  c.jtr_output = b.diag_jtr_output;
  c.iterations_output = b.diag_iterations_output;
  c.tape.validate();
  if (run_passes) {
    standard_passes(c.tape);
    c.tape.validate();
  }
  return c;
}

// ---------------------------------------------------------------------------------------------
// Two curves: disc (the M1 curve) and proj (6 knots) with basis swaps reading both.
// ---------------------------------------------------------------------------------------------

inline constexpr int n_proj_knots = 6;
inline constexpr std::array<double, n_proj_knots> proj_knot_times = {1.0, 2.0, 3.0, 5.0, 7.0, 10.0};
// The generating projection curve (synthetic): disc + a tenor basis of 15..40 bp.
inline constexpr std::array<double, n_proj_knots> proj_record_state = {0.0435, 0.0430, 0.0425, 0.0425, 0.0435, 0.0450};

// Basis swap spread: Σ τ_j (fwd_proj_j − fwd_disc_j)·DF_disc(t_j) / Σ τ_j DF_disc(t_j).
template <class Scalar>
Scalar basis_spread(const double* kt_d, const Scalar* zd, int nd, const double* kt_p, const Scalar* zp, int np, const CalSwap& s) {
  auto term = [&](int j) {
    const double tau = s.tau[static_cast<std::size_t>(j)];
    const Scalar dfd_s = curve::linear::df(kt_d, zd, nd, s.t[static_cast<std::size_t>(j)]);
    const Scalar dfd_e = curve::linear::df(kt_d, zd, nd, s.t[static_cast<std::size_t>(j) + 1]);
    const Scalar dfp_s = curve::linear::df(kt_p, zp, np, s.t[static_cast<std::size_t>(j)]);
    const Scalar dfp_e = curve::linear::df(kt_p, zp, np, s.t[static_cast<std::size_t>(j) + 1]);
    const Scalar fwd_d = (dfd_s / dfd_e - 1.0) / tau;
    const Scalar fwd_p = (dfp_s / dfp_e - 1.0) / tau;
    return tau * (fwd_p - fwd_d) * dfd_e;
  };
  Scalar acc = term(0);
  for (int j = 1; j < s.n_periods; ++j) acc = acc + term(j);
  return acc / cal_annuity(kt_d, zd, nd, s);
}

// The basis swaps: one per proj knot, maturing at the knot (annual schedule).
inline std::vector<CalSwap> proj_calibration_swaps() {
  std::vector<CalSwap> v;
  for (double T : proj_knot_times) {
    CalSwap s;
    s.tenor = T;
    s.n_periods = static_cast<int>(std::lround(T));
    s.days.resize(static_cast<std::size_t>(s.n_periods) + 1);
    calendar::annual_schedule(0, s.n_periods, s.days.data());
    for (int j = 0; j < s.n_periods; ++j) s.tau.push_back(calendar::year_fraction(s.days[static_cast<std::size_t>(j)], s.days[static_cast<std::size_t>(j) + 1]));
    for (int d : s.days) s.t.push_back(calendar::time_of_day(d));
    v.push_back(s);
  }
  return v;
}

// Curve 0 "disc" (12 M1 instruments), curve 1 "proj" (6 basis instruments reading both).
inline solver::CurveSet make_two_curve_set(std::span<const double> disc_start, std::span<const double> proj_start) {
  solver::CurveSet set = make_m1_curve_set(disc_start);
  solver::CurveSpec spec;
  spec.name = "proj";
  spec.knot_t.assign(proj_knot_times.begin(), proj_knot_times.end());
  spec.start.assign(proj_start.begin(), proj_start.end());
  const int c = set.add_curve(spec);
  const std::vector<CalSwap> swaps = proj_calibration_swaps();
  for (int k = 0; k < n_proj_knots; ++k) {
    const CalSwap s = swaps[static_cast<std::size_t>(k)];
    set.add_instrument(c, "basis-swap-" + std::to_string(k), [s](const auto& states, const auto& quote) {
      using Scalar = std::decay_t<decltype(quote)>;
      const auto& zd = states.curve(0);
      const auto& zp = states.curve(1);
      return basis_spread<Scalar>(states.spec(0).knot_t.data(), zd.data(), n_knots, states.spec(1).knot_t.data(), zp.data(), n_proj_knots, s) - quote;
    });
  }
  return set;
}

// The 18 quotes (12 par rates, 6 basis spreads) generated at (zd, zp): instrument order.
inline std::vector<double> two_curve_quotes(const double* zd, const double* zp) {
  std::vector<double> q = m1_par_quotes(zd);
  const std::vector<CalSwap> swaps = proj_calibration_swaps();
  for (int k = 0; k < n_proj_knots; ++k) {
    q.push_back(basis_spread<double>(knot_times.data(), zd, n_knots, proj_knot_times.data(), zp, n_proj_knots, swaps[static_cast<std::size_t>(k)]));
  }
  return q;
}

// A small book priced off both curves: for T in {2, 5, 10} the PV of a basis swap paying the
// proj forward against the disc forward plus a fixed spread (quote + 5 bp) on notional 1e6, and
// for T in {3, 7} the PV of a disc swap receiving K = quote + 10 bp; 5 outputs.
template <class Scalar>
void two_curve_book(const std::vector<CalSwap>& disc_swaps, const std::vector<CalSwap>& proj_swaps, const double* quotes,
                    const Scalar* zd, const Scalar* zp, Scalar* out) {
  const double N = 1.0e6;
  int o = 0;
  for (int k : {1, 3, 5}) {  // proj knots 2, 5, 10 years
    const CalSwap& s = proj_swaps[static_cast<std::size_t>(k)];
    const double spread = quotes[static_cast<std::size_t>(n_knots + k)] + 0.0005;
    auto term = [&](int j) {
      const double tau = s.tau[static_cast<std::size_t>(j)];
      const Scalar dfd_s = curve::linear::df(knot_times.data(), zd, n_knots, s.t[static_cast<std::size_t>(j)]);
      const Scalar dfd_e = curve::linear::df(knot_times.data(), zd, n_knots, s.t[static_cast<std::size_t>(j) + 1]);
      const Scalar dfp_s = curve::linear::df(proj_knot_times.data(), zp, n_proj_knots, s.t[static_cast<std::size_t>(j)]);
      const Scalar dfp_e = curve::linear::df(proj_knot_times.data(), zp, n_proj_knots, s.t[static_cast<std::size_t>(j) + 1]);
      const Scalar fwd_d = (dfd_s / dfd_e - 1.0) / tau;
      const Scalar fwd_p = (dfp_s / dfp_e - 1.0) / tau;
      return N * tau * (fwd_p - fwd_d - spread) * dfd_e;
    };
    Scalar acc = term(0);
    for (int j = 1; j < s.n_periods; ++j) acc = acc + term(j);
    out[o++] = acc;
  }
  for (int k : {6, 8}) {  // disc knots 3, 7 years
    const CalSwap& s = disc_swaps[static_cast<std::size_t>(k)];
    const double K = quotes[static_cast<std::size_t>(k)] + 0.0010;
    auto term = [&](int j) {
      const double tau = s.tau[static_cast<std::size_t>(j)];
      const Scalar df_s = curve::linear::df(knot_times.data(), zd, n_knots, s.t[static_cast<std::size_t>(j)]);
      const Scalar df_e = curve::linear::df(knot_times.data(), zd, n_knots, s.t[static_cast<std::size_t>(j) + 1]);
      return ois::fixed_coupon_pv(N, tau, K, df_e) - ois::float_coupon_pv(N, tau, df_s, df_e);
    };
    Scalar acc = term(0);
    for (int j = 1; j < s.n_periods; ++j) acc = acc + term(j);
    out[o++] = acc;
  }
}
inline constexpr int n_two_curve_book_outputs = 5;

struct CalibratedTwoCurves {
  Tape tape;
  solver::ImplicitRegistry registry;
  std::vector<int> quote_inputs;        // 18
  std::vector<int> zd_outputs, zp_outputs;
  std::vector<int> book_outputs;        // 5
  std::vector<std::vector<int>> block_curves;
  std::vector<solver::SolveReport> record_reports;
  std::vector<double> zd_record, zp_record;
};

inline CalibratedTwoCurves record_two_curves(std::span<const double> quotes, std::span<const double> disc_start, std::span<const double> proj_start,
                                             solver::CurveSet::Mode mode, const solver::SolveOptions& options = {}, bool run_passes = true) {
  CalibratedTwoCurves c;
  {
    Tape::Scope scope(c.tape);
    std::vector<Rec> q;
    for (double v : quotes) {
      c.quote_inputs.push_back(static_cast<int>(c.tape.num_inputs()));
      q.push_back(make_input(c.tape, v));
    }
    const solver::CurveSet set = make_two_curve_set(disc_start, proj_start);
    const solver::CurveSet::Calibration cal = set.calibrate(c.tape, c.registry, q, mode, options);
    c.block_curves = cal.block_curves;
    for (const solver::ImplicitResult& r : cal.results) c.record_reports.push_back(r.report);
    const std::vector<Rec>& zd = cal.states.curve(0);
    const std::vector<Rec>& zp = cal.states.curve(1);
    for (const Rec& z : zd) {
      c.zd_outputs.push_back(register_output(c.tape, z));
      c.zd_record.push_back(z.v);
    }
    for (const Rec& z : zp) {
      c.zp_outputs.push_back(register_output(c.tape, z));
      c.zp_record.push_back(z.v);
    }
    const std::vector<CalSwap> ds = m1_calibration_swaps();
    const std::vector<CalSwap> ps = proj_calibration_swaps();
    std::vector<double> qv(quotes.begin(), quotes.end());
    Rec out[n_two_curve_book_outputs];
    two_curve_book<Rec>(ds, ps, qv.data(), zd.data(), zp.data(), out);
    for (const Rec& o : out) c.book_outputs.push_back(register_output(c.tape, o));
  }
  c.tape.validate();
  if (run_passes) {
    standard_passes(c.tape);
    c.tape.validate();
  }
  return c;
}

}  // namespace epykos::fixtures
