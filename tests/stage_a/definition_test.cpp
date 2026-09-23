// M3/G5: the Stage A problem definition (blueprints/problems/stage_a.json, D36) and what the
// fixture generates from it: the strict schema (an unknown key fails with file:line:column), the
// trade population in the blueprint's mix and proportions (about 20% seasoned, netting sets
// assigned), quotes at the stated plausible levels around the generating curves' par quotes,
// the scenario families as lanes (lane 0 unshocked; per-curve shocks touch one curve), the
// scheme variants resolve, and the CSV dump writes its files.
#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "epykos/maths/instrument/blueprint.hpp"
#include "stage_a/stage_a_test_helpers.hpp"

namespace fixtures = epykos::fixtures;
using epykos::test::stage_a;

TEST(StageADefinition, LoadsAndRejectsAnUnknownKey) {
  const fixtures::StageADefinition d = fixtures::load_stage_a_definition();
  EXPECT_EQ(d.name, "stage_a");
  EXPECT_EQ(epykos::conventions::to_iso(d.valuation), "2026-09-23");
  EXPECT_EQ(d.curves.size(), 4u);
  EXPECT_EQ(d.curves[0].definition, "USD-SOFR");
  EXPECT_EQ(d.curves[0].variants.size(), 3u);
  EXPECT_EQ(d.book.trades, 2000);
  EXPECT_EQ(d.scenarios.count, 1000);
  EXPECT_EQ(d.scenarios.families.size(), 4u);
  double mix = 0.0;
  for (const auto& m : d.book.mix) mix += m.weight;
  EXPECT_NEAR(mix, 100.0, 1e-12);
  // A copy with an unknown key fails naming the place.
  std::ifstream in(d.path);
  std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  const std::string needle = "\"netting_sets\"";
  const std::size_t at = text.find(needle);
  ASSERT_NE(at, std::string::npos);
  text.insert(at, "\"nettng_sets\": 3, ");
  const std::string tmp = "stage_a_definition_bad.json";
  std::ofstream(tmp) << text;
  try {
    fixtures::load_stage_a_definition(tmp);
    FAIL() << "an unknown key was accepted";
  } catch (const epykos::instrument::BlueprintError& e) {
    const std::string what = e.what();
    EXPECT_NE(what.find("unknown key \"nettng_sets\""), std::string::npos) << what;
    EXPECT_NE(what.find(tmp + ":"), std::string::npos) << what;
    std::cout << "[  schema  ] " << what << '\n';
  }
}

TEST(StageADefinition, ThePopulationFollowsTheMix) {
  const fixtures::StageA& s = stage_a();
  std::map<std::string, int> counts;
  int seasoned = 0, usd = 0, eur = 0;
  std::vector<int> per_set(static_cast<std::size_t>(s.n_netting_sets()), 0);
  double notional_min = 1e300, notional_max = 0.0;
  for (const fixtures::StageATrade& t : s.trades) {
    ++counts[t.blueprint];
    seasoned += t.seasoned;
    ++per_set[static_cast<std::size_t>(t.netting_set)];
    (t.currency == "USD" ? usd : eur)++;
    notional_min = std::min(notional_min, *t.trade.notional);
    notional_max = std::max(notional_max, *t.trade.notional);
  }
  std::cout << "[  mix     ]";
  for (const auto& m : s.def.book.mix) {
    const double expected = m.weight / 100.0 * s.n_trades();
    std::cout << ' ' << m.name << ' ' << counts[m.name] << " (expected " << expected << ")";
    EXPECT_NEAR(static_cast<double>(counts[m.name]), expected, 0.15 * expected + 5.0) << m.name;
  }
  std::cout << "; seasoned " << seasoned << ", USD " << usd << ", EUR " << eur << ", notionals " << notional_min << ".." << notional_max << '\n';
  EXPECT_NEAR(static_cast<double>(seasoned) / s.n_trades(), s.def.book.seasoned_fraction, 0.04);
  for (int n = 0; n < s.n_netting_sets(); ++n) EXPECT_GT(per_set[static_cast<std::size_t>(n)], 0) << "netting set " << n;
  EXPECT_GE(notional_min, s.def.book.notional_min);
  EXPECT_LE(notional_max, s.def.book.notional_max);
  // Every mechanism of PROBLEM.md section 4 Stage A is in the book.
  int compounded = 0, averaged = 0, term = 0, current = 0, realised_term = 0, lockout_or_shift = 0;
  for (std::size_t i = 0; i < s.instruments.size(); ++i) {
    const epykos::instrument::Instrument& in = s.instruments[i];
    if (in.blueprint.find("SHIFT2") != std::string::npos) ++lockout_or_shift;
    for (const epykos::instrument::Leg& leg : in.legs) {
      for (const epykos::instrument::Coupon& c : leg.coupons) {
        compounded += c.kind == epykos::instrument::CouponKind::RfrCompounded;
        averaged += c.kind == epykos::instrument::CouponKind::RfrAveraged;
        term += c.kind == epykos::instrument::CouponKind::TermRate;
        current += c.current;
        realised_term += c.realised;
      }
    }
  }
  std::cout << "[  coupons ] compounded " << compounded << ", averaged " << averaged << ", term " << term << ", current (seasoned) " << current
            << ", realised term fixings " << realised_term << ", trades with shift / lockout " << lockout_or_shift << '\n';
  EXPECT_GT(compounded, 1000);
  EXPECT_GT(averaged, 100);
  EXPECT_GT(term, 1000);
  EXPECT_GT(current, 300);
  EXPECT_GT(realised_term, 50);
  EXPECT_GT(lockout_or_shift, 200);
}

TEST(StageADefinition, QuotesAreParPlusNoiseAtPlausibleLevels) {
  const fixtures::StageA& s = stage_a();
  auto df_gen = [&](int slot, double t) { return s.generating_df(slot, t); };
  std::size_t k = 0;
  double worst_noise = 0.0;
  for (std::size_t c = 0; c < s.sets.size(); ++c) {
    const std::vector<double> par = epykos::instrument::par_quotes(s.sets[c], df_gen);
    for (std::size_t i = 0; i < par.size(); ++i, ++k) {
      const double q = s.quotes[k];
      if (s.quote_is_price[k]) {
        EXPECT_GT(q, 94.0) << s.quote_keys[k];
        EXPECT_LT(q, 99.0) << s.quote_keys[k];
        EXPECT_LE(std::fabs(q - par[i]), s.def.quotes.futures_noise_price + 1e-12) << s.quote_keys[k];
        worst_noise = std::max(worst_noise, std::fabs(q - par[i]) / 0.01 * 1e-4);
      } else {
        EXPECT_GT(q, 0.0) << s.quote_keys[k];
        EXPECT_LT(q, 0.06) << s.quote_keys[k];
        EXPECT_LE(std::fabs(q - par[i]), s.def.quotes.noise_bp * 1e-4 + 1e-15) << s.quote_keys[k];
        worst_noise = std::max(worst_noise, std::fabs(q - par[i]));
      }
      EXPECT_EQ(s.quote_curve[k], static_cast<int>(c));
    }
  }
  EXPECT_EQ(k, s.quotes.size());
  EXPECT_GT(worst_noise, 0.5e-4) << "the quotes are a fit, not the par rates";
  std::cout << "[  quotes  ] " << s.n_quotes() << " quotes; USD-SOFR 1D " << s.quotes[0] << ", 30Y " << s.quotes[21] << "; EUR-ESTR 1D " << s.quotes[22]
            << "; worst noise " << worst_noise * 1e4 << " bp\n";
  // Noise 0: the par quotes exactly.
  fixtures::StageAOptions o;
  o.trades = 0;
  o.scenarios = 0;
  o.quote_noise_bp = 0.0;
  const fixtures::StageA exact = fixtures::make_stage_a(o);
  k = 0;
  for (std::size_t c = 0; c < exact.sets.size(); ++c) {
    for (double p : epykos::instrument::par_quotes(exact.sets[c], [&](int slot, double t) { return exact.generating_df(slot, t); })) EXPECT_EQ(exact.quotes[k++], p);
  }
}

TEST(StageADefinition, ScenarioLanesFollowTheFamilies) {
  const fixtures::StageA& s = stage_a();
  ASSERT_EQ(s.n_scenarios(), 1000);
  std::map<std::string, int> per_family;
  for (const fixtures::StageAScenario& sc : s.scenarios) ++per_family[sc.family];
  for (const auto& f : s.def.scenarios.families) EXPECT_EQ(per_family[f.name], f.count) << f.name;
  EXPECT_EQ(s.scenario_quotes[0], s.quotes) << "lane 0 is the base";
  for (std::size_t l = 0; l < s.scenarios.size(); ++l) {
    const fixtures::StageAScenario& sc = s.scenarios[l];
    const std::vector<double>& q = s.scenario_quotes[l];
    int shocked_curves = 0;
    for (int c = 0; c < s.n_curves(); ++c) {
      bool touched = false;
      for (std::size_t k = 0; k < q.size(); ++k) touched |= s.quote_curve[k] == c && q[k] != s.quotes[k];
      shocked_curves += touched;
    }
    if (sc.family == "per_curve") {
      EXPECT_LE(shocked_curves, 1) << "lane " << l;
    } else if (sc.size != 0.0) {
      EXPECT_EQ(shocked_curves, s.n_curves()) << "lane " << l << ' ' << sc.family;
    }
    for (std::size_t k = 0; k < q.size(); ++k) {
      const double shift = s.quote_is_price[k] ? (s.quotes[k] - q[k]) / 100.0 : q[k] - s.quotes[k];
      EXPECT_LE(std::fabs(shift), 0.0101) << "lane " << l;
    }
  }
  std::cout << "[  grid    ] 1000 lanes: parallel " << per_family["parallel"] << ", twist " << per_family["twist"] << ", butterfly " << per_family["butterfly"]
            << ", per_curve " << per_family["per_curve"] << "; lane 1 " << s.scenarios[1].family << ' ' << s.scenarios[1].size * 1e4 << " bp\n";
}

TEST(StageADefinition, VariantsResolveAndTheDumpWrites) {
  const fixtures::StageA& s = stage_a();
  for (const std::string& v : s.def.curves[0].variants) {
    fixtures::StageAOptions o;
    o.usd_curve = v;
    o.trades = 0;
    o.scenarios = 0;
    const fixtures::StageA x = fixtures::make_stage_a(o);
    EXPECT_EQ(x.curve_definitions[0], v);
    EXPECT_EQ(x.quotes, s.quotes) << "the same market for every variant";
    EXPECT_FALSE(x.sets[0].regions.empty());
  }
  EXPECT_THROW(fixtures::make_stage_a([] { fixtures::StageAOptions o; o.usd_curve = "EUR-ESTR"; o.trades = 0; return o; }()), epykos::instrument::BlueprintError);
  const std::vector<std::string> files = fixtures::stage_a_dump(s, nullptr, "stage_a_dump_");
  EXPECT_EQ(files.size(), 4u);
  for (const std::string& f : files) {
    std::ifstream in(f);
    EXPECT_TRUE(in.good()) << f;
    std::string line;
    EXPECT_TRUE(static_cast<bool>(std::getline(in, line))) << f;
  }
}
