// EpykosEngine — holiday calendars from published rules (M3/G0, D36).
//
// A calendar is DATA: a weekend and a list of holiday-rule INSTANCES read from
// blueprints/conventions/*.json (with the rule's source cited in the JSON). The rule KINDS are
// this code:
//
//   fixed          {month, day, from_year?, to_year?, observance?}      New Year's Day, Christmas
//   nth_weekday    {month, weekday, n}  n = 1..5 or -1 for the last     MLK Day, Memorial Day
//   easter_offset  {days, skip_if_first_weekday_of_month?}              Good Friday (-2), Easter Monday (+1)
//
// and the observance (what happens when a fixed-date holiday lands on the weekend):
//
//   none                the date itself, weekend or not (TARGET; every weekday-relative rule)
//   sat_to_fri_sun_to_mon   Saturday -> preceding Friday, Sunday -> following Monday (SIFMA)
//   sun_to_mon          Sunday -> Monday only; a Saturday holiday is not observed (Federal Reserve)
//   sat_to_fri          Saturday -> Friday only
//   next_weekday        Saturday and Sunday -> the following Monday (UK bank-holiday style)
//
// A calendar's observance is the default for its fixed rules; a rule may override it. A rule
// may also carry an explicit list of extra dates (one-off closes) and of exclusions.
//
// A joint calendar is closed when any of its members is closed. Calendars are immutable once
// built; every query is an integer lookup in a bitmap that spans [first_year, last_year].
// Nothing here depends on a Scalar.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "epykos/conventions/date.hpp"

namespace epykos::conventions {

enum class Observance : std::uint8_t { None, SatToFriSunToMon, SunToMon, SatToFri, NextWeekday };

const char* to_string(Observance o) noexcept;
Observance observance_from_string(const std::string& s);  // throws DateError on an unknown name

struct HolidayRule {
  enum class Kind : std::uint8_t { Fixed, NthWeekday, EasterOffset };
  Kind kind = Kind::Fixed;
  std::string label;
  int month = 1;                  // Fixed, NthWeekday
  int day = 1;                    // Fixed
  int weekday = 0;                // NthWeekday: 0 = Monday .. 6 = Sunday
  int n = 1;                      // NthWeekday: 1..5, or -1 for the last
  int offset_days = 0;            // EasterOffset: days relative to Easter Sunday
  bool skip_if_first_weekday_of_month = false;  // EasterOffset: SIFMA's Good Friday exception
  int from_year = 0;              // 0 = always
  int to_year = 0;                // 0 = always (inclusive otherwise)
  bool has_observance = false;    // false: the calendar's default applies
  Observance observance = Observance::None;
  std::string source;             // citation, carried from the JSON

  // The holiday's date(s) generated for calendar year y, after observance. A rule yields at
  // most one date per year; the observed date may fall in the previous year (Jan 1 on a
  // Saturday observed on Dec 31). Returns false if the rule does not apply in y.
  bool date_for_year(int y, Observance default_obs, Date* out) const noexcept;
};

enum class BusinessDayConvention : std::uint8_t {
  Unadjusted,
  Following,
  ModifiedFollowing,
  Preceding,
  ModifiedPreceding,
};

const char* to_string(BusinessDayConvention c) noexcept;
BusinessDayConvention bdc_from_string(const std::string& s);  // throws DateError on an unknown name

class Calendar {
 public:
  // A rule calendar. `weekend` lists weekdays (0 = Monday .. 6 = Sunday). Holidays are
  // generated for every year in [first_year, last_year]; queries outside that span throw.
  Calendar(std::string name, std::vector<int> weekend, std::vector<HolidayRule> rules,
           Observance default_observance, std::vector<Date> extra_holidays = {},
           std::vector<Date> not_holidays = {}, int first_year = 1990, int last_year = 2100);

  // A joint calendar: closed when any member is closed. Members must outlive it (the registry
  // owns them).
  Calendar(std::string name, std::vector<const Calendar*> members);

  const std::string& name() const noexcept { return name_; }
  bool is_joint() const noexcept { return !members_.empty(); }
  const std::vector<const Calendar*>& members() const noexcept { return members_; }
  const std::vector<HolidayRule>& rules() const noexcept { return rules_; }
  int first_year() const noexcept { return first_year_; }
  int last_year() const noexcept { return last_year_; }

  bool is_weekend(Date d) const noexcept;
  bool is_holiday(Date d) const;             // a non-weekend closing day
  bool is_business_day(Date d) const;

  // The holidays (weekday closes) of calendar year y, ascending.
  std::vector<Date> holidays(int y) const;
  // The holidays in [from, to], ascending.
  std::vector<Date> holidays(Date from, Date to) const;

  Date adjust(Date d, BusinessDayConvention c) const;
  Date next_business_day(Date d) const;      // the first business day >= d
  Date previous_business_day(Date d) const;  // the last business day <= d
  // Moves n business days (n may be negative or zero; zero returns d unchanged, even on a
  // holiday — the ISDA "no adjustment" of a zero offset).
  Date add_business_days(Date d, int n) const;
  // Number of business days in [from, to) (to < from gives the negative count).
  int business_days_between(Date from, Date to) const;
  // The business days in [from, to), ascending.
  std::vector<Date> business_days(Date from, Date to) const;

 private:
  void build();
  std::size_t index(Date d) const;

  std::string name_;
  std::vector<int> weekend_;
  std::vector<HolidayRule> rules_;
  Observance default_observance_ = Observance::None;
  std::vector<Date> extra_;
  std::vector<Date> not_;
  std::vector<const Calendar*> members_;
  int first_year_ = 1990;
  int last_year_ = 2100;
  Date first_day_ = 0;
  Date last_day_ = 0;
  std::vector<std::uint8_t> holiday_;  // 1 = weekday closing day, indexed from first_day_
};

}  // namespace epykos::conventions
