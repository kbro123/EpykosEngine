// M1/P1: the synthetic calendar of docs/WORKLOADS.md.
#include <gtest/gtest.h>

#include <cmath>
#include <vector>

#include "epykos/maths/calendar.hpp"

namespace cal = epykos::calendar;

TEST(Calendar, ScheduleOffsetIsRoundOf365Point25) {
  for (int j = 0; j <= 60; ++j) {
    EXPECT_EQ(cal::schedule_offset(j), std::lround(365.25 * j)) << "j=" << j;
  }
  EXPECT_EQ(cal::schedule_offset(1), 365);   // 365.25 -> 365
  EXPECT_EQ(cal::schedule_offset(2), 731);   // 730.5  -> 731 (half away from zero)
  EXPECT_EQ(cal::schedule_offset(3), 1096);  // 1095.75 -> 1096
  EXPECT_EQ(cal::schedule_offset(4), 1461);  // exact
}

TEST(Calendar, ScheduleDayAddsStart) {
  for (int d0 : {0, -30, -300, 17}) {
    for (int j = 0; j <= 30; ++j) {
      EXPECT_EQ(cal::schedule_day(d0, j), d0 + std::lround(365.25 * j));
    }
  }
}

TEST(Calendar, AnnualScheduleFillsEnds) {
  std::vector<int> days(31);
  cal::annual_schedule(-45, 30, days.data());
  EXPECT_EQ(days[0], -45);
  for (int j = 1; j <= 30; ++j) {
    EXPECT_EQ(days[static_cast<std::size_t>(j)], -45 + std::lround(365.25 * j));
    EXPECT_GT(days[static_cast<std::size_t>(j)], days[static_cast<std::size_t>(j - 1)]);
  }
}

TEST(Calendar, DayCounts) {
  EXPECT_EQ(cal::year_fraction(0, 365), 365.0 / 360.0);
  EXPECT_EQ(cal::year_fraction(365, 731), 366.0 / 360.0);
  EXPECT_EQ(cal::year_fraction(-100, 265), 365.0 / 360.0);
  EXPECT_EQ(cal::time_of_day(0), 0.0);
  EXPECT_EQ(cal::time_of_day(365), 1.0);
  EXPECT_EQ(cal::time_of_day(7), 7.0 / 365.0);
  EXPECT_EQ(cal::time_of_day(-30), -30.0 / 365.0);
  EXPECT_EQ(cal::accrual_basis, 360.0);
  EXPECT_EQ(cal::discount_basis, 365.0);
  EXPECT_EQ(cal::valuation_day, 0);
}

TEST(Calendar, IsConstexpr) {
  static_assert(cal::schedule_day(0, 2) == 731);
  static_assert(cal::year_fraction(0, 360) == 1.0);
  static_assert(cal::time_of_day(730) == 2.0);
  SUCCEED();
}
