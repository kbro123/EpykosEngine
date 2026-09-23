// EpykosEngine — a calibration set as the instruments of a solver::CurveSet (M3/G2 -> G4).
//
// A curve definition resolved by builder.hpp (a CalibrationSet: knot times, instruments in
// maturity order, quote keys) becomes one curve of a CurveSet with one residual per instrument:
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
// The CurveSet's scheme is the linear zero-rate one (its CurveStates::df); a definition on
// another scheme or variable keeps its regions in CalibrationSet::regions for a Composite-aware
// solver (G5): the instruments do not care which curve object answers df(slot, t).
#pragma once

#include <cstddef>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "epykos/maths/instrument/builder.hpp"
#include "epykos/maths/instrument/instrument.hpp"
#include "epykos/solver/curve_set.hpp"

namespace epykos::instrument {

// Adds the set's curve (knots, `start` per knot) and its instruments to `set`. Returns the
// curve index (== cs.slot). Throws std::invalid_argument when the set's next curve index is
// not the slot or `start` has the wrong length.
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
