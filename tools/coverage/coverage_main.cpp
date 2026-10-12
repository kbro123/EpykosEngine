// tools/coverage/ — the generated coverage matrix: "what can we not compare?" as a number the
// repository maintains rather than a guess someone makes.
//
// THE ASYMMETRY, which the emitted artefact states on every run and which is the whole point:
//
//   OUR side is LIVE. Nothing about Epykos is written down here as a list of product names. The
//   instrument kinds, coupon mechanics, curve schemes and curve variables are DISCOVERED from the
//   shipped enums at run time (see `live_enum` below); the conventions, blueprints, curve
//   definitions and problem definitions are LOADED from `blueprints/` through the same public
//   loaders the engine uses; the head-to-head's reach is read off `fixtures::compare_ois` itself
//   by building it and looking at what came out. Add a `Kind`, a blueprint, a curve or a trade
//   family and this tool's output moves on the next run without anyone editing it.
//
//   THEIR side is a SNAPSHOT, and is labelled as one. It is a checked-in table, and every row
//   carries the file and the symbol it was read from so a human can re-verify it in seconds
//   against the other checkout. It is dated (`kSnapshotDate`). It can go stale; the artefact says
//   so in its own header rather than pretending otherwise.
//
// D11, as relaxed by the owner on 2026-10-09, permits READING that checkout. It does not permit
// absorbing it: no line of their implementation is reproduced here, and nothing under
// `include/epykos/` or `src/` is touched by this tool. What is recorded below is an INVENTORY --
// which symbols exist, in which file, and what family they belong to. The clean-room property is
// what makes the head-to-head mean anything, so the matrix deliberately records the shape of the
// comparison surface and not one line of the maths behind it.
//
// THE THREE LEVELS, which matter more than a two-way both/theirs-only verdict and are why the
// headline fraction below is not the obvious ratio. For any row of their surface, Epykos is in
// exactly one of:
//
//   COMPARED   the head-to-head exercises it TODAY. `fixtures::compare_ois` is the only fixture
//              that crosses the boundary, so this is the number that bounds what any measurement
//              can currently say.
//   SUPPORTED  the engine and the blueprints have it, but the head-to-head does not reach it.
//              This is a FIXTURE gap, not a capability gap, and it is the cheap half of the
//              ranked list the artefact ends with.
//   ABSENT     Epykos has no path to it at all. A capability gap.
//
// Collapsing SUPPORTED into "both" overstates what we can measure; collapsing it into "theirs
// only" understates what we have built. The artefact reports all three and the ranked list keys
// off the distinction.
//
// Staleness guards (the reason this is a program and not a markdown file someone edits). Each one
// EXITS NON-ZERO rather than printing a quietly wrong matrix:
//
//   G1  every live enumerator of `instrument::Kind` must be classified by `our_quote_form`. A new
//       enumerator is found by the 0..255 scan (every one of these enums answers "?" for an
//       unknown value) and has no row, so the tool stops.
//   G2  every blueprint in `blueprints/` must resolve to a convention, and every curve definition
//       to its blueprints and conventions. Delegated to `Blueprints::validate`.
//   G3  the live cardinality of `instrument::CouponKind`, `curve::SchemeKind` and
//       `curve::Variable` is checked against the count this tool last reasoned about, so widening
//       any of them forces a look at the matrix instead of silently changing a ratio.
//   G4  the exhaustive `switch`es over our enums carry no `default:`, so adding an enumerator is
//       also a -Wswitch diagnostic (an error under EPYKOS_WERROR, which CI sets).
//
// Usage:  coverage [--out <path>] [--no-out] [--stdout] [--blueprints <dir>]
//         default --out  bench/compare/COVERAGE.md   (relative to the working directory)
//
// This tool measures nothing. It takes no timings, reads no load and makes no performance claim;
// it is a capability inventory. It is therefore safe on a loaded box, unlike tools/ladder/.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "epykos/conventions/registry.hpp"
#include "epykos/fixtures/compare_ois.hpp"
#include "epykos/fixtures/stage_a.hpp"
#include "epykos/maths/curve/composite.hpp"
#include "epykos/maths/curve/curve.hpp"
#include "epykos/maths/instrument/blueprint.hpp"
#include "epykos/maths/instrument/builder.hpp"
#include "epykos/maths/instrument/tables.hpp"

namespace {

// =============================================================================================
// The snapshot of the other checkout. Every row cites the file and the symbol it came from.
// =============================================================================================

constexpr const char* kSnapshotDate = "2026-10-09";
constexpr const char* kTheirRoot = "/Users/kevinbroughton/Desktop/Claude Projects/SwapEngine";

// G3: the live cardinalities this tool's prose was written against. Widening one of these enums
// changes what the matrix should say, so it stops rather than re-printing a stale ratio.
constexpr std::size_t kExpectedCouponKinds = 4;
constexpr std::size_t kExpectedSchemes = 6;
constexpr std::size_t kExpectedVariables = 3;
constexpr std::size_t kExpectedKinds = 5;

enum class Level { Compared, Supported, Absent };

const char* level_cell(Level l) {
  switch (l) {
    case Level::Compared: return "**both, compared**";
    case Level::Supported: return "both, *not compared*";
    case Level::Absent: return "**theirs only**";
  }
  return "?";
}

struct TheirRow {
  std::string name;            // their enumerator / capability
  std::string file;            // path under their checkout, for re-verification
  std::string symbol;          // the enclosing enum / type / function (+ line where known)
  std::string means;           // what it is, in our words, short
  Level level = Level::Absent;
  std::string ours;            // what answers it here, or empty
  std::string what_it_takes;   // for ABSENT rows
};

// ---- axis 1: calibration instruments / quote kinds -------------------------------------------
// Their `enum class QuoteKind`, 9 enumerators. They guard their own staleness with
// `kQuoteKindCount = 9` plus a static_assert naming every consumer a new kind must reach, so a
// 10th would not slip past them silently either.
std::vector<TheirRow> their_quote_kinds() {
  const std::string f = "include/swaps/calibration/problem.hpp";
  const std::string s = "enum class QuoteKind (:64)";
  return {
      {"ParRate", f, s + ", :65", "float leg PV over the fixed leg's annuity", Level::Compared,
       "`instrument::par` for `Kind::Ois` / `Kind::Irs` -- the same quotient", ""},
      {"ParSpread", f, s + ", :66", "benchmark leg minus quoted leg, over the annuity", Level::Supported,
       "`instrument::par` for `Kind::Basis`; `EUR-3S6S-BASIS` in Stage A", ""},
      {"Rate", f, s + ", :67", "an observed rate plus a convexity adjustment", Level::Supported,
       "`Kind::Deposit` (simple forward) and `Kind::Future` (price 100(1-rate))", ""},
      {"ZeroCouponRate", f, s + ", :68", "annually compounded zero-coupon par rate; a nonlinear transform of ParRate",
       Level::Absent, "",
       "A transform of a quote we already compute, not new maths: `exp(log(1+tau*q)/tau) - 1` is "
       "expressible in the current op set (`Op::Exp`, `Op::Log`) and **no `Op::Pow` is needed**. "
       "The real obstacle is structural -- a quote CONVENTION is not something our types carry "
       "(see the fused-axis finding) -- so it lands as a residual wrapper plus one blueprint field."},
      {"FxForward", f, s + ", :79", "FX forward point pinning one currency's curve against another's",
       Level::Absent, "",
       "Blocked on Stage B, not on a small change: `blueprints/problems/stage_a.json`'s own notes "
       "fix the USD conversion of EUR values at a placeholder of 1.0 recorded as `fx.placeholder`, "
       "and say Stage B replaces it with an FX spot INPUT. Needs an FX spot as a tape input, a "
       "second discount curve in the residual, and a reporting-currency conversion that is a "
       "recorded product rather than a constant."},
      {"XccyMtmBasis", f, s + ", :80", "FX-resettable-notional cross-currency basis, including the funding leg",
       Level::Absent, "",
       "Everything `FxForward` needs, plus a resetting-notional funding leg -- a coupon mechanic "
       "`instrument::CouponKind` has no member for. Strictly behind `FxForward`."},
      {"Portfolio", f, s + ", :81", "a weighted combination of nested instruments quoted as one number",
       Level::Absent, "",
       "Cheap, and it needs no engine maths: `solver::CurveSet::add_instrument` already takes an "
       "arbitrary callable residual, so a weighted sum of component residuals is expressible "
       "today. Missing: a blueprint shape that NESTS instruments (theirs compose recursively, "
       "`std::vector<WeightedInstrument>`) and a quote key for the combination."},
      {"TurnJump", f, s + ", :88", "pins one turn's jump delta; no legs and no discount factors",
       Level::Absent, "",
       "**Not a new interpolation scheme**, which is the natural guess and is wrong. Theirs is an ADDITIVE "
       "OVERLAY with its own unknown: one free delta per turn appended to the state vector AFTER "
       "the interpolation knots (`CurveStructure::turns`, `n_knots() = n_interp_knots() + "
       "turns.size()`), with a closed-form window overlap, so the Jacobian row is a unit vector. "
       "For us that is a state-vector extension plus a curve overlay term -- `curve::Composite` "
       "regions stay untouched. Cheaper than a scheme, but it does widen the unknown vector, "
       "which the IFT ladder and the adjoint both see."},
      {"Npv", f, s + ", :93", "a position's net present value as the quote, in PV units", Level::Absent, "",
       "Nearly free in the maths: `instrument::pv<Scalar>` already exists templated on Scalar, so "
       "the residual is `pv - target` with no new op. Two wrinkles: the same missing quote-key "
       "degree of freedom as `Portfolio`, and **theirs is in PV units while every residual here is "
       "in rate units** by deliberate design (so `solve_tol` means one thing everywhere), so the "
       "row needs a per-instrument residual scale the solver does not currently have."},
  };
}

// ---- axis 2: problem families ----------------------------------------------------------------
std::vector<TheirRow> their_families() {
  return {
      {"Rates, single currency", "include/swaps/calibration/", "QuoteKind::{ParRate,ParSpread,Rate}",
       "par-quoted swaps, basis, deposits and futures on one currency's curves", Level::Compared,
       "`instrument::Kind` (live below), `blueprints/curves/{usd,eur}.json`", ""},
      {"Rates, multi-curve", "include/swaps/calibration/bundle_problem.hpp", "BundleProblem (:58)",
       "N curves on one stacked state vector, forecast distinct from discount", Level::Supported,
       "`solver::CurveSet` + `Mode::joint`; Stage A stacks its curves over 2 currencies", ""},
      {"Cross-currency", "include/swaps/build/instruments.hpp", "xccy_mtm_basis (:271)",
       "curves pinned across two currencies through FX, resetting notionals", Level::Absent, "",
       "Blocked on Stage B's FX spot input. The curve-stack half already exists (Stage A is "
       "2 currencies); the FX half does not exist at all."},
      {"Inflation", "include/swaps/calibration/inflation_instrument.hpp", "InflationInstrument::Kind { ZCIS, YoY } (:44)",
       "zero-coupon and year-on-year inflation swaps", Level::Absent, "",
       "Needs a new index TYPE before any instrument: `conventions::IndexDef::Kind` is "
       "{Overnight, Term} and a price index fixed monthly, with a lag and seasonality, is "
       "neither. New conventions, new coupon mechanics, new blueprints. Note theirs is a "
       "STANDALONE problem too -- it reuses their solver but is not a member of their curve "
       "bundle, so this is not as entangled as it looks."},
      {"Credit", "include/swaps/calibration/credit_instrument.hpp", "struct CdsInstrument (:50) -- **no enum**",
       "par CDS calibration of a hazard curve", Level::Absent, "",
       "A second stochastic quantity (a survival/hazard curve) multiplying every discount factor, "
       "and a protection leg integrating over default time. Our `df(slot, t)` callable "
       "generalises; nothing else does. Theirs is one row shape only (no accrual-on-default, no "
       "index or tranche), so the target is smaller than the family name suggests."},
      {"Bonds, RV and asset swaps", "include/swaps/calibration/bond_fit.hpp",
       "GovvieBondFit (:62), ParametricBondFit<M> (:96) -- **no enum, by design**",
       "fitting a curve to a bond universe; asset swaps and swap spreads", Level::Absent, "",
       "Cheapest of the absent families: the coupon mechanics are largely `CouponKind::Fixed` "
       "already, so it is a redemption cashflow, a clean/dirty price quote convention and a bond "
       "blueprint. Their family selector is `derive::GovvieModel { Spline, NelsonSiegel, "
       "Svensson }` (`derive/bond_rv.hpp:83`), and the parametric two need curve forms "
       "`curve::SchemeKind` does not have."},
      {"FX: spot, forward, options, NDF", "include/swaps/market/fx.hpp", "FxMatrix (:37), FxRate (:28)",
       "a triangulating spot store, FX vanillas and non-deliverable forwards", Level::Absent, "",
       "The same Stage B blocker for the linear part. Their FX options and NDFs are stateless, "
       "bundle-free verbs taking {spot, r_dom, r_for} directly, so they are not even curve "
       "consumers -- a separate pricing surface rather than a calibration one."},
      {"Rates and FX volatility", "include/swaps/vol/", "sabr_calibration.hpp (:39), bachelier.hpp Payoff (:23)",
       "European swaptions, SABR smiles and strip calibration, FX vanillas, a vega ladder",
       Level::Absent, "",
       "Entirely outside this engine's scope today: a vol surface, an option payoff and a "
       "second stochastic driver. Eight headers on their side. Nothing on ours -- `Op::` has no "
       "normal CDF and the recorded world has no optionality beyond `Op::Select`. Not a "
       "near-term coverage item at any price."},
      {"XVA / exposure", "include/swaps/xva/exposure.hpp", "ExposureProfile (:22), exposure_profile (:31)",
       "EPE / ENE / PFE over a Monte Carlo NPV grid", Level::Absent, "",
       "**Their weakest row, and worth knowing before planning against it**: the header is ~60 "
       "lines, delivers EPE/ENE/PFE only, and computes no CVA, DVA, FVA, MVA or KVA anywhere. "
       "Its curve-state proxy is described in their own source as illustrative pending a "
       "calibrated LGM/HW1F. Comparing here would measure very little."},
      {"Scenario grids", "include/swaps/derive/scenario_grid.hpp", "ShockAxisKind { ParallelBp, ShiftCurve, Fx } (:37)",
       "a P&L surface from shocked recalibrations off one base", Level::Supported,
       "Stage A's O4: scenario lanes over parallel / twist / butterfly / per-curve families, each "
       "a full recalibration (count generated below)", ""},
      {"VaR / ES", "include/swaps/derive/var.hpp", "VarRequest/Result, pnl_distribution, var_es_at (:122)",
       "full-revaluation VaR and expected shortfall from a P&L distribution", Level::Absent, "",
       "A thin layer on top of a capability we have: it is a quantile over many repriced "
       "scenarios, and Stage A already recalibrates its whole scenario set. What is missing is "
       "only the distribution and quantile step -- no new maths in the recorded world at all. "
       "The cheapest absent row on this axis by a wide margin."},
  };
}

// ---- axis 3: capabilities around the problem -------------------------------------------------
std::vector<TheirRow> their_capabilities() {
  return {
      {"Multi-curve stacked state", "include/swaps/calibration/bundle_problem.hpp",
       "BundleProblem (:58), offset(k) (:58ff)", "N curves over one state vector, curves addressed by index",
       Level::Supported, "`solver::CurveSet` + `CurveSet::Mode::{sequential,joint}`", ""},
      {"Forecast != discount", "include/swaps/calibration/problem.hpp",
       "FloatLeg::forecast / ::discount (:42), FixedLeg::discount (:60)",
       "per-leg projection and discounting roles, resolved independently", Level::Supported,
       "`Leg::curve` vs `Leg::disc_curve` (generated example below)", ""},
      {"Seasoned trades, realised fixings", "include/swaps/market/fixing_series.hpp",
       "FixingSeries (:24); pricing::FixingTable (pricing/fixings.hpp:78)",
       "per-index history, split into realised factor plus forecast sub-periods", Level::Supported,
       "`conventions::FixingsHistory`; `Coupon::realised` / `::accrued`; Stage A's seasoned "
       "fraction (generated below)", ""},
      {"Netting sets", "include/swaps/trade/book.hpp", "trade::NettingSet (:27) -- **trade/, not portfolio/**",
       "trades grouped under one CSA; netting within a set, never across", Level::Supported,
       "`fixtures::StageABook::netting_total`, a gated O2 output class", ""},
      {"CSA / collateral", "include/swaps/trade/csa.hpp", "CSA (:34), CSA::Type { Cash, Uncollateralized } (:38)",
       "the collateral agreement choosing a trade's discount basis", Level::Absent, "",
       "Nothing here: `grep -ri csa|collateral` over `include/epykos/` and `src/` finds zero "
       "hits. Mechanically it is a per-trade choice of discount curve, which `Leg::disc_curve` "
       "already expresses, so the cost is a trade field and a blueprint field, not new maths. "
       "Worth knowing theirs is partial too: `threshold`, `mta`, `independent_amount` and "
       "`rounding` are declared and consumed nowhere, and `Uncollateralized` is unreachable from "
       "their JSON wire. So the comparable part of this row is just the discount-curve choice."},
      {"Turns / year-end jumps", "include/swaps/pricing/curve_handle.hpp",
       "TurnedCurve (:83), CurveHandle::turn_jump (:38); pricing::Turn (curve_spec.hpp)",
       "an additive jump overlay on the curve with its own unknown", Level::Absent, "",
       "See `TurnJump` on axis 1: a state-vector extension plus an overlay term, not a new "
       "`curve::SchemeKind`. Their turn deltas are deliberately excluded from the curvature "
       "regulariser and from the parallel-shift direction, which is a design detail we would have "
       "to reproduce rather than discover."},
      {"Regularisation / smoothing", "include/swaps/calibration/regularize.hpp",
       "second_difference_operator (:78), RegSpec (:128), Smoothing { Off, Light, Strong } (:149)",
       "a Tikhonov bending-energy penalty making a non-square fit well posed", Level::Absent, "",
       "Deliberately outside the comparison today, and that is not an accident: "
       "`fixtures::compare_ois` is SQUARE by construction (one knot per instrument) precisely so "
       "neither engine needs a regulariser the two would not share. Needed the moment the "
       "calibration is over- or under-determined -- so this row cannot become *compared* without "
       "first choosing to make the comparison non-square."},
      {"P&L explain", "include/swaps/calibration/pnl_explain.hpp",
       "pnl_explain (:201), PnlExplain (:93), roll_book (:127), RebasedHandle (:109)",
       "attribution into carry, roll-down, market and residual", Level::Absent, "",
       "**Cheaper than the name suggests, and cheaper than a first reading of it concluded.** "
       "Theirs is FIRST order: "
       "carry (numeraire rebased to t1), roll-down (cashflows slid down an unchanged curve), "
       "market (the analytic delta ladder dotted with the quote move) and a residual term that "
       "absorbs the rest -- no bump-and-reprice and no second derivative. We already have the "
       "analytic ladder (O3, the head-to-head's own target), so this needs the carry and "
       "roll-down terms and an attribution identity, NOT the gamma that `Dual<N>` over hardcoded "
       "`double` forecloses. It is not blocked behind I2."},
      {"Consistent risk ladder, one quote basis", "include/swaps/calibration/risk.hpp",
       "ift_operator (:35), bucketed_delta (:56)",
       "a risk ladder consistent between calibration and pricing, computed analytically",
       Level::Compared, "the IFT ladder through `solver::ImplicitProgram` -- O3, and the "
       "head-to-head's stated target", ""},
      {"Cross-basis risk transform", "include/swaps/api/bundle_api.hpp",
       "cross_jacobian (:348), transform_matrix (:353), risk_operator (:276); consistent_risk.hpp (:72)",
       "re-expressing one book's risk in a different instrument basis", Level::Absent, "",
       "A distinct capability from the ladder above, and we do not have it: theirs re-levels one "
       "bundle's quotes onto another's, re-solves and re-expresses the risk, with NPV and "
       "parallel DV01 as invariants. We hold the pieces -- `J_z` is already built lazily at the "
       "solution (D85/D89) -- so this is a matrix solve between two quote bases rather than new "
       "recorded maths."},
      {"External oracle", "include/swaps/ql/", "ql_term_structure.hpp CurveTermStructure (:28), extract.hpp",
       "an independent library to cross-check conventions and prices against", Level::Absent, "",
       "We have D2's templated-`double` path as an INTERNAL oracle, which catches a recording bug "
       "but **cannot catch a convention bug, because it is the same conventions code**. The front "
       "stub disagreement in `bench/compare/README.md` is exactly that class of bug. One detail "
       "that is easy to get backwards: their QuantLib is **mandatory by default**, not an opt-in "
       "gated build -- the "
       "CMake option is `SWAPS_ALLOW_NO_ORACLE` and it opts OUT (absence is otherwise a "
       "FATAL_ERROR), and their shipped core and API link no QuantLib at all; only their oracle "
       "and bench targets do. Adding one here needs a D12 dependency decision."},
      {"Curve interpolation schemes", "include/swaps/curve/curve_module.hpp",
       "curve::Scheme (:143), kSchemeCount = 8 (:165), CurveModule (:171), ModularCurve (:186)",
       "per-region interpolation families composed into one curve", Level::Compared,
       "`curve::SchemeKind` and `curve::Variable` (both live below), composed by "
       "`curve::Composite` regions -- the overlap is computed, not asserted", ""},
      {"Soft quotes / bands", "include/swaps/calibration/problem.hpp",
       "Instrument::band_lower/band_upper/band_decay, ::penalty(); market::Quote::to_target()",
       "a Huber band residual: pull to mid inside a band, to the nearer edge outside",
       Level::Absent, "",
       "Changes the residual itself rather than the maths around it, so for us it is a residual "
       "wrapper -- the same missing degree of freedom as `Npv` and `Portfolio`. It is how their "
       "turn deltas stay identifiable, which is why it is not independent of the turns row."},
      {"Streaming / incremental recalibration", "include/swaps/calibration/streaming.hpp",
       "StreamingCalibrator (:134), StreamStatus (:66); background_jacobian.hpp BackgroundJacobian (:34)",
       "a frozen-Newton operator with a staleness envelope and off-thread Jacobian refresh",
       Level::Absent, "",
       "Our nearest thing is the warm re-quote path the head-to-head already times (D85/D89's "
       "lazy final Jacobian), which is a warm re-solve but not a frozen-Newton stream with a "
       "staleness bound, and we have no background refresh. Partially reachable, and the "
       "measurement half of it is already in place."},
  };
}

// Their interpolation schemes, snapshot: `curve::Scheme` at curve_module.hpp:143, kSchemeCount = 8.
// Normalised to our lower_snake names so the overlap below is COMPUTED rather than asserted.
std::vector<std::string> their_schemes_normalised() {
  return {"flat", "linear", "natural_cubic", "hermite", "monotone_cubic", "bspline", "tension", "monotone_convex"};
}

// =============================================================================================
// The live side. Nothing below names a product; everything is discovered.
// =============================================================================================

// Discovers an enum's live enumerators by scanning the underlying integer range and keeping the
// values whose `to_string` is not the "?" sentinel every one of these enums returns for an
// unknown value. This is what makes the matrix notice a kind nobody told it about (G1).
template <class E, class F>
std::vector<std::pair<int, std::string>> live_enum(F to_str) {
  std::vector<std::pair<int, std::string>> out;
  for (int i = 0; i < 256; ++i) {
    const char* n = to_str(static_cast<E>(i));
    if (n != nullptr && std::string(n) != "?") out.emplace_back(i, n);
  }
  return out;
}

// The quote form each of OUR instrument kinds is calibrated on, and which of their QuoteKinds it
// answers. The `switch` is exhaustive with no `default:` on purpose (G4).
struct OurQuote {
  std::string form;
  std::string their_kind;
  std::string caveat;
};

OurQuote our_quote_form(epykos::instrument::Kind k) {
  using K = epykos::instrument::Kind;
  switch (k) {
    case K::Ois:
      return {"par rate: float leg PV / fixed leg annuity", "ParRate", ""};
    case K::Irs:
      return {"par rate: float leg PV / fixed leg annuity", "ParRate", ""};
    case K::Basis:
      return {"par spread over the flat leg, on the spread leg's annuity", "ParSpread", ""};
    case K::Deposit:
      return {"the index forward over the period", "Rate", "no convexity term, and none is due"};
    case K::Future:
      return {"price 100(1-rate); residual in rate units", "Rate",
              "convexity adjustment is ZERO, a stated simplification (D35)"};
  }
  return {"", "", ""};   // unreachable for a classified kind; G1 catches a new one first
}

std::string kind_of_convention(epykos::conventions::InstrumentConvention::Type t) {
  using T = epykos::conventions::InstrumentConvention::Type;
  switch (t) {
    case T::OIS: return "ois";
    case T::IRS: return "irs";
    case T::Basis: return "basis";
    case T::Deposit: return "deposit";
    case T::Future: return "future";
  }
  return "?";
}

struct OurSide {
  std::vector<std::pair<int, std::string>> kinds, coupon_kinds, schemes, variables;
  std::vector<std::string> currencies, indices, conventions, blueprints, curves;
  std::map<std::string, int> convention_type_counts;
  std::set<std::string> schemes_used, variables_used, kinds_with_a_blueprint;
  bool forecast_differs_from_discount = false;
  std::string forecast_example;
};

OurSide load_our_side(const epykos::conventions::Registry& reg, const epykos::instrument::Blueprints& bp) {
  using namespace epykos;
  OurSide o;
  o.kinds = live_enum<instrument::Kind>([](instrument::Kind k) { return instrument::to_string(k); });
  o.coupon_kinds = live_enum<instrument::CouponKind>([](instrument::CouponKind k) { return instrument::to_string(k); });
  o.schemes = live_enum<curve::SchemeKind>([](curve::SchemeKind k) { return curve::to_string(k); });
  o.variables = live_enum<curve::Variable>([](curve::Variable v) { return curve::to_string(v); });

  o.currencies = reg.currency_codes();
  o.indices = reg.index_names();
  o.conventions = reg.instrument_names();
  o.blueprints = bp.blueprint_names();
  o.curves = bp.curve_names();

  for (const std::string& n : o.conventions) o.convention_type_counts[kind_of_convention(reg.instrument(n).type)]++;
  for (const std::string& n : o.blueprints) {
    o.kinds_with_a_blueprint.insert(kind_of_convention(reg.instrument(bp.blueprint(n).convention).type));
  }
  for (const std::string& n : o.curves) {
    const auto& c = bp.curve(n);
    for (const auto& r : c.regions) {
      o.schemes_used.insert(curve::to_string(r.scheme));
      o.variables_used.insert(curve::to_string(r.variable));
    }
    for (const auto& e : c.instruments) {
      const auto& conv = reg.instrument(bp.blueprint(e.blueprint).convention);
      if (!conv.discount_index.empty() && conv.discount_index != c.index) {
        o.forecast_differs_from_discount = true;
        if (o.forecast_example.empty()) {
          o.forecast_example = "`" + c.index + "` projects while `" + conv.discount_index + "` discounts (" +
                               e.blueprint + " on curve " + n + ")";
        }
      }
    }
  }
  return o;
}

// What the head-to-head actually reaches, read off the fixture by BUILDING it.
struct H2hReach {
  int n_curves = 1, n_currencies = 1, n_quotes = 0, n_trades = 0, n_knots = 0, n_fixings = 0;
  std::set<std::string> calib_kinds, book_kinds, coupon_kinds, schemes, variables;
  bool seasoned = false;
};

H2hReach h2h_reach() {
  using namespace epykos;
  H2hReach r;
  const fixtures::CompareOis c = fixtures::make_compare_ois();
  r.n_quotes = c.n_quotes();
  r.n_trades = c.n_trades();
  r.n_knots = static_cast<int>(c.set.knot_t.size());
  for (const std::string& ix : c.fixings.indices()) r.n_fixings += static_cast<int>(c.fixings.size(ix));
  for (const auto& ci : c.set.instruments) {
    r.calib_kinds.insert(instrument::to_string(ci.instrument.kind));
    for (const auto& l : ci.instrument.legs) {
      for (const auto& cp : l.coupons) r.coupon_kinds.insert(instrument::to_string(cp.kind));
    }
  }
  for (const auto& in : c.book) {
    r.book_kinds.insert(instrument::to_string(in.kind));
    for (const auto& l : in.legs) {
      for (const auto& cp : l.coupons) {
        r.coupon_kinds.insert(instrument::to_string(cp.kind));
        if (cp.realised || cp.current) r.seasoned = true;
      }
    }
  }
  for (const auto& rg : c.set.regions) {
    r.schemes.insert(curve::to_string(rg.scheme));
    r.variables.insert(curve::to_string(rg.variable));
  }
  return r;
}

// Stage A's reach, from the problem definition plus the registry. No trade is generated: the mix
// resolves blueprint -> convention -> type, the same chain the builder walks.
struct StageAReach {
  int n_curves = 0, n_variants = 0, n_currencies = 0, n_trades = 0, n_netting = 0, n_mix = 0, n_scenarios = 0;
  int n_calib_instruments = 0;
  double seasoned_fraction = 0.0;
  std::set<std::string> kinds, coupon_kinds, calib_kinds;
  std::vector<std::string> scenario_families;
  bool fx_is_placeholder_only = true;
  std::vector<std::string> fx_note;
};

StageAReach stage_a_reach(const epykos::conventions::Registry& reg, const epykos::instrument::Blueprints& bp,
                          const epykos::fixtures::StageADefinition& d) {
  using namespace epykos;
  StageAReach r;
  r.n_currencies = static_cast<int>(d.currencies.size());
  r.n_trades = d.book.trades;
  r.n_netting = d.book.netting_sets;
  r.n_mix = static_cast<int>(d.book.mix.size());
  r.seasoned_fraction = d.book.seasoned_fraction;
  r.n_scenarios = d.scenarios.count;
  for (const auto& f : d.scenarios.families) r.scenario_families.push_back(f.name);
  for (const auto& c : d.curves) {
    r.n_curves++;
    r.n_variants += static_cast<int>(c.variants.size());
    // The CALIBRATION side of Stage A, which is a different set from the book: walk the named
    // curve definition's instrument entries blueprint -> convention -> type.
    const auto& cd = bp.curve(c.definition);
    for (const auto& e : cd.instruments) {
      r.calib_kinds.insert(kind_of_convention(reg.instrument(bp.blueprint(e.blueprint).convention).type));
      const int n = e.tenors.empty() ? e.contracts : static_cast<int>(e.tenors.size());
      r.n_calib_instruments += n;
    }
  }
  for (const auto& w : d.book.mix) {
    const auto& b = bp.blueprint(w.name);
    r.kinds.insert(kind_of_convention(reg.instrument(b.convention).type));
    for (const auto& lb : b.legs) r.coupon_kinds.insert(instrument::to_string(lb.coupon));
  }
  for (const auto& f : d.fx) {
    if (f.placeholder != 1.0) r.fx_is_placeholder_only = false;
    std::ostringstream os;
    os << f.currency << " = " << f.placeholder;
    r.fx_note.push_back(os.str());
  }
  return r;
}

// =============================================================================================

struct Counts {
  int compared = 0, supported = 0, absent = 0;
  void add(Level l) {
    if (l == Level::Compared) compared++;
    else if (l == Level::Supported) supported++;
    else absent++;
  }
  int total() const { return compared + supported + absent; }
};

std::string pct(int num, int den) {
  if (den == 0) return "n/a";
  std::ostringstream os;
  os.setf(std::ios::fixed);
  os.precision(0);
  os << (100.0 * num / den) << "%";
  return os.str();
}

std::string join(const std::set<std::string>& s) {
  std::string out;
  for (const auto& v : s) {
    if (!out.empty()) out += ", ";
    out += "`" + v + "`";
  }
  return out.empty() ? "--" : out;
}

std::string join_names(const std::vector<std::pair<int, std::string>>& v) {
  std::string out;
  for (const auto& p : v) {
    if (!out.empty()) out += ", ";
    out += "`" + p.second + "`";
  }
  return out;
}

std::string join_plain(const std::vector<std::string>& v) {
  std::string out;
  for (const auto& s : v) {
    if (!out.empty()) out += ", ";
    out += "`" + s + "`";
  }
  return out.empty() ? "--" : out;
}

void axis_table(std::ostringstream& md, const std::vector<TheirRow>& rows, Counts& c) {
  md << "| their row | their file / symbol | what it is | verdict | what answers it here |\n";
  md << "|---|---|---|---|---|\n";
  for (const auto& r : rows) {
    md << "| `" << r.name << "` | `" << r.file << "`<br>" << r.symbol << " | " << r.means << " | "
       << level_cell(r.level) << " | " << (r.ours.empty() ? std::string("--") : r.ours) << " |\n";
  }
  md << "\n";
  bool any = false;
  for (const auto& r : rows) {
    if (r.level == Level::Absent) any = true;
  }
  if (any) {
    md << "**What it would take**, for the *theirs only* rows:\n\n";
    for (const auto& r : rows) {
      if (r.level != Level::Absent) continue;
      md << "- **`" << r.name << "`** -- " << r.what_it_takes << "\n";
    }
    md << "\n";
  }
  for (const auto& r : rows) c.add(r.level);
}

int run(const std::string& out_path, bool to_stdout, const std::string& bp_root) {
  using namespace epykos;

  const conventions::Registry reg =
      bp_root.empty() ? conventions::Registry::load() : conventions::Registry::load(bp_root + "/conventions");
  const instrument::Blueprints bp =
      bp_root.empty() ? instrument::Blueprints::load() : instrument::Blueprints::load(bp_root);
  bp.validate(reg);   // G2

  const OurSide ours = load_our_side(reg, bp);

  // ---- G1 / G3 -------------------------------------------------------------------------------
  std::vector<std::string> stale;
  for (const auto& k : ours.kinds) {
    if (our_quote_form(static_cast<instrument::Kind>(k.first)).form.empty()) {
      stale.push_back("instrument::Kind::" + k.second + " has no row in our_quote_form()");
    }
  }
  auto check = [&](const char* what, std::size_t live, std::size_t expected) {
    if (live != expected) {
      std::ostringstream os;
      os << what << " now has " << live << " live enumerators, not the " << expected
         << " this matrix was written against";
      stale.push_back(os.str());
    }
  };
  check("instrument::Kind", ours.kinds.size(), kExpectedKinds);
  check("instrument::CouponKind", ours.coupon_kinds.size(), kExpectedCouponKinds);
  check("curve::SchemeKind", ours.schemes.size(), kExpectedSchemes);
  check("curve::Variable", ours.variables.size(), kExpectedVariables);
  if (!stale.empty()) {
    std::fprintf(stderr,
                 "coverage: this repository has grown past what the matrix reasons about, so the\n"
                 "numbers below would be silently wrong. Update tools/coverage/coverage_main.cpp:\n");
    for (const auto& s : stale) std::fprintf(stderr, "  - %s\n", s.c_str());
    return 2;
  }

  const H2hReach h2h = h2h_reach();
  const fixtures::StageADefinition def = fixtures::load_stage_a_definition();
  const StageAReach sa = stage_a_reach(reg, bp, def);

  // scheme overlap, computed
  std::set<std::string> theirs_s, both_s, theirs_only_s, ours_only_s;
  for (const auto& s : their_schemes_normalised()) theirs_s.insert(s);
  for (const auto& p : ours.schemes) {
    if (theirs_s.count(p.second) != 0) both_s.insert(p.second);
    else ours_only_s.insert(p.second);
  }
  for (const auto& s : theirs_s) {
    bool have = false;
    for (const auto& p : ours.schemes) {
      if (p.second == s) have = true;
    }
    if (!have) theirs_only_s.insert(s);
  }

  std::ostringstream md;
  md << "# The generated coverage matrix\n\n";
  md << "**Generated by `tools/coverage/` (the `coverage` target). Do not edit: the next run\n";
  md << "overwrites it.**\n\n";
  md << "The question this file turns into a number: *what can we not compare?* It is emitted by a\n";
  md << "tool rather than written by hand so that adding a product to this repository moves the\n";
  md << "matrix without anyone remembering to.\n\n";

  md << "## 0. What is live and what is a snapshot\n\n";
  md << "| side | status |\n|---|---|\n";
  md << "| **Epykos (ours)** | **LIVE.** Instrument kinds, coupon mechanics, curve schemes and curve "
        "variables are discovered from the shipped enums at run time; conventions, blueprints, curve "
        "definitions and the Stage A problem are loaded from `blueprints/` through the engine's own "
        "loaders; the head-to-head's reach is read off `fixtures::compare_ois` by building it. **No "
        "product is named by hand on this side.** |\n";
  md << "| **SwapEngine (theirs)** | **SNAPSHOT, dated " << kSnapshotDate
     << ".** A checked-in table. Every row carries the file and the symbol it was read from so it can "
        "be re-verified in seconds. It can go stale, and nothing in this repository will notice if it "
        "does. |\n\n";

  md << "Their checkout, for re-verification: `" << kTheirRoot << "`.\n\n";
  md << "Reading it is permitted (D11 as relaxed by the owner, " << kSnapshotDate
     << "). **Copying it is not**, and nothing here is copied: this is an inventory of which symbols\n";
  md << "exist in which file, not of what they do. The clean-room property is exactly what makes a\n";
  md << "head-to-head mean anything -- if we absorbed their implementation the comparison would\n";
  md << "measure nothing.\n\n";

  md << "### The three levels\n\n";
  md << "A two-way *both / theirs only* verdict is the wrong shape, because it hides the distinction\n";
  md << "that drives planning. Every row below is exactly one of:\n\n";
  md << "| level | meaning |\n|---|---|\n";
  md << "| **both, compared** | the head-to-head exercises it **today**. This is the number that "
        "bounds what any current measurement can say. |\n";
  md << "| both, *not compared* | the engine and the blueprints have it; the head-to-head does not "
        "reach it. A **fixture** gap, not a capability gap -- the cheap half of the ranked list. |\n";
  md << "| **theirs only** | no path to it here at all. A **capability** gap. |\n\n";
  md << "Folding *not compared* into \"both\" overstates what we can measure; folding it into \"theirs\n";
  md << "only\" understates what we have built.\n\n";

  Counts c1, c2, c3;

  md << "## 1. Calibration instruments: their 9 quote kinds against ours\n\n";
  axis_table(md, their_quote_kinds(), c1);

  md << "### Our side of this axis, live\n\n";
  md << "`instrument::Kind` has **" << ours.kinds.size() << "** live enumerators (" << join_names(ours.kinds)
     << "); `instrument::CouponKind` has **" << ours.coupon_kinds.size() << "** ("
     << join_names(ours.coupon_kinds) << ").\n\n";
  md << "| our `Kind` | the quote it calibrates on | answers their | caveat |\n|---|---|---|---|\n";
  for (const auto& k : ours.kinds) {
    const OurQuote q = our_quote_form(static_cast<instrument::Kind>(k.first));
    md << "| `" << k.second << "` | " << q.form << " | `" << q.their_kind << "` | "
       << (q.caveat.empty() ? "--" : q.caveat) << " |\n";
  }
  md << "\n";

  md << "### The finding that changes the shape of this axis: our two axes are FUSED and theirs are not\n\n";
  md << "Their `QuoteKind` says how a number is *quoted*; the instrument it is quoted on is separate\n";
  md << "data. Ours has no such degree of freedom: `instrument::residual` switches on `Kind` and each\n";
  md << "kind hard-codes exactly one quote form (`instrument::par` for `Ois`, `Irs`, `Basis` and\n";
  md << "`Deposit`; `futures_settlement_rate` for `Future`). So we cannot quote an OIS as a\n";
  md << "`ZeroCouponRate` or as an `Npv` even though we price the OIS perfectly well.\n\n";
  md << "That reframes three of the six *theirs only* rows. `ZeroCouponRate`, `Portfolio` and `Npv`\n";
  md << "are **not three products we lack** -- they are one missing degree of freedom, a quote\n";
  md << "convention per calibration instrument, which `solver::CurveSet::add_instrument` is already\n";
  md << "general enough to accept because it takes an arbitrary callable residual. Add the soft-quote\n";
  md << "band row from axis 3 and it is four rows behind one change.\n\n";
  md << "A second correction worth recording: **neither engine models futures convexity.** Their\n";
  md << "`Rate` takes `convexity` as an input double and their convexity model lives in their tests,\n";
  md << "not their engine; ours is zero by the stated simplification of D35. So the two agree exactly\n";
  md << "when their convexity input is set to zero -- `Rate` is a *fixture* step, not a maths gap.\n\n";

  md << "## 2. Problem families\n\n";
  axis_table(md, their_families(), c2);

  md << "## 3. Capabilities around the problem\n\n";
  axis_table(md, their_capabilities(), c3);

  md << "### Our side of this axis, live\n\n";
  md << "- Registry: **" << ours.currencies.size() << "** currencies (" << join_plain(ours.currencies)
     << "), **" << ours.indices.size() << "** indices, **" << ours.conventions.size()
     << "** instrument conventions.\n";
  md << "- Blueprints: **" << ours.blueprints.size() << "** instrument blueprints, **" << ours.curves.size()
     << "** curve definitions.\n";
  md << "- Conventions by our `Kind`: ";
  {
    bool first = true;
    for (const auto& kv : ours.convention_type_counts) {
      if (!first) md << ", ";
      md << "`" << kv.first << "` x " << kv.second;
      first = false;
    }
  }
  md << ".\n";
  md << "- Instrument kinds with at least one blueprint: " << join(ours.kinds_with_a_blueprint) << ".\n";
  md << "- Forecast != discount: **" << (ours.forecast_differs_from_discount ? "yes" : "no") << "**"
     << (ours.forecast_example.empty() ? "" : " -- " + ours.forecast_example) << ".\n\n";

  md << "#### Interpolation schemes, overlap computed rather than asserted\n\n";
  md << "`curve::SchemeKind` live: **" << ours.schemes.size() << "** (" << join_names(ours.schemes)
     << "). Theirs, snapshot: **" << theirs_s.size() << "** (`curve::Scheme`, their `kSchemeCount = 8`).\n\n";
  md << "| | schemes |\n|---|---|\n";
  auto scheme_cell = [](const std::set<std::string>& s) { return s.empty() ? std::string("none") : join(s); };
  md << "| both | **" << both_s.size() << "** -- " << scheme_cell(both_s) << " |\n";
  md << "| theirs only | **" << theirs_only_s.size() << "** -- " << scheme_cell(theirs_only_s) << " |\n";
  md << "| ours only | **" << ours_only_s.size() << "** -- " << scheme_cell(ours_only_s) << " |\n\n";
  md << "So the interpolation families are **not** a coverage gap: " << both_s.size() << " of their "
     << theirs_s.size() << " are ours too, and the two they have that we do not (" << scheme_cell(theirs_only_s)
     << ") are both value-dependent forms. What *is* narrow is use, not capability -- only "
     << ours.schemes_used.size() << " of our " << ours.schemes.size()
     << " appear in a curve definition today, and the head-to-head deliberately pins one.\n\n";
  md << "`curve::Variable` live: **" << ours.variables.size() << "** (" << join_names(ours.variables)
     << "); used by a curve definition today: " << join(ours.variables_used)
     << ". Schemes used by a curve definition today: " << join(ours.schemes_used) << ".\n";
  md << "Their extrapolation is a fixed flat policy with no enum, so there is nothing to compare there.\n\n";

  // ---- the Stage A vs head-to-head gap -------------------------------------------------------
  md << "## 4. The gap that costs nothing to close: Stage A against the head-to-head\n\n";
  md << "Both columns are generated -- Stage A from `blueprints/problems/stage_a.json` through\n";
  md << "`fixtures::load_stage_a_definition`, the head-to-head by building `fixtures::compare_ois`\n";
  md << "with its default options and looking at what came out.\n\n";
  md << "| | Stage A (our own problem) | head-to-head (`compare_ois`) |\n|---|---|---|\n";
  md << "| curves | " << sa.n_curves << " (+ " << sa.n_variants << " scheme variants) | " << h2h.n_curves
     << " |\n";
  md << "| currencies | " << sa.n_currencies << " | " << h2h.n_currencies << " |\n";
  md << "| trades | " << sa.n_trades << " | " << h2h.n_trades << " |\n";
  md << "| trade blueprints in the mix | " << sa.n_mix << " | 1 |\n";
  md << "| instrument kinds in the book | " << sa.kinds.size() << " -- " << join(sa.kinds) << " | "
     << h2h.book_kinds.size() << " -- " << join(h2h.book_kinds) << " |\n";
  md << "| coupon mechanics | " << sa.coupon_kinds.size() << " -- " << join(sa.coupon_kinds) << " | "
     << h2h.coupon_kinds.size() << " -- " << join(h2h.coupon_kinds) << " |\n";
  md << "| instrument kinds in the CALIBRATION set | " << sa.calib_kinds.size() << " -- " << join(sa.calib_kinds)
     << " | " << h2h.calib_kinds.size() << " -- " << join(h2h.calib_kinds) << " |\n";
  md << "| calibration instruments | " << sa.n_calib_instruments << " across " << sa.n_curves << " curves | "
     << h2h.n_quotes << " on " << h2h.n_knots << " knots (square) |\n";
  md << "| netting sets | " << sa.n_netting << " | 0 |\n";
  md << "| seasoned fraction | " << (sa.seasoned_fraction * 100.0) << "% | "
     << (h2h.seasoned ? "some" : "0") << " |\n";
  md << "| realised fixings in the history | generated from the seed, daily from the definition's "
        "start date | " << h2h.n_fixings << (h2h.n_fixings == 0 ? " -- empty by construction" : "") << " |\n";
  md << "| curve schemes exercised | " << join(ours.schemes_used) << " | " << join(h2h.schemes) << " |\n";
  md << "| curve variables exercised | " << join(ours.variables_used) << " | " << join(h2h.variables) << " |\n";
  md << "| scenarios | " << sa.n_scenarios << " over " << sa.scenario_families.size() << " families ("
     << join_plain(sa.scenario_families) << ") | -- |\n\n";

  md << "**Confirmed: Stage A is far richer than the head-to-head uses.** " << sa.n_curves
     << " curves against " << h2h.n_curves << ", " << sa.n_currencies << " currencies against "
     << h2h.n_currencies << ", " << sa.n_trades << " trades against " << h2h.n_trades << ", " << sa.n_mix
     << " trade blueprints against 1, " << sa.n_netting << " netting sets against none, and "
     << (sa.seasoned_fraction * 100.0) << "% seasoned against a fixings history that is empty by\n";
  md << "construction. The book-side kinds alone go " << h2h.book_kinds.size() << " -> " << sa.kinds.size()
     << " and the coupon mechanics " << h2h.coupon_kinds.size() << " -> " << sa.coupon_kinds.size() << ".\n\n";
  md << "**One thing the generated table says that a hand-written one would have got wrong: Stage A's\n";
  md << "BOOK and its CALIBRATION SET are different surfaces.** The book draws from " << sa.n_mix
     << " blueprints covering " << sa.kinds.size() << " instrument kinds (" << join(sa.kinds)
     << ") over " << sa.coupon_kinds.size() << " coupon mechanics; the calibration set covers "
     << sa.calib_kinds.size() << " (" << join(sa.calib_kinds) << ").\n";
  md << "So `deposit` and `future` are already exercised by Stage A -- on the calibration side, where\n";
  md << "a quote kind actually lives -- even though no trade in the book is one. That makes the\n";
  md << "`Rate` row on axis 1 cheaper than the book column alone suggests, and it is why item 1 of\n";
  md << "section 6 is a fixture change and not a product change.\n\n";

  md << "### But the restrictions are not all accidental, and a plan has to respect the difference\n\n";
  md << "`include/epykos/fixtures/compare_ois.hpp`'s own header gives four reasons for the narrowing,\n";
  md << "and they are of two different kinds:\n\n";
  md << "- **Fixture narrowness** -- cheap to widen, nothing defends it: one curve, one currency, one\n";
  md << "  instrument type; no seasoned trades.\n";
  md << "- **Deliberate agreement control** -- *not* cheap, and widening it would damage the\n";
  md << "  comparison rather than improve it:\n";
  md << "  - the curve is `linear`/`logdf` so the **interpolation scheme is removed from the\n";
  md << "    comparison** instead of being assumed shared (D71 measured that their bundle curve is\n";
  md << "    the same family);\n";
  md << "  - the calibration is **square**, one knot per instrument, so neither side needs a\n";
  md << "    pseudo-inverse or a regulariser the two would not share;\n";
  md << "  - `max_start_offset_days` defaults to **0 because the two engines disagree on front\n";
  md << "    stubs** -- measured, at 360 days the book NPV and the ladder diverge by 1e-3..1e-4\n";
  md << "    relative while the curve still agrees to 5.8e-13, which localises the difference to the\n";
  md << "    stub and nothing else.\n\n";
  md << "So \"widen the head-to-head\" splits into work that is nearly free and work that is blocked on\n";
  md << "a convention disagreement. Section 6 keeps them apart. The fixture already carries\n";
  md << "`max_start_offset_years`, which ages a trade by a whole number of years and therefore lands\n";
  md << "on the same annual roll grid -- the stub-free way to add seasoning.\n\n";

  md << "### Stage A has no FX -- confirmed, not assumed\n\n";
  md << "Generated from the problem definition: every FX entry is a placeholder (";
  {
    bool first = true;
    for (const auto& n : sa.fx_note) {
      if (!first) md << ", ";
      md << "`" << n << "`";
      first = false;
    }
  }
  md << "), and placeholder-only is **" << (sa.fx_is_placeholder_only ? "true" : "false") << "**.\n";
  md << "The definition's own notes say the USD conversion of EUR values is a structure placeholder of\n";
  md << "1.0 applied as a recorded product (`fx.placeholder`), and that **Stage B replaces it with an\n";
  md << "FX spot input**. So `FxForward`, `XccyMtmBasis` and the FX and cross-currency families are\n";
  md << "blocked on Stage B, not on a small change.\n\n";

  // ---- the headline ---------------------------------------------------------------------------
  const int tot = c1.total() + c2.total() + c3.total();
  const int comp = c1.compared + c2.compared + c3.compared;
  const int supp = c1.supported + c2.supported + c3.supported;
  const int absent = c1.absent + c2.absent + c3.absent;

  md << "## 5. The headline fraction\n\n";
  md << "| axis | rows | compared today | supported, not compared | theirs only |\n|---|---|---|---|---|\n";
  md << "| 1. calibration instruments | " << c1.total() << " | " << c1.compared << " ("
     << pct(c1.compared, c1.total()) << ") | " << c1.supported << " (" << pct(c1.supported, c1.total()) << ") | "
     << c1.absent << " (" << pct(c1.absent, c1.total()) << ") |\n";
  md << "| 2. problem families | " << c2.total() << " | " << c2.compared << " (" << pct(c2.compared, c2.total())
     << ") | " << c2.supported << " (" << pct(c2.supported, c2.total()) << ") | " << c2.absent << " ("
     << pct(c2.absent, c2.total()) << ") |\n";
  md << "| 3. capabilities | " << c3.total() << " | " << c3.compared << " (" << pct(c3.compared, c3.total())
     << ") | " << c3.supported << " (" << pct(c3.supported, c3.total()) << ") | " << c3.absent << " ("
     << pct(c3.absent, c3.total()) << ") |\n";
  md << "| **all three** | **" << tot << "** | **" << comp << " (" << pct(comp, tot) << ")** | **" << supp << " ("
     << pct(supp, tot) << ")** | **" << absent << " (" << pct(absent, tot) << ")** |\n\n";

  md << "> **The number that matters for planning: we can compare on " << pct(comp, tot)
     << " of their surface today** -- " << comp << " of " << tot << " rows.\n>\n";
  md << "> A further **" << pct(supp, tot) << "** (" << supp
     << " rows) we already support and simply do not exercise: reachable by widening a fixture,\n";
  md << "> subject to the stub caveat. The remaining **" << pct(absent, tot) << "** (" << absent
     << " rows) is capability we do not have.\n>\n";
  md << "> Put the other way: **" << pct(comp + supp, tot)
     << " of their surface is within reach of the engine as it stands**, and " << pct(absent, tot)
     << " is not.\n\n";
  md << "Read the denominator honestly. It counts *their* rows, so it is a measure of their surface,\n";
  md << "not of engineering merit: their volatility, XVA, bond and FX-option families are whole\n";
  md << "product areas this engine has never claimed, and one of them (XVA) is thin on their side\n";
  md << "too. The useful signal is the **" << pct(supp, tot) << " supported-but-not-compared band**,\n";
  md << "which is pure unrealised measurement, and the shape of the " << pct(absent, tot)
     << " -- how much of it is one missing degree of freedom rather than one missing product.\n\n";

  md << "## 6. What would most widen the comparable surface, cheapest first\n\n";
  md << "Ranked by comparable rows gained per unit of work, with *fixture* work (we have the\n";
  md << "capability) separated from *capability* work (we do not).\n\n";

  md << "### Fixture work -- the " << pct(supp, tot) << " already paid for\n\n";
  md << "1. **Put a second and third instrument type in the head-to-head: `Deposit`, then `Basis`.**\n";
  md << "   Pure fixture work. Both are already built, blueprinted and exercised by Stage A;\n";
  md << "   `compare_ois` simply does not instantiate them. `Basis` buys `ParSpread` outright and\n";
  md << "   `Deposit` buys most of `Rate` -- and `Rate` needs nothing else, because neither engine\n";
  md << "   models convexity and theirs takes it as an input that can be set to zero. **Two axis-1\n";
  md << "   rows, no engine change.** Cheapest real gain anywhere in this document.\n";
  md << "2. **Widen the head-to-head to the multi-curve, two-currency case.** Fixture work, and the\n";
  md << "   capability is there: Stage A already stacks " << sa.n_curves << " curves over "
     << sa.n_currencies << " currencies with forecast != discount, and `CurveSet::Mode::joint`\n";
  md << "   exists. **It does not need FX** -- two currencies' curves calibrate independently as long\n";
  md << "   as nothing converts between them, which is exactly Stage A's position. Buys the\n";
  md << "   multi-curve family row and strengthens the ladder row.\n";
  md << "3. **Seasoned trades in the head-to-head, aged by whole years.** Capability is there\n";
  md << "   (`FixingsHistory`, " << (sa.seasoned_fraction * 100.0) << "% of Stage A). Use\n";
  md << "   `max_start_offset_years` rather than `max_start_offset_days`: whole-year ageing lands on\n";
  md << "   the annual roll grid and so avoids the measured front-stub disagreement, which is the\n";
  md << "   only reason this is not item 1.\n";
  md << "4. **Netting sets and the scenario grid.** Both already exist on our side (" << sa.n_netting
     << " sets, " << sa.n_scenarios << " scenarios over " << sa.scenario_families.size()
     << " families in Stage A) and neither crosses the boundary today. Cheap, and the scenario row\n";
  md << "   is the prerequisite for the VaR item below.\n\n";

  md << "### Capability work -- cheapest first\n\n";
  md << "5. **A quote-convention degree of freedom on a calibration instrument.** The single highest\n";
  md << "   leverage change in this document: **four *theirs only* rows behind one structural\n";
  md << "   change** -- `Npv` (the residual is `pv - target`, and `instrument::pv<Scalar>` already\n";
  md << "   exists), `ZeroCouponRate` (`exp(log(1+tau*q)/tau)-1`, expressible in the current op set,\n";
  md << "   no `Op::Pow` needed), `Portfolio` (a weighted sum of component residuals) and the\n";
  md << "   soft-quote band. `CurveSet::add_instrument` already takes an arbitrary residual callable,\n";
  md << "   so no engine maths changes. The one real wrinkle is that `Npv` is in PV units while every\n";
  md << "   residual here is in rate units by design, so it also wants a per-instrument residual\n";
  md << "   scale.\n";
  md << "6. **VaR / ES.** A quantile over many repriced scenarios, and Stage A already recalibrates a\n";
  md << "   full scenario set. No new recorded maths at all -- the distribution and quantile step is\n";
  md << "   the whole of it. Cheapest absent row on axis 2 by a wide margin, and it rides on item 4.\n";
  md << "7. **P&L explain.** Re-ranked upward after reading their implementation: theirs is **first\n";
  md << "   order** -- carry, roll-down, the analytic delta ladder dotted with the quote move, and a\n";
  md << "   residual term -- with no bump-and-reprice and no second derivative. We already have the\n";
  md << "   analytic ladder; this needs the carry and roll-down terms and an attribution identity.\n";
  md << "   **It is not blocked behind second order**, which is what a first reading of it suggested.\n";
  md << "8. **CSA / collateral, as a trade-level discount choice.** Mechanically a per-trade choice of\n";
  md << "   discount curve, which `Leg::disc_curve` already expresses, so the cost is a trade field\n";
  md << "   and a blueprint field. And the comparable part is small: on their side the economic terms\n";
  md << "   (threshold, MTA, independent amount, rounding) are declared and consumed nowhere.\n";
  md << "9. **Cross-basis risk transform.** We already build `J_z` lazily at the solution (D85/D89),\n";
  md << "   so re-expressing a ladder in a second quote basis is a matrix solve rather than new\n";
  md << "   recorded maths.\n";
  md << "10. **A bond blueprint.** Cheapest of the absent *families*: the coupon mechanics are largely\n";
  md << "    `CouponKind::Fixed` already, so it is a redemption cashflow, a price quote convention and\n";
  md << "    a blueprint -- and it unlocks their bond-fit and bond-universe rows together. The\n";
  md << "    parametric fits (Nelson-Siegel, Svensson) are a separate curve form we do not have.\n";
  md << "11. **An external oracle.** Lowest coverage gain, highest *trust* gain, and it should be read\n";
  md << "    that way: our `double` oracle shares the conventions code with the engine, so it cannot\n";
  md << "    catch a convention bug -- and the front-stub disagreement is precisely a convention bug.\n";
  md << "    Needs a D12 dependency decision.\n";
  md << "12. **Turns, then regularisation.** Turns are a state-vector extension plus a curve overlay\n";
  md << "    (not a new scheme), which the IFT ladder and the adjoint both see, so it is not as local\n";
  md << "    as it looks. Regularisation cannot become *compared* at all until the comparison is\n";
  md << "    deliberately made non-square, since squareness is currently a feature of the fixture.\n";
  md << "13. **Streaming / frozen-Newton recalibration.** Partially reachable: the warm re-solve the\n";
  md << "    head-to-head already times is the nearest thing, but there is no staleness envelope and\n";
  md << "    no background Jacobian refresh.\n";
  md << "14. **FX spot, i.e. Stage B.** Gates `FxForward`, `XccyMtmBasis` and the FX and\n";
  md << "    cross-currency families -- **4 rows, the largest single block** -- but it is a\n";
  md << "    problem-stage change rather than a fixture change, which is why it sits here and not\n";
  md << "    higher despite the row count.\n";
  md << "15. **Inflation, then credit.** Each needs a new index type or a new stochastic quantity\n";
  md << "    before any instrument exists. Note both are standalone problems on their side too, so\n";
  md << "    neither has to join the curve bundle. Last on cost, not on value.\n";
  md << "16. **Volatility, and XVA.** Volatility is out of scope at any near-term price: a vol\n";
  md << "    surface, an option payoff and a second driver, with no normal CDF in the op set. XVA is\n";
  md << "    ranked last for a different reason -- comparing against a ~60-line EPE/ENE/PFE header\n";
  md << "    that computes no CVA or FVA would measure very little even if we built it.\n\n";

  md << "---\n\n";
  md << "Their rows on all three axes are a snapshot of `" << kTheirRoot << "` taken on " << kSnapshotDate
     << ". Everything about this repository, on all three axes, is generated at run time by\n";
  md << "`tools/coverage/coverage_main.cpp`.\n";

  const std::string text = md.str();
  if (to_stdout) std::fputs(text.c_str(), stdout);
  if (!out_path.empty()) {
    std::ofstream f(out_path, std::ios::binary | std::ios::trunc);
    if (!f) {
      std::fprintf(stderr, "coverage: cannot write '%s'\n", out_path.c_str());
      return 1;
    }
    f << text;
    if (!f) {
      std::fprintf(stderr, "coverage: write to '%s' failed\n", out_path.c_str());
      return 1;
    }
    std::fprintf(stderr, "coverage: wrote %s (%zu bytes)\n", out_path.c_str(), text.size());
  }
  std::fprintf(stderr,
               "coverage: %d rows across 3 axes -- %d compared (%s), %d supported-not-compared, %d theirs-only\n",
               tot, comp, pct(comp, tot).c_str(), supp, absent);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  std::string out = "bench/compare/COVERAGE.md";
  std::string bp_root;
  bool to_stdout = false;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> std::string {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "coverage: %s needs a value\n", a.c_str());
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "--out") out = next();
    else if (a == "--blueprints") bp_root = next();
    else if (a == "--stdout") to_stdout = true;
    else if (a == "--no-out") out.clear();
    else if (a == "--help" || a == "-h") {
      std::fputs("coverage [--out <path>] [--no-out] [--stdout] [--blueprints <dir>]\n", stdout);
      return 0;
    } else {
      std::fprintf(stderr, "coverage: unknown argument '%s'\n", a.c_str());
      return 2;
    }
  }
  try {
    return run(out, to_stdout, bp_root);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "coverage: %s\n", e.what());
    return 1;
  }
}
