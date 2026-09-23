// M3/G0: day-count fractions per the 2006 ISDA Definitions 4.16, incl. ACT/ACT ISDA on a
// leap-year straddle; periods (tenors).
#include <gtest/gtest.h>

#include "epykos/conventions/daycount.hpp"

using namespace epykos::conventions;

TEST(DayCount, Act360AndAct365F) {
  const Date a = parse_iso("2026-01-15");
  const Date b = parse_iso("2026-07-15");
  EXPECT_EQ(year_fraction(DayCount::Act360, a, b), 181.0 / 360.0);
  EXPECT_EQ(year_fraction(DayCount::Act365F, a, b), 181.0 / 365.0);
  EXPECT_EQ(year_fraction(DayCount::Act360, b, a), -181.0 / 360.0);
  EXPECT_EQ(year_fraction(DayCount::Act360, a, a), 0.0);
}

TEST(DayCount, ActActIsdaLeapYearStraddle) {
  // 2023-12-15 -> 2024-02-15: 17 days in 2023 (365) and 45 days in 2024 (366).
  const double yf = year_fraction(DayCount::ActActISDA, parse_iso("2023-12-15"), parse_iso("2024-02-15"));
  EXPECT_DOUBLE_EQ(yf, 17.0 / 365.0 + 45.0 / 366.0);
  // within a leap year
  EXPECT_EQ(year_fraction(DayCount::ActActISDA, parse_iso("2024-02-28"), parse_iso("2024-03-01")), 2.0 / 366.0);
  // within a common year
  EXPECT_EQ(year_fraction(DayCount::ActActISDA, parse_iso("2026-02-28"), parse_iso("2026-03-01")), 1.0 / 365.0);
  // whole years in between: 2023-06-30 -> 2027-06-30 = 185/365 + 3 (2024, 2025, 2026) + 180/365
  EXPECT_DOUBLE_EQ(year_fraction(DayCount::ActActISDA, parse_iso("2023-06-30"), parse_iso("2027-06-30")),
                   185.0 / 365.0 + 3.0 + 180.0 / 365.0);
  // exactly one leap year
  EXPECT_DOUBLE_EQ(year_fraction(DayCount::ActActISDA, parse_iso("2024-01-01"), parse_iso("2025-01-01")), 1.0);
}

TEST(DayCount, Thirty360UsAndEurobond) {
  // ISDA 4.16(f) 30/360: D1 = 30 if 31; D2 = 30 if 31 and D1 in {30, 31}. 4.16(g) 30E/360: both -> 30.
  EXPECT_EQ(days_30_360_us(parse_iso("2026-01-31"), parse_iso("2026-02-28")), 28);
  EXPECT_EQ(days_30e_360(parse_iso("2026-01-31"), parse_iso("2026-02-28")), 28);
  EXPECT_EQ(days_30_360_us(parse_iso("2026-01-15"), parse_iso("2026-03-31")), 76);   // D2 stays 31
  EXPECT_EQ(days_30e_360(parse_iso("2026-01-15"), parse_iso("2026-03-31")), 75);     // D2 -> 30
  EXPECT_EQ(days_30_360_us(parse_iso("2026-01-30"), parse_iso("2026-03-31")), 60);   // D1 = 30 -> D2 -> 30
  EXPECT_EQ(days_30_360_us(parse_iso("2026-01-31"), parse_iso("2027-01-31")), 360);
  EXPECT_EQ(days_30e_360(parse_iso("2026-01-31"), parse_iso("2027-01-31")), 360);
  EXPECT_EQ(days_30_360_us(parse_iso("2026-02-28"), parse_iso("2026-03-31")), 33);   // no February rule in ISDA 30/360
  EXPECT_EQ(year_fraction(DayCount::Thirty360US, parse_iso("2026-09-25"), parse_iso("2027-09-27")), 362.0 / 360.0);
  EXPECT_EQ(year_fraction(DayCount::ThirtyE360, parse_iso("2026-09-25"), parse_iso("2027-09-27")), 362.0 / 360.0);
}

TEST(DayCount, Names) {
  EXPECT_EQ(daycount_from_string("ACT/360"), DayCount::Act360);
  EXPECT_EQ(daycount_from_string("ACT/365F"), DayCount::Act365F);
  EXPECT_EQ(daycount_from_string("30/360"), DayCount::Thirty360US);
  EXPECT_EQ(daycount_from_string("30U/360"), DayCount::Thirty360US);
  EXPECT_EQ(daycount_from_string("30E/360"), DayCount::ThirtyE360);
  EXPECT_EQ(daycount_from_string("ACT/ACT ISDA"), DayCount::ActActISDA);
  EXPECT_EQ(daycount_from_string("ACT/ACT-ISDA"), DayCount::ActActISDA);
  EXPECT_THROW(daycount_from_string("ACT/ACT ICMA"), DateError);
  EXPECT_THROW(daycount_from_string("BUS/252"), DateError);
  EXPECT_STREQ(to_string(DayCount::Thirty360US), "30/360");
}

TEST(Period, ParseAndAdd) {
  EXPECT_EQ(Period::parse("3M"), (Period{3, 'M'}));
  EXPECT_EQ(Period::parse("18m"), (Period{18, 'M'}));
  EXPECT_EQ(Period::parse("1Y"), (Period{1, 'Y'}));
  EXPECT_EQ(Period::parse("2W"), (Period{2, 'W'}));
  EXPECT_EQ(Period::parse("1D"), (Period{1, 'D'}));
  EXPECT_EQ(Period::parse("1Y").months(), 12);
  EXPECT_EQ(Period::parse("2W").days(), 14);
  EXPECT_THROW(Period::parse("M"), DateError);
  EXPECT_THROW(Period::parse("3Q"), DateError);
  EXPECT_THROW(Period::parse("-3M"), DateError);
  EXPECT_THROW(Period::parse("1W").months(), DateError);
  const Date d = parse_iso("2026-11-30");
  EXPECT_EQ(add_period(d, Period::parse("3M")), parse_iso("2027-02-28"));
  EXPECT_EQ(add_period(d, Period::parse("3M"), true), parse_iso("2027-02-28"));
  EXPECT_EQ(add_period(parse_iso("2026-02-28"), Period::parse("1M"), true), parse_iso("2026-03-31"));
  EXPECT_EQ(add_period(d, Period::parse("2W")), parse_iso("2026-12-14"));
  EXPECT_EQ(add_period(d, Period::parse("1D")), parse_iso("2026-12-01"));
  EXPECT_EQ(add_period(d, Period::parse("30Y")), parse_iso("2056-11-30"));
  EXPECT_EQ(Period::parse("18M").to_string(), "18M");
}
