// EpykosEngine — dates as integers (M3/G0).
//
// A Date is the number of days since the epoch 1970-01-01 (proleptic Gregorian; day 0 is a
// Thursday). Conversions to and from civil (y, m, d) are the exact integer algorithms of
// Howard Hinnant ("chrono-Compatible Low-Level Date Algorithms"), valid for every year the
// engine will ever see. Everything here is structure: ints in, ints out, never a Scalar.
//
// Weekday numbering follows ISO 8601 shifted to zero: 0 = Monday ... 6 = Sunday.
#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace epykos::conventions {

using Date = std::int32_t;

struct YMD {
  int y;
  int m;  // 1..12
  int d;  // 1..31
  friend constexpr bool operator==(const YMD& a, const YMD& b) noexcept {
    return a.y == b.y && a.m == b.m && a.d == b.d;
  }
  friend constexpr bool operator!=(const YMD& a, const YMD& b) noexcept { return !(a == b); }
};

class DateError : public std::runtime_error {
 public:
  explicit DateError(const std::string& what) : std::runtime_error(what) {}
};

constexpr bool is_leap(int y) noexcept { return (y % 4 == 0) && (y % 100 != 0 || y % 400 == 0); }

constexpr int days_in_month(int y, int m) noexcept {
  constexpr int t[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  return (m == 2 && is_leap(y)) ? 29 : t[m - 1];
}

constexpr bool valid_ymd(int y, int m, int d) noexcept {
  return m >= 1 && m <= 12 && d >= 1 && d <= days_in_month(y, m);
}

// Days since 1970-01-01 for a valid civil date (Hinnant, days_from_civil).
constexpr Date days_from_civil(int y, int m, int d) noexcept {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);                                   // [0, 399]
  const unsigned doy = (153 * static_cast<unsigned>(m + (m > 2 ? -3 : 9)) + 2) / 5 + static_cast<unsigned>(d) - 1;  // [0, 365]
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;                                  // [0, 146096]
  return static_cast<Date>(era * 146097 + static_cast<int>(doe) - 719468);
}

constexpr Date days_from_civil(const YMD& x) noexcept { return days_from_civil(x.y, x.m, x.d); }

// Civil date of a day count (Hinnant, civil_from_days).
constexpr YMD civil_from_days(Date z) noexcept {
  z += 719468;
  const int era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(z - era * 146097);                                 // [0, 146096]
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;                   // [0, 399]
  const int y = static_cast<int>(yoe) + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);                                 // [0, 365]
  const unsigned mp = (5 * doy + 2) / 153;                                                      // [0, 11]
  const unsigned d = doy - (153 * mp + 2) / 5 + 1;                                              // [1, 31]
  const unsigned m = mp < 10 ? mp + 3 : mp - 9;                                                 // [1, 12]
  return YMD{y + (m <= 2), static_cast<int>(m), static_cast<int>(d)};
}

// Checked construction from y/m/d: an invalid civil date throws DateError.
inline Date make_date(int y, int m, int d) {
  if (!valid_ymd(y, m, d)) {
    throw DateError("date: invalid civil date " + std::to_string(y) + "-" + std::to_string(m) + "-" +
                    std::to_string(d));
  }
  return days_from_civil(y, m, d);
}

constexpr int year_of(Date d) noexcept { return civil_from_days(d).y; }
constexpr int month_of(Date d) noexcept { return civil_from_days(d).m; }
constexpr int day_of_month(Date d) noexcept { return civil_from_days(d).d; }

// 0 = Monday ... 6 = Sunday. 1970-01-01 was a Thursday (3).
constexpr int weekday(Date d) noexcept {
  const int w = (d + 3) % 7;
  return w < 0 ? w + 7 : w;
}

constexpr bool is_weekend_sat_sun(Date d) noexcept { return weekday(d) >= 5; }

constexpr Date end_of_month(Date d) noexcept {
  const YMD x = civil_from_days(d);
  return days_from_civil(x.y, x.m, days_in_month(x.y, x.m));
}

constexpr bool is_end_of_month(Date d) noexcept { return end_of_month(d) == d; }

constexpr Date start_of_month(Date d) noexcept {
  const YMD x = civil_from_days(d);
  return days_from_civil(x.y, x.m, 1);
}

// Adds n months keeping the day of month, clipped to the target month's length
// (2026-01-31 + 1M = 2026-02-28). With eom = true a date that is the last day of its month
// lands on the last day of the target month (2026-02-28 + 1M = 2026-03-31).
constexpr Date add_months(Date d, int n, bool eom = false) noexcept {
  const YMD x = civil_from_days(d);
  int total = x.y * 12 + (x.m - 1) + n;
  const int y = total >= 0 ? total / 12 : (total - 11) / 12;
  const int m = total - y * 12 + 1;
  const int dim = days_in_month(y, m);
  int day = x.d < dim ? x.d : dim;
  if (eom && x.d == days_in_month(x.y, x.m)) day = dim;
  return days_from_civil(y, m, day);
}

constexpr Date add_years(Date d, int n, bool eom = false) noexcept { return add_months(d, 12 * n, eom); }

// The n-th (1-based) given weekday of the month; n = -1 is the last one.
constexpr Date nth_weekday_of_month(int y, int m, int wd, int n) noexcept {
  if (n > 0) {
    const Date first = days_from_civil(y, m, 1);
    int delta = wd - weekday(first);
    if (delta < 0) delta += 7;
    return first + delta + 7 * (n - 1);
  }
  const Date last = days_from_civil(y, m, days_in_month(y, m));
  int delta = weekday(last) - wd;
  if (delta < 0) delta += 7;
  return last - delta + 7 * (n + 1);
}

// Easter Sunday of the Gregorian calendar (the anonymous Gregorian algorithm / Meeus).
constexpr Date easter_sunday(int y) noexcept {
  const int a = y % 19;
  const int b = y / 100;
  const int c = y % 100;
  const int d = b / 4;
  const int e = b % 4;
  const int f = (b + 8) / 25;
  const int g = (b - f + 1) / 3;
  const int h = (19 * a + b - d - g + 15) % 30;
  const int i = c / 4;
  const int k = c % 4;
  const int l = (32 + 2 * e + 2 * i - h - k) % 7;
  const int m = (a + 11 * h + 22 * l) / 451;
  const int month = (h + l - 7 * m + 114) / 31;
  const int day = ((h + l - 7 * m + 114) % 31) + 1;
  return days_from_civil(y, month, day);
}

// The IMM date of a month: its third Wednesday.
constexpr Date imm_date(int y, int m) noexcept { return nth_weekday_of_month(y, m, 2, 3); }

constexpr bool is_imm_month(int m) noexcept { return m == 3 || m == 6 || m == 9 || m == 12; }

// The first quarterly IMM date strictly after d (or on d when on_or_after = true).
constexpr Date next_imm(Date d, bool on_or_after = false) noexcept {
  YMD x = civil_from_days(d);
  int y = x.y;
  int m = x.m;
  for (int k = 0; k < 8; ++k) {
    if (is_imm_month(m)) {
      const Date cand = imm_date(y, m);
      if (cand > d || (on_or_after && cand == d)) return cand;
    }
    if (++m > 12) {
      m = 1;
      ++y;
    }
  }
  return d;  // unreachable
}

// ISO 8601 text "YYYY-MM-DD" <-> Date.
inline std::string to_iso(Date d) {
  const YMD x = civil_from_days(d);
  char buf[16];
  const int n = x.y < 0 ? 0 : x.y;
  buf[0] = static_cast<char>('0' + (n / 1000) % 10);
  buf[1] = static_cast<char>('0' + (n / 100) % 10);
  buf[2] = static_cast<char>('0' + (n / 10) % 10);
  buf[3] = static_cast<char>('0' + n % 10);
  buf[4] = '-';
  buf[5] = static_cast<char>('0' + x.m / 10);
  buf[6] = static_cast<char>('0' + x.m % 10);
  buf[7] = '-';
  buf[8] = static_cast<char>('0' + x.d / 10);
  buf[9] = static_cast<char>('0' + x.d % 10);
  buf[10] = '\0';
  return buf;
}

inline Date parse_iso(std::string_view s) {
  auto bad = [&] { throw DateError("date: expected YYYY-MM-DD, got \"" + std::string(s) + "\""); };
  if (s.size() != 10 || s[4] != '-' || s[7] != '-') bad();
  auto num = [&](std::size_t from, std::size_t len) {
    int v = 0;
    for (std::size_t i = from; i < from + len; ++i) {
      if (s[i] < '0' || s[i] > '9') bad();
      v = v * 10 + (s[i] - '0');
    }
    return v;
  };
  return make_date(num(0, 4), num(5, 2), num(8, 2));
}

}  // namespace epykos::conventions
