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

// THE ROLL CONVENTION IS A TERM, NOT AN INFERENCE (D129).
//
// Until 2026-10-13 this struct carried a single `bool eom`, and `generate_schedule` then INFERRED
// whether to roll on month ends by testing the anchor: `spec.eom && is_end_of_month(anchor)`. That
// inference sat in the middle of the trade's terms, so two implementations given the same terms
// were free to disagree about them — and did. QuantLib's `Schedule` gate is
// `Calendar::isEndOfMonth`, a BUSINESS-day test, which fires on a roll date that merely happens to
// be the last business day of its month; passing it our `eom` moved three of Stage A's 2,000
// trades (D127, withdrawn by D128).
//
// The standardised decomposition separates the three things that boolean was doing. FpML's
// `RollConventionEnum` carries an explicit roll — `EOM`, `IMM`, or a day-of-month 1..30 — so there
// is nothing to infer; the field split below is the one SwapEngine uses (`eom`, `eom_auto`,
// `roll_dom`), read under D107 and named here rather than presented as independently arrived at
// (D92 §5's laundering guard).
//
//   eom       FORCE every rolled boundary to the month end. An instruction, not a guess — and a
//             real snap: `add_months`'s own eom rule only carries an existing month end forward
//             and cannot create one, so forcing from an anchor that is not a month end takes
//             `end_of_month` of each rolled date. It is the identity on every anchor `eom_auto`
//             resolves true, so the inference path is unchanged. The endpoints never move; this
//             is QuantLib's `endOfMonth` reading of a non-month-end seed, now expressible here.
//   eom_auto  the ISDA inference: set `eom` when the roll ANCHOR is a calendar month end. This is
//             what the old single `eom` flag actually meant, and `blueprints/`'s `"eom": true`
//             maps here, so the dates are unchanged.
//
// The anchor is the termination for a front stub (rolling backward) and the effective for a back
// stub (rolling forward) — `generate_schedule` resolves it, and `Schedule` reports the resolved
// term (`roll_eom` / `roll_dom`) so a caller records it instead of re-deriving it.
//
// SwapEngine's third field, an explicit `roll_dom` INPUT, is deliberately NOT taken. This
// generator rolls by whole months from an anchor endpoint, so the roll day is the anchor's day by
// construction and a different one is not expressible without making both endpoints stubs. An
// input it could not honour is the very defect this split exists to remove — it would be accepted
// at its name and reported back as a term that the dates do not obey. Nothing in `blueprints/`
// asks for one (every convention rolls on its anchor's day); when something does, the generator
// changes first and the field arrives with it.
struct ScheduleSpec {
  Date effective = 0;
  Date termination = 0;
  Period frequency{1, 'Y'};      // ignored when term = true
  bool term = false;             // one period from effective to termination
  BusinessDayConvention bdc = BusinessDayConvention::ModifiedFollowing;
  const Calendar* calendar = nullptr;  // required unless bdc = Unadjusted
  StubKind stub = StubKind::ShortFront;
  bool eom = false;              // force every rolled boundary to the month end
  bool eom_auto = false;         // ISDA EOM: set `eom` when the roll anchor is a month end
};

struct Schedule {
  std::vector<Date> unadjusted;  // roll dates, effective first, termination last
  std::vector<Date> adjusted;    // the same after the business day convention
  bool front_stub = false;       // the first period is irregular
  bool back_stub = false;        // the last period is irregular
  // The roll convention this schedule was actually generated on, after `eom_auto` was resolved
  // against the anchor. A caller records THIS rather than re-running the inference, so the term
  // has one definition and one place it is decided.
  bool roll_eom = false;         // every rolled boundary is the month end
  int roll_dom = 0;              // otherwise, the day of month they land on
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
