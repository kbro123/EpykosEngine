// EpykosEngine — accrual schedules with stubs and the end-of-month rule (M3/G0).
//
// A schedule is the list of unadjusted roll dates from the effective date to the termination
// date, and the same list adjusted by a business day convention on a calendar. Period i
// accrues [adjusted[i], adjusted[i + 1]).
//
//   * Front stubs roll BACKWARD from the termination date (the regular dates are anchored on
//     the termination date), back stubs roll FORWARD from the effective date.
//   * A short stub keeps the partial first / last period; a long stub merges it with the
//     adjacent regular period. When the tenor divides the term exactly there is no stub.
//   * End-of-month: when the anchor date is the last day of its month and eom is set, every
//     regular roll date is the last day of its month (2006 ISDA Definitions 4.11 "EOM").
//   * A "TERM" (zero-coupon) frequency gives a single period.
//
// Structure only: dates are ints, nothing depends on a Scalar.
#pragma once

#include <string>
#include <vector>

#include "epykos/conventions/calendar.hpp"
#include "epykos/conventions/daycount.hpp"

namespace epykos::conventions {

enum class StubKind : unsigned char { ShortFront, LongFront, ShortBack, LongBack };

const char* to_string(StubKind s) noexcept;
StubKind stub_from_string(const std::string& s);   // throws DateError on an unknown name

struct ScheduleSpec {
  Date effective = 0;
  Date termination = 0;
  Period frequency{1, 'Y'};      // ignored when term = true
  bool term = false;             // one period from effective to termination
  BusinessDayConvention bdc = BusinessDayConvention::ModifiedFollowing;
  const Calendar* calendar = nullptr;  // required unless bdc = Unadjusted
  StubKind stub = StubKind::ShortFront;
  bool eom = false;
};

struct Schedule {
  std::vector<Date> unadjusted;  // roll dates, effective first, termination last
  std::vector<Date> adjusted;    // the same after the business day convention
  bool front_stub = false;       // the first period is irregular
  bool back_stub = false;        // the last period is irregular
  std::size_t periods() const noexcept { return adjusted.empty() ? 0 : adjusted.size() - 1; }
};

Schedule generate_schedule(const ScheduleSpec& spec);

// Payment dates: each period's adjusted end moved by `lag` business days on `calendar`
// (lag = 0 pays on the period end). Returns one date per period.
std::vector<Date> payment_dates(const Schedule& s, int lag, const Calendar& calendar);

// Fixing dates of a term (IBOR-style) leg fixing in advance: each period's adjusted start moved
// back by `fixing_lag` business days on `fixing_calendar`. One date per period.
std::vector<Date> fixing_dates_in_advance(const Schedule& s, int fixing_lag, const Calendar& fixing_calendar);

// The spot date: `spot_lag` business days after `trade_date` on `calendar` (lag 0 = the trade
// date itself, unadjusted — ISDA's "no adjustment" of a zero offset).
Date spot_date(Date trade_date, int spot_lag, const Calendar& calendar);

}  // namespace epykos::conventions
