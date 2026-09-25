// Shared helpers for tests/instrument/*_test.cpp: the registry and blueprints loaded once, a
// SYNTHETIC fixings history (seeded, labelled; never data), curve slots for the four Stage A
// indices and flat / linear-zero curve callables of the shape the maths takes.
#pragma once

#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

#include "epykos/conventions/fixings.hpp"
#include "epykos/conventions/registry.hpp"
#include "epykos/maths/curve/linear.hpp"
#include "epykos/maths/instrument/blueprint.hpp"
#include "epykos/maths/instrument/builder.hpp"
#include "epykos/maths/instrument/instrument.hpp"

namespace epykos::test {

inline const conventions::Registry& registry() {
  static const conventions::Registry r = conventions::Registry::load();
  return r;
}

inline const instrument::Blueprints& blueprints() {
  static const instrument::Blueprints b = [] {
    instrument::Blueprints x = instrument::Blueprints::load();
    x.validate(registry());
    return x;
  }();
  return b;
}

// Slots 0..3: USD-SOFR, EUR-ESTR, EUR-EURIBOR-3M, EUR-EURIBOR-6M.
inline const instrument::CurveSlots& slots() {
  static const instrument::CurveSlots s = [] {
    instrument::CurveSlots x;
    x.add("USD-SOFR");
    x.add("EUR-ESTR");
    x.add("EUR-EURIBOR-3M");
    x.add("EUR-EURIBOR-6M");
    return x;
  }();
  return s;
}
inline constexpr int n_slots = 4;

// SYNTHETIC fixings for every index from 2025-01-01 to the day before `valuation` (so that a
// rate FOR the valuation date is never realised), levels 4% USD / 2% EUR, seed 7.
inline conventions::FixingsHistory synthetic_history(conventions::Date valuation) {
  conventions::FixingsHistory h;
  const conventions::Registry& reg = registry();
  const conventions::Date from = conventions::parse_iso("2025-01-01");
  conventions::SyntheticFixingsSpec spec;
  spec.seed = 7;
  spec.level = 0.04;
  conventions::synthetic_fixings(h, "USD-SOFR", reg.calendar("USD-SOFR"), from, valuation - 1, spec);
  spec.level = 0.02;
  spec.seed = 8;
  conventions::synthetic_fixings(h, "EUR-ESTR", reg.calendar("EUR-TARGET"), from, valuation - 1, spec);
  spec.level = 0.022;
  spec.seed = 9;
  conventions::synthetic_fixings(h, "EUR-EURIBOR-3M", reg.calendar("EUR-TARGET"), from, valuation - 1, spec);
  spec.level = 0.025;
  spec.seed = 10;
  conventions::synthetic_fixings(h, "EUR-EURIBOR-6M", reg.calendar("EUR-TARGET"), from, valuation - 1, spec);
  return h;
}

// A build context over the shared registry / blueprints / slots; owns its history.
struct Context {
  conventions::FixingsHistory fixings;
  instrument::BuildContext ctx;
  explicit Context(conventions::Date valuation) : fixings(synthetic_history(valuation)) {
    ctx.registry = &registry();
    ctx.fixings = &fixings;
    ctx.curves = &slots();
    ctx.valuation = valuation;
  }
  explicit Context(conventions::Date valuation, conventions::FixingsHistory history) : fixings(std::move(history)) {
    ctx.registry = &registry();
    ctx.fixings = &fixings;
    ctx.curves = &slots();
    ctx.valuation = valuation;
  }
  instrument::Instrument build(const instrument::Trade& t) const { return instrument::build_instrument(ctx, blueprints(), t); }
  instrument::Instrument build(const instrument::Blueprint& b, const instrument::Trade& t) const {
    return instrument::build_instrument(ctx, b, t);
  }
};

inline conventions::Date date(const char* iso) { return conventions::parse_iso(iso); }

// Flat continuously compounded curves: DF(t) = exp(−z_slot · t).
struct FlatCurves {
  double z[n_slots] = {0.04, 0.02, 0.022, 0.025};
  double operator()(int slot, double t) const { return std::exp(-z[slot] * t); }
};

// Linear-zero curves on a common knot grid, one state vector per slot (Scalar-templated: the
// recording tests use it on Rec / Dual).
inline const std::vector<double>& test_knots() {
  static const std::vector<double> k = {0.25, 0.5, 1.0, 2.0, 3.0, 5.0, 7.0, 10.0, 15.0, 20.0, 30.0};
  return k;
}
inline constexpr int n_test_knots = 11;

template <class Scalar>
struct LinearCurves {
  const Scalar* state = nullptr;   // n_slots × n_test_knots, slot-major
  Scalar operator()(int slot, double t) const {
    return curve::linear::df(test_knots().data(), state + static_cast<std::size_t>(slot) * static_cast<std::size_t>(n_test_knots), n_test_knots, t);
  }
};

// The generating state: slot s's zero curve rises from z_s to z_s + 50 bp over the grid.
inline std::vector<double> test_state() {
  const double base[n_slots] = {0.038, 0.019, 0.021, 0.024};
  std::vector<double> z;
  for (int s = 0; s < n_slots; ++s) {
    for (int k = 0; k < n_test_knots; ++k) z.push_back(base[s] + 0.005 * static_cast<double>(k) / static_cast<double>(n_test_knots - 1));
  }
  return z;
}

}  // namespace epykos::test
