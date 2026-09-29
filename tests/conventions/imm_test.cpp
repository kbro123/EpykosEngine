// M3/G0: IMM dates and money-market futures periods (CME SR3 / SR1, Eurex FEU3).
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "epykos/conventions/imm.hpp"
#include "epykos/conventions/registry.hpp"

using namespace epykos::conventions;

namespace {
const Registry& reg() {
  static Registry r = Registry::load();
  return r;
}
}  // namespace

TEST(Imm, QuarterlyDatesFrom2026) {
  const std::vector<Date> d = imm_dates(parse_iso("2026-09-23"), 6);
  ASSERT_EQ(d.size(), 6u);
  EXPECT_EQ(to_iso(d[0]), "2026-12-16");
  EXPECT_EQ(to_iso(d[1]), "2027-03-17");
  EXPECT_EQ(to_iso(d[2]), "2027-06-16");
  EXPECT_EQ(to_iso(d[3]), "2027-09-15");
  EXPECT_EQ(to_iso(d[4]), "2027-12-15");
  EXPECT_EQ(to_iso(d[5]), "2028-03-15");
  EXPECT_EQ(futures_code(2026, 12), "Z26");
  EXPECT_EQ(futures_code(2027, 3), "H27");
  EXPECT_EQ(futures_code(2027, 6), "M27");
  EXPECT_EQ(futures_code(2027, 9), "U27");
}

TEST(Imm, Sofr3mFuturesReferenceQuarters) {
  // CME: the reference quarter runs from the 3rd Wednesday of the 3rd month preceding the
  // delivery month to, but not including, the 3rd Wednesday of the delivery month; the
  // contract is named by its delivery (end) month, here by its start IMM month as the code.
  const auto f = sofr_3m_futures(parse_iso("2026-09-23"), 4, reg().calendar("USD-SOFR"));
  ASSERT_EQ(f.size(), 4u);
  EXPECT_EQ(to_iso(f[0].start), "2026-12-16");
  EXPECT_EQ(to_iso(f[0].end), "2027-03-17");
  EXPECT_EQ(f[0].end - f[0].start, 91);
  EXPECT_EQ(to_iso(f[0].last_trading), "2027-03-16");
  EXPECT_EQ(f[0].code, "SR3 Z26");
  EXPECT_EQ(to_iso(f[3].start), "2027-09-15");
  EXPECT_EQ(to_iso(f[3].end), "2027-12-15");
  // a contract whose quarter has started is not listed as a front contract
  const auto g = sofr_3m_futures(parse_iso("2026-12-16"), 1, reg().calendar("USD-SOFR"));
  EXPECT_EQ(to_iso(g[0].start), "2027-03-17");
}

TEST(Imm, Sofr1mFuturesCalendarMonths) {
  const auto f = sofr_1m_futures(parse_iso("2026-09-23"), 3, reg().calendar("USD-SOFR"));
  ASSERT_EQ(f.size(), 3u);
  EXPECT_EQ(f[0].code, "SR1 V26");
  EXPECT_EQ(to_iso(f[0].start), "2026-10-01");
  EXPECT_EQ(to_iso(f[0].end), "2026-11-01");
  EXPECT_EQ(to_iso(f[0].last_trading), "2026-10-30");   // the last business day of October (10-31 is a Saturday)
  EXPECT_EQ(f[1].code, "SR1 X26");
  EXPECT_EQ(to_iso(f[1].last_trading), "2026-11-30");
  EXPECT_EQ(f[2].code, "SR1 Z26");
  EXPECT_EQ(to_iso(f[2].last_trading), "2026-12-31");
}

TEST(Imm, EuriborFuturesFEU3) {
  // Eurex: last trading day two exchange days before the third Wednesday of the delivery month;
  // the underlying 3M deposit runs from the third Wednesday (spot of the fixing) for 3 months.
  const Calendar& t = reg().calendar("EUR-TARGET");
  const auto f = euribor_3m_futures(parse_iso("2026-09-23"), 3, t);
  ASSERT_EQ(f.size(), 3u);
  EXPECT_EQ(f[0].code, "FEU3 Z26");
  EXPECT_EQ(to_iso(f[0].last_trading), "2026-12-14");   // Mon; third Wednesday 2026-12-16
  EXPECT_EQ(f[0].fixing, f[0].last_trading);
  EXPECT_EQ(to_iso(f[0].start), "2026-12-16");
  EXPECT_EQ(to_iso(f[0].end), "2027-03-16");
  EXPECT_EQ(f[1].code, "FEU3 H27");
  EXPECT_EQ(to_iso(f[1].last_trading), "2027-03-15");
  EXPECT_EQ(to_iso(f[1].start), "2027-03-17");
  EXPECT_EQ(to_iso(f[1].end), "2027-06-17");
  // a contract whose last trading day has passed is skipped
  const auto g = euribor_3m_futures(parse_iso("2026-12-14"), 1, t);
  EXPECT_EQ(g[0].code, "FEU3 H27");
}
