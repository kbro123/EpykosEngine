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

// WHICH PERIOD'S DAY COUNT THE COMPOUNDED AMOUNT ACCRUES OVER — a TERM, not an inference (D130).
//
// Under a shift the observation period and the interest period can differ in length (365 against
// 367 on a 1Y SOFR coupon), and then it matters what the compounded product is applied over. Two
// arithmetics are documented and both are in production code in the wild, so this engine states
// which one a convention means instead of choosing for it:
//
//   ObservationPeriod   interest = N·(Π − 1). The ARRC's option (1) — "apply interest for the
//                       number of days in the observation period" — and what ISDA's own paper
//                       describes: "the rate will be annualized and apply based on the number of
//                       days in the Calculation Period", i.e. annualised by d_c and applied over
//                       d_c, which cancel. The two authorities DIFFER on the quoted rate's basis
//                       (ARRC d_o, ISDA d_c) and AGREE on the cash flow. QuantLib computes this.
//   InterestPeriod      interest = N·(Π − 1)·d_c/d_o: the rate annualised over the observation
//                       period and applied over the interest period. The ARRC's option (2), which
//                       it labels an "interest-period weighted shift", records as "widely used in
//                       FRNs", and does NOT recommend ("not the principle recommendation";
//                       "can at times result in a negative daily accrual even if SOFR rates are
//                       positive"). SwapEngine computes this, and so did this engine until D123.
//
// It is a NO-OP wherever there is no shift, because the observation weights then sum to the
// interest period's own length — so a plain or lookback coupon is bit-identical under either.
enum class ShiftAccrual : unsigned char { ObservationPeriod, InterestPeriod };

const char* to_string(ShiftAccrual a) noexcept;   // "observation_period", "interest_period"
ShiftAccrual shift_accrual_from_string(const std::string& s);

struct ObservationSpec {
  ObservationMethod method = ObservationMethod::Plain;
  int lookback_days = 0;   // business days; used by Lookback and ObservationShift
  int lockout_days = 0;    // business days frozen at the end of the period
  // The default is the reading ISDA and the ARRC agree on; a convention that means the other one
  // says so (`"accrual": "interest_period"`).
  ShiftAccrual accrual = ShiftAccrual::ObservationPeriod;
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
