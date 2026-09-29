// M3/G2: swaps at par have zero PV (PROBLEM.md §6 "par swaps at zero PV") for every Stage A
// swap blueprint — SOFR OIS (plain, shifted, shifted + locked out), the SOFR averaging swap,
// €STR OIS, EURIBOR 3M / 6M swaps and the 3s6s basis swap — spot-starting and seasoned; the
// residual is zero at the par quote; sides and spreads behave; the conventions show in the
// tables (frequencies, day counts, lags, stubs, TERM tenors).
#include <gtest/gtest.h>

#include <cmath>
#include <string>
#include <vector>

#include "instrument/instrument_test_helpers.hpp"

using namespace epykos;
using namespace epykos::conventions;
using namespace epykos::instrument;
using epykos::test::date;

namespace {

const Date valuation = date("2026-09-23");

const char* swap_blueprints[] = {"USD-SOFR-OIS", "USD-SOFR-OIS-SHIFT2", "USD-SOFR-OIS-SHIFT2-LOCKOUT2", "USD-SOFR-AVG-SWAP",
                                 "EUR-ESTR-OIS", "EUR-EURIBOR-3M-IRS", "EUR-EURIBOR-6M-IRS", "EUR-3S6S-BASIS"};

double leg_scale(const Instrument& in, const epykos::test::FlatCurves& flat) {
  return std::fabs(leg_pv<double>(in.legs[0], flat)) + std::fabs(leg_pv<double>(in.legs[1], flat));
}

}  // namespace

TEST(Swap, ParSwapsHaveZeroPv) {
  const epykos::test::Context c(valuation);
  const epykos::test::FlatCurves flat;
  for (const char* bp : swap_blueprints) {
    for (const char* tenor : {"6M", "1Y", "2Y", "5Y", "10Y"}) {
      Trade t;
      t.blueprint = bp;
      t.tenor = Period::parse(tenor);
      t.notional = 1.0e7;
      const Instrument probe = c.build(t);
      const double p = par<double>(probe, flat);
      if (probe.kind == Kind::Basis) {
        t.spread = p;
      } else {
        t.fixed_rate = p;
      }
      for (int side : {+1, -1}) {
        t.side = side;
        const Instrument in = c.build(t);
        const double v = pv<double>(in, flat);
        EXPECT_NEAR(v, 0.0, 1e-12 * leg_scale(in, flat)) << bp << " " << tenor << " side " << side;
        EXPECT_NEAR(residual<double>(in, flat, p), 0.0, 1e-15) << bp << " " << tenor;
        // paying the par rate + 1 bp is worth −1 bp of annuity to the receiver
        Trade up = t;
        if (in.kind == Kind::Basis) {
          up.spread = p + 1e-4;
        } else {
          up.fixed_rate = p + 1e-4;
        }
        const Instrument iu = c.build(up);
        EXPECT_NEAR(pv<double>(iu, flat), side * 1e-4 * leg_annuity<double>(iu.legs[0], flat), 1e-9 * in.notional) << bp << " " << tenor;
      }
    }
  }
}

TEST(Swap, SeasonedParSwapsHaveZeroPv) {
  const epykos::test::Context c(valuation);
  const epykos::test::FlatCurves flat;
  for (const char* bp : swap_blueprints) {
    Trade t;
    t.blueprint = bp;
    t.effective = date("2024-11-29");   // a month-end effective date: EOM rolls
    t.tenor = Period::parse("5Y");
    t.notional = 2.5e7;
    const Instrument probe = c.build(t);
    EXPECT_TRUE(probe.legs[1].coupons.front().current) << bp;
    EXPECT_EQ(to_iso(probe.legs[0].schedule.front()), "2024-11-29") << bp;
    EXPECT_GT(probe.legs[0].schedule.size(), probe.legs[0].coupons.size() + 1u) << bp << ": paid coupons were dropped";
    const double p = par<double>(probe, flat);
    if (probe.kind == Kind::Basis) {
      t.spread = p;
    } else {
      t.fixed_rate = p;
    }
    const Instrument in = c.build(t);
    EXPECT_NEAR(pv<double>(in, flat), 0.0, 1e-12 * leg_scale(in, flat)) << bp;
    // accrued interest is reported on both legs of the current period
    EXPECT_NE(leg_accrued(in.legs[0]), 0.0) << bp;
    EXPECT_NE(leg_accrued(in.legs[1]), 0.0) << bp;
  }
}

TEST(Swap, ConventionsShowInTheTables) {
  const epykos::test::Context c(valuation);
  // SOFR OIS 5Y: annual / annual, ACT/360, T+2 spot, 2 day payment delay, plain observation
  {
    Trade t;
    t.blueprint = "USD-SOFR-OIS";
    t.tenor = Period::parse("5Y");
    const Instrument in = c.build(t);
    EXPECT_EQ(in.kind, Kind::Ois);
    EXPECT_EQ(to_iso(in.effective), "2026-09-25");
    EXPECT_EQ(to_iso(in.termination), "2031-09-25");
    EXPECT_EQ(in.legs[0].coupons.size(), 5u);
    EXPECT_EQ(in.legs[1].coupons.size(), 5u);
    EXPECT_EQ(in.legs[0].day_count, DayCount::Act360);
    EXPECT_EQ(in.legs[1].payment_lag, 2);
    for (std::size_t j = 0; j < 5; ++j) {
      const Coupon& f = in.legs[0].coupons[j];
      const Coupon& v = in.legs[1].coupons[j];
      EXPECT_EQ(f.start, v.start);
      EXPECT_EQ(f.pay, v.pay);
      EXPECT_EQ(f.pay, epykos::test::registry().calendar("USD-SOFR-SWAP").add_business_days(f.end, 2));
      EXPECT_EQ(v.obs_start, v.start);
      EXPECT_EQ(v.obs_end_date, v.end);
      EXPECT_GE(v.obs_end - v.obs_begin, 245);
      EXPECT_LE(v.obs_end - v.obs_begin, 255);
    }
    EXPECT_EQ(in.t_maturity, c.ctx.time_of(in.legs[0].coupons.back().pay));
  }
  // SOFR OIS 6M: a TERM swap (one period on both legs)
  {
    Trade t;
    t.blueprint = "USD-SOFR-OIS";
    t.tenor = Period::parse("6M");
    const Instrument in = c.build(t);
    EXPECT_EQ(in.legs[0].coupons.size(), 1u);
    EXPECT_EQ(in.legs[1].coupons.size(), 1u);
  }
  // 18M SOFR OIS: a short front stub (annual roll anchored on the termination)
  {
    Trade t;
    t.blueprint = "USD-SOFR-OIS";
    t.tenor = Period::parse("18M");
    const Instrument in = c.build(t);
    EXPECT_EQ(in.legs[0].coupons.size(), 2u);
    EXPECT_LT(in.legs[0].coupons[0].accrual, 0.6);
    EXPECT_GT(in.legs[0].coupons[1].accrual, 0.95);
  }
  // shifted observation: the observation period starts two SOFR business days before the accrual
  {
    Trade t;
    t.blueprint = "USD-SOFR-OIS-SHIFT2";
    t.tenor = Period::parse("1Y");
    const Instrument in = c.build(t);
    const Coupon& v = in.legs[1].coupons[0];
    EXPECT_EQ(to_iso(v.start), "2026-09-25");
    EXPECT_EQ(to_iso(v.obs_start), "2026-09-23");
    EXPECT_EQ(in.legs[1].obs[static_cast<std::size_t>(v.obs_begin)].rate_date, valuation);
    EXPECT_NE(v.obs_tau, v.accrual);
  }
  // lockout: the last two observation days carry the third-last day's rate
  {
    Trade t;
    t.blueprint = "USD-SOFR-OIS-SHIFT2-LOCKOUT2";
    t.tenor = Period::parse("1Y");
    const Instrument in = c.build(t);
    const Coupon& v = in.legs[1].coupons[0];
    const ObsDay* d = in.legs[1].obs.data() + v.obs_end;
    EXPECT_EQ(d[-1].rate_date, d[-3].rate_date);
    EXPECT_EQ(d[-2].rate_date, d[-3].rate_date);
    EXPECT_NE(d[-4].rate_date, d[-3].rate_date);
  }
  // EURIBOR 3M IRS 2Y: fixed annual 30/360, float quarterly ACT/360, no payment delay, T+2
  {
    Trade t;
    t.blueprint = "EUR-EURIBOR-3M-IRS";
    t.tenor = Period::parse("2Y");
    const Instrument in = c.build(t);
    EXPECT_EQ(in.kind, Kind::Irs);
    EXPECT_EQ(in.legs[0].coupons.size(), 2u);
    EXPECT_EQ(in.legs[1].coupons.size(), 8u);
    EXPECT_EQ(in.legs[0].day_count, DayCount::Thirty360US);
    EXPECT_EQ(in.legs[1].day_count, DayCount::Act360);
    EXPECT_EQ(in.legs[0].coupons[0].pay, in.legs[0].coupons[0].end);
    EXPECT_EQ(in.legs[1].curve, 2);
    EXPECT_EQ(in.legs[0].disc_curve, 1);
    EXPECT_EQ(in.legs[1].disc_curve, 1);
  }
  // 3s6s basis 2Y: the spread on the quarterly 3M leg, the 6M leg flat, both €STR-discounted
  {
    Trade t;
    t.blueprint = "EUR-3S6S-BASIS";
    t.tenor = Period::parse("2Y");
    t.spread = 0.0025;
    const Instrument in = c.build(t);
    EXPECT_EQ(in.kind, Kind::Basis);
    EXPECT_TRUE(in.legs[0].spread_leg);
    EXPECT_FALSE(in.legs[1].spread_leg);
    EXPECT_EQ(in.legs[0].coupons.size(), 8u);
    EXPECT_EQ(in.legs[1].coupons.size(), 4u);
    EXPECT_EQ(in.legs[0].curve, 2);
    EXPECT_EQ(in.legs[1].curve, 3);
    for (const Coupon& q : in.legs[0].coupons) EXPECT_EQ(q.spread, 0.0025);
    for (const Coupon& q : in.legs[1].coupons) EXPECT_EQ(q.spread, 0.0);
    const epykos::test::FlatCurves flat;
    // receiving 3M + spread against 6M flat: pv = (spread − par spread) · annuity(3M leg)
    const double p = par<double>(in, flat);
    EXPECT_NEAR(pv<double>(in, flat), (0.0025 - p) * leg_annuity<double>(in.legs[0], flat), 1e-9 * in.notional);
  }
}

TEST(Swap, ParRatesOnTheLinearCurvesAreSensible) {
  // Rates near the curve's level on the linear-zero test curves (the recording fixture's curves).
  const epykos::test::Context c(valuation);
  const std::vector<double> z = epykos::test::test_state();
  epykos::test::LinearCurves<double> curves;
  curves.state = z.data();
  Trade t;
  t.blueprint = "USD-SOFR-OIS";
  t.tenor = Period::parse("5Y");
  const double p = par<double>(c.build(t), curves);
  EXPECT_GT(p, 0.035);
  EXPECT_LT(p, 0.045);
  t.blueprint = "EUR-3S6S-BASIS";
  const double s = par<double>(c.build(t), curves);
  EXPECT_GT(s, 0.002);
  EXPECT_LT(s, 0.004);
}
