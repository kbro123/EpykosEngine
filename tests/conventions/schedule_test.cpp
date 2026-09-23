// M3/G0: schedule generation with front/back short/long stubs, the end-of-month rule,
// payment lags and fixing-in-advance dates.
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "epykos/conventions/registry.hpp"
#include "epykos/conventions/schedule.hpp"

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

ScheduleSpec spec(const char* eff, const char* term, const char* freq, StubKind stub, bool eom) {
  ScheduleSpec s;
  s.effective = parse_iso(eff);
  s.termination = parse_iso(term);
  s.frequency = Period::parse(freq);
  s.bdc = BusinessDayConvention::ModifiedFollowing;
  s.calendar = &reg().calendar("EUR-TARGET");
  s.stub = stub;
  s.eom = eom;
  return s;
}

}  // namespace

TEST(Schedule, ShortFrontStubWithEndOfMonth) {
  // Annual, anchored on the termination date 2029-02-28 (a month end): with EOM every regular
  // date is a month end (2028-02-29), the short front stub runs 2026-11-30 -> 2027-02-28.
  const Schedule s = generate_schedule(spec("2026-11-30", "2029-02-28", "1Y", StubKind::ShortFront, true));
  EXPECT_EQ(iso(s.unadjusted), (std::vector<std::string>{"2026-11-30", "2027-02-28", "2028-02-29", "2029-02-28"}));
  // 2027-02-28 is a Sunday: modified following keeps February -> Fri 2027-02-26.
  EXPECT_EQ(iso(s.adjusted), (std::vector<std::string>{"2026-11-30", "2027-02-26", "2028-02-29", "2029-02-28"}));
  EXPECT_TRUE(s.front_stub);
  EXPECT_FALSE(s.back_stub);
  EXPECT_EQ(s.periods(), 3u);
  // Without EOM the day of month 28 is kept.
  const Schedule n = generate_schedule(spec("2026-11-30", "2029-02-28", "1Y", StubKind::ShortFront, false));
  EXPECT_EQ(iso(n.unadjusted), (std::vector<std::string>{"2026-11-30", "2027-02-28", "2028-02-28", "2029-02-28"}));
}

TEST(Schedule, LongFrontStub) {
  const Schedule s = generate_schedule(spec("2026-11-30", "2029-02-28", "1Y", StubKind::LongFront, true));
  EXPECT_EQ(iso(s.unadjusted), (std::vector<std::string>{"2026-11-30", "2028-02-29", "2029-02-28"}));
  EXPECT_TRUE(s.front_stub);
}

TEST(Schedule, BackStubs) {
  const Schedule sb = generate_schedule(spec("2026-01-15", "2027-04-15", "6M", StubKind::ShortBack, false));
  EXPECT_EQ(iso(sb.unadjusted), (std::vector<std::string>{"2026-01-15", "2026-07-15", "2027-01-15", "2027-04-15"}));
  EXPECT_TRUE(sb.back_stub);
  EXPECT_FALSE(sb.front_stub);
  const Schedule lb = generate_schedule(spec("2026-01-15", "2027-04-15", "6M", StubKind::LongBack, false));
  EXPECT_EQ(iso(lb.unadjusted), (std::vector<std::string>{"2026-01-15", "2026-07-15", "2027-04-15"}));
  EXPECT_TRUE(lb.back_stub);
}

TEST(Schedule, RegularAndTermAndWeekly) {
  const Schedule r = generate_schedule(spec("2026-09-25", "2031-09-25", "1Y", StubKind::ShortFront, false));
  EXPECT_EQ(r.periods(), 5u);
  EXPECT_FALSE(r.front_stub);
  EXPECT_FALSE(r.back_stub);
  EXPECT_EQ(to_iso(r.unadjusted[1]), "2027-09-25");   // Saturday
  EXPECT_EQ(to_iso(r.adjusted[1]), "2027-09-27");     // modified following -> Monday
  ScheduleSpec t = spec("2026-09-25", "2027-03-25", "1Y", StubKind::ShortFront, false);
  t.term = true;
  const Schedule ts = generate_schedule(t);
  EXPECT_EQ(ts.periods(), 1u);
  const Schedule w = generate_schedule(spec("2026-09-25", "2026-10-23", "2W", StubKind::ShortFront, false));
  EXPECT_EQ(iso(w.unadjusted), (std::vector<std::string>{"2026-09-25", "2026-10-09", "2026-10-23"}));
  EXPECT_THROW(generate_schedule(spec("2026-09-25", "2026-09-25", "1Y", StubKind::ShortFront, false)), DateError);
}

TEST(Schedule, PaymentAndFixingDates) {
  // €STR OIS style: annual periods, 2-day payment lag on TARGET; EURIBOR fixing 2 TARGET days in advance.
  const Schedule s = generate_schedule(spec("2026-04-01", "2028-04-01", "1Y", StubKind::ShortFront, false));
  EXPECT_EQ(iso(s.adjusted), (std::vector<std::string>{"2026-04-01", "2027-04-01", "2028-04-03"}));  // 2028-04-01 Sat
  const Calendar& t = reg().calendar("EUR-TARGET");
  EXPECT_EQ(iso(payment_dates(s, 2, t)), (std::vector<std::string>{"2027-04-05", "2028-04-05"}));   // +2 TARGET days over the weekends
  EXPECT_EQ(iso(payment_dates(s, 0, t)), (std::vector<std::string>{"2027-04-01", "2028-04-03"}));
  EXPECT_EQ(iso(fixing_dates_in_advance(s, 2, t)), (std::vector<std::string>{"2026-03-30", "2027-03-30"}));
  EXPECT_EQ(spot_date(parse_iso("2026-04-01"), 2, t), parse_iso("2026-04-07"));   // over Easter 2026
  EXPECT_EQ(spot_date(parse_iso("2026-04-01"), 0, t), parse_iso("2026-04-01"));
  EXPECT_EQ(stub_from_string("LongBack"), StubKind::LongBack);
  EXPECT_THROW(stub_from_string("Short"), DateError);
}
