#include "epykos/conventions/calendar.hpp"

#include <algorithm>

namespace epykos::conventions {

const char* to_string(Observance o) noexcept {
  switch (o) {
    case Observance::None: return "none";
    case Observance::SatToFriSunToMon: return "sat_to_fri_sun_to_mon";
    case Observance::SunToMon: return "sun_to_mon";
    case Observance::SatToFri: return "sat_to_fri";
    case Observance::NextWeekday: return "next_weekday";
  }
  return "?";
}

Observance observance_from_string(const std::string& s) {
  if (s == "none") return Observance::None;
  if (s == "sat_to_fri_sun_to_mon") return Observance::SatToFriSunToMon;
  if (s == "sun_to_mon") return Observance::SunToMon;
  if (s == "sat_to_fri") return Observance::SatToFri;
  if (s == "next_weekday") return Observance::NextWeekday;
  throw DateError("calendar: unknown observance \"" + s +
                  "\" (none | sat_to_fri_sun_to_mon | sun_to_mon | sat_to_fri | next_weekday)");
}

const char* to_string(BusinessDayConvention c) noexcept {
  switch (c) {
    case BusinessDayConvention::Unadjusted: return "Unadjusted";
    case BusinessDayConvention::Following: return "Following";
    case BusinessDayConvention::ModifiedFollowing: return "ModifiedFollowing";
    case BusinessDayConvention::Preceding: return "Preceding";
    case BusinessDayConvention::ModifiedPreceding: return "ModifiedPreceding";
  }
  return "?";
}

BusinessDayConvention bdc_from_string(const std::string& s) {
  if (s == "Unadjusted") return BusinessDayConvention::Unadjusted;
  if (s == "Following") return BusinessDayConvention::Following;
  if (s == "ModifiedFollowing") return BusinessDayConvention::ModifiedFollowing;
  if (s == "Preceding") return BusinessDayConvention::Preceding;
  if (s == "ModifiedPreceding") return BusinessDayConvention::ModifiedPreceding;
  throw DateError("calendar: unknown business day convention \"" + s +
                  "\" (Unadjusted | Following | ModifiedFollowing | Preceding | ModifiedPreceding)");
}

// Applies an observance rule to a nominal date (Saturday/Sunday handling). Returns false when
// the holiday is not observed at all (a Saturday under sun_to_mon).
static bool observe(Date nominal, Observance obs, Date* out) noexcept {
  const int wd = weekday(nominal);
  *out = nominal;
  switch (obs) {
    case Observance::None: return true;
    case Observance::SatToFriSunToMon:
      if (wd == 5) *out = nominal - 1;
      if (wd == 6) *out = nominal + 1;
      return true;
    case Observance::SunToMon:
      if (wd == 5) return false;
      if (wd == 6) *out = nominal + 1;
      return true;
    case Observance::SatToFri:
      if (wd == 5) *out = nominal - 1;
      if (wd == 6) return false;
      return true;
    case Observance::NextWeekday:
      if (wd == 5) *out = nominal + 2;
      if (wd == 6) *out = nominal + 1;
      return true;
  }
  return true;
}

bool HolidayRule::date_for_year(int y, Observance default_obs, Date* out) const noexcept {
  if (from_year != 0 && y < from_year) return false;
  if (to_year != 0 && y > to_year) return false;
  switch (kind) {
    case Kind::Fixed: {
      if (!valid_ymd(y, month, day)) return false;
      const Date nominal = days_from_civil(y, month, day);
      return observe(nominal, has_observance ? observance : default_obs, out);
    }
    case Kind::NthWeekday: {
      const Date d = nth_weekday_of_month(y, month, weekday, n);
      if (month_of(d) != month) return false;  // a 5th weekday that does not exist
      *out = d;
      return true;
    }
    case Kind::EasterOffset: {
      const Date d = easter_sunday(y) + offset_days;
      if (skip_if_first_weekday_of_month && day_of_month(d) <= 7) return false;
      *out = d;
      return true;
    }
  }
  return false;
}

Calendar::Calendar(std::string name, std::vector<int> weekend, std::vector<HolidayRule> rules,
                   Observance default_observance, std::vector<Date> extra_holidays,
                   std::vector<Date> not_holidays, int first_year, int last_year)
    : name_(std::move(name)),
      weekend_(std::move(weekend)),
      rules_(std::move(rules)),
      default_observance_(default_observance),
      extra_(std::move(extra_holidays)),
      not_(std::move(not_holidays)),
      first_year_(first_year),
      last_year_(last_year) {
  if (first_year_ > last_year_) throw DateError("calendar " + name_ + ": first_year > last_year");
  for (int w : weekend_) {
    if (w < 0 || w > 6) throw DateError("calendar " + name_ + ": weekend weekday out of range");
  }
  build();
}

Calendar::Calendar(std::string name, std::vector<const Calendar*> members)
    : name_(std::move(name)), members_(std::move(members)) {
  if (members_.empty()) throw DateError("calendar " + name_ + ": a joint calendar needs members");
  first_year_ = members_[0]->first_year_;
  last_year_ = members_[0]->last_year_;
  for (const Calendar* m : members_) {
    first_year_ = std::max(first_year_, m->first_year_);
    last_year_ = std::min(last_year_, m->last_year_);
  }
  if (first_year_ > last_year_) throw DateError("calendar " + name_ + ": members' year spans do not overlap");
  first_day_ = days_from_civil(first_year_, 1, 1);
  last_day_ = days_from_civil(last_year_, 12, 31);
  // Weekend: the union of the members' weekends; holidays: the union (any member closed).
  for (const Calendar* m : members_) {
    for (int w : m->weekend_) {
      if (std::find(weekend_.begin(), weekend_.end(), w) == weekend_.end()) weekend_.push_back(w);
    }
  }
  std::sort(weekend_.begin(), weekend_.end());
  holiday_.assign(static_cast<std::size_t>(last_day_ - first_day_ + 1), 0);
  for (Date d = first_day_; d <= last_day_; ++d) {
    if (is_weekend(d)) continue;
    for (const Calendar* m : members_) {
      if (m->is_holiday(d)) {
        holiday_[static_cast<std::size_t>(d - first_day_)] = 1;
        break;
      }
    }
  }
}

void Calendar::build() {
  first_day_ = days_from_civil(first_year_, 1, 1);
  last_day_ = days_from_civil(last_year_, 12, 31);
  holiday_.assign(static_cast<std::size_t>(last_day_ - first_day_ + 1), 0);
  auto mark = [&](Date d) {
    if (d < first_day_ || d > last_day_) return;
    if (is_weekend(d)) return;  // a holiday on the weekend is not a weekday close
    holiday_[static_cast<std::size_t>(d - first_day_)] = 1;
  };
  // Rules of year y may observe on Dec 31 of y-1, so generate one year beyond each end.
  for (int y = first_year_ - 1; y <= last_year_ + 1; ++y) {
    for (const HolidayRule& r : rules_) {
      Date d;
      if (r.date_for_year(y, default_observance_, &d)) mark(d);
    }
  }
  for (Date d : extra_) mark(d);
  for (Date d : not_) {
    if (d >= first_day_ && d <= last_day_) holiday_[static_cast<std::size_t>(d - first_day_)] = 0;
  }
}

std::size_t Calendar::index(Date d) const {
  if (d < first_day_ || d > last_day_) {
    throw DateError("calendar " + name_ + ": " + to_iso(d) + " is outside the generated span " +
                    std::to_string(first_year_) + "-" + std::to_string(last_year_));
  }
  return static_cast<std::size_t>(d - first_day_);
}

bool Calendar::is_weekend(Date d) const noexcept {
  const int wd = weekday(d);
  for (int w : weekend_) {
    if (w == wd) return true;
  }
  return false;
}

bool Calendar::is_holiday(Date d) const { return holiday_[index(d)] != 0; }

bool Calendar::is_business_day(Date d) const { return !is_weekend(d) && !is_holiday(d); }

std::vector<Date> Calendar::holidays(int y) const {
  return holidays(days_from_civil(y, 1, 1), days_from_civil(y, 12, 31));
}

std::vector<Date> Calendar::holidays(Date from, Date to) const {
  std::vector<Date> out;
  for (Date d = from; d <= to; ++d) {
    if (is_holiday(d)) out.push_back(d);
  }
  return out;
}

Date Calendar::next_business_day(Date d) const {
  while (!is_business_day(d)) ++d;
  return d;
}

Date Calendar::previous_business_day(Date d) const {
  while (!is_business_day(d)) --d;
  return d;
}

Date Calendar::adjust(Date d, BusinessDayConvention c) const {
  switch (c) {
    case BusinessDayConvention::Unadjusted: return d;
    case BusinessDayConvention::Following: return next_business_day(d);
    case BusinessDayConvention::Preceding: return previous_business_day(d);
    case BusinessDayConvention::ModifiedFollowing: {
      const Date f = next_business_day(d);
      return month_of(f) == month_of(d) ? f : previous_business_day(d);
    }
    case BusinessDayConvention::ModifiedPreceding: {
      const Date p = previous_business_day(d);
      return month_of(p) == month_of(d) ? p : next_business_day(d);
    }
  }
  return d;
}

Date Calendar::add_business_days(Date d, int n) const {
  if (n > 0) {
    for (int k = 0; k < n; ++k) d = next_business_day(d + 1);
  } else if (n < 0) {
    for (int k = 0; k < -n; ++k) d = previous_business_day(d - 1);
  }
  return d;
}

int Calendar::business_days_between(Date from, Date to) const {
  if (to < from) return -business_days_between(to, from);
  int n = 0;
  for (Date d = from; d < to; ++d) {
    if (is_business_day(d)) ++n;
  }
  return n;
}

std::vector<Date> Calendar::business_days(Date from, Date to) const {
  std::vector<Date> out;
  for (Date d = from; d < to; ++d) {
    if (is_business_day(d)) out.push_back(d);
  }
  return out;
}

}  // namespace epykos::conventions
