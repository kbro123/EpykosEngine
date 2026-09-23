#include "epykos/conventions/schedule.hpp"

#include <algorithm>

namespace epykos::conventions {

const char* to_string(StubKind s) noexcept {
  switch (s) {
    case StubKind::ShortFront: return "ShortFront";
    case StubKind::LongFront: return "LongFront";
    case StubKind::ShortBack: return "ShortBack";
    case StubKind::LongBack: return "LongBack";
  }
  return "?";
}

StubKind stub_from_string(const std::string& s) {
  if (s == "ShortFront") return StubKind::ShortFront;
  if (s == "LongFront") return StubKind::LongFront;
  if (s == "ShortBack") return StubKind::ShortBack;
  if (s == "LongBack") return StubKind::LongBack;
  throw DateError("schedule: unknown stub rule \"" + s + "\" (ShortFront | LongFront | ShortBack | LongBack)");
}

Schedule generate_schedule(const ScheduleSpec& spec) {
  if (spec.termination <= spec.effective) {
    throw DateError("schedule: termination " + to_iso(spec.termination) + " is not after effective " +
                    to_iso(spec.effective));
  }
  if (spec.bdc != BusinessDayConvention::Unadjusted && spec.calendar == nullptr) {
    throw DateError("schedule: a business day convention needs a calendar");
  }
  Schedule out;
  if (spec.term) {
    out.unadjusted = {spec.effective, spec.termination};
  } else {
    if (spec.frequency.n <= 0) throw DateError("schedule: frequency must be positive");
    const bool front = spec.stub == StubKind::ShortFront || spec.stub == StubKind::LongFront;
    const bool longer = spec.stub == StubKind::LongFront || spec.stub == StubKind::LongBack;
    std::vector<Date> dates;
    if (front) {
      // roll backward from the termination date
      const bool eom = spec.eom && !spec.frequency.is_calendar_days() && is_end_of_month(spec.termination);
      dates.push_back(spec.termination);
      for (int k = 1;; ++k) {
        Period p{spec.frequency.n * k, spec.frequency.unit};
        Date d = spec.frequency.is_calendar_days() ? spec.termination - p.days()
                                                   : add_months(spec.termination, -p.months(), eom);
        if (d <= spec.effective) {
          if (d < spec.effective) {
            // stub between effective and dates.back()
            out.front_stub = true;
            if (longer && dates.size() >= 2) dates.pop_back();  // merge the short stub into the next period
          }
          dates.push_back(spec.effective);
          break;
        }
        dates.push_back(d);
        if (k > 100000) throw DateError("schedule: runaway roll");
      }
      std::reverse(dates.begin(), dates.end());
    } else {
      // roll forward from the effective date
      const bool eom = spec.eom && !spec.frequency.is_calendar_days() && is_end_of_month(spec.effective);
      dates.push_back(spec.effective);
      for (int k = 1;; ++k) {
        Period p{spec.frequency.n * k, spec.frequency.unit};
        Date d = spec.frequency.is_calendar_days() ? spec.effective + p.days()
                                                   : add_months(spec.effective, p.months(), eom);
        if (d >= spec.termination) {
          if (d > spec.termination) {
            out.back_stub = true;
            if (longer && dates.size() >= 2) dates.pop_back();
          }
          dates.push_back(spec.termination);
          break;
        }
        dates.push_back(d);
        if (k > 100000) throw DateError("schedule: runaway roll");
      }
    }
    out.unadjusted = std::move(dates);
  }
  out.adjusted.reserve(out.unadjusted.size());
  for (Date d : out.unadjusted) {
    out.adjusted.push_back(spec.bdc == BusinessDayConvention::Unadjusted ? d : spec.calendar->adjust(d, spec.bdc));
  }
  // Adjustment must not collapse a period (two roll dates on one business day).
  for (std::size_t i = 1; i < out.adjusted.size(); ++i) {
    if (out.adjusted[i] <= out.adjusted[i - 1]) {
      throw DateError("schedule: adjusted roll dates are not increasing at " + to_iso(out.adjusted[i]));
    }
  }
  return out;
}

std::vector<Date> payment_dates(const Schedule& s, int lag, const Calendar& calendar) {
  std::vector<Date> out;
  out.reserve(s.periods());
  for (std::size_t i = 1; i < s.adjusted.size(); ++i) out.push_back(calendar.add_business_days(s.adjusted[i], lag));
  return out;
}

std::vector<Date> fixing_dates_in_advance(const Schedule& s, int fixing_lag, const Calendar& fixing_calendar) {
  std::vector<Date> out;
  out.reserve(s.periods());
  for (std::size_t i = 0; i + 1 < s.adjusted.size(); ++i) {
    out.push_back(fixing_calendar.add_business_days(s.adjusted[i], -fixing_lag));
  }
  return out;
}

Date spot_date(Date trade_date, int spot_lag, const Calendar& calendar) {
  return calendar.add_business_days(trade_date, spot_lag);
}

}  // namespace epykos::conventions
