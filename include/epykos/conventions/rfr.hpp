// EpykosEngine — RFR observation windows and realised compounding (M3/G0).
//
// For a compounded-in-arrears or averaged overnight coupon the STRUCTURE is the list of
// observation days: which day's published rate applies, and for how many calendar days. The
// three conventions of the 2021 ISDA Definitions / ARRC user's guide are:
//
//   plain               business days b_i of the fixing calendar in [start, end); the rate FOR
//                       b_i applies from b_i to the next business day (n_i calendar days; the
//                       last one runs to the period end).
//   lookback (k)        the same days and weights, but the rate applied on b_i is the rate for
//                       the business day k business days before b_i ("lookback without
//                       observation shift").
//   observation shift   the observation period is [start - k bd, end - k bd); its business
//   (k)                 days o_j carry their own rate and their own weight n_j (the calendar
//                       days to the next business day in the observation period).
//
// A lockout of m business days (applied after the method) freezes the rate of the last m
// observation days at the rate of the day before them (ARRC: "the SOFR rate applied for the
// last k days of the interest period is frozen at the rate observed k days before the period
// ends"). Weights do not change.
//
// Compounding from a history gives the realised part of a seasoned coupon as doubles and
// the unfixed days as structure for the pricing maths (G2): nothing here depends on a Scalar.
#pragma once

#include <string>
#include <vector>

#include "epykos/conventions/calendar.hpp"
#include "epykos/conventions/fixings.hpp"

namespace epykos::conventions {

enum class ObservationMethod : unsigned char { Plain, Lookback, ObservationShift };

const char* to_string(ObservationMethod m) noexcept;   // "plain", "lookback", "observation_shift"
ObservationMethod observation_method_from_string(const std::string& s);

struct ObservationSpec {
  ObservationMethod method = ObservationMethod::Plain;
  int lookback_days = 0;   // business days; used by Lookback and ObservationShift
  int lockout_days = 0;    // business days frozen at the end of the period
};

struct ObservationDay {
  Date accrual_day;   // the business day of the (observation) period this entry accrues for
  Date rate_date;     // the day whose published rate applies
  int weight_days;    // calendar days the rate is applied for (n_i)
};

struct ObservationPeriod {
  Date accrual_start;   // the calculation period [accrual_start, accrual_end)
  Date accrual_end;
  Date obs_start;       // the observation period (equal to the accrual period unless shifted)
  Date obs_end;
  std::vector<ObservationDay> days;
  int weight_total() const noexcept;   // sum of weight_days = obs_end - obs_start
  int accrual_days() const noexcept { return accrual_end - accrual_start; }
};

// The observation days of one calculation period on the index's fixing calendar.
// If accrual_start (or the shifted start) is not a fixing business day, the first entry
// carries the rate of the preceding business day from the start to the first business day,
// so the weights always sum to the period length.
ObservationPeriod observation_days(Date accrual_start, Date accrual_end, const ObservationSpec& spec,
                                   const Calendar& fixing_calendar);

// The realised part of a coupon: the compounding factor and weighted sum over the observation
// days whose rate is in the history, and the days still to be fixed. Fixed days must be a prefix
// of the observation days; a fixing that reappears after a gap is an error.
struct RealisedObservations {
  double factor = 1.0;        // prod (1 + r_i * n_i / basis) over the fixed days
  double weighted_sum = 0.0;  // sum r_i * n_i over the fixed days
  int fixed_count = 0;        // observation entries with a rate in the history
  int fixed_weight = 0;       // sum n_i over the fixed entries
  std::vector<ObservationDay> unfixed;  // the remaining entries, in order
  bool complete() const noexcept { return unfixed.empty(); }
};

RealisedObservations realised_observations(const ObservationPeriod& p, const FixingsHistory& history,
                                           const std::string& index, double basis = 360.0);

// Fully realised coupon rates (every observation day fixed; otherwise DateError):
//   compounded: (prod (1 + r_i n_i / basis) - 1) * basis / weight_total
//   average:    sum r_i n_i / weight_total
double compounded_rate(const ObservationPeriod& p, const FixingsHistory& history, const std::string& index,
                       double basis = 360.0);
double average_rate(const ObservationPeriod& p, const FixingsHistory& history, const std::string& index);

}  // namespace epykos::conventions
