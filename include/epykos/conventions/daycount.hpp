// EpykosEngine — periods (tenors) and day-count fractions (M3/G0).
//
// Day counts follow the 2006 / 2021 ISDA Definitions:
//   ACT/360        (d2 - d1) / 360
//   ACT/365F       (d2 - d1) / 365
//   30/360 US      ISDA "30/360" (Bond Basis): D1 = min(D1, 30); D2 = 30 if D2 = 31 and D1 >= 30
//   30E/360        ISDA "30E/360" (Eurobond Basis): D1 = min(D1, 30); D2 = min(D2, 30)
//   ACT/ACT ISDA   days in leap years / 366 + days in non-leap years / 365
// The year fraction is a double computed from integers: structure, never a Scalar.
#pragma once

#include <string>
#include <string_view>

#include "epykos/conventions/date.hpp"

namespace epykos::conventions {

// A tenor such as "3M", "1Y", "2W", "1D", "18M". "0D" is allowed. Unit is one of D, W, M, Y.
struct Period {
  int n = 0;
  char unit = 'D';

  static Period parse(std::string_view s);   // throws DateError
  std::string to_string() const;
  int months() const;                         // for M / Y units; throws for D / W
  bool is_calendar_days() const noexcept { return unit == 'D' || unit == 'W'; }
  int days() const noexcept { return unit == 'W' ? 7 * n : n; }  // D / W only
  friend bool operator==(const Period& a, const Period& b) noexcept { return a.n == b.n && a.unit == b.unit; }
  friend bool operator!=(const Period& a, const Period& b) noexcept { return !(a == b); }
};

// d + p (calendar arithmetic, no adjustment). Month/year units keep the day of month clipped
// to the target month, with the end-of-month rule when eom = true (see add_months).
Date add_period(Date d, const Period& p, bool eom = false);

enum class DayCount : unsigned char { Act360, Act365F, Thirty360US, ThirtyE360, ActActISDA };

const char* to_string(DayCount dc) noexcept;   // "ACT/360", "ACT/365F", "30/360", "30E/360", "ACT/ACT ISDA"
DayCount daycount_from_string(const std::string& s);   // throws DateError on an unknown name
                                                       // (accepts the ISDA names and 30U/360, ACT/ACT-ISDA)

// The year fraction from d1 to d2 (d2 >= d1; a reversed pair returns the negative).
double year_fraction(DayCount dc, Date d1, Date d2);

// The number of days counted by a 30/360 basis (before dividing by 360).
int days_30_360_us(Date d1, Date d2) noexcept;
int days_30e_360(Date d1, Date d2) noexcept;

}  // namespace epykos::conventions
