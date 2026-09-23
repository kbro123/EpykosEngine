#include "epykos/conventions/imm.hpp"

#include "epykos/conventions/daycount.hpp"

namespace epykos::conventions {

std::vector<Date> imm_dates(Date from, int count) {
  std::vector<Date> out;
  Date d = from;
  for (int i = 0; i < count; ++i) {
    d = next_imm(d);
    out.push_back(d);
  }
  return out;
}

std::string futures_code(int year, int month) {
  static const char codes[13] = "FGHJKMNQUVXZ";
  std::string s(1, codes[month - 1]);
  const int yy = year % 100;
  s += static_cast<char>('0' + yy / 10);
  s += static_cast<char>('0' + yy % 10);
  return s;
}

std::vector<FuturesPeriod> sofr_3m_futures(Date from, int count, const Calendar& cal) {
  std::vector<FuturesPeriod> out;
  Date start = next_imm(from);
  for (int i = 0; i < count; ++i) {
    FuturesPeriod p;
    const YMD s = civil_from_days(start);
    p.year = s.y;
    p.month = s.m;
    p.code = "SR3 " + futures_code(s.y, s.m);
    p.start = start;
    p.end = next_imm(start);
    // CME: trading terminates on the business day preceding the third Wednesday of the month
    // following the delivery month's quarter end, i.e. the exchange day before p.end.
    p.last_trading = cal.previous_business_day(p.end - 1);
    out.push_back(p);
    start = p.end;
  }
  return out;
}

std::vector<FuturesPeriod> sofr_1m_futures(Date from, int count, const Calendar& cal) {
  std::vector<FuturesPeriod> out;
  Date month_start = add_months(start_of_month(from), 1);
  for (int i = 0; i < count; ++i) {
    FuturesPeriod p;
    const YMD s = civil_from_days(month_start);
    p.year = s.y;
    p.month = s.m;
    p.code = "SR1 " + futures_code(s.y, s.m);
    p.start = month_start;
    p.end = add_months(month_start, 1);
    p.last_trading = cal.previous_business_day(p.end - 1);  // the last business day of the month
    out.push_back(p);
    month_start = p.end;
  }
  return out;
}

std::vector<FuturesPeriod> euribor_3m_futures(Date from, int count, const Calendar& target, int spot_lag) {
  std::vector<FuturesPeriod> out;
  Date probe = from;
  while (static_cast<int>(out.size()) < count) {
    const Date imm = next_imm(probe);
    probe = imm;
    const YMD s = civil_from_days(imm);
    FuturesPeriod p;
    p.year = s.y;
    p.month = s.m;
    p.code = "FEU3 " + futures_code(s.y, s.m);
    p.last_trading = target.add_business_days(imm, -2);   // two exchange days before the third Wednesday
    if (p.last_trading <= from) continue;
    p.fixing = p.last_trading;
    p.start = target.add_business_days(p.fixing, spot_lag); // the deposit's value date (= the IMM date on TARGET)
    p.end = target.adjust(add_months(p.start, 3), BusinessDayConvention::ModifiedFollowing);
    out.push_back(p);
  }
  return out;
}

}  // namespace epykos::conventions
