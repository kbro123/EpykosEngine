// EpykosEngine — instrument blueprints and curve definitions as DATA (M3/G2; D36).
//
// An instrument blueprint names a convention of the registry (conventions/registry.hpp) and
// says which coupon kind each leg records with (its parameters may override the convention's:
// an observation shift on a trade population variant), plus default trade fields. A trade of
// the population (builder.hpp Trade) names a blueprint and supplies notional, side, dates,
// tenor, rate or spread; the builder resolves schedules, day counts, lags, observation windows
// and realised fixings from the registry into the row tables of tables.hpp. A trade of a known
// coupon kind therefore needs no C++: a new convention plus a new blueprint is two JSON edits
// (tests/instrument/blueprint_test.cpp adds a quarterly-fixed OIS purely as data and prices it).
//
// A curve definition names the index the curve projects, its scheme and interpolation variable
// (one region, or a list of regions with a scheme and variable each: maths/curve/composite.hpp),
// its knots (the calibration instruments' maturities, or explicit tenors) and its calibration
// instrument set: blueprints with tenor lists, or the front n futures contracts, each with a
// quote key. builder.hpp turns a definition into a CalibrationSet (knot times, instruments,
// keys) for solver/curve_set.hpp.
//
// Files: blueprints/instruments/*.json and blueprints/curves/*.json, read by the strict in-house
// JSON reader (util/json.hpp). Schema (unknown keys and missing fields fail with file:line:column):
//
//   { "meta": { "title", "description"?, "sources"?: [..], "notes"?: [..] },
//     "blueprints": { NAME: { "convention", "description"?, "legs"?: [ LEG, LEG ],
//                             "notional"?, "side"?: "receive" | "pay" | "long" | "short",
//                             "tenor"?, "start"?: "spot" | "YYYY-MM-DD", "fixed_rate"?, "spread"?,
//                             "traded_price"?, "sources"? } },
//       LEG = { "coupon": "fixed" | "rfr_compounded" | "rfr_averaged" | "term_rate",
//               "observation"?: { "method", "lookback_days"?, "lockout_days"? }, "spread"?: bool }
//     "curves": { NAME: { "index", "currency"?, "description"?,
//                         "scheme"?, "variable"?  |  "regions"?: [ { "from"?, "scheme", "variable" } ],
//                         "knots"?: "maturities" | [ tenor... ],
//                         "instruments": [ { "blueprint", "tenors": [..] } | { "blueprint", "tenor", "key"? }
//                                          | { "blueprint", "contracts": n } ],
//                         "sources"? } } }
//
// Everything here is structure: strings, dates, doubles.
#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "epykos/conventions/daycount.hpp"
#include "epykos/conventions/registry.hpp"
#include "epykos/conventions/rfr.hpp"
#include "epykos/maths/curve/composite.hpp"
#include "epykos/maths/instrument/tables.hpp"
#include "epykos/util/json.hpp"

namespace epykos::instrument {

class BlueprintError : public std::runtime_error {
 public:
  explicit BlueprintError(const std::string& what) : std::runtime_error(what) {}
};

struct LegBlueprint {
  CouponKind coupon = CouponKind::Fixed;
  std::optional<conventions::ObservationSpec> observation;   // RFR coupons: overrides the convention's
  std::optional<bool> spread;                                 // overrides the convention's spread flag
};

struct Blueprint {
  std::string name;
  std::string convention;            // registry instrument name
  std::string description;
  std::vector<LegBlueprint> legs;    // empty: every leg's kind derives from the convention
  // trade defaults
  double notional = 1.0;
  int side = +1;                     // +1 receive legs[0] / lend / long
  std::optional<conventions::Period> tenor;
  std::string start = "spot";        // "spot" or an ISO date
  double fixed_rate = 0.0;
  double spread = 0.0;
  double traded_price = 0.0;
  conventions::Sources sources;
};

struct CurveRegionDef {
  std::string from;                  // tenor of the boundary (empty for the first region: t = 0)
  curve::SchemeKind scheme = curve::SchemeKind::linear;
  curve::Variable variable = curve::Variable::zero;
};

struct CurveInstrumentEntry {
  std::string blueprint;
  std::vector<std::string> tenors;   // one instrument per tenor
  std::vector<std::string> keys;     // parallel to tenors when given (else "<blueprint> <tenor>")
  int contracts = 0;                 // futures: the front n contracts after the valuation date
};

struct CurveDefinition {
  std::string name;
  std::string index;                 // the index the curve projects (its discount factors)
  std::string currency;
  std::string description;
  std::vector<CurveRegionDef> regions;         // at least one
  std::vector<std::string> knot_tenors;        // empty: the instruments' maturities
  std::vector<CurveInstrumentEntry> instruments;
  conventions::Sources sources;
};

struct BlueprintFileMeta {
  std::string path;
  std::string title;
  std::string description;
  std::vector<std::string> sources;
  std::vector<std::string> notes;
};

class Blueprints {
 public:
  Blueprints() = default;

  // The blueprints root: $EPYKOS_BLUEPRINTS if set, else the source tree's blueprints/
  // compiled in at build time (the registry's parent directory).
  static std::string default_root();
  // Loads every *.json under <root>/instruments and <root>/curves (name order).
  static Blueprints load(const std::string& root = default_root());
  static Blueprints load_files(const std::vector<std::string>& paths);

  void add_file(const std::string& path);
  void add_document(const json::Value& doc, const std::string& origin);

  const std::vector<BlueprintFileMeta>& files() const noexcept { return files_; }
  const Blueprint& blueprint(const std::string& name) const;
  const CurveDefinition& curve(const std::string& name) const;
  bool has_blueprint(const std::string& n) const noexcept { return blueprints_.count(n) != 0; }
  bool has_curve(const std::string& n) const noexcept { return curves_.count(n) != 0; }
  std::vector<std::string> blueprint_names() const;
  std::vector<std::string> curve_names() const;

  // Checks every blueprint against a registry (its convention exists, its legs match the
  // convention's legs) and every curve definition against the blueprints and the registry.
  void validate(const conventions::Registry& registry) const;

 private:
  std::vector<BlueprintFileMeta> files_;
  std::map<std::string, Blueprint> blueprints_;
  std::map<std::string, CurveDefinition> curves_;
};

// The coupon kind a convention leg records with when the blueprint does not say: fixed -> Fixed;
// an overnight index compounded -> RfrCompounded, averaged -> RfrAveraged; a term index ->
// TermRate. Throws BlueprintError for a combination the mechanics do not cover.
CouponKind default_coupon_kind(const conventions::Registry& registry, const conventions::LegConvention& leg);

}  // namespace epykos::instrument
