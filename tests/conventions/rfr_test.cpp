// M3/G0: RFR observation windows (plain, lookback, observation shift, lockout) and the
// realised compounding / averaging from a fixings history — a compounded SOFR coupon with a
// 2-day observation shift and a 2-day lockout against a hand computation written out here.
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "epykos/conventions/fixings.hpp"
#include "epykos/conventions/registry.hpp"
#include "epykos/conventions/rfr.hpp"

using namespace epykos::conventions;

namespace {

const Registry& reg() {
  static Registry r = Registry::load();
  return r;
}

const Calendar& sofr_cal() { return reg().calendar("USD-SOFR"); }

std::vector<std::string> rate_dates(const ObservationPeriod& p) {
  std::vector<std::string> out;
  for (const auto& d : p.days) out.push_back(to_iso(d.rate_date));
  return out;
}

std::vector<int> weights(const ObservationPeriod& p) {
  std::vector<int> out;
  for (const auto& d : p.days) out.push_back(d.weight_days);
  return out;
}

// A hand-written history for late February / March 2026 (SYNTHETIC values, labelled).
FixingsHistory march_history() {
  FixingsHistory h;
  const char* days[] = {"2026-02-23", "2026-02-24", "2026-02-25", "2026-02-26", "2026-02-27", "2026-03-02", "2026-03-03",
                        "2026-03-04", "2026-03-05", "2026-03-06", "2026-03-09", "2026-03-10", "2026-03-11", "2026-03-12",
                        "2026-03-13", "2026-03-16"};
  const double rates[] = {0.0397, 0.0398, 0.0399, 0.0400, 0.0401, 0.0402, 0.0403, 0.0404,
                          0.0405, 0.0406, 0.0407, 0.0408, 0.0409, 0.0410, 0.0411, 0.0412};
  for (int i = 0; i < 16; ++i) h.set("USD-SOFR", parse_iso(days[i]), rates[i]);
  return h;
}

}  // namespace

TEST(Rfr, PlainObservationDays) {
  // Accrual Mon 2026-03-02 -> Mon 2026-03-16: ten business days; Fridays carry three calendar days.
  const ObservationPeriod p = observation_days(parse_iso("2026-03-02"), parse_iso("2026-03-16"), ObservationSpec{}, sofr_cal());
  EXPECT_EQ(p.obs_start, p.accrual_start);
  EXPECT_EQ(p.obs_end, p.accrual_end);
  EXPECT_EQ(rate_dates(p), (std::vector<std::string>{"2026-03-02", "2026-03-03", "2026-03-04", "2026-03-05", "2026-03-06",
                                                     "2026-03-09", "2026-03-10", "2026-03-11", "2026-03-12", "2026-03-13"}));
  EXPECT_EQ(weights(p), (std::vector<int>{1, 1, 1, 1, 3, 1, 1, 1, 1, 3}));
  EXPECT_EQ(p.weight_total(), 14);
  EXPECT_EQ(p.accrual_days(), 14);
  // A period starting on a holiday carries the preceding rate to the first business day:
  // Good Friday 2026-04-03 (no SOFR) to Wed 2026-04-08 = 04-02's rate for 3 days, 04-06, 04-07.
  const ObservationPeriod q = observation_days(parse_iso("2026-04-03"), parse_iso("2026-04-08"), ObservationSpec{}, sofr_cal());
  EXPECT_EQ(rate_dates(q), (std::vector<std::string>{"2026-04-02", "2026-04-06", "2026-04-07"}));
  EXPECT_EQ(weights(q), (std::vector<int>{3, 1, 1}));
}

TEST(Rfr, LookbackWithoutShift) {
  ObservationSpec spec;
  spec.method = ObservationMethod::Lookback;
  spec.lookback_days = 2;
  const ObservationPeriod p = observation_days(parse_iso("2026-03-02"), parse_iso("2026-03-16"), spec, sofr_cal());
  // same days and weights as plain, rates from two business days earlier (ARRC: "lookback without observation shift")
  EXPECT_EQ(rate_dates(p), (std::vector<std::string>{"2026-02-26", "2026-02-27", "2026-03-02", "2026-03-03", "2026-03-04",
                                                     "2026-03-05", "2026-03-06", "2026-03-09", "2026-03-10", "2026-03-11"}));
  EXPECT_EQ(weights(p), (std::vector<int>{1, 1, 1, 1, 3, 1, 1, 1, 1, 3}));
  EXPECT_EQ(p.obs_start, p.accrual_start);
}

TEST(Rfr, ObservationShiftAndLockoutByHand) {
  // Accrual Mon 2026-03-02 -> Mon 2026-03-16 with a 2 business day observation shift: the
  // observation period is Thu 2026-02-26 -> Thu 2026-03-12 (14 calendar days, 10 business days),
  // each observation day weighted by the calendar days to the next business day of the
  // observation period. A 2 day lockout freezes the last two days (03-10, 03-11) at 03-09's rate.
  ObservationSpec spec;
  spec.method = ObservationMethod::ObservationShift;
  spec.lookback_days = 2;
  spec.lockout_days = 2;
  const ObservationPeriod p = observation_days(parse_iso("2026-03-02"), parse_iso("2026-03-16"), spec, sofr_cal());
  EXPECT_EQ(to_iso(p.obs_start), "2026-02-26");
  EXPECT_EQ(to_iso(p.obs_end), "2026-03-12");
  EXPECT_EQ(rate_dates(p), (std::vector<std::string>{"2026-02-26", "2026-02-27", "2026-03-02", "2026-03-03", "2026-03-04",
                                                     "2026-03-05", "2026-03-06", "2026-03-09", "2026-03-09", "2026-03-09"}));
  EXPECT_EQ(weights(p), (std::vector<int>{1, 3, 1, 1, 1, 1, 3, 1, 1, 1}));
  EXPECT_EQ(p.weight_total(), 14);

  const FixingsHistory h = march_history();
  // Hand computation, the same products in the same order:
  double f = 1.0;
  f *= 1.0 + 0.0400 * 1.0 / 360.0;   // Thu 02-26
  f *= 1.0 + 0.0401 * 3.0 / 360.0;   // Fri 02-27 (Sat, Sun)
  f *= 1.0 + 0.0402 * 1.0 / 360.0;   // Mon 03-02
  f *= 1.0 + 0.0403 * 1.0 / 360.0;   // Tue 03-03
  f *= 1.0 + 0.0404 * 1.0 / 360.0;   // Wed 03-04
  f *= 1.0 + 0.0405 * 1.0 / 360.0;   // Thu 03-05
  f *= 1.0 + 0.0406 * 3.0 / 360.0;   // Fri 03-06 (Sat, Sun)
  f *= 1.0 + 0.0407 * 1.0 / 360.0;   // Mon 03-09
  f *= 1.0 + 0.0407 * 1.0 / 360.0;   // Tue 03-10: locked out, 03-09's rate
  f *= 1.0 + 0.0407 * 1.0 / 360.0;   // Wed 03-11: locked out, 03-09's rate
  const double hand_rate = (f - 1.0) * 360.0 / 14.0;
  const RealisedObservations r = realised_observations(p, h, "USD-SOFR");
  EXPECT_TRUE(r.complete());
  EXPECT_EQ(r.fixed_count, 10);
  EXPECT_EQ(r.fixed_weight, 14);
  EXPECT_DOUBLE_EQ(r.factor, f);
  EXPECT_DOUBLE_EQ(compounded_rate(p, h, "USD-SOFR"), hand_rate);
  // The coupon is then notional * rate * (accrual DCF) in the pricing maths (G2); its
  // annualisation uses the observation period's 14 days here (both are 14).
  const double hand_avg = (0.0400 * 1 + 0.0401 * 3 + 0.0402 + 0.0403 + 0.0404 + 0.0405 + 0.0406 * 3 + 0.0407 * 3) / 14.0;
  EXPECT_DOUBLE_EQ(average_rate(p, h, "USD-SOFR"), hand_avg);
}

TEST(Rfr, LockoutMakesTheCouponKnownEarly) {
  // With the lockout the coupon is fully known once 03-09's SOFR is published (on 03-10):
  // a history ending on 03-09 completes it; without the lockout 03-10 and 03-11 are missing.
  ObservationSpec locked;
  locked.method = ObservationMethod::ObservationShift;
  locked.lookback_days = 2;
  locked.lockout_days = 2;
  ObservationSpec open = locked;
  open.lockout_days = 0;
  FixingsHistory h;
  for (const char* d : {"2026-02-26", "2026-02-27", "2026-03-02", "2026-03-03", "2026-03-04", "2026-03-05", "2026-03-06", "2026-03-09"}) {
    h.set("USD-SOFR", parse_iso(d), 0.04);
  }
  const ObservationPeriod pl = observation_days(parse_iso("2026-03-02"), parse_iso("2026-03-16"), locked, sofr_cal());
  const ObservationPeriod po = observation_days(parse_iso("2026-03-02"), parse_iso("2026-03-16"), open, sofr_cal());
  EXPECT_TRUE(realised_observations(pl, h, "USD-SOFR").complete());
  const RealisedObservations ro = realised_observations(po, h, "USD-SOFR");
  EXPECT_FALSE(ro.complete());
  EXPECT_EQ(ro.fixed_count, 8);
  EXPECT_EQ(ro.unfixed.size(), 2u);
  EXPECT_EQ(to_iso(ro.unfixed[0].rate_date), "2026-03-10");
  EXPECT_THROW(compounded_rate(po, h, "USD-SOFR"), DateError);
  // A flat 4% history compounds to the textbook value.
  const double flat = compounded_rate(pl, h, "USD-SOFR");
  double f = 1.0;
  for (int w : {1, 3, 1, 1, 1, 1, 3, 1, 1, 1}) f *= 1.0 + 0.04 * w / 360.0;
  EXPECT_DOUBLE_EQ(flat, (f - 1.0) * 360.0 / 14.0);
  EXPECT_NEAR(flat, 0.04, 5e-5);
}

TEST(Rfr, PartiallyFixedPeriodAndGaps) {
  const FixingsHistory h = march_history();
  const ObservationPeriod p = observation_days(parse_iso("2026-03-09"), parse_iso("2026-03-23"), ObservationSpec{}, sofr_cal());
  // history ends 03-16: 03-09 .. 03-16 fixed (6 entries), 03-17 .. 03-20 unfixed (4)
  const RealisedObservations r = realised_observations(p, h, "USD-SOFR");
  EXPECT_EQ(r.fixed_count, 6);
  EXPECT_EQ(r.unfixed.size(), 4u);
  EXPECT_EQ(r.fixed_weight, 8);   // 03-09..03-12 (1 each), 03-13 (3), 03-16 (1)
  EXPECT_EQ(to_iso(r.unfixed.front().rate_date), "2026-03-17");
  EXPECT_EQ(to_iso(r.unfixed.back().rate_date), "2026-03-20");
  EXPECT_EQ(r.unfixed.back().weight_days, 3);
  // a fixing after a gap is an error
  FixingsHistory gap = h;
  gap.set("USD-SOFR", parse_iso("2026-03-19"), 0.04);
  EXPECT_THROW(realised_observations(p, gap, "USD-SOFR"), DateError);
  // lockout larger than the period is an error; negative lags are errors
  ObservationSpec bad;
  bad.lockout_days = 10;
  EXPECT_THROW(observation_days(parse_iso("2026-03-09"), parse_iso("2026-03-23"), bad, sofr_cal()), DateError);
  ObservationSpec plain_with_lookback;
  plain_with_lookback.lookback_days = 2;
  EXPECT_THROW(observation_days(parse_iso("2026-03-09"), parse_iso("2026-03-23"), plain_with_lookback, sofr_cal()), DateError);
}

TEST(Rfr, FixingsHistoryTable) {
  FixingsHistory h;
  EXPECT_FALSE(h.has("USD-SOFR", parse_iso("2026-03-02")));
  EXPECT_THROW(h.at("USD-SOFR", parse_iso("2026-03-02")), DateError);
  h.set("USD-SOFR", parse_iso("2026-03-02"), 0.0402);
  h.set("EUR-ESTR", parse_iso("2026-03-02"), 0.0205);
  EXPECT_EQ(h.at("USD-SOFR", parse_iso("2026-03-02")), 0.0402);
  EXPECT_EQ(h.size("USD-SOFR"), 1u);
  EXPECT_EQ(h.indices(), (std::vector<std::string>{"EUR-ESTR", "USD-SOFR"}));
  EXPECT_EQ(*h.last_on_or_before("USD-SOFR", parse_iso("2026-03-05")), parse_iso("2026-03-02"));
  EXPECT_FALSE(h.last_on_or_before("USD-SOFR", parse_iso("2026-03-01")).has_value());
  EXPECT_NE(h.series("EUR-ESTR"), nullptr);
  EXPECT_EQ(h.series("GBP-SONIA"), nullptr);
}

TEST(Rfr, SyntheticFixingsAreSeededAndOnBusinessDays) {
  // SYNTHETIC history (a fixture, not market data): deterministic for a seed, one value per
  // fixing business day, within the clamp.
  FixingsHistory a;
  FixingsHistory b;
  SyntheticFixingsSpec spec;
  spec.seed = 42;
  spec.level = 0.043;
  synthetic_fixings(a, "USD-SOFR", sofr_cal(), parse_iso("2026-01-01"), parse_iso("2026-06-30"), spec);
  synthetic_fixings(b, "USD-SOFR", sofr_cal(), parse_iso("2026-01-01"), parse_iso("2026-06-30"), spec);
  EXPECT_EQ(*a.series("USD-SOFR"), *b.series("USD-SOFR"));
  EXPECT_EQ(a.size("USD-SOFR"), static_cast<std::size_t>(sofr_cal().business_days_between(parse_iso("2026-01-01"), parse_iso("2026-07-01"))));
  EXPECT_FALSE(a.has("USD-SOFR", parse_iso("2026-04-03")));   // Good Friday: no SOFR
  EXPECT_FALSE(a.has("USD-SOFR", parse_iso("2026-01-19")));   // MLK Day
  EXPECT_TRUE(a.has("USD-SOFR", parse_iso("2026-04-02")));
  for (const auto& kv : *a.series("USD-SOFR")) {
    EXPECT_GE(kv.second, 0.0);
    EXPECT_LE(kv.second, 0.25);
    EXPECT_NEAR(kv.second, 0.043, 0.02);
  }
  FixingsHistory c;
  spec.seed = 43;
  synthetic_fixings(c, "USD-SOFR", sofr_cal(), parse_iso("2026-01-01"), parse_iso("2026-06-30"), spec);
  EXPECT_NE(*a.series("USD-SOFR"), *c.series("USD-SOFR"));
  // and the realised part of a period inside the history is complete
  const ObservationPeriod p = observation_days(parse_iso("2026-03-02"), parse_iso("2026-06-01"), ObservationSpec{}, sofr_cal());
  EXPECT_TRUE(realised_observations(p, a, "USD-SOFR").complete());
}
