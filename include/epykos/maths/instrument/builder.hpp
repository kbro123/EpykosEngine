// EpykosEngine — from a trade (or a curve definition) to the row tables (M3/G2).
//
// The builder is the one place where conventions become numbers: it takes the registry
// (calendars, day counts, lags, observation windows, index definitions), a fixings history, a
// valuation date and a slot per curve, and resolves a trade of a blueprint into the
// tables.hpp rows the templated maths prices from. Structure only: nothing here is a Scalar.
//
//   Trade            what the population supplies: blueprint, dates or tenor, notional, side,
//                    rate / spread / traded price (each optional: the blueprint's default applies)
//   CurveSlots       index name -> curve slot: the discount curve of a currency is the slot of
//                    its discount index, the projection curve of a float leg the slot of its
//                    index. The caller assigns slots in the order of its curve objects (a
//                    solver::CurveSet's curve indices, a vector of Composite states)
//   BuildContext     registry + fixings + valuation date + slots
//   build_instrument the tables of one trade
//   CalibrationSet   a curve definition resolved for the valuation date: knot times, the
//                    calibration instruments in maturity order with their quote keys, and the
//                    regions of the curve (maths/curve/composite.hpp) — what G5 hands to the
//                    solver, one residual per instrument (instrument.hpp residual)
//
// Rules applied (all structure, all reported in the rows):
//   * cash flows paid on or before the valuation date are dropped (pay <= valuation);
//   * an RFR coupon's observation days whose rate is in the history form the realised factor
//     and weighted sum; the rest are projected — a projected day dated before the valuation
//     date is a missing fixing and an error;
//   * a term-rate coupon is realised when its fixing date's rate is in the history; a fixing
//     date before the valuation date without a rate is an error; the projected forward runs
//     over the accrual period with the index's day count (a stub's forward is not interpolated
//     between index tenors: a stated simplification);
//   * accrued interest of a period that started before the valuation date is the rate known so
//     far (the fixed rate, the realised fixing, the realised compounded / averaged rate) times
//     the accrual from the start to the valuation date;
//   * curve times are (date − valuation) / 365 (ACT/365F).
#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "epykos/conventions/date.hpp"
#include "epykos/conventions/daycount.hpp"
#include "epykos/conventions/fixings.hpp"
#include "epykos/conventions/registry.hpp"
#include "epykos/maths/curve/composite.hpp"
#include "epykos/maths/instrument/blueprint.hpp"
#include "epykos/maths/instrument/tables.hpp"

namespace epykos::instrument {

struct Trade {
  std::string id;                              // empty: "<blueprint> <tenor>" / the contract code
  std::string blueprint;
  conventions::Date trade_date = 0;            // 0: the valuation date
  conventions::Date effective = 0;             // 0: the blueprint's start (spot of the trade date)
  std::optional<conventions::Period> tenor;    // else the blueprint's
  conventions::Date termination = 0;           // 0: effective + tenor
  std::optional<double> notional;
  std::optional<int> side;                     // +1 / −1
  std::optional<double> fixed_rate;            // Ois / Irs fixed leg, Deposit rate
  std::optional<double> spread;                // Basis: the spread leg's spread
  std::optional<double> traded_price;          // Future
  int contract = 0;                            // Future: the n-th contract after the valuation date (0-based)
  std::string contract_code;                   // Future: "Z26" or "SR3 Z26" (overrides `contract`)
};

class CurveSlots {
 public:
  // Adds an index name; returns its slot (the next integer). A duplicate throws std::invalid_argument.
  int add(const std::string& index);
  int slot(const std::string& index) const;   // throws std::invalid_argument when unknown
  bool has(const std::string& index) const noexcept { return slots_.count(index) != 0; }
  int size() const noexcept { return static_cast<int>(names_.size()); }
  const std::vector<std::string>& names() const noexcept { return names_; }

 private:
  std::map<std::string, int> slots_;
  std::vector<std::string> names_;
};

struct BuildContext {
  const conventions::Registry* registry = nullptr;
  const conventions::FixingsHistory* fixings = nullptr;
  const CurveSlots* curves = nullptr;
  conventions::Date valuation = 0;

  double time_of(conventions::Date d) const noexcept { return static_cast<double>(d - valuation) / 365.0; }
};

// The tables of one trade. Throws BlueprintError / DateError / RegistryError on an inconsistency
// (a missing fixing, a matured trade, a blueprint the convention cannot record).
Instrument build_instrument(const BuildContext& ctx, const Blueprint& blueprint, const Trade& trade);
Instrument build_instrument(const BuildContext& ctx, const Blueprints& blueprints, const Trade& trade);

struct CalibrationInstrument {
  std::string key;                   // the quote key
  std::string tenor;                 // the tenor string, or the contract code
  Instrument instrument;
};

struct CalibrationSet {
  std::string curve;                 // the definition's name
  std::string index;
  int slot = -1;                     // the curve's slot (CurveSlots)
  std::vector<double> knot_t;        // ascending
  std::vector<curve::RegionSpec> regions;
  std::vector<CalibrationInstrument> instruments;   // in maturity order

  int n_instruments() const noexcept { return static_cast<int>(instruments.size()); }
  std::vector<std::string> keys() const;
  // The curve object of the definition on these knots (a single-region definition is a
  // one-region Composite, bitwise the Curve<S, V> template, D38).
  curve::Composite composite() const { return curve::Composite(knot_t, regions); }
};

// Throws when two instruments share a maturity time or the knots are not increasing.
CalibrationSet build_calibration_set(const BuildContext& ctx, const Blueprints& blueprints, const CurveDefinition& def);

// The trades a calibration set stands for (notional 1, side +1, rate 0): for inspection.
std::vector<Trade> calibration_trades(const CurveDefinition& def, const Blueprints& blueprints, const BuildContext& ctx);

}  // namespace epykos::instrument
