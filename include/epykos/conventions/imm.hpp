// EpykosEngine — IMM dates and money-market futures periods (M3/G0).
//
//   IMM date         the third Wednesday of March / June / September / December (date.hpp)
//   SR3 (CME 3M SOFR)  reference quarter [IMM(m), IMM(m + 3)); settles to 100 minus the
//                    compounded SOFR over the quarter (calendar-day weighted, ACT/360)
//   SR1 (CME 1M SOFR)  the calendar month; settles to 100 minus the arithmetic average of the
//                    daily SOFR over the month (calendar-day weighted, ACT/360)
//   FEU3 (Eurex 3M EURIBOR)  last trading day two exchange days before the third Wednesday of
//                    the delivery month, the fixing of that day; the underlying deposit runs
//                    from the third Wednesday (spot of the fixing) for three months (TARGET,
//                    modified following)
//
// Everything here is dates (ints) and codes; the convexity treatment of a futures quote is a
// pricing matter (PROBLEM.md section 9, stated as a simplification). Nothing depends on a Scalar.
#pragma once

#include <string>
#include <vector>

#include "epykos/conventions/calendar.hpp"

namespace epykos::conventions {

// The `count` quarterly IMM dates strictly after `from`.
std::vector<Date> imm_dates(Date from, int count);

// A futures month code "H26", "M26", "U26", "Z26" (F G H J K M N Q U V X Z) for a period.
std::string futures_code(int year, int month);

struct FuturesPeriod {
  std::string code;      // e.g. "SR3 M26", "SR1 K26", "FEU3 M26"
  int year = 0;
  int month = 0;         // delivery / contract month
  Date start = 0;        // first day of the reference period (SR3: IMM; SR1: 1st; FEU3: value date)
  Date end = 0;          // exclusive end of the reference period
  Date last_trading = 0; // last trading day
  Date fixing = 0;       // FEU3: the EURIBOR fixing day (= last trading day); SR1/SR3: 0
};

// CME SR3 contracts: the `count` quarterly reference periods starting with the first IMM date
// strictly after `from` (a contract whose quarter has begun is excluded).
std::vector<FuturesPeriod> sofr_3m_futures(Date from, int count, const Calendar& fixing_calendar);
// CME SR1 contracts: the `count` calendar months starting with the month after `from`'s.
std::vector<FuturesPeriod> sofr_1m_futures(Date from, int count, const Calendar& fixing_calendar);
// Eurex FEU3 contracts: the `count` quarterly contracts whose last trading day is after `from`.
// `target` is the TARGET calendar (fixing and exchange days), `spot_lag` the EURIBOR spot lag.
std::vector<FuturesPeriod> euribor_3m_futures(Date from, int count, const Calendar& target, int spot_lag = 2);

}  // namespace epykos::conventions
