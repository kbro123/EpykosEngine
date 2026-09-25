// M3/G2: the coupon mechanics against closed forms and hand computations (PROBLEM.md §6 O2) —
// the telescoping identity of a plainly compounded OIS leg when projection = discount, a
// compounded coupon with a 2-day observation shift and a 2-day lockout against the hand
// computation of tests/conventions/rfr_test.cpp extended to a projected part, a lookback-only
// coupon likewise (M3-fix: closes the review gap that method="lookback" was never priced), the
// arithmetic average against an explicit average, a fixing-in-advance coupon equal to the
// realised fixing times the accrual when the fixing date is past, accrued interest, the futures
// price identity, and the EUR-ESTR-OIS payment-lag variant (M3-fix) against the 2-day standard.
// The double instantiation of the templated maths is the side under test.
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

Trade swap_trade(const char* blueprint, const char* tenor, double K = 0.03, double spread = 0.0) {
  Trade t;
  t.blueprint = blueprint;
  t.tenor = Period::parse(tenor);
  t.fixed_rate = K;
  t.spread = spread;
  return t;
}

// The float leg's telescoped value with projection = discount: Σ_j N (DF(s_j)/DF(e_j) − 1) DF(p_j),
// a seasoned current coupon telescoping over its projected days only:
// N τ_acc ((F_real · DF(t_u)/DF(e) − 1) / τ_obs) DF(p), t_u the first projected day.
template <class DF>
double telescoped_float_leg(const Leg& leg, DF&& df) {
  double pv = 0.0;
  for (const Coupon& c : leg.coupons) {
    const int slot = leg.curve;
    double factor = c.realised_factor;
    if (c.obs_end > c.obs_begin) {
      const ObsDay& first = leg.obs[static_cast<std::size_t>(c.obs_begin)];
      factor *= df(slot, first.t_rate) / df(slot, c.t_end);
    }
    pv += c.notional * c.accrual * ((factor - 1.0) / c.obs_tau) * df(leg.disc_curve, c.t_pay);
  }
  return pv;
}

// The hand-written history of the G0 test (SYNTHETIC values, labelled).
FixingsHistory march_history(const char* last_day) {
  FixingsHistory h;
  const char* days[] = {"2026-02-23", "2026-02-24", "2026-02-25", "2026-02-26", "2026-02-27", "2026-03-02", "2026-03-03",
                        "2026-03-04", "2026-03-05", "2026-03-06", "2026-03-09", "2026-03-10", "2026-03-11", "2026-03-12",
                        "2026-03-13", "2026-03-16"};
  const double rates[] = {0.0397, 0.0398, 0.0399, 0.0400, 0.0401, 0.0402, 0.0403, 0.0404,
                          0.0405, 0.0406, 0.0407, 0.0408, 0.0409, 0.0410, 0.0411, 0.0412};
  const Date last = parse_iso(last_day);
  for (int i = 0; i < 16; ++i) {
    if (parse_iso(days[i]) <= last) h.set("USD-SOFR", parse_iso(days[i]), rates[i]);
  }
  return h;
}

// One coupon 2026-03-02 -> 2026-03-16 of the shifted / locked-out convention (a TERM swap).
Trade march_trade(const char* blueprint) {
  Trade t;
  t.blueprint = blueprint;
  t.effective = date("2026-03-02");
  t.termination = date("2026-03-16");
  t.notional = 1.0e6;
  t.fixed_rate = 0.04;
  return t;
}

}  // namespace

TEST(Coupon, PlainCompoundedLegTelescopesWhenProjectionIsDiscount) {
  const epykos::test::Context c(valuation);
  const epykos::test::FlatCurves flat;
  for (const char* tenor : {"3M", "1Y", "2Y", "5Y", "10Y"}) {
    const Instrument in = c.build(swap_trade("USD-SOFR-OIS", tenor));
    ASSERT_EQ(in.legs.size(), 2u);
    const Leg& flt = in.legs[1];
    EXPECT_EQ(flt.curve, flt.disc_curve);
    const double natural = leg_pv<double>(flt, flat);
    const double closed = telescoped_float_leg(flt, flat);
    EXPECT_NEAR(natural, closed, 1e-13 * in.notional) << tenor;
    // and per coupon: N (DF(s)/DF(e) − 1) DF(p) with s = the first observation day, e = the period end
    for (const Coupon& cp : flt.coupons) {
      const double one = float_coupon_pv<double>(flt, cp, flat);
      const ObsDay& first = flt.obs[static_cast<std::size_t>(cp.obs_begin)];
      EXPECT_EQ(first.t_rate, cp.t_start) << "plain: the first observation day is the accrual start";
      EXPECT_EQ(cp.obs_tau, cp.accrual) << "plain: observation and accrual periods coincide (ACT/360)";
      const double closed_one = cp.notional * (flat(0, cp.t_start) / flat(0, cp.t_end) - 1.0) * flat(0, cp.t_pay);
      EXPECT_NEAR(one, closed_one, 1e-13 * in.notional) << tenor;
    }
  }
  // A seasoned trade: the current coupon's projected days telescope from the first projected day.
  Trade s = swap_trade("USD-SOFR-OIS", "3Y");
  s.effective = date("2025-06-30");
  const Instrument in = c.build(s);
  const Leg& flt = in.legs[1];
  ASSERT_GE(flt.coupons.size(), 2u);
  EXPECT_TRUE(flt.coupons[0].current);
  EXPECT_GT(flt.coupons[0].fixed_days, 0);
  EXPECT_GT(flt.coupons[0].realised_factor, 1.0);
  EXPECT_NEAR(leg_pv<double>(flt, flat), telescoped_float_leg(flt, flat), 1e-13 * in.notional);
}

TEST(Coupon, ZeroPaymentLagLegIsNotionalTimesDfDifference) {
  // A data-only variant of the SOFR OIS with no payment delay: Σ_j N (DF(s_j) − DF(e_j)) =
  // N (DF(s_0) − DF(e_last)) (PROBLEM.md §6: "telescoping OIS legs").
  const char* doc = R"({
    "meta": {"title": "test fixture: SOFR OIS without payment delay"},
    "instruments": {"USD-SOFR-OIS-PAY0": {"type": "ois", "currency": "USD", "calendar": "USD-SOFR-SWAP", "spot_lag": 2, "payment_lag": 0,
      "bdc": "ModifiedFollowing", "eom": true, "stub": "ShortFront",
      "legs": [{"role": "fixed", "frequency": "1Y", "term_up_to": "1Y", "day_count": "ACT/360"},
               {"role": "float", "index": "USD-SOFR", "compounding": "compounded", "frequency": "1Y", "term_up_to": "1Y", "day_count": "ACT/360"}],
      "discount_index": "USD-SOFR", "tenors": ["1Y", "5Y"]}}
  })";
  const std::string path = (std::filesystem::temp_directory_path() / "epykos_g2_pay0.json").string();
  std::ofstream(path) << doc;
  Registry reg = Registry::load();
  reg.add_file(path);
  std::remove(path.c_str());
  Blueprint bp;
  bp.name = "USD-SOFR-OIS-PAY0";
  bp.convention = "USD-SOFR-OIS-PAY0";
  bp.legs = {LegBlueprint{CouponKind::Fixed, {}, {}}, LegBlueprint{CouponKind::RfrCompounded, {}, {}}};
  const FixingsHistory h = epykos::test::synthetic_history(valuation);
  BuildContext ctx;
  ctx.registry = &reg;
  ctx.fixings = &h;
  ctx.curves = &epykos::test::slots();
  ctx.valuation = valuation;
  const epykos::test::FlatCurves flat;
  for (const char* tenor : {"1Y", "2Y", "5Y"}) {
    const Instrument in = build_instrument(ctx, bp, swap_trade("USD-SOFR-OIS-PAY0", tenor));
    const Leg& flt = in.legs[1];
    for (const Coupon& cp : flt.coupons) EXPECT_EQ(cp.pay, cp.end);
    const double natural = leg_pv<double>(flt, flat);
    const double first = flt.coupons.front().t_start, last = flt.coupons.back().t_end;
    EXPECT_NEAR(natural, in.notional * (flat(0, first) - flat(0, last)), 1e-13 * in.notional) << tenor;
  }
}

TEST(Coupon, ShiftAndLockoutByHandWithAProjectedPart) {
  // Accrual Mon 2026-03-02 -> Mon 2026-03-16, 2 business day observation shift and 2 day lockout:
  // observation period Thu 02-26 -> Thu 03-12 (tests/conventions/rfr_test.cpp). Valuation Thu
  // 2026-03-05 with SOFR published through Wed 03-04: five entries realised (02-26, 02-27 x3,
  // 03-02, 03-03, 03-04), five projected (03-05, 03-06 x3, 03-09, and the two locked-out days
  // 03-10 / 03-11 at 03-09's rate). On the flat curve DF(t) = exp(−z t) the projected rate FOR a
  // day r with span n calendar days is (exp(z n/365) − 1) · 360 / n.
  const Date val = date("2026-03-05");
  epykos::test::Context c(val, march_history("2026-03-04"));
  const Instrument in = c.build(march_trade("USD-SOFR-OIS-SHIFT2-LOCKOUT2"));
  ASSERT_EQ(in.legs.size(), 2u);
  const Leg& flt = in.legs[1];
  ASSERT_EQ(flt.coupons.size(), 1u);
  const Coupon& cp = flt.coupons[0];
  EXPECT_EQ(cp.kind, CouponKind::RfrCompounded);
  EXPECT_EQ(to_iso(cp.obs_start), "2026-02-26");
  EXPECT_EQ(to_iso(cp.obs_end_date), "2026-03-12");
  EXPECT_EQ(to_iso(cp.pay), "2026-03-18");   // two business day payment delay
  EXPECT_EQ(cp.fixed_days, 7);
  EXPECT_EQ(cp.obs_days, 14.0);
  EXPECT_EQ(cp.obs_end - cp.obs_begin, 5);
  EXPECT_TRUE(cp.current);
  double f_real = 1.0;
  f_real *= 1.0 + 0.0400 * 1.0 / 360.0;   // Thu 02-26
  f_real *= 1.0 + 0.0401 * 3.0 / 360.0;   // Fri 02-27 (Sat, Sun)
  f_real *= 1.0 + 0.0402 * 1.0 / 360.0;   // Mon 03-02
  f_real *= 1.0 + 0.0403 * 1.0 / 360.0;   // Tue 03-03
  f_real *= 1.0 + 0.0404 * 1.0 / 360.0;   // Wed 03-04
  EXPECT_DOUBLE_EQ(cp.realised_factor, f_real);
  const ObsDay* d = &flt.obs[static_cast<std::size_t>(cp.obs_begin)];
  EXPECT_EQ(to_iso(d[0].rate_date), "2026-03-05");
  EXPECT_EQ(to_iso(d[1].rate_date), "2026-03-06");
  EXPECT_EQ(to_iso(d[2].rate_date), "2026-03-09");
  EXPECT_EQ(to_iso(d[3].rate_date), "2026-03-09");
  EXPECT_EQ(to_iso(d[4].rate_date), "2026-03-09");
  EXPECT_EQ(d[0].weight_days, 1.0);
  EXPECT_EQ(d[1].weight_days, 3.0);
  EXPECT_EQ(d[1].tau_rate, 3.0 / 360.0);   // Friday's rate spans to Monday
  EXPECT_EQ(d[4].tau_rate, 1.0 / 360.0);   // the frozen Monday's own span is one day
  const double z = 0.04;
  auto flat = [&](int, double t) { return std::exp(-z * t); };
  auto fwd = [&](int n) { return (std::exp(z * n / 365.0) - 1.0) * 360.0 / n; };
  double f = f_real;
  f *= 1.0 + fwd(1) * 1.0 / 360.0;   // Thu 03-05
  f *= 1.0 + fwd(3) * 3.0 / 360.0;   // Fri 03-06 (Sat, Sun)
  f *= 1.0 + fwd(1) * 1.0 / 360.0;   // Mon 03-09
  f *= 1.0 + fwd(1) * 1.0 / 360.0;   // Tue 03-10: locked out, 03-09's rate
  f *= 1.0 + fwd(1) * 1.0 / 360.0;   // Wed 03-11: locked out, 03-09's rate
  const double hand_rate = (f - 1.0) * 360.0 / 14.0;
  const double t_pay = 13.0 / 365.0;   // 03-05 -> 03-18
  const double hand_pv = 1.0e6 * (14.0 / 360.0) * hand_rate * std::exp(-z * t_pay);
  EXPECT_NEAR(coupon_rate<double>(flt, cp, flat), hand_rate, 1e-15);
  EXPECT_NEAR(float_coupon_pv<double>(flt, cp, flat), hand_pv, 1e-9);
  EXPECT_NEAR(leg_pv<double>(flt, flat), hand_pv, 1e-9);
  // accrued to the valuation date: the realised compounded rate over the 7 fixed days times the
  // accrual from 03-02 to 03-05 (3 days ACT/360)
  const double r_real = (f_real - 1.0) * 360.0 / 7.0;
  EXPECT_NEAR(cp.accrued, 1.0e6 * r_real * 3.0 / 360.0, 1e-9);
  EXPECT_NEAR(leg_accrued(flt), cp.accrued, 0.0);
  // the fixed leg: N · 14/360 · K · DF(t_pay), accrued N · K · 3/360
  const Leg& fixed = in.legs[0];
  ASSERT_EQ(fixed.coupons.size(), 1u);
  EXPECT_NEAR(leg_pv<double>(fixed, flat), 1.0e6 * (14.0 / 360.0) * 0.04 * std::exp(-z * t_pay), 1e-9);
  EXPECT_NEAR(fixed.coupons[0].accrued, 1.0e6 * 0.04 * 3.0 / 360.0, 1e-9);
  // and the swap: side +1 receives fixed
  EXPECT_NEAR(pv<double>(in, flat), leg_pv<double>(fixed, flat) - hand_pv, 1e-9);
}

TEST(Coupon, ShiftAndLockoutFullyRealisedMatchesTheConventionsLayer) {
  // Valuation 03-10 with SOFR through 03-09: the lockout makes the coupon fully known
  // (rfr_test.cpp LockoutMakesTheCouponKnownEarly); the coupon rate is G0's compounded_rate.
  const Date val = date("2026-03-10");
  epykos::test::Context c(val, march_history("2026-03-09"));
  const Instrument in = c.build(march_trade("USD-SOFR-OIS-SHIFT2-LOCKOUT2"));
  const Leg& flt = in.legs[1];
  const Coupon& cp = flt.coupons[0];
  EXPECT_EQ(cp.obs_end, cp.obs_begin);
  EXPECT_EQ(cp.fixed_days, 14);
  ObservationSpec spec;
  spec.method = ObservationMethod::ObservationShift;
  spec.lookback_days = 2;
  spec.lockout_days = 2;
  const ObservationPeriod p = observation_days(date("2026-03-02"), date("2026-03-16"), spec, epykos::test::registry().calendar("USD-SOFR"));
  const double g0 = compounded_rate(p, c.fixings, "USD-SOFR");
  const epykos::test::FlatCurves flat;
  EXPECT_NEAR(coupon_rate<double>(flt, cp, flat), g0, 4e-16);
  // the average of the same days, G0's average_rate
  Blueprint avg = epykos::test::blueprints().blueprint("USD-SOFR-AVG-SWAP");
  avg.legs[1].observation = spec;
  const Instrument ia = c.build(avg, march_trade("USD-SOFR-AVG-SWAP"));
  EXPECT_NEAR(coupon_rate<double>(ia.legs[1], ia.legs[1].coupons[0], flat), average_rate(p, c.fixings, "USD-SOFR"), 4e-16);
}

TEST(Coupon, LookbackByHandWithAProjectedPart) {
  // M3-fix: ObservationMethod::Lookback (a 2 business day lookback, NO observation shift) was
  // implemented and unit-tested at the conventions layer only (tests/conventions/rfr_test.cpp
  // LookbackWithoutShift), never wired into a blueprint or priced — closing that gap with the
  // same accrual period as the shift/lockout test above (Mon 2026-03-02 -> Mon 2026-03-16) so the
  // two are directly comparable: under lookback the accrual days and weights are the PLAIN ones
  // (tests/conventions/rfr_test.cpp PlainObservationDays), but the rate applied on each accrual
  // day is the rate published two business days earlier (ARRC "lookback without observation
  // shift"). Valuation Thu 2026-03-05 with SOFR published through Wed 03-04: the accrual days
  // 03-02..03-06 look back to rate dates 02-26..03-04 (all realised); 03-09..03-13 look back to
  // rate dates 03-05..03-11 (all projected). USD-SOFR-OIS-LOOKBACK2 keeps USD-SOFR-OIS's 2
  // business day payment delay, so the payment date and the fixed leg are identical to the
  // shift/lockout test; only the float coupon's realised factor and projected forwards differ.
  const Date val = date("2026-03-05");
  epykos::test::Context c(val, march_history("2026-03-04"));
  const Instrument in = c.build(march_trade("USD-SOFR-OIS-LOOKBACK2"));
  ASSERT_EQ(in.legs.size(), 2u);
  const Leg& flt = in.legs[1];
  ASSERT_EQ(flt.coupons.size(), 1u);
  const Coupon& cp = flt.coupons[0];
  EXPECT_EQ(cp.kind, CouponKind::RfrCompounded);
  EXPECT_EQ(to_iso(cp.obs_start), "2026-03-02") << "lookback does not shift the observation period";
  EXPECT_EQ(to_iso(cp.obs_end_date), "2026-03-16");
  EXPECT_EQ(to_iso(cp.pay), "2026-03-18");   // two business day payment delay, as USD-SOFR-OIS
  EXPECT_EQ(cp.fixed_days, 7);
  EXPECT_EQ(cp.obs_days, 14.0);
  EXPECT_EQ(cp.obs_end - cp.obs_begin, 5);
  EXPECT_TRUE(cp.current);
  // Fixed part: rates published on 02-26, 02-27, 03-02, 03-03, 03-04 (two business days before the
  // accrual days 03-02..03-06), weighted by the ACCRUAL day's own span (rfr_test.cpp Rfr.LookbackWithoutShift).
  double f_real = 1.0;
  f_real *= 1.0 + 0.0400 * 1.0 / 360.0;   // rate FOR 02-26, accrual day 03-02 (weight 1)
  f_real *= 1.0 + 0.0401 * 1.0 / 360.0;   // rate FOR 02-27, accrual day 03-03 (weight 1)
  f_real *= 1.0 + 0.0402 * 1.0 / 360.0;   // rate FOR 03-02, accrual day 03-04 (weight 1)
  f_real *= 1.0 + 0.0403 * 1.0 / 360.0;   // rate FOR 03-03, accrual day 03-05 (weight 1)
  f_real *= 1.0 + 0.0404 * 3.0 / 360.0;   // rate FOR 03-04, accrual day 03-06 (weight 3: Sat, Sun)
  EXPECT_DOUBLE_EQ(cp.realised_factor, f_real);
  const ObsDay* d = &flt.obs[static_cast<std::size_t>(cp.obs_begin)];
  EXPECT_EQ(to_iso(d[0].rate_date), "2026-03-05");   // looks back from accrual day 03-09
  EXPECT_EQ(to_iso(d[1].rate_date), "2026-03-06");   // looks back from accrual day 03-10
  EXPECT_EQ(to_iso(d[2].rate_date), "2026-03-09");   // looks back from accrual day 03-11
  EXPECT_EQ(to_iso(d[3].rate_date), "2026-03-10");   // looks back from accrual day 03-12
  EXPECT_EQ(to_iso(d[4].rate_date), "2026-03-11");   // looks back from accrual day 03-13
  EXPECT_EQ(d[0].weight_days, 1.0);   // accrual day 03-09's own span (a Monday)
  EXPECT_EQ(d[4].weight_days, 3.0);   // accrual day 03-13's own span (a Friday: Sat, Sun)
  EXPECT_EQ(d[0].tau_rate, 1.0 / 360.0);       // rate day 03-05's own span to 03-06
  EXPECT_EQ(d[1].tau_rate, 3.0 / 360.0);       // rate day 03-06's own span to 03-09 (Sat, Sun)
  const double z = 0.04;
  auto flat = [&](int, double t) { return std::exp(-z * t); };
  auto fwd = [&](int n) { return (std::exp(z * n / 365.0) - 1.0) * 360.0 / n; };
  double f = f_real;
  f *= 1.0 + fwd(1) * d[0].weight_days / 360.0;   // rate day 03-05 (own span 1), accrual weight 1
  f *= 1.0 + fwd(3) * d[1].weight_days / 360.0;   // rate day 03-06 (own span 3), accrual weight 1
  f *= 1.0 + fwd(1) * d[2].weight_days / 360.0;   // rate day 03-09 (own span 1), accrual weight 1
  f *= 1.0 + fwd(1) * d[3].weight_days / 360.0;   // rate day 03-10 (own span 1), accrual weight 1
  f *= 1.0 + fwd(1) * d[4].weight_days / 360.0;   // rate day 03-11 (own span 1), accrual weight 3
  const double hand_rate = (f - 1.0) * 360.0 / 14.0;
  const double t_pay = 13.0 / 365.0;   // 03-05 -> 03-18
  const double hand_pv = 1.0e6 * (14.0 / 360.0) * hand_rate * std::exp(-z * t_pay);
  EXPECT_NEAR(coupon_rate<double>(flt, cp, flat), hand_rate, 1e-15);
  EXPECT_NEAR(float_coupon_pv<double>(flt, cp, flat), hand_pv, 1e-9);
  EXPECT_NEAR(leg_pv<double>(flt, flat), hand_pv, 1e-9);
  const double r_real = (f_real - 1.0) * 360.0 / 7.0;
  EXPECT_NEAR(cp.accrued, 1.0e6 * r_real * 3.0 / 360.0, 1e-9);
  const Leg& fixed = in.legs[0];
  ASSERT_EQ(fixed.coupons.size(), 1u);
  EXPECT_NEAR(leg_pv<double>(fixed, flat), 1.0e6 * (14.0 / 360.0) * 0.04 * std::exp(-z * t_pay), 1e-9);
  EXPECT_NEAR(pv<double>(in, flat), leg_pv<double>(fixed, flat) - hand_pv, 1e-9);
}

TEST(Coupon, PaymentLagVariantOnlyShiftsThePaymentDate) {
  // M3-fix: the EUR-ESTR-OIS payment-lag disagreement (2 business days, Strata / SwapEngine, vs 1
  // business day, TP ICAP MET template / LCH 2019; docs/G4_BUNDLE.md EUR.3) had no alternate
  // variant exposed, unlike SHIFT2 / LOCKOUT2. EUR-ESTR-OIS-LAG1 is EUR-ESTR-OIS with payment_lag
  // 1 instead of 2 and is otherwise identical, so every coupon field but the payment date should
  // match exactly, and the PV difference should be exactly the extra business day of discounting
  // (the coupon rate does not depend on the payment date at all).
  const epykos::test::Context c(valuation);
  const epykos::test::FlatCurves flat;
  const Calendar& target = epykos::test::registry().calendar("EUR-TARGET");
  for (const char* tenor : {"1Y", "2Y", "5Y", "10Y"}) {
    const Instrument in2 = c.build(swap_trade("EUR-ESTR-OIS", tenor, 0.02));
    const Instrument in1 = c.build(swap_trade("EUR-ESTR-OIS-LAG1", tenor, 0.02));
    ASSERT_EQ(in1.legs[1].coupons.size(), in2.legs[1].coupons.size()) << tenor;
    for (std::size_t i = 0; i < in2.legs[1].coupons.size(); ++i) {
      const Coupon& cp1 = in1.legs[1].coupons[i];
      const Coupon& cp2 = in2.legs[1].coupons[i];
      EXPECT_EQ(cp1.start, cp2.start) << tenor << " coupon " << i;
      EXPECT_EQ(cp1.end, cp2.end) << tenor << " coupon " << i;
      EXPECT_EQ(cp1.accrual, cp2.accrual) << tenor << " coupon " << i;
      EXPECT_EQ(cp1.realised_factor, cp2.realised_factor) << tenor << " coupon " << i;
      EXPECT_EQ(cp1.pay, target.add_business_days(cp1.end, 1)) << tenor << " coupon " << i;
      EXPECT_EQ(cp2.pay, target.add_business_days(cp2.end, 2)) << tenor << " coupon " << i;
      EXPECT_LT(cp1.pay, cp2.pay) << tenor << " coupon " << i << ": the 1-day lag pays earlier";
    }
    const double rate = coupon_rate<double>(in1.legs[1], in1.legs[1].coupons[0], flat);
    EXPECT_DOUBLE_EQ(rate, coupon_rate<double>(in2.legs[1], in2.legs[1].coupons[0], flat))
        << tenor << ": the payment lag does not enter the coupon rate";
    // Every coupon's PV difference is N * accrual * rate * (DF(pay1) - DF(pay2)), the earlier
    // payment discounted less; summed over the leg this must equal the leg PV difference exactly.
    const int float_disc1 = in1.legs[1].disc_curve, float_disc2 = in2.legs[1].disc_curve;
    const int fixed_disc1 = in1.legs[0].disc_curve, fixed_disc2 = in2.legs[0].disc_curve;
    double float_diff = 0.0, fixed_diff = 0.0;
    for (std::size_t i = 0; i < in1.legs[1].coupons.size(); ++i) {
      const Coupon& f1 = in1.legs[1].coupons[i];
      const Coupon& f2 = in2.legs[1].coupons[i];
      const double r = coupon_rate<double>(in1.legs[1], f1, flat);
      float_diff += f1.notional * f1.accrual * r * (flat(float_disc1, f1.t_pay) - flat(float_disc2, f2.t_pay));
    }
    for (std::size_t i = 0; i < in1.legs[0].coupons.size(); ++i) {
      const Coupon& x1 = in1.legs[0].coupons[i];
      const Coupon& x2 = in2.legs[0].coupons[i];
      fixed_diff += x1.notional * x1.accrual * x1.rate * (flat(fixed_disc1, x1.t_pay) - flat(fixed_disc2, x2.t_pay));
    }
    const double pv_diff = pv<double>(in1, flat) - pv<double>(in2, flat);
    EXPECT_NEAR(pv_diff, fixed_diff - float_diff, 1e-6 * in1.notional) << tenor;
    EXPECT_GT(fixed_diff, 0.0) << tenor << ": paying one day earlier raises a receiver's discounted cashflow";
  }
}

TEST(Coupon, ArithmeticAverageEqualsTheExplicitAverage) {
  // The same March coupon averaged: (Σ_fixed r_i n_i + Σ_projected f_i n_i) / 14, the projected
  // daily forwards on the flat curve.
  const Date val = date("2026-03-05");
  epykos::test::Context c(val, march_history("2026-03-04"));
  Blueprint avg = epykos::test::blueprints().blueprint("USD-SOFR-AVG-SWAP");
  ObservationSpec spec;
  spec.method = ObservationMethod::ObservationShift;
  spec.lookback_days = 2;
  spec.lockout_days = 2;
  avg.legs[1].observation = spec;
  const Instrument in = c.build(avg, march_trade("USD-SOFR-AVG-SWAP"));
  const Leg& flt = in.legs[1];
  const Coupon& cp = flt.coupons[0];
  EXPECT_EQ(cp.kind, CouponKind::RfrAveraged);
  const double z = 0.04;
  auto flat = [&](int, double t) { return std::exp(-z * t); };
  auto fwd = [&](int n) { return (std::exp(z * n / 365.0) - 1.0) * 360.0 / n; };
  const double fixed_sum = 0.0400 * 1 + 0.0401 * 3 + 0.0402 + 0.0403 + 0.0404;
  EXPECT_DOUBLE_EQ(cp.realised_sum, fixed_sum);
  const double hand_avg = (fixed_sum + fwd(1) * 1 + fwd(3) * 3 + fwd(1) * 3) / 14.0;
  EXPECT_NEAR(coupon_rate<double>(flt, cp, flat), hand_avg, 1e-15);
  EXPECT_NEAR(cp.accrued, 1.0e6 * (fixed_sum / 7.0) * 3.0 / 360.0, 1e-9);
  // plain averaging over a year does not telescope: the average differs from the compounded rate
  const epykos::test::Context today(valuation);
  const epykos::test::FlatCurves curves;
  const Instrument a = today.build(swap_trade("USD-SOFR-AVG-SWAP", "1Y"));
  const Instrument o = today.build(swap_trade("USD-SOFR-OIS", "1Y"));
  const double ra = coupon_rate<double>(a.legs[1], a.legs[1].coupons[0], curves);
  const double ro = coupon_rate<double>(o.legs[1], o.legs[1].coupons[0], curves);
  EXPECT_GT(ro - ra, 1e-5) << "compounding a 4% rate over a year exceeds its arithmetic average";
  EXPECT_LT(ro - ra, 1e-3);
}

TEST(Coupon, FixingInAdvanceIsTheRealisedFixingWhenPast) {
  const epykos::test::Context c(valuation);
  const epykos::test::FlatCurves flat;
  Trade t = swap_trade("EUR-EURIBOR-3M-IRS", "3Y", 0.025);
  t.effective = date("2025-06-30");
  const Instrument in = c.build(t);
  const Leg& flt = in.legs[1];
  ASSERT_GE(flt.coupons.size(), 2u);
  const Coupon& cur = flt.coupons[0];
  EXPECT_EQ(to_iso(cur.start), "2026-06-30");
  EXPECT_EQ(to_iso(cur.end), "2026-09-30");
  EXPECT_EQ(to_iso(cur.fixing_date), "2026-06-26");   // two TARGET business days before the start
  EXPECT_TRUE(cur.realised);
  EXPECT_TRUE(cur.current);
  const double fixing = c.fixings.at("EUR-EURIBOR-3M", cur.fixing_date);
  EXPECT_EQ(cur.rate, fixing);
  EXPECT_NEAR(float_coupon_pv<double>(flt, cur, flat), in.notional * cur.accrual * fixing * flat(1, cur.t_pay), 1e-9);
  EXPECT_NEAR(cur.accrued, in.notional * fixing * year_fraction(DayCount::Act360, cur.start, valuation), 1e-9);
  // the next coupon fixes on 2026-09-28: projected off the 3M curve (slot 2), discounted on €STR (slot 1)
  const Coupon& nxt = flt.coupons[1];
  EXPECT_FALSE(nxt.realised);
  EXPECT_EQ(to_iso(nxt.fixing_date), "2026-09-28");
  EXPECT_EQ(flt.curve, 2);
  EXPECT_EQ(flt.disc_curve, 1);
  const double fwd = (flat(2, nxt.t_fix_start) / flat(2, nxt.t_fix_end) - 1.0) / nxt.fix_tau;
  EXPECT_NEAR(float_coupon_pv<double>(flt, nxt, flat), in.notional * nxt.accrual * fwd * flat(1, nxt.t_pay), 1e-9);
  // a spot-starting swap fixes on the trade date, whose rate is not yet in the history: projected
  const Instrument spot = c.build(swap_trade("EUR-EURIBOR-6M-IRS", "2Y"));
  EXPECT_FALSE(spot.legs[1].coupons[0].realised);
  EXPECT_EQ(to_iso(spot.legs[1].coupons[0].fixing_date), "2026-09-23");
  // a seasoned trade whose fixing is missing from the history is an error, not a projection
  FixingsHistory gap = c.fixings;
  FixingsHistory empty;
  epykos::test::Context none(valuation, empty);
  EXPECT_THROW(none.build(t), BlueprintError);
}

TEST(Coupon, FuturesPriceIdentity) {
  const epykos::test::Context c(valuation);
  const epykos::test::FlatCurves flat;
  // SR3: 100 (1 − rate), the rate the plainly compounded SOFR over the IMM quarter, which
  // telescopes on the curve to (DF(s)/DF(e) − 1)/τ_obs.
  Trade t;
  t.blueprint = "USD-SOFR-3M-FUTURE";
  t.contract = 0;
  const Instrument sr3 = c.build(t);
  EXPECT_EQ(sr3.kind, Kind::Future);
  EXPECT_EQ(sr3.contract_code, "SR3 Z26");
  EXPECT_EQ(to_iso(sr3.effective), "2026-12-16");
  EXPECT_EQ(to_iso(sr3.termination), "2027-03-17");
  const Coupon& q = sr3.legs[0].coupons[0];
  const double rate = futures_settlement_rate<double>(sr3, flat);
  const double closed = (flat(0, q.t_start) / flat(0, q.t_end) - 1.0) / q.obs_tau;
  EXPECT_NEAR(rate, closed, 1e-13);
  EXPECT_EQ(par<double>(sr3, flat), 100.0 * (1.0 - rate));
  EXPECT_NEAR(residual<double>(sr3, flat, par<double>(sr3, flat)), 0.0, 1e-15);
  EXPECT_NEAR(futures_rate(futures_price(rate)), rate, 1e-15);
  // a long position at a traded price: (price − traded)/100 · notional, undiscounted
  Trade pos = t;
  pos.traded_price = 96.0;
  pos.notional = 250000.0 * 4;
  pos.side = -1;
  const Instrument sh = c.build(pos);
  EXPECT_NEAR(pv<double>(sh, flat), -1.0 * 1.0e6 * (par<double>(sh, flat) - 96.0) / 100.0, 1e-9);
  // the contract by its code
  Trade byc = t;
  byc.contract_code = "M27";
  EXPECT_EQ(c.build(byc).contract_code, "SR3 M27");
  byc.contract_code = "SR3 U27";
  EXPECT_EQ(c.build(byc).contract_code, "SR3 U27");
  // SR1: the arithmetic average over the calendar month
  Trade m;
  m.blueprint = "USD-SOFR-1M-FUTURE";
  m.contract = 0;
  const Instrument sr1 = c.build(m);
  EXPECT_EQ(sr1.contract_code, "SR1 V26");
  EXPECT_EQ(sr1.legs[0].coupons[0].kind, CouponKind::RfrAveraged);
  EXPECT_EQ(par<double>(sr1, flat), 100.0 * (1.0 - futures_settlement_rate<double>(sr1, flat)));
  // FEU3: the forward over the deposit period from the third Wednesday, ACT/360, fixing two
  // exchange days before
  Trade e;
  e.blueprint = "EUR-EURIBOR-3M-FUTURE";
  e.contract = 0;
  const Instrument feu = c.build(e);
  EXPECT_EQ(feu.contract_code, "FEU3 Z26");
  const Coupon& d = feu.legs[0].coupons[0];
  EXPECT_EQ(d.kind, CouponKind::TermRate);
  EXPECT_EQ(to_iso(d.start), "2026-12-16");
  EXPECT_EQ(to_iso(d.fixing_date), "2026-12-14");
  EXPECT_FALSE(d.realised);
  const double fwd = (flat(2, d.t_fix_start) / flat(2, d.t_fix_end) - 1.0) / d.fix_tau;
  EXPECT_EQ(par<double>(feu, flat), 100.0 * (1.0 - fwd));
}

TEST(Coupon, DepositIsAOnePeriodSwap) {
  const epykos::test::Context c(valuation);
  const epykos::test::FlatCurves flat;
  Trade t;
  t.blueprint = "USD-SOFR-ON-DEPOSIT";
  t.fixed_rate = 0.04;
  t.notional = 1.0e6;
  const Instrument on = c.build(t);
  EXPECT_EQ(on.kind, Kind::Deposit);
  EXPECT_EQ(on.effective, valuation);
  EXPECT_EQ(to_iso(on.termination), "2026-09-24");
  const Coupon& k = on.legs[0].coupons[0];
  const Coupon& f = on.legs[1].coupons[0];
  const double fwd = (flat(0, f.t_fix_start) / flat(0, f.t_fix_end) - 1.0) / f.fix_tau;
  EXPECT_NEAR(par<double>(on, flat), fwd, 1e-15);
  EXPECT_NEAR(pv<double>(on, flat), 1.0e6 * k.accrual * (0.04 - fwd) * flat(0, k.t_pay), 1e-9);
  EXPECT_NEAR(residual<double>(on, flat, fwd), 0.0, 1e-15);
  Trade e;
  e.blueprint = "EUR-EURIBOR-6M-DEPOSIT";
  const Instrument six = c.build(e);
  EXPECT_EQ(to_iso(six.effective), "2026-09-25");
  EXPECT_EQ(to_iso(six.termination), "2027-03-25");
  EXPECT_EQ(six.legs[1].curve, 3);
  EXPECT_EQ(six.legs[1].disc_curve, 1);
  EXPECT_EQ(six.legs[0].coupons[0].accrual, year_fraction(DayCount::Act360, six.effective, six.termination));
}
