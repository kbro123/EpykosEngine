#include "epykos/conventions/daycount.hpp"

#include <cctype>

namespace epykos::conventions {

Period Period::parse(std::string_view s) {
  auto bad = [&] { throw DateError("period: expected <n><D|W|M|Y>, got \"" + std::string(s) + "\""); };
  if (s.size() < 2) bad();
  int n = 0;
  std::size_t i = 0;
  for (; i + 1 < s.size(); ++i) {
    if (s[i] < '0' || s[i] > '9') bad();
    n = n * 10 + (s[i] - '0');
    if (n > 100000) bad();
  }
  char u = static_cast<char>(std::toupper(static_cast<unsigned char>(s[i])));
  if (u != 'D' && u != 'W' && u != 'M' && u != 'Y') bad();
  return Period{n, u};
}

std::string Period::to_string() const { return std::to_string(n) + unit; }

int Period::months() const {
  if (unit == 'M') return n;
  if (unit == 'Y') return 12 * n;
  throw DateError("period: " + to_string() + " is not a monthly tenor");
}

Date add_period(Date d, const Period& p, bool eom) {
  switch (p.unit) {
    case 'D': return d + p.n;
    case 'W': return d + 7 * p.n;
    case 'M': return add_months(d, p.n, eom);
    case 'Y': return add_months(d, 12 * p.n, eom);
    default: throw DateError("period: bad unit");
  }
}

const char* to_string(DayCount dc) noexcept {
  switch (dc) {
    case DayCount::Act360: return "ACT/360";
    case DayCount::Act365F: return "ACT/365F";
    case DayCount::Thirty360US: return "30/360";
    case DayCount::ThirtyE360: return "30E/360";
    case DayCount::ActActISDA: return "ACT/ACT ISDA";
  }
  return "?";
}

DayCount daycount_from_string(const std::string& s) {
  if (s == "ACT/360" || s == "Act/360" || s == "A/360") return DayCount::Act360;
  if (s == "ACT/365F" || s == "Act/365F" || s == "ACT/365.FIXED" || s == "A/365F") return DayCount::Act365F;
  if (s == "30/360" || s == "30U/360" || s == "30/360 US" || s == "30/360 Bond Basis") return DayCount::Thirty360US;
  if (s == "30E/360" || s == "30E/360 Eurobond") return DayCount::ThirtyE360;
  if (s == "ACT/ACT ISDA" || s == "ACT/ACT-ISDA" || s == "Act/Act ISDA" || s == "ACT/ACT") return DayCount::ActActISDA;
  throw DateError("daycount: unknown day count \"" + s +
                  "\" (ACT/360 | ACT/365F | 30/360 | 30E/360 | ACT/ACT ISDA)");
}

int days_30_360_us(Date d1, Date d2) noexcept {
  YMD a = civil_from_days(d1);
  YMD b = civil_from_days(d2);
  // ISDA 2006 4.16(f) "30/360": D1 = 30 if D1 = 31; D2 = 30 if D2 = 31 and D1 in {30, 31}.
  if (a.d == 31) a.d = 30;
  if (b.d == 31 && a.d == 30) b.d = 30;
  return 360 * (b.y - a.y) + 30 * (b.m - a.m) + (b.d - a.d);
}

int days_30e_360(Date d1, Date d2) noexcept {
  YMD a = civil_from_days(d1);
  YMD b = civil_from_days(d2);
  // ISDA 2006 4.16(g) "30E/360": D1 = 30 if D1 = 31; D2 = 30 if D2 = 31.
  if (a.d == 31) a.d = 30;
  if (b.d == 31) b.d = 30;
  return 360 * (b.y - a.y) + 30 * (b.m - a.m) + (b.d - a.d);
}

double year_fraction(DayCount dc, Date d1, Date d2) {
  if (d2 < d1) return -year_fraction(dc, d2, d1);
  switch (dc) {
    case DayCount::Act360: return static_cast<double>(d2 - d1) / 360.0;
    case DayCount::Act365F: return static_cast<double>(d2 - d1) / 365.0;
    case DayCount::Thirty360US: return static_cast<double>(days_30_360_us(d1, d2)) / 360.0;
    case DayCount::ThirtyE360: return static_cast<double>(days_30e_360(d1, d2)) / 360.0;
    case DayCount::ActActISDA: {
      // ISDA 2006 4.16(b): split at calendar-year boundaries; each piece over its year's length.
      const int y1 = year_of(d1);
      const int y2 = year_of(d2);
      if (y1 == y2) return static_cast<double>(d2 - d1) / (is_leap(y1) ? 366.0 : 365.0);
      double sum = static_cast<double>(days_from_civil(y1 + 1, 1, 1) - d1) / (is_leap(y1) ? 366.0 : 365.0);
      sum += static_cast<double>(y2 - y1 - 1);
      sum += static_cast<double>(d2 - days_from_civil(y2, 1, 1)) / (is_leap(y2) ? 366.0 : 365.0);
      return sum;
    }
  }
  return 0.0;
}

}  // namespace epykos::conventions
