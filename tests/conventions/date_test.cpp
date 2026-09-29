// M3/G0: dates as day counts from the epoch, y/m/d conversion, weekdays, Easter, IMM dates.
#include <gtest/gtest.h>

#include "epykos/conventions/date.hpp"

using namespace epykos::conventions;

TEST(Date, EpochAndCivilRoundTrip) {
  EXPECT_EQ(days_from_civil(1970, 1, 1), 0);
  EXPECT_EQ(days_from_civil(1970, 1, 2), 1);
  EXPECT_EQ(days_from_civil(1969, 12, 31), -1);
  EXPECT_EQ(days_from_civil(2000, 3, 1), 11017);
  EXPECT_EQ(days_from_civil(2026, 9, 23), 20719);
  for (Date d = days_from_civil(1899, 12, 1); d <= days_from_civil(2101, 1, 31); ++d) {
    const YMD x = civil_from_days(d);
    ASSERT_TRUE(valid_ymd(x.y, x.m, x.d)) << d;
    ASSERT_EQ(days_from_civil(x), d);
  }
  static_assert(civil_from_days(0) == YMD{1970, 1, 1});
  static_assert(days_from_civil(2024, 2, 29) - days_from_civil(2024, 2, 28) == 1);
}

TEST(Date, LeapYearsAndMonthLengths) {
  EXPECT_TRUE(is_leap(2024));
  EXPECT_FALSE(is_leap(2026));
  EXPECT_FALSE(is_leap(2100));
  EXPECT_TRUE(is_leap(2000));
  EXPECT_EQ(days_in_month(2024, 2), 29);
  EXPECT_EQ(days_in_month(2026, 2), 28);
  EXPECT_EQ(days_in_month(2026, 12), 31);
  EXPECT_FALSE(valid_ymd(2026, 2, 29));
  EXPECT_THROW(make_date(2026, 2, 29), DateError);
  EXPECT_THROW(make_date(2026, 13, 1), DateError);
  EXPECT_NO_THROW(make_date(2028, 2, 29));
}

TEST(Date, Weekdays) {
  EXPECT_EQ(weekday(days_from_civil(1970, 1, 1)), 3);   // Thursday
  EXPECT_EQ(weekday(days_from_civil(2026, 1, 1)), 3);   // Thursday (SIFMA: "Thursday, January 1, 2026")
  EXPECT_EQ(weekday(days_from_civil(2026, 9, 23)), 2);  // Wednesday
  EXPECT_EQ(weekday(days_from_civil(2026, 4, 3)), 4);   // Good Friday 2026
  EXPECT_EQ(weekday(days_from_civil(2027, 6, 19)), 5);  // Juneteenth 2027 is a Saturday
  EXPECT_EQ(weekday(days_from_civil(2027, 7, 4)), 6);   // Independence Day 2027 is a Sunday
  EXPECT_EQ(weekday(days_from_civil(1969, 12, 28)), 6); // negative day counts
  EXPECT_TRUE(is_weekend_sat_sun(days_from_civil(2026, 9, 26)));
  EXPECT_FALSE(is_weekend_sat_sun(days_from_civil(2026, 9, 25)));
}

TEST(Date, MonthArithmeticAndEndOfMonth) {
  const Date jan31 = days_from_civil(2026, 1, 31);
  EXPECT_EQ(add_months(jan31, 1), days_from_civil(2026, 2, 28));        // clipped
  EXPECT_EQ(add_months(jan31, 1, true), days_from_civil(2026, 2, 28));
  EXPECT_EQ(add_months(days_from_civil(2026, 2, 28), 1), days_from_civil(2026, 3, 28));       // no EOM: keeps 28
  EXPECT_EQ(add_months(days_from_civil(2026, 2, 28), 1, true), days_from_civil(2026, 3, 31));  // EOM: month end
  EXPECT_EQ(add_months(days_from_civil(2024, 2, 29), 12), days_from_civil(2025, 2, 28));
  EXPECT_EQ(add_months(days_from_civil(2026, 3, 15), -3), days_from_civil(2025, 12, 15));
  EXPECT_EQ(add_months(days_from_civil(2026, 1, 15), -1), days_from_civil(2025, 12, 15));
  EXPECT_EQ(add_years(days_from_civil(2026, 9, 23), 30), days_from_civil(2056, 9, 23));
  EXPECT_TRUE(is_end_of_month(days_from_civil(2026, 2, 28)));
  EXPECT_FALSE(is_end_of_month(days_from_civil(2024, 2, 28)));
  EXPECT_EQ(end_of_month(days_from_civil(2026, 9, 1)), days_from_civil(2026, 9, 30));
  EXPECT_EQ(start_of_month(days_from_civil(2026, 9, 23)), days_from_civil(2026, 9, 1));
}

TEST(Date, NthWeekday) {
  EXPECT_EQ(nth_weekday_of_month(2026, 1, 0, 3), days_from_civil(2026, 1, 19));   // MLK 2026
  EXPECT_EQ(nth_weekday_of_month(2026, 5, 0, -1), days_from_civil(2026, 5, 25));  // Memorial Day 2026
  EXPECT_EQ(nth_weekday_of_month(2026, 11, 3, 4), days_from_civil(2026, 11, 26)); // Thanksgiving 2026
  EXPECT_EQ(nth_weekday_of_month(2027, 5, 0, -1), days_from_civil(2027, 5, 31));  // Memorial Day 2027
  EXPECT_EQ(nth_weekday_of_month(2027, 11, 3, 4), days_from_civil(2027, 11, 25)); // Thanksgiving 2027
}

TEST(Date, Easter) {
  EXPECT_EQ(easter_sunday(2024), days_from_civil(2024, 3, 31));
  EXPECT_EQ(easter_sunday(2025), days_from_civil(2025, 4, 20));
  EXPECT_EQ(easter_sunday(2026), days_from_civil(2026, 4, 5));    // Good Friday 2026-04-03 (SIFMA, Bundesbank)
  EXPECT_EQ(easter_sunday(2027), days_from_civil(2027, 3, 28));   // Good Friday 2027-03-26 (SIFMA)
  EXPECT_EQ(easter_sunday(2038), days_from_civil(2038, 4, 25));   // the latest possible Easter
  EXPECT_EQ(easter_sunday(2285), days_from_civil(2285, 3, 22));   // the earliest possible Easter
  EXPECT_EQ(easter_sunday(2026) - 2, days_from_civil(2026, 4, 3));
  EXPECT_EQ(easter_sunday(2026) + 1, days_from_civil(2026, 4, 6));
}

TEST(Date, ImmDates2026) {
  // Third Wednesdays of March, June, September, December 2026 (the FOMC met on three of them).
  EXPECT_EQ(imm_date(2026, 3), days_from_civil(2026, 3, 18));
  EXPECT_EQ(imm_date(2026, 6), days_from_civil(2026, 6, 17));
  EXPECT_EQ(imm_date(2026, 9), days_from_civil(2026, 9, 16));
  EXPECT_EQ(imm_date(2026, 12), days_from_civil(2026, 12, 16));
  for (int m : {3, 6, 9, 12}) EXPECT_EQ(weekday(imm_date(2026, m)), 2);
  EXPECT_EQ(next_imm(days_from_civil(2026, 9, 15)), days_from_civil(2026, 9, 16));
  EXPECT_EQ(next_imm(days_from_civil(2026, 9, 16)), days_from_civil(2026, 12, 16));
  EXPECT_EQ(next_imm(days_from_civil(2026, 9, 16), true), days_from_civil(2026, 9, 16));
  EXPECT_EQ(next_imm(days_from_civil(2026, 12, 20)), days_from_civil(2027, 3, 17));
}

TEST(Date, IsoText) {
  EXPECT_EQ(to_iso(days_from_civil(2026, 9, 23)), "2026-09-23");
  EXPECT_EQ(to_iso(days_from_civil(2027, 1, 1)), "2027-01-01");
  EXPECT_EQ(parse_iso("2026-09-23"), days_from_civil(2026, 9, 23));
  EXPECT_THROW(parse_iso("2026-9-23"), DateError);
  EXPECT_THROW(parse_iso("2026-02-30"), DateError);
  EXPECT_THROW(parse_iso("20260923"), DateError);
  for (Date d = days_from_civil(2020, 1, 1); d <= days_from_civil(2080, 12, 31); d += 37) {
    ASSERT_EQ(parse_iso(to_iso(d)), d);
  }
}
