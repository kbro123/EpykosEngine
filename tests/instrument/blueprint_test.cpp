// M3/G2: definitions are data (D36). The Stage A blueprints and curve definitions load and
// validate against the registry; a NEW instrument convention plus a NEW blueprint (a
// quarterly-fixed SOFR OIS) added purely as JSON prices with no C++ change; schema errors name
// the file, line and column; every curve definition resolves to a calibration set with
// increasing knots and a curve object of the stated scheme / regions.
#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "instrument/instrument_test_helpers.hpp"

using namespace epykos;
using namespace epykos::conventions;
using namespace epykos::instrument;
using epykos::test::date;

namespace {

const Date valuation = date("2026-09-23");

std::string temp_file(const char* name, const char* doc) {
  const std::string path = (std::filesystem::temp_directory_path() / name).string();
  std::ofstream(path) << doc;
  return path;
}

}  // namespace

TEST(Blueprint, StageAFilesLoadAndValidate) {
  const Blueprints& b = epykos::test::blueprints();
  EXPECT_EQ(b.files().size(), 3u);
  for (const char* name : {"USD-SOFR-OIS", "USD-SOFR-OIS-SHIFT2", "USD-SOFR-OIS-SHIFT2-LOCKOUT2", "USD-SOFR-AVG-SWAP", "USD-SOFR-ON-DEPOSIT",
                           "USD-SOFR-3M-FUTURE", "USD-SOFR-1M-FUTURE", "EUR-ESTR-OIS", "EUR-ESTR-ON-DEPOSIT", "EUR-EURIBOR-3M-DEPOSIT",
                           "EUR-EURIBOR-6M-DEPOSIT", "EUR-EURIBOR-3M-IRS", "EUR-EURIBOR-6M-IRS", "EUR-3S6S-BASIS", "EUR-EURIBOR-3M-FUTURE"}) {
    EXPECT_TRUE(b.has_blueprint(name)) << name;
    EXPECT_TRUE(epykos::test::registry().has_instrument(b.blueprint(name).convention)) << name;
  }
  for (const char* name : {"USD-SOFR", "USD-SOFR-LOGDF", "USD-SOFR-MONOTONE", "USD-SOFR-COMPOSITE", "EUR-ESTR", "EUR-EURIBOR-3M", "EUR-EURIBOR-6M"}) {
    EXPECT_TRUE(b.has_curve(name)) << name;
  }
  const Blueprint& lock = b.blueprint("USD-SOFR-OIS-SHIFT2-LOCKOUT2");
  ASSERT_EQ(lock.legs.size(), 2u);
  EXPECT_EQ(lock.legs[1].coupon, CouponKind::RfrCompounded);
  ASSERT_TRUE(lock.legs[1].observation.has_value());
  EXPECT_EQ(lock.legs[1].observation->lockout_days, 2);
  EXPECT_EQ(b.blueprint("EUR-3S6S-BASIS").legs[0].spread.value_or(false), true);
  const CurveDefinition& comp = b.curve("USD-SOFR-COMPOSITE");
  ASSERT_EQ(comp.regions.size(), 3u);
  EXPECT_EQ(comp.regions[1].from, "3Y");
  EXPECT_EQ(comp.regions[1].scheme, curve::SchemeKind::monotone_cubic);
  EXPECT_EQ(comp.regions[2].variable, curve::Variable::logdf);
  EXPECT_THROW(b.blueprint("NO-SUCH"), BlueprintError);
  EXPECT_THROW(b.curve("NO-SUCH"), BlueprintError);
}

TEST(Blueprint, ANewConventionAndBlueprintNeedNoCode) {
  // A quarterly-fixed SOFR OIS (fixed 3M ACT/360 vs SOFR compounded 3M, one day payment delay,
  // short back stub) as a SYNTHETIC extra convention file and an extra blueprint file.
  const char* conv = R"({
    "meta": {"title": "test fixture: a quarterly-fixed SOFR OIS"},
    "instruments": {"USD-SOFR-OIS-Q": {"type": "ois", "description": "quarterly / quarterly SOFR OIS", "currency": "USD",
      "calendar": "USD-SOFR-SWAP", "spot_lag": 2, "payment_lag": 1, "bdc": "ModifiedFollowing", "eom": true, "stub": "ShortBack",
      "legs": [{"role": "fixed", "frequency": "3M", "day_count": "ACT/360"},
               {"role": "float", "index": "USD-SOFR", "compounding": "compounded", "frequency": "3M", "day_count": "ACT/360",
                "observation": {"method": "lookback", "lookback_days": 5}}],
      "discount_index": "USD-SOFR", "tenors": ["1Y", "2Y", "5Y"]}}
  })";
  const char* bp = R"({
    "meta": {"title": "test fixture: the quarterly-fixed SOFR OIS blueprint"},
    "blueprints": {"USD-SOFR-OIS-Q": {"convention": "USD-SOFR-OIS-Q", "legs": [{"coupon": "fixed"}, {"coupon": "rfr_compounded"}],
                                      "notional": 5000000, "side": "pay", "tenor": "2Y", "fixed_rate": 0.04}},
    "curves": {"USD-SOFR-Q": {"index": "USD-SOFR", "scheme": "linear", "variable": "logdf",
                              "instruments": [{"blueprint": "USD-SOFR-ON-DEPOSIT", "tenor": "1D"},
                                              {"blueprint": "USD-SOFR-OIS-Q", "tenors": ["1Y", "2Y", "5Y"]}]}}
  })";
  const std::string cpath = temp_file("epykos_g2_q_conv.json", conv);
  const std::string bpath = temp_file("epykos_g2_q_bp.json", bp);
  Registry reg = Registry::load();
  reg.add_file(cpath);
  Blueprints bps = Blueprints::load();
  bps.add_file(bpath);
  std::remove(cpath.c_str());
  std::remove(bpath.c_str());
  ASSERT_NO_THROW(bps.validate(reg));
  const FixingsHistory h = epykos::test::synthetic_history(valuation);
  BuildContext ctx;
  ctx.registry = &reg;
  ctx.fixings = &h;
  ctx.curves = &epykos::test::slots();
  ctx.valuation = valuation;
  Trade t;
  t.blueprint = "USD-SOFR-OIS-Q";   // every other field from the blueprint's defaults
  const Instrument in = build_instrument(ctx, bps, t);
  EXPECT_EQ(in.id, "USD-SOFR-OIS-Q 2Y");
  EXPECT_EQ(in.side, -1);
  EXPECT_EQ(in.notional, 5.0e6);
  EXPECT_EQ(in.fixed_rate, 0.04);
  EXPECT_EQ(in.legs[0].coupons.size(), 8u);
  EXPECT_EQ(in.legs[1].coupons.size(), 8u);
  EXPECT_EQ(in.legs[1].payment_lag, 1);
  const Coupon& v = in.legs[1].coupons[0];
  EXPECT_EQ(v.pay, reg.calendar("USD-SOFR-SWAP").add_business_days(v.end, 1));
  // lookback 5: the same days as plain, rates from five business days earlier — so the first
  // three accrual days (Fri 09-25, Mon 09-28, Tue 09-29) apply the rates of 09-18, 09-21 and
  // 09-22, already in the history: realised on a spot-starting swap; the first projected day
  // applies the valuation date's rate
  EXPECT_EQ(v.fixed_days, 5);
  EXPECT_GT(v.realised_factor, 1.0);
  const ObsDay& d0 = in.legs[1].obs[static_cast<std::size_t>(v.obs_begin)];
  EXPECT_EQ(d0.rate_date, valuation);
  EXPECT_EQ(v.obs_start, v.start);
  const epykos::test::FlatCurves flat;
  const double value = pv<double>(in, flat);
  EXPECT_TRUE(std::isfinite(value));
  // the same trade at par is worth zero
  Trade p = t;
  p.fixed_rate = par<double>(in, flat);
  const Instrument ip = build_instrument(ctx, bps, p);
  EXPECT_NEAR(pv<double>(ip, flat), 0.0, 1e-12 * (std::fabs(leg_pv<double>(ip.legs[0], flat)) + std::fabs(leg_pv<double>(ip.legs[1], flat))));
  // and the new curve definition resolves
  const CalibrationSet cs = build_calibration_set(ctx, bps, bps.curve("USD-SOFR-Q"));
  EXPECT_EQ(cs.n_instruments(), 4);
  EXPECT_EQ(cs.keys()[1], "USD-SOFR-OIS-Q 1Y");
  EXPECT_EQ(cs.regions[0].variable, curve::Variable::logdf);
}

TEST(Blueprint, SchemaErrorsNameTheLocation) {
  auto load = [](const char* doc) {
    Blueprints b;
    b.add_document(json::parse(doc, "<test>"), "<test>");
    return b;
  };
  // unknown key
  try {
    load(R"({"meta": {"title": "x"}, "blueprints": {"A": {"convention": "USD-SOFR-OIS", "colour": "blue"}}})");
    FAIL() << "unknown key accepted";
  } catch (const BlueprintError& e) {
    EXPECT_NE(std::string(e.what()).find("<test>:1:"), std::string::npos) << e.what();
    EXPECT_NE(std::string(e.what()).find("colour"), std::string::npos) << e.what();
  }
  // missing convention
  EXPECT_THROW(load(R"({"meta": {"title": "x"}, "blueprints": {"A": {"legs": [{"coupon": "fixed"}]}}})"), BlueprintError);
  // a bad coupon kind
  EXPECT_THROW(load(R"({"meta": {"title": "x"}, "blueprints": {"A": {"convention": "USD-SOFR-OIS", "legs": [{"coupon": "floating"}]}}})"), BlueprintError);
  // observation on a fixed leg
  EXPECT_THROW(load(R"({"meta": {"title": "x"}, "blueprints": {"A": {"convention": "USD-SOFR-OIS", "legs": [{"coupon": "fixed", "observation": {"method": "plain"}}]}}})"),
               BlueprintError);
  // a curve with both scheme and regions, an instrument entry with two forms, a bad tenor
  EXPECT_THROW(load(R"({"meta": {"title": "x"}, "curves": {"C": {"index": "USD-SOFR", "scheme": "linear", "regions": [{"scheme": "linear", "variable": "zero"}], "instruments": [{"blueprint": "USD-SOFR-OIS", "tenors": ["1Y"]}]}}})"),
               BlueprintError);
  EXPECT_THROW(load(R"({"meta": {"title": "x"}, "curves": {"C": {"index": "USD-SOFR", "instruments": [{"blueprint": "USD-SOFR-OIS", "tenors": ["1Y"], "contracts": 2}]}}})"),
               BlueprintError);
  EXPECT_THROW(load(R"({"meta": {"title": "x"}, "curves": {"C": {"index": "USD-SOFR", "instruments": [{"blueprint": "USD-SOFR-OIS", "tenors": ["1Q"]}]}}})"), BlueprintError);
  // a name defined twice across documents
  Blueprints twice;
  twice.add_document(json::parse(R"({"meta": {"title": "x"}, "blueprints": {"A": {"convention": "USD-SOFR-OIS"}}})", "<a>"), "<a>");
  EXPECT_THROW(twice.add_document(json::parse(R"({"meta": {"title": "y"}, "blueprints": {"A": {"convention": "USD-SOFR-OIS"}}})", "<b>"), "<b>"), BlueprintError);
  // validation against the registry: an unknown convention, a coupon kind the leg cannot record,
  // a future given tenors
  const Registry& reg = epykos::test::registry();
  EXPECT_THROW(load(R"({"meta": {"title": "x"}, "blueprints": {"A": {"convention": "USD-SOFR-XXX"}}})").validate(reg), BlueprintError);
  EXPECT_THROW(load(R"({"meta": {"title": "x"}, "blueprints": {"A": {"convention": "USD-SOFR-OIS", "legs": [{"coupon": "fixed"}, {"coupon": "term_rate"}]}}})").validate(reg),
               BlueprintError);
  EXPECT_THROW(load(R"({"meta": {"title": "x"}, "blueprints": {"A": {"convention": "USD-SOFR-OIS", "legs": [{"coupon": "term_rate"}, {"coupon": "rfr_compounded"}]}}})").validate(reg),
               BlueprintError);
  EXPECT_THROW(load(R"({"meta": {"title": "x"}, "blueprints": {"F": {"convention": "USD-SOFR-3M-FUTURE"}}, "curves": {"C": {"index": "USD-SOFR", "instruments": [{"blueprint": "F", "tenors": ["1Y"]}]}}})").validate(reg),
               BlueprintError);
  // an OIS leg may record as averaged (the convention says compounded): a variant, not an error
  EXPECT_NO_THROW(load(R"({"meta": {"title": "x"}, "blueprints": {"A": {"convention": "USD-SOFR-OIS", "legs": [{"coupon": "fixed"}, {"coupon": "rfr_averaged"}]}}})").validate(reg));
  // the default root and the environment override
  EXPECT_NE(Blueprints::default_root().find("blueprints"), std::string::npos);
  EXPECT_EQ(Registry::default_dir(), Blueprints::default_root() + "/conventions");
}

TEST(Blueprint, EveryCurveDefinitionResolvesToACalibrationSet) {
  const epykos::test::Context c(valuation);
  const Blueprints& b = epykos::test::blueprints();
  const epykos::test::FlatCurves flat;
  for (const std::string& name : b.curve_names()) {
    const CurveDefinition& def = b.curve(name);
    const CalibrationSet cs = build_calibration_set(c.ctx, b, def);
    EXPECT_EQ(cs.curve, name);
    EXPECT_EQ(cs.slot, epykos::test::slots().slot(def.index));
    EXPECT_EQ(cs.knot_t.size(), cs.instruments.size()) << name;
    for (std::size_t i = 1; i < cs.knot_t.size(); ++i) EXPECT_GT(cs.knot_t[i], cs.knot_t[i - 1]) << name;
    for (std::size_t i = 0; i < cs.instruments.size(); ++i) {
      const CalibrationInstrument& ci = cs.instruments[i];
      EXPECT_EQ(ci.instrument.t_maturity, cs.knot_t[i]) << name << " " << ci.key;
      EXPECT_EQ(ci.instrument.notional, 1.0) << ci.key;
      EXPECT_EQ(ci.instrument.side, 1) << ci.key;
      EXPECT_TRUE(std::isfinite(par<double>(ci.instrument, flat))) << ci.key;
      EXPECT_NEAR(residual<double>(ci.instrument, flat, par<double>(ci.instrument, flat)), 0.0, 1e-14) << ci.key;
    }
    const curve::Composite comp = cs.composite();
    EXPECT_EQ(comp.n_regions(), static_cast<int>(def.regions.size())) << name;
    EXPECT_EQ(comp.n(), static_cast<int>(cs.knot_t.size())) << name;
    // the curve object prices a flat 3% curve at every knot from the state in each region's
    // variable (zero: 0.03; logdf: −0.03 t_k)
    std::vector<double> state(cs.knot_t.size(), 0.03);
    for (int r = 0; r < comp.n_regions(); ++r) {
      const curve::Composite::Region& reg = comp.region(r);
      if (reg.spec.variable == curve::Variable::logdf) {
        for (int k = 0; k < reg.count; ++k) state[static_cast<std::size_t>(reg.first + k)] = -0.03 * cs.knot_t[static_cast<std::size_t>(reg.first + k)];
      }
    }
    for (double t : cs.knot_t) EXPECT_NEAR(comp.df(state.data(), t), std::exp(-0.03 * t), 1e-12) << name;
  }
  // the USD curve: the overnight fixing, three OIS, eight SR3 quarters, ten OIS; the composite's
  // boundaries sit on the 3Y and 15Y knots
  const CalibrationSet usd = build_calibration_set(c.ctx, b, b.curve("USD-SOFR"));
  ASSERT_EQ(usd.n_instruments(), 22);
  EXPECT_EQ(usd.keys()[0], "USD-SOFR-ON-DEPOSIT 1D");
  EXPECT_EQ(usd.keys()[1], "USD-SOFR-OIS 1M");
  EXPECT_EQ(usd.keys()[4], "SR3 Z26");
  EXPECT_EQ(usd.keys()[11], "SR3 U28");
  EXPECT_EQ(usd.keys()[12], "USD-SOFR-OIS 3Y");
  EXPECT_EQ(usd.keys()[21], "USD-SOFR-OIS 30Y");
  const CalibrationSet comp = build_calibration_set(c.ctx, b, b.curve("USD-SOFR-COMPOSITE"));
  ASSERT_EQ(comp.regions.size(), 3u);
  EXPECT_EQ(comp.regions[1].t_a, comp.instruments[12].instrument.t_maturity);
  EXPECT_EQ(comp.regions[0].t_b, comp.regions[1].t_a);
  EXPECT_EQ(comp.regions[2].t_a, comp.instruments[18].instrument.t_maturity);
  EXPECT_EQ(comp.instruments[18].key, "USD-SOFR-OIS 15Y");
  // the EUR 3M curve reads the 6M and €STR curves: basis swaps on slots 2 and 3, discounted on 1
  const CalibrationSet eur3 = build_calibration_set(c.ctx, b, b.curve("EUR-EURIBOR-3M"));
  ASSERT_EQ(eur3.n_instruments(), 18);
  EXPECT_EQ(eur3.keys()[1], "FEU3 Z26");
  const Instrument& basis = eur3.instruments[9].instrument;
  EXPECT_EQ(basis.kind, Kind::Basis);
  EXPECT_EQ(basis.legs[0].curve, 2);
  EXPECT_EQ(basis.legs[1].curve, 3);
  EXPECT_EQ(basis.legs[0].disc_curve, 1);
  // explicit knot tenors
  CurveDefinition d = b.curve("EUR-ESTR");
  d.knot_tenors = {"1Y", "2Y", "5Y", "10Y", "30Y"};
  const CalibrationSet k = build_calibration_set(c.ctx, b, d);
  EXPECT_EQ(k.knot_t.size(), 5u);
  EXPECT_EQ(k.n_instruments(), 18);
  // two instruments at one maturity is an error
  d = b.curve("EUR-ESTR");
  d.instruments.push_back(d.instruments.back());
  EXPECT_THROW(build_calibration_set(c.ctx, b, d), BlueprintError);
}
