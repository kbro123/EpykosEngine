// The head-to-head's problem DATA, gated. `tools/h2h` is opt-in, never configured in CI and needs
// a second checkout, so nothing there can be tested here -- but the two things it rests on are
// ordinary data of this repository and are tested here, with no second engine anywhere in sight:
//
//   1. `blueprints/problems/stage_a_h2h.json` is `stage_a.json` with the FOUR CURVE DEFINITIONS
//      changed and nothing else. That is the whole claim that makes a head-to-head number a
//      statement about Stage A: if the mix, the tenors, the notionals, the seasoned fraction, the
//      netting sets, the seed or the valuation date drifted apart, the comparison would be of a
//      different book and would quietly stop meaning what the README says it means.
//   2. The four definitions it names are on ONE interpolation family -- a single
//      {linear, logdf} region, i.e. log DF piecewise linear on {0, knots...} == piecewise-constant
//      instantaneous forward -- and are otherwise identical to their linear-zero parents (same
//      index, same currency, same calibration instruments, same knots). That family is the
//      load-bearing matched-ness clause of `bench/compare/README.md` §3: it is the only one the
//      second engine's bundle curve was MEASURED to share (D71). A curve definition edited to a
//      different scheme or a different instrument set would make the agreement gate fail in a way
//      that looks like an engine disagreement, which is the most expensive kind of confusion.
//
// So this file is the gate on the only part of the 2026-10-09 widening that lives in `blueprints/`.
#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "epykos/fixtures/stage_a.hpp"
#include "epykos/maths/curve/composite.hpp"
#include "epykos/maths/curve/curve.hpp"
#include "epykos/maths/instrument/blueprint.hpp"

namespace fixtures = epykos::fixtures;
namespace instrument = epykos::instrument;
namespace curve = epykos::curve;

namespace {

std::string h2h_path() {
  return instrument::Blueprints::default_root() + "/problems/stage_a_h2h.json";
}

// The -LOGDF definition each Stage A slot is expected to name, in slot order.
const std::vector<std::string>& expected_logdf() {
  static const std::vector<std::string> v{"USD-SOFR-LOGDF", "EUR-ESTR-LOGDF", "EUR-EURIBOR-3M-LOGDF",
                                          "EUR-EURIBOR-6M-LOGDF"};
  return v;
}

}  // namespace

// The two problem definitions differ in the four curve names and in NOTHING else that reaches a
// number. Checked field by field rather than by a hash, so a failure says which field moved.
TEST(H2hProblem, IsStageAWithOnlyTheCurveDefinitionsChanged) {
  const fixtures::StageADefinition a = fixtures::load_stage_a_definition();
  const fixtures::StageADefinition b = fixtures::load_stage_a_definition(h2h_path());

  EXPECT_EQ(a.name, "stage_a");
  EXPECT_EQ(b.name, "stage_a_h2h");

  // Same problem: the same day, the same seed, the same currencies and reporting currency.
  EXPECT_EQ(b.valuation, a.valuation);
  EXPECT_EQ(b.seed, a.seed);
  EXPECT_EQ(b.currencies, a.currencies);
  EXPECT_EQ(b.reporting_currency, a.reporting_currency);
  ASSERT_EQ(b.fx.size(), a.fx.size());
  for (std::size_t i = 0; i < a.fx.size(); ++i) {
    EXPECT_EQ(b.fx[i].currency, a.fx[i].currency);
    EXPECT_EQ(b.fx[i].placeholder, a.fx[i].placeholder);
  }

  // Same quote noise, so the calibration is a fit of the same size on both.
  EXPECT_EQ(b.quotes.noise_bp, a.quotes.noise_bp);
  EXPECT_EQ(b.quotes.futures_noise_price, a.quotes.futures_noise_price);

  // Same synthetic fixings, so the seasoned trades carry the same realised history.
  EXPECT_EQ(b.fixings.from, a.fixings.from);
  EXPECT_EQ(b.fixings.daily_vol, a.fixings.daily_vol);
  EXPECT_EQ(b.fixings.reversion, a.fixings.reversion);
  EXPECT_EQ(b.fixings.levels, a.fixings.levels);

  // Same book, trade for trade: the draws are a function of the seed and these fields alone.
  EXPECT_EQ(b.book.trades, a.book.trades);
  ASSERT_EQ(b.book.mix.size(), a.book.mix.size());
  for (std::size_t i = 0; i < a.book.mix.size(); ++i) {
    EXPECT_EQ(b.book.mix[i].name, a.book.mix[i].name);
    EXPECT_EQ(b.book.mix[i].weight, a.book.mix[i].weight);
  }
  ASSERT_EQ(b.book.tenors.size(), a.book.tenors.size());
  for (std::size_t i = 0; i < a.book.tenors.size(); ++i) {
    EXPECT_EQ(b.book.tenors[i].name, a.book.tenors[i].name);
    EXPECT_EQ(b.book.tenors[i].weight, a.book.tenors[i].weight);
  }
  EXPECT_EQ(b.book.notional_min, a.book.notional_min);
  EXPECT_EQ(b.book.notional_max, a.book.notional_max);
  EXPECT_EQ(b.book.notional_distribution, a.book.notional_distribution);
  EXPECT_EQ(b.book.seasoned_fraction, a.book.seasoned_fraction);
  EXPECT_EQ(b.book.seasoned_age_min, a.book.seasoned_age_min);
  EXPECT_EQ(b.book.seasoned_age_max, a.book.seasoned_age_max);
  EXPECT_EQ(b.book.netting_sets, a.book.netting_sets);
  EXPECT_EQ(b.book.rate_moneyness, a.book.rate_moneyness);

  // Same generating levels per slot, in the same slot order -- so the quotes sit at the same place
  // and the curves being compared are the same curves.
  ASSERT_EQ(b.curves.size(), a.curves.size());
  ASSERT_EQ(b.curves.size(), expected_logdf().size());
  for (std::size_t c = 0; c < a.curves.size(); ++c) {
    EXPECT_EQ(b.curves[c].definition, expected_logdf()[c]) << "slot " << c;
    EXPECT_EQ(b.curves[c].definition, a.curves[c].definition + "-LOGDF") << "slot " << c;
    EXPECT_TRUE(b.curves[c].variants.empty()) << "slot " << c << ": the matched problem offers no scheme sweep";
    EXPECT_EQ(b.curves[c].generating.short_rate, a.curves[c].generating.short_rate) << "slot " << c;
    EXPECT_EQ(b.curves[c].generating.long_rate, a.curves[c].generating.long_rate) << "slot " << c;
    EXPECT_EQ(b.curves[c].generating.reversion_years, a.curves[c].generating.reversion_years) << "slot " << c;
  }
}

// Every curve the matched problem names is ONE {linear, logdf} region: log DF piecewise linear on
// {0, knots...}, the family README §3 calls load-bearing. A second region, or any other variable,
// would silently stop being the family the other engine shares.
TEST(H2hProblem, EveryCurveIsOneLogDfRegion) {
  const fixtures::StageADefinition b = fixtures::load_stage_a_definition(h2h_path());
  const instrument::Blueprints bp = instrument::Blueprints::load();
  for (const fixtures::StageADefinition::CurveEntry& e : b.curves) {
    const instrument::CurveDefinition& d = bp.curve(e.definition);
    ASSERT_EQ(d.regions.size(), 1u) << e.definition << " has " << d.regions.size() << " regions";
    EXPECT_TRUE(d.regions[0].from.empty()) << e.definition;
    EXPECT_EQ(d.regions[0].scheme, curve::SchemeKind::linear)
        << e.definition << " is " << curve::to_string(d.regions[0].scheme);
    EXPECT_EQ(d.regions[0].variable, curve::Variable::logdf)
        << e.definition << " interpolates " << curve::to_string(d.regions[0].variable);
    EXPECT_TRUE(d.knot_tenors.empty()) << e.definition << ": knots are the instruments' own maturities";
  }
}

// Each -LOGDF definition is its parent's instrument set verbatim. If it were not, the two problems
// would calibrate different curves from different quotes and the comparison would be of neither.
TEST(H2hProblem, EachLogDfCurveIsItsParentsInstrumentSet) {
  const instrument::Blueprints bp = instrument::Blueprints::load();
  const fixtures::StageADefinition a = fixtures::load_stage_a_definition();
  for (const fixtures::StageADefinition::CurveEntry& e : a.curves) {
    const instrument::CurveDefinition& parent = bp.curve(e.definition);
    const instrument::CurveDefinition& child = bp.curve(e.definition + "-LOGDF");
    EXPECT_EQ(child.index, parent.index) << child.name;
    EXPECT_EQ(child.currency, parent.currency) << child.name;
    ASSERT_EQ(child.instruments.size(), parent.instruments.size()) << child.name;
    for (std::size_t i = 0; i < parent.instruments.size(); ++i) {
      EXPECT_EQ(child.instruments[i].blueprint, parent.instruments[i].blueprint) << child.name << " entry " << i;
      EXPECT_EQ(child.instruments[i].tenors, parent.instruments[i].tenors) << child.name << " entry " << i;
      EXPECT_EQ(child.instruments[i].keys, parent.instruments[i].keys) << child.name << " entry " << i;
      EXPECT_EQ(child.instruments[i].contracts, parent.instruments[i].contracts) << child.name << " entry " << i;
    }
  }
}

// The matched problem BUILDS: the same number of curves, quotes and knots as stage_a.json, every
// block square (one knot per instrument, which is what lets both engines solve it without a
// pseudo-inverse or a regulariser -- README §4), and the same book.
TEST(H2hProblem, BuildsSquareWithTheSameBookAsStageA) {
  fixtures::StageAOptions oa;
  oa.trades = 40;
  oa.scenarios = 0;
  const fixtures::StageA a = fixtures::make_stage_a(oa);

  fixtures::StageAOptions ob = oa;
  ob.path = h2h_path();
  const fixtures::StageA b = fixtures::make_stage_a(ob);

  EXPECT_EQ(b.n_curves(), a.n_curves());
  EXPECT_EQ(b.n_quotes(), a.n_quotes());
  EXPECT_EQ(b.n_trades(), a.n_trades());
  EXPECT_EQ(b.quote_keys, a.quote_keys);
  EXPECT_EQ(b.quote_curve, a.quote_curve);
  EXPECT_EQ(b.quote_is_price, a.quote_is_price);
  for (int c = 0; c < b.n_curves(); ++c) {
    const epykos::instrument::CalibrationSet& cs = b.sets[static_cast<std::size_t>(c)];
    EXPECT_EQ(static_cast<int>(cs.knot_t.size()), cs.n_instruments())
        << "slot " << c << " (" << cs.curve << ") is not square";
    EXPECT_EQ(cs.index, a.sets[static_cast<std::size_t>(c)].index) << "slot " << c;
    EXPECT_EQ(cs.knot_t, a.sets[static_cast<std::size_t>(c)].knot_t) << "slot " << c;
  }
  // The book is the SAME book: same blueprint, tenor, notional, side and seasoning per trade.
  ASSERT_EQ(b.trades.size(), a.trades.size());
  for (std::size_t i = 0; i < a.trades.size(); ++i) {
    EXPECT_EQ(b.trades[i].blueprint, a.trades[i].blueprint) << "trade " << i;
    EXPECT_EQ(b.trades[i].currency, a.trades[i].currency) << "trade " << i;
    EXPECT_EQ(b.trades[i].netting_set, a.trades[i].netting_set) << "trade " << i;
    EXPECT_EQ(b.trades[i].seasoned, a.trades[i].seasoned) << "trade " << i;
    EXPECT_EQ(b.trades[i].age_days, a.trades[i].age_days) << "trade " << i;
    EXPECT_EQ(b.instruments[i].notional, a.instruments[i].notional) << "trade " << i;
    EXPECT_EQ(b.instruments[i].side, a.instruments[i].side) << "trade " << i;
    EXPECT_EQ(b.instruments[i].effective, a.instruments[i].effective) << "trade " << i;
    EXPECT_EQ(b.instruments[i].termination, a.instruments[i].termination) << "trade " << i;
  }
}
