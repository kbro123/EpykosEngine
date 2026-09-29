// M3/G0: holiday lists for 2026-2027 from the published rules against the official lists
// cited in docs/G4_BUNDLE.md (SIFMA, NY Fed / ISDA, Federal Reserve, ECB / Bundesbank); joint
// calendars; business day conventions incl. modified following across a month end.
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "epykos/conventions/calendar.hpp"
#include "epykos/conventions/registry.hpp"

using namespace epykos::conventions;

namespace {

const Registry& reg() {
  static Registry r = Registry::load();
  return r;
}

std::vector<std::string> iso(const std::vector<Date>& ds) {
  std::vector<std::string> out;
  for (Date d : ds) out.push_back(to_iso(d));
  return out;
}

}  // namespace

TEST(Calendar, SifmaFullCloses2026) {
  // SIFMA Holiday Schedule, U.S. 2026 tab (read 2026-09-23). Good Friday 2026-04-03 is an early
  // close, i.e. a U.S. Government Securities Business Day (ISDA guidance 2026-03-27).
  const std::vector<std::string> expected = {
      "2026-01-01", "2026-01-19", "2026-02-16", "2026-05-25", "2026-06-19", "2026-07-03",
      "2026-09-07", "2026-10-12", "2026-11-11", "2026-11-26", "2026-12-25"};
  EXPECT_EQ(iso(reg().calendar("USD-SIFMA").holidays(2026)), expected);
  EXPECT_TRUE(reg().calendar("USD-SIFMA").is_business_day(parse_iso("2026-04-03")));
}

TEST(Calendar, SifmaFullCloses2027) {
  // SIFMA U.S. 2027 tab: Good Friday 2027-03-26 full close; Juneteenth observed Fri 2027-06-18;
  // Independence Day observed Mon 2027-07-05; Christmas observed Fri 2027-12-24; New Year's Day
  // 2028 (a Saturday) is not observed on Friday 2027-12-31 (early close only).
  const std::vector<std::string> expected = {
      "2027-01-01", "2027-01-18", "2027-02-15", "2027-03-26", "2027-05-31", "2027-06-18",
      "2027-07-05", "2027-09-06", "2027-10-11", "2027-11-11", "2027-11-25", "2027-12-24"};
  EXPECT_EQ(iso(reg().calendar("USD-SIFMA").holidays(2027)), expected);
  EXPECT_TRUE(reg().calendar("USD-SIFMA").is_business_day(parse_iso("2027-12-31")));
}

TEST(Calendar, SofrPublicationDays) {
  // NY Fed (via ISDA 2026-03-27): no SOFR on or for Good Friday 2026-04-03 although SIFMA is open.
  std::vector<std::string> expected2026 = {
      "2026-01-01", "2026-01-19", "2026-02-16", "2026-04-03", "2026-05-25", "2026-06-19", "2026-07-03",
      "2026-09-07", "2026-10-12", "2026-11-11", "2026-11-26", "2026-12-25"};
  EXPECT_EQ(iso(reg().calendar("USD-SOFR").holidays(2026)), expected2026);
  EXPECT_EQ(reg().calendar("USD-SOFR").holidays(2027), reg().calendar("USD-SIFMA").holidays(2027));
  EXPECT_FALSE(reg().calendar("USD-SOFR").is_business_day(parse_iso("2026-04-03")));
  EXPECT_TRUE(reg().calendar("USD-SOFR").is_business_day(parse_iso("2026-04-02")));
  EXPECT_TRUE(reg().calendar("USD-SOFR").is_business_day(parse_iso("2026-04-06")));
}

TEST(Calendar, FederalReserve2026And2027) {
  // frbservices.org holiday schedules: Saturday holidays -> open the preceding Friday;
  // Sunday holidays -> closed the following Monday. 2026-07-04 Sat (open Fri), 2027-06-19 Sat,
  // 2027-07-04 Sun (Mon 07-05), 2027-12-25 Sat (open Fri 12-24).
  const std::vector<std::string> e2026 = {
      "2026-01-01", "2026-01-19", "2026-02-16", "2026-05-25", "2026-06-19",
      "2026-09-07", "2026-10-12", "2026-11-11", "2026-11-26", "2026-12-25"};
  const std::vector<std::string> e2027 = {
      "2027-01-01", "2027-01-18", "2027-02-15", "2027-05-31", "2027-07-05",
      "2027-09-06", "2027-10-11", "2027-11-11", "2027-11-25"};
  EXPECT_EQ(iso(reg().calendar("USD-FED").holidays(2026)), e2026);
  EXPECT_EQ(iso(reg().calendar("USD-FED").holidays(2027)), e2027);
  EXPECT_TRUE(reg().calendar("USD-FED").is_business_day(parse_iso("2026-07-03")));   // SIFMA closed, Fed open
  EXPECT_TRUE(reg().calendar("USD-FED").is_business_day(parse_iso("2027-12-24")));   // SIFMA closed, Fed open
  EXPECT_TRUE(reg().calendar("USD-FED").is_business_day(parse_iso("2026-04-03")));   // Good Friday: Fed open
}

TEST(Calendar, Target2026And2027) {
  // Bundesbank "TARGET holidays" 2026: 1 Jan, 3 Apr, 6 Apr, 1 May, 25 Dec, 26 Dec (Sat: not a weekday close).
  // 2027 from the ECB rule: 1 Jan (Fri), Good Friday 26 Mar, Easter Monday 29 Mar; 1 May, 25 and 26 Dec on the weekend.
  const std::vector<std::string> e2026 = {"2026-01-01", "2026-04-03", "2026-04-06", "2026-05-01", "2026-12-25"};
  const std::vector<std::string> e2027 = {"2027-01-01", "2027-03-26", "2027-03-29"};
  EXPECT_EQ(iso(reg().calendar("EUR-TARGET").holidays(2026)), e2026);
  EXPECT_EQ(iso(reg().calendar("EUR-TARGET").holidays(2027)), e2027);
  // No observance shifts on TARGET: 2027-12-27 (Monday) is a business day.
  EXPECT_TRUE(reg().calendar("EUR-TARGET").is_business_day(parse_iso("2027-12-27")));
  EXPECT_TRUE(reg().calendar("EUR-TARGET").is_business_day(parse_iso("2027-05-03")));
}

TEST(Calendar, JointCalendars) {
  const Calendar& swap = reg().calendar("USD-SOFR-SWAP");
  EXPECT_TRUE(swap.is_joint());
  // SIFMA closes plus Fed closes: in 2026 and 2027 the union equals SIFMA's list (Fed adds nothing),
  // and a Fed-only close would still count.
  EXPECT_EQ(swap.holidays(2026), reg().calendar("USD-SIFMA").holidays(2026));
  EXPECT_EQ(swap.holidays(2027), reg().calendar("USD-SIFMA").holidays(2027));
  EXPECT_TRUE(swap.is_business_day(parse_iso("2026-04-03")));   // Good Friday 2026: both open
  const Calendar& eurusd = reg().calendar("EURUSD");
  EXPECT_FALSE(eurusd.is_business_day(parse_iso("2026-04-06")));  // Easter Monday: TARGET closed
  EXPECT_FALSE(eurusd.is_business_day(parse_iso("2026-07-03")));  // SIFMA closed
  EXPECT_FALSE(eurusd.is_business_day(parse_iso("2026-05-01")));  // TARGET closed
  EXPECT_TRUE(eurusd.is_business_day(parse_iso("2026-05-04")));
  // ISDA MPN 2022-04-08 example: SOFR swaps traded Wed 2022-04-13 have effective date Mon 2022-04-18
  // (Good Friday 2022-04-15 was a NY business day but not a USGS business day). Our calendars
  // start in 2020, so the same logic on 2026: trade Wed 2026-04-01 -> spot Mon 2026-04-06 on
  // the SOFR fixing calendar (no SOFR on Good Friday), but Fri 2026-04-03 on the swap calendar.
  EXPECT_EQ(reg().calendar("USD-SOFR").add_business_days(parse_iso("2026-04-01"), 2), parse_iso("2026-04-06"));
  EXPECT_EQ(swap.add_business_days(parse_iso("2026-04-01"), 2), parse_iso("2026-04-03"));
  EXPECT_EQ(reg().calendar("USD-SIFMA").add_business_days(parse_iso("2022-04-13"), 2), parse_iso("2022-04-18"));
}

TEST(Calendar, BusinessDayConventions) {
  const Calendar& t = reg().calendar("EUR-TARGET");
  const Date sun = parse_iso("2026-05-31");   // Sunday, month end
  EXPECT_EQ(t.adjust(sun, BusinessDayConvention::Unadjusted), sun);
  EXPECT_EQ(t.adjust(sun, BusinessDayConvention::Following), parse_iso("2026-06-01"));
  EXPECT_EQ(t.adjust(sun, BusinessDayConvention::ModifiedFollowing), parse_iso("2026-05-29"));   // stays in May
  EXPECT_EQ(t.adjust(sun, BusinessDayConvention::Preceding), parse_iso("2026-05-29"));
  EXPECT_EQ(t.adjust(sun, BusinessDayConvention::ModifiedPreceding), parse_iso("2026-05-29"));
  const Date sat = parse_iso("2026-08-01");   // Saturday, month start
  EXPECT_EQ(t.adjust(sat, BusinessDayConvention::Preceding), parse_iso("2026-07-31"));
  EXPECT_EQ(t.adjust(sat, BusinessDayConvention::ModifiedPreceding), parse_iso("2026-08-03"));  // stays in August
  EXPECT_EQ(t.adjust(sat, BusinessDayConvention::ModifiedFollowing), parse_iso("2026-08-03"));
  // A holiday at month end: 2026-12-31 is a Thursday and a TARGET business day; 2027-01-01 a holiday.
  EXPECT_EQ(t.adjust(parse_iso("2027-01-01"), BusinessDayConvention::ModifiedFollowing), parse_iso("2027-01-04"));
  EXPECT_EQ(t.adjust(parse_iso("2027-01-01"), BusinessDayConvention::ModifiedPreceding), parse_iso("2027-01-04"));
  // Easter weekend 2026: Fri 04-03 and Mon 04-06 closed.
  EXPECT_EQ(t.adjust(parse_iso("2026-04-03"), BusinessDayConvention::Following), parse_iso("2026-04-07"));
  EXPECT_EQ(t.adjust(parse_iso("2026-04-04"), BusinessDayConvention::Preceding), parse_iso("2026-04-02"));
  EXPECT_EQ(bdc_from_string("ModifiedFollowing"), BusinessDayConvention::ModifiedFollowing);
  EXPECT_THROW(bdc_from_string("MODFOLLOWING"), DateError);
}

TEST(Calendar, BusinessDayArithmetic) {
  const Calendar& c = reg().calendar("USD-SOFR");
  EXPECT_EQ(c.add_business_days(parse_iso("2026-09-23"), 0), parse_iso("2026-09-23"));
  EXPECT_EQ(c.add_business_days(parse_iso("2026-09-26"), 0), parse_iso("2026-09-26"));  // zero: unadjusted
  EXPECT_EQ(c.add_business_days(parse_iso("2026-09-23"), 2), parse_iso("2026-09-25"));
  EXPECT_EQ(c.add_business_days(parse_iso("2026-09-25"), 1), parse_iso("2026-09-28"));
  EXPECT_EQ(c.add_business_days(parse_iso("2026-09-28"), -1), parse_iso("2026-09-25"));
  EXPECT_EQ(c.add_business_days(parse_iso("2026-11-27"), -1), parse_iso("2026-11-25"));  // over Thanksgiving
  EXPECT_EQ(c.business_days_between(parse_iso("2026-11-23"), parse_iso("2026-11-30")), 4);
  EXPECT_EQ(c.business_days_between(parse_iso("2026-11-30"), parse_iso("2026-11-23")), -4);
  EXPECT_EQ(c.business_days(parse_iso("2026-11-23"), parse_iso("2026-11-30")).size(), 4u);
  EXPECT_EQ(c.next_business_day(parse_iso("2026-04-03")), parse_iso("2026-04-06"));
  EXPECT_EQ(c.previous_business_day(parse_iso("2026-04-03")), parse_iso("2026-04-02"));
  EXPECT_THROW(c.is_business_day(parse_iso("2019-12-31")), DateError);   // outside 2020-2080
  EXPECT_THROW(c.is_business_day(parse_iso("2081-01-01")), DateError);
  EXPECT_NO_THROW(c.is_business_day(parse_iso("2080-12-31")));
}

TEST(Calendar, RuleKindsInCode) {
  // The rule kinds are exercised directly (data of a tiny calendar written here).
  std::vector<HolidayRule> rules;
  HolidayRule jan1;
  jan1.kind = HolidayRule::Kind::Fixed;
  jan1.label = "New Year";
  jan1.month = 1;
  jan1.day = 1;
  rules.push_back(jan1);
  HolidayRule uk;   // next_weekday observance: Sat/Sun -> Monday
  uk.kind = HolidayRule::Kind::Fixed;
  uk.label = "Boxing Day";
  uk.month = 12;
  uk.day = 26;
  uk.has_observance = true;
  uk.observance = Observance::NextWeekday;
  rules.push_back(uk);
  HolidayRule gf;
  gf.kind = HolidayRule::Kind::EasterOffset;
  gf.label = "Good Friday";
  gf.offset_days = -2;
  rules.push_back(gf);
  HolidayRule fifth;   // a 5th Monday that does not exist in some months
  fifth.kind = HolidayRule::Kind::NthWeekday;
  fifth.label = "fifth Monday of February";
  fifth.month = 2;
  fifth.weekday = 0;
  fifth.n = 5;
  rules.push_back(fifth);
  HolidayRule window;
  window.kind = HolidayRule::Kind::Fixed;
  window.label = "windowed";
  window.month = 7;
  window.day = 15;
  window.from_year = 2026;
  window.to_year = 2026;
  rules.push_back(window);
  Calendar c("test", {5, 6}, rules, Observance::SatToFriSunToMon, {parse_iso("2026-03-04")}, {parse_iso("2026-01-01")},
             2025, 2028);
  // 2026-01-01 removed by not_holidays; 2026-03-04 added; Boxing Day 2026 (Sat) -> Mon 2026-12-28
  EXPECT_EQ(iso(c.holidays(2026)), (std::vector<std::string>{"2026-03-04", "2026-04-03", "2026-07-15", "2026-12-28"}));
  // 2027: Jan 1 Fri; Boxing Day Sun -> Mon 12-27; no windowed; 5th Monday of Feb 2027? Feb 2027 starts Monday -> Mondays 1,8,15,22 (no 5th);
  // New Year's Day 2028 is a Saturday -> observed Fri 2027-12-31 (in the previous year's list).
  EXPECT_EQ(iso(c.holidays(2027)), (std::vector<std::string>{"2027-01-01", "2027-03-26", "2027-12-27", "2027-12-31"}));
  EXPECT_TRUE(c.is_holiday(parse_iso("2027-12-31")));
  // Feb 2028 starts on a Tuesday: Mondays 7, 14, 21, 28 -> no 5th Monday either
  EXPECT_EQ(iso(c.holidays(2028)), (std::vector<std::string>{"2028-04-14", "2028-12-26"}));
  EXPECT_EQ(observance_from_string("sun_to_mon"), Observance::SunToMon);
  EXPECT_THROW(observance_from_string("saturday"), DateError);
}
