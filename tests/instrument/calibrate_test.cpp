// M3/G2 -> G4: the curve definitions of blueprints/curves/*.json as calibration instruments of a
// solver::CurveSet (maths/instrument/calibrate.hpp). Quotes that are the par quotes of a
// generating curve recover it through the implicit node (PROBLEM.md §6 O1) — the USD SOFR set
// (the overnight fixing, front OIS, eight SR3 futures, OIS to 30Y) alone, and the three EUR
// curves together, where the CurveSet discovers from the maths that the 6M curve reads €STR
// and the 3M curve reads both (sequential blocks in that order); a book trade priced off the
// calibrated Recs on the same tape.
#include <gtest/gtest.h>

#include <cmath>
#include <iostream>
#include <string>
#include <vector>

#include "epykos/maths/instrument/calibrate.hpp"
#include "epykos/scalar/rec.hpp"
#include "epykos/solver/curve_set.hpp"
#include "epykos/solver/implicit.hpp"
#include "epykos/tape/tape.hpp"
#include "instrument/instrument_test_helpers.hpp"

using namespace epykos;
using namespace epykos::conventions;
using namespace epykos::instrument;
using epykos::test::date;

namespace {

const Date valuation = date("2026-09-23");

// The generating curves: linear zero on the set's own knots, slot s from base_s rising 60 bp.
std::vector<double> generating_state(const CalibrationSet& cs, double base) {
  std::vector<double> z;
  for (std::size_t k = 0; k < cs.knot_t.size(); ++k) z.push_back(base + 0.006 * static_cast<double>(k) / static_cast<double>(cs.knot_t.size() - 1));
  return z;
}

}  // namespace

TEST(Calibrate, UsdSofrSetRecoversTheGeneratingCurve) {
  const epykos::test::Context c(valuation);
  CurveSlots one;
  one.add("USD-SOFR");
  BuildContext ctx = c.ctx;
  ctx.curves = &one;
  const CalibrationSet cs = build_calibration_set(ctx, epykos::test::blueprints(), epykos::test::blueprints().curve("USD-SOFR"));
  ASSERT_EQ(cs.slot, 0);
  const std::vector<double> z_gen = generating_state(cs, 0.038);
  auto df_gen = [&](int, double t) { return curve::linear::df(cs.knot_t.data(), z_gen.data(), static_cast<int>(cs.knot_t.size()), t); };
  const std::vector<double> quotes = par_quotes(cs, df_gen);
  ASSERT_EQ(quotes.size(), cs.knot_t.size());
  // futures quotes are prices near 96, the rest rates near 4%
  for (std::size_t i = 0; i < quotes.size(); ++i) {
    if (cs.instruments[i].instrument.kind == Kind::Future) {
      EXPECT_GT(quotes[i], 95.0) << cs.keys()[i];
      EXPECT_LT(quotes[i], 97.0) << cs.keys()[i];
    } else {
      EXPECT_GT(quotes[i], 0.03) << cs.keys()[i];
      EXPECT_LT(quotes[i], 0.05) << cs.keys()[i];
    }
  }
  solver::CurveSet set;
  const std::vector<double> start(cs.knot_t.size(), 0.03);
  const int curve = add_calibration_set(set, cs, start);
  EXPECT_EQ(curve, 0);
  EXPECT_EQ(set.n_instruments(), cs.n_instruments());
  EXPECT_EQ(set.instrument(4).label, "SR3 Z26");
  // the residuals at the generating state are zero
  const solver::CurveStates<double> states = set.states_at(z_gen);
  for (double r : set.residuals(states, quotes)) EXPECT_NEAR(r, 0.0, 1e-15);
  Tape tape;
  solver::ImplicitRegistry registry;
  std::vector<double> z_solved;
  double book_pv = 0.0;
  {
    Tape::Scope scope(tape);
    std::vector<Rec> q;
    for (double v : quotes) q.push_back(make_input(tape, v));
    const solver::CurveSet::Calibration cal = set.calibrate(tape, registry, q);
    ASSERT_EQ(cal.results.size(), 1u);
    EXPECT_TRUE(cal.results[0].report.converged);
    EXPECT_LT(cal.results[0].report.jtr_inf, 1e-12);
    for (const Rec& zk : cal.states.curve(0)) z_solved.push_back(zk.v);
    // a book trade off the calibrated knots, on the same tape
    Trade t;
    t.blueprint = "USD-SOFR-OIS-SHIFT2-LOCKOUT2";
    t.tenor = Period::parse("7Y");
    t.notional = 1.0e7;
    t.fixed_rate = 0.04;
    const Instrument in = build_instrument(ctx, epykos::test::blueprints(), t);
    const std::vector<Rec>& zr = cal.states.curve(0);
    const Rec pv_rec = pv<Rec>(in, [&](int, double tt) { return curve::linear::df(cs.knot_t.data(), zr.data(), static_cast<int>(cs.knot_t.size()), tt); });
    register_output(tape, pv_rec);
    book_pv = pv_rec.v;
  }
  tape.validate();
  ASSERT_EQ(z_solved.size(), z_gen.size());
  double worst = 0.0;
  for (std::size_t k = 0; k < z_gen.size(); ++k) worst = std::max(worst, std::fabs(z_solved[k] - z_gen[k]));
  std::cout << "[ calibrate] USD-SOFR: " << cs.n_instruments() << " instruments, recovered the generating curve to " << worst
            << " from a flat 3% start (" << registry.blocks[0].residuals.size() << " residuals, " << tape.size() << " nodes); book pv " << book_pv << '\n';
  EXPECT_LT(worst, 1e-12);
  EXPECT_TRUE(std::isfinite(book_pv));
  EXPECT_GT(tape.num_outputs(), 0u);
}

TEST(Calibrate, EurCurvesCalibrateInDependencyOrder) {
  const epykos::test::Context c(valuation);
  CurveSlots eur;
  eur.add("EUR-ESTR");
  eur.add("EUR-EURIBOR-3M");
  eur.add("EUR-EURIBOR-6M");
  BuildContext ctx = c.ctx;
  ctx.curves = &eur;
  const Blueprints& b = epykos::test::blueprints();
  const CalibrationSet estr = build_calibration_set(ctx, b, b.curve("EUR-ESTR"));
  const CalibrationSet e3 = build_calibration_set(ctx, b, b.curve("EUR-EURIBOR-3M"));
  const CalibrationSet e6 = build_calibration_set(ctx, b, b.curve("EUR-EURIBOR-6M"));
  ASSERT_EQ(estr.slot, 0);
  ASSERT_EQ(e3.slot, 1);
  ASSERT_EQ(e6.slot, 2);
  const std::vector<double> z0 = generating_state(estr, 0.019), z1 = generating_state(e3, 0.021), z2 = generating_state(e6, 0.024);
  const CalibrationSet* sets[3] = {&estr, &e3, &e6};
  const std::vector<double>* zs[3] = {&z0, &z1, &z2};
  auto df_gen = [&](int slot, double t) {
    const CalibrationSet& s = *sets[slot];
    return curve::linear::df(s.knot_t.data(), zs[slot]->data(), static_cast<int>(s.knot_t.size()), t);
  };
  std::vector<double> quotes;
  for (const CalibrationSet* s : sets) {
    for (double q : par_quotes(*s, df_gen)) quotes.push_back(q);
  }
  solver::CurveSet set;
  add_calibration_set(set, estr, std::vector<double>(estr.knot_t.size(), 0.01));
  add_calibration_set(set, e3, std::vector<double>(e3.knot_t.size(), 0.01));
  add_calibration_set(set, e6, std::vector<double>(e6.knot_t.size(), 0.01));
  // discovered from the maths: the 6M swaps read €STR; the basis swaps read both; €STR reads itself only
  const std::vector<std::vector<int>> deps = set.dependencies();
  ASSERT_EQ(deps.size(), 3u);
  EXPECT_TRUE(deps[0].empty());
  EXPECT_EQ(deps[1], (std::vector<int>{0, 2}));
  EXPECT_EQ(deps[2], (std::vector<int>{0}));
  const std::vector<std::vector<int>> blocks = set.blocks(solver::CurveSet::Mode::sequential);
  ASSERT_EQ(blocks.size(), 3u);
  EXPECT_EQ(blocks[0], (std::vector<int>{0}));
  EXPECT_EQ(blocks[1], (std::vector<int>{2}));
  EXPECT_EQ(blocks[2], (std::vector<int>{1}));
  Tape tape;
  solver::ImplicitRegistry registry;
  std::vector<std::vector<double>> solved(3);
  {
    Tape::Scope scope(tape);
    std::vector<Rec> q;
    for (double v : quotes) q.push_back(make_input(tape, v));
    const solver::CurveSet::Calibration cal = set.calibrate(tape, registry, q);
    ASSERT_EQ(cal.results.size(), 3u);
    for (const solver::ImplicitResult& r : cal.results) {
      EXPECT_TRUE(r.report.converged);
      EXPECT_LT(r.report.jtr_inf, 1e-12);
    }
    for (int s = 0; s < 3; ++s) {
      for (const Rec& zk : cal.states.curve(s)) solved[static_cast<std::size_t>(s)].push_back(zk.v);
    }
  }
  for (int s = 0; s < 3; ++s) {
    double worst = 0.0;
    for (std::size_t k = 0; k < zs[s]->size(); ++k) worst = std::max(worst, std::fabs(solved[static_cast<std::size_t>(s)][k] - (*zs[s])[k]));
    std::cout << "[ calibrate] " << sets[s]->curve << ": " << sets[s]->n_instruments() << " instruments, recovered to " << worst << '\n';
    EXPECT_LT(worst, 1e-11) << sets[s]->curve;
  }
}
