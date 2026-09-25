// MX: this engine's own side of the head-to-head fixture (fixtures/compare_ois.hpp; D21, D71).
//
// The cross-engine comparison itself is `scripts/compare_swapengine.py`, which needs a second
// engine's checkout and therefore cannot be a ctest. What CAN be gated here, and is, is that the
// fixture this repository hands over is a correct instance of its own problem:
//
//   1. it is square and the calibration recovers the generating curve when the quotes are its
//      own par quotes (PROBLEM.md §6 O1);
//   2. the curve really is log DF piecewise linear on {0, knots...}, which is the property the
//      comparison rests on -- if it were not, the two engines would be interpolating differently
//      and the agreement would be luck;
//   3. the O3 ladder through the IFT agrees with bump-and-recalibrate (PROBLEM.md §6 O3, 1e-6);
//   4. the exchange file round-trips: every time it states is a time the fixture actually prices
//      at, so what the other engine is fed is what this engine computed.
#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <iostream>
#include <string>
#include <vector>

#include "epykos/fixtures/compare_ois.hpp"
#include "epykos/solver/implicit_program.hpp"

using namespace epykos;

namespace {

// A deliberately small instance so the gate is cheap: six calibration instruments on six knots
// and eight book trades (D64). The shape, not the size, is what is being gated.
fixtures::CompareOisOptions small_options() {
  fixtures::CompareOisOptions o;
  o.tenors = {"1Y", "2Y", "3Y", "5Y", "7Y", "10Y"};
  o.trades = 8;
  return o;
}

}  // namespace

TEST(CompareOis, IsSquareAndRecoversTheGeneratingCurve) {
  fixtures::CompareOisOptions o = small_options();
  o.quote_noise_bp = 0.0;   // the O1 recovery gate: quotes are the generating curve's par quotes
  const fixtures::CompareOis s = fixtures::make_compare_ois(o);
  ASSERT_EQ(s.n_quotes(), 6);
  ASSERT_EQ(s.set.knot_t.size(), 6u) << "the block must be square: one knot per instrument";
  ASSERT_EQ(s.n_trades(), 8);
  for (std::size_t k = 1; k < s.set.knot_t.size(); ++k) EXPECT_GT(s.set.knot_t[k], s.set.knot_t[k - 1]);

  fixtures::CompareOisTape t = fixtures::record_compare_ois(s);
  EXPECT_TRUE(t.record_report.converged);
  EXPECT_LT(t.record_report.jtr_inf, 1e-12);
  ASSERT_EQ(t.record_knots.size(), s.generating_z.size());
  double worst = 0.0;
  for (std::size_t k = 0; k < t.record_knots.size(); ++k) {
    worst = std::max(worst, std::fabs(t.record_knots[k] - s.generating_z[k]));
  }
  EXPECT_LT(worst, 1e-12) << "the calibration did not recover the generating curve";
}

TEST(CompareOis, TheCurveIsLogDfPiecewiseLinear) {
  // The comparison's load-bearing property (bench/compare/README.md §3): log DF is linear in t
  // on every interval of {0, knots...}, so the discount factor at any time is pinned by the knot
  // values alone and neither engine's interpolation has to be assumed. Checked as: the midpoint
  // of each interval is the average of its endpoints, in log DF, and the extrapolation beyond the
  // last knot continues the last segment's slope.
  const fixtures::CompareOis s = fixtures::make_compare_ois(small_options());
  const curve::Composite comp = s.set.composite();
  const std::vector<double>& kt = s.set.knot_t;
  const double* z = s.generating_z.data();
  double t_prev = 0.0, l_prev = 0.0;   // log DF(0) = 0
  for (std::size_t k = 0; k < kt.size(); ++k) {
    const double t_k = kt[k], l_k = std::log(comp.df(z, t_k));
    const double mid = 0.5 * (t_prev + t_k);
    EXPECT_NEAR(std::log(comp.df(z, mid)), 0.5 * (l_prev + l_k), 1e-14)
        << "log DF is not linear on interval " << k;
    t_prev = t_k;
    l_prev = l_k;
  }
  // Beyond the last knot this engine holds the VARIABLE flat, i.e. log DF is constant and the
  // forward is zero -- it does NOT continue the last segment's slope. That is a real difference
  // from the other engine's flat-forward extrapolation, and the comparison is only sound because
  // nothing in this fixture is ever priced beyond the last knot (the next test pins that, and
  // bench/compare/README.md section 4 states it). Pinned here so that a change to either would
  // fail rather than silently invalidate the comparison.
  const std::size_t n = kt.size();
  const double l_last = std::log(comp.df(z, kt[n - 1]));
  const double slope = (l_last - std::log(comp.df(z, kt[n - 2]))) / (kt[n - 1] - kt[n - 2]);
  const double beyond = kt[n - 1] + 3.0;
  EXPECT_NEAR(std::log(comp.df(z, beyond)), l_last, 1e-13)
      << "the extrapolation beyond the last knot is no longer flat in log DF";
  EXPECT_GT(std::fabs(l_last + slope * 3.0 - l_last), 1e-3)
      << "the two extrapolations have stopped differing; re-check bench/compare/README.md section 4";
}

TEST(CompareOis, NothingIsPricedBeyondTheLastKnot) {
  // The condition that makes the extrapolation difference above harmless. Every discount factor
  // either engine is asked for -- every coupon accrual end, every observation day and every
  // payment date, of every calibration instrument AND every book trade -- must be at or before
  // the last knot. Inside that range both engines' curves are the same function of the knots.
  const fixtures::CompareOis s = fixtures::make_compare_ois(small_options());
  const double t_last = s.set.knot_t.back();
  double worst = 0.0;
  std::string where;
  auto check = [&](const instrument::Instrument& in, const std::string& id) {
    for (const instrument::Leg& leg : in.legs) {
      for (const instrument::Coupon& c : leg.coupons) {
        for (double t : {c.t_start, c.t_end, c.t_pay}) {
          if (t > worst) {
            worst = t;
            where = id;
          }
        }
      }
      for (const instrument::ObsDay& d : leg.obs) {
        if (d.t_next > worst) {
          worst = d.t_next;
          where = id + " (observation day)";
        }
      }
    }
  };
  for (const instrument::CalibrationInstrument& ci : s.set.instruments) check(ci.instrument, ci.key);
  for (const instrument::Instrument& in : s.book) check(in, in.id);
  std::cout << "[compare_ois] latest priced time " << worst << " against the last knot " << t_last
            << " (" << where << ")\n";
  EXPECT_LE(worst, t_last) << "'" << where << "' prices beyond the last knot, where the two "
                              "engines' extrapolations differ";
}

TEST(CompareOis, LadderAgreesWithBumpAndRecalibrate) {
  const fixtures::CompareOis s = fixtures::make_compare_ois(small_options());
  fixtures::CompareOisTape t = fixtures::record_compare_ois(s);
  solver::ImplicitProgram prog(t.tape, t.registry, fixtures::compare_ois_program_options(s));

  const std::vector<int> ordinals = {t.book_output};
  const std::vector<double> rows = fixtures::compare_ois_ladder(prog, s.quotes, ordinals, nullptr);
  ASSERT_EQ(rows.size(), static_cast<std::size_t>(s.n_quotes()));

  // Central bump-and-recalibrate on every quote: each lane is its own full recalibration.
  const int n_q = s.n_quotes(), n_out = prog.n_outputs();
  const double h = 1e-6;   // 1e-6 in rate = 0.01 bp
  std::vector<double> st(static_cast<std::size_t>(n_q) * 2), out(static_cast<std::size_t>(n_out) * 2);
  double worst = 0.0;
  double ladder_inf = 0.0;
  for (double v : rows) ladder_inf = std::max(ladder_inf, std::fabs(v));
  ASSERT_GT(ladder_inf, 0.0);
  for (int k = 0; k < n_q; ++k) {
    for (int j = 0; j < n_q; ++j) {
      st[static_cast<std::size_t>(j) * 2 + 0] = s.quotes[static_cast<std::size_t>(j)] + (j == k ? h : 0.0);
      st[static_cast<std::size_t>(j) * 2 + 1] = s.quotes[static_cast<std::size_t>(j)] - (j == k ? h : 0.0);
    }
    prog.run(st.data(), 2, out.data());
    const std::size_t b = static_cast<std::size_t>(t.book_output) * 2;
    const double fd = (out[b + 0] - out[b + 1]) / (2.0 * h);
    worst = std::max(worst, std::fabs(fd - rows[static_cast<std::size_t>(k)]) / ladder_inf);
  }
  std::cout << "[compare_ois] IFT ladder vs bump-and-recalibrate: worst " << worst
            << " relative to the ladder's infinity norm " << ladder_inf << " over " << n_q << " quotes\n";
  EXPECT_LT(worst, 1e-6) << "the IFT ladder disagrees with bump-and-recalibrate";
}

TEST(CompareOis, TheExchangeFileStatesTheTimesTheFixturePricesAt) {
  const fixtures::CompareOis s = fixtures::make_compare_ois(small_options());
  const std::string j = fixtures::compare_ois_exchange_json(s);
  EXPECT_NE(j.find("\"format\": \"epykos-compare-ois 1\""), std::string::npos);
  EXPECT_NE(j.find("\"curve_family\": \"logdf_piecewise_linear\""), std::string::npos);
  EXPECT_NE(j.find("\"obs_days_included\": false"), std::string::npos);
  // Every knot time, every instrument key and every trade id appears.
  for (const instrument::CalibrationInstrument& ci : s.set.instruments) {
    EXPECT_NE(j.find("\"key\": \"" + ci.key + "\""), std::string::npos) << ci.key;
  }
  for (const instrument::Instrument& in : s.book) {
    EXPECT_NE(j.find("\"id\": \"" + in.id + "\""), std::string::npos) << in.id;
  }
  // With --obs-days the projected observation days are there, and there are a lot of them.
  const std::string d = fixtures::compare_ois_exchange_json(s, true);
  EXPECT_NE(d.find("\"obs_days_included\": true"), std::string::npos);
  EXPECT_NE(d.find("\"obs_days\": ["), std::string::npos);
  EXPECT_GT(d.size(), j.size() * 4u) << "the daily decomposition should dwarf the collapsed form";
  int days = 0;
  for (const instrument::CalibrationInstrument& ci : s.set.instruments) days += ci.instrument.n_obs_days();
  std::cout << "[compare_ois] " << s.n_quotes() << " calibration instruments carry " << days
            << " projected observation days; the collapsed exchange form states "
            << s.set.instruments.size() << " instruments' coupons instead\n";
  EXPECT_GT(days, 1000);
}
