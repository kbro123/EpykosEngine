// EpykosEngine — a calibration set as the instruments of a solver::CurveSet (M3/G2 -> G4 -> G5).
//
// A curve definition resolved by builder.hpp (a CalibrationSet: knot times, regions,
// instruments in maturity order, quote keys) becomes one curve of a CurveSet with one residual
// per instrument:
//
//     residual(states, q) = instrument::residual<Scalar>(inst, df, q),  df(slot, t) = states.df(slot, t)
//
// The curve slots of the tables ARE the CurveSet's curve indices: add the curves to the set in
// slot order (add_calibration_set checks that the set's next curve index is the set's slot).
// Which other curves an instrument reads (a basis swap reads the 6M and the €STR curves) is
// discovered by the CurveSet from the maths (D40.6), so the solve order follows from the
// blueprints alone. Quotes are given to calibrate() in instrument order per curve, i.e. in the
// order of CalibrationSet::keys() curve after curve.
//
// The curve's scheme is the definition's (M3/G5): the CurveSet builds a curve::Composite from
// the set's regions (a one-region {linear, zero} definition is bitwise the M1 linear zero-rate
// curve, D38), so the same instruments calibrate a log-DF, a monotone cubic or a composite
// curve — the instruments do not care which curve object answers df(slot, t). The knot values
// (the unknowns, O1) are in each region's variable; start_values() gives a flat start in it.
#pragma once

#include <cstddef>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "epykos/maths/curve/curve.hpp"
#include "epykos/maths/instrument/builder.hpp"
#include "epykos/maths/instrument/instrument.hpp"
#include "epykos/solver/curve_set.hpp"

namespace epykos::instrument {

// The start of every solve for a set: the flat continuously compounded zero rate `flat_zero`
// expressed in each knot's own variable (zero: the rate; logdf: −rate·t_k; forward: the rate).
inline std::vector<double> start_values(const CalibrationSet& cs, double flat_zero) {
  std::vector<double> start;
  start.reserve(cs.knot_t.size());
  const curve::Composite comp = cs.composite();
  for (std::size_t k = 0; k < cs.knot_t.size(); ++k) {
    const double t = cs.knot_t[k];
    const curve::Variable v = comp.region(comp.region_of(t)).spec.variable;
    start.push_back(v == curve::Variable::logdf ? -flat_zero * t : flat_zero);
  }
  return start;
}

// Adds the set's curve (knots, regions, `start` per knot in the knot's variable) and its
// instruments to `set`. Returns the curve index (== cs.slot). Throws std::invalid_argument when
// the set's next curve index is not the slot or `start` has the wrong length.
inline int add_calibration_set(solver::CurveSet& set, const CalibrationSet& cs, std::span<const double> start) {
  if (set.n_curves() != cs.slot) {
    throw std::invalid_argument("add_calibration_set: curve '" + cs.curve + "' has slot " + std::to_string(cs.slot) + " but the set's next curve index is " +
                                std::to_string(set.n_curves()) + " (add the curves in slot order)");
  }
  if (start.size() != cs.knot_t.size()) {
    throw std::invalid_argument("add_calibration_set: curve '" + cs.curve + "': " + std::to_string(start.size()) + " start values for " +
                                std::to_string(cs.knot_t.size()) + " knots");
  }
  solver::CurveSpec spec;
  spec.name = cs.curve;
  spec.knot_t = cs.knot_t;
  spec.start.assign(start.begin(), start.end());
  spec.regions = cs.regions;
  const int c = set.add_curve(spec);
  for (const CalibrationInstrument& ci : cs.instruments) {
    const Instrument inst = ci.instrument;
    set.add_instrument(c, ci.key, [inst](const auto& states, const auto& q) {
      using Scalar = std::decay_t<decltype(q)>;
      return residual<Scalar>(inst, [&states](int slot, double t) { return states.df(slot, t); }, q);
    });
  }
  return c;
}

// The par quotes of a set on double curves (the quotes whose calibration recovers those curves):
// one per instrument, in the set's order.
template <class Curves>
std::vector<double> par_quotes(const CalibrationSet& cs, Curves&& df) {
  std::vector<double> q;
  for (const CalibrationInstrument& ci : cs.instruments) q.push_back(par<double>(ci.instrument, df));
  return q;
}

}  // namespace epykos::instrument
