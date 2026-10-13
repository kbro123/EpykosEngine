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

// `eom` here is the FORCE flag (D129); the cases below anchor on a month end, where forcing and
// the ISDA `eom_auto` inference coincide. `EndOfMonthForcedVersusInferred` is what separates them.
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

// D129: `eom` and `eom_auto` are two different things and this is where that is pinned. On an
// anchor that is NOT a month end they disagree — FORCE snaps every rolled boundary to the month
// end, the ISDA inference does nothing at all — and `Schedule` reports which one it ran on, so a
// consumer records the term instead of re-deriving it from a date.
TEST(Schedule, EndOfMonthForcedVersusInferred) {
  // Anchor 2029-02-27: a Tuesday, not its month's end.
  const char* kEff = "2026-11-30";
  const char* kAnchor = "2029-02-27";

  // (a) `eom_auto` alone: the anchor is not a month end, so nothing fires and the day of month is
  //     kept. This is what every convention in blueprints/ asks for.
  ScheduleSpec a = spec(kEff, kAnchor, "1Y", StubKind::ShortFront, false);
  a.eom_auto = true;
  const Schedule sa = generate_schedule(a);
  EXPECT_EQ(iso(sa.unadjusted), (std::vector<std::string>{"2026-11-30", "2027-02-27", "2028-02-27", "2029-02-27"}));
  EXPECT_FALSE(sa.roll_eom) << "an anchor that is not a month end must not resolve to an EOM roll";
  EXPECT_EQ(sa.roll_dom, 27);

  // (b) `eom` forces it: every rolled boundary becomes its month's end, 2028 included (a leap
  //     year, so 02-29), and the schedule says so.
  ScheduleSpec f = spec(kEff, kAnchor, "1Y", StubKind::ShortFront, true);
  const Schedule sf = generate_schedule(f);
  EXPECT_EQ(iso(sf.unadjusted), (std::vector<std::string>{"2026-11-30", "2027-02-28", "2028-02-29", "2029-02-27"}));
  EXPECT_TRUE(sf.roll_eom);
  EXPECT_EQ(sf.roll_dom, 27) << "roll_dom still reports the anchor's day; roll_eom is what overrides it";

  // (c) neither: identical dates to (a), which is why passing the AUTO flag where FORCE is meant
  //     is invisible on most books and visible on exactly the EOM trades.
  const Schedule sn = generate_schedule(spec(kEff, kAnchor, "1Y", StubKind::ShortFront, false));
  EXPECT_EQ(iso(sn.unadjusted), iso(sa.unadjusted));
  EXPECT_FALSE(sn.roll_eom);

  // (d) on a month-end anchor the two coincide, and both report the EOM term. This is the subset
  //     relation the oracle harness relies on (stage_a_book_oracle_e0_test.cpp §ROLL).
  ScheduleSpec m = spec(kEff, "2029-02-28", "1Y", StubKind::ShortFront, false);
  m.eom_auto = true;
  const Schedule sm = generate_schedule(m);
  const Schedule smf = generate_schedule(spec(kEff, "2029-02-28", "1Y", StubKind::ShortFront, true));
  EXPECT_EQ(iso(sm.unadjusted), iso(smf.unadjusted));
  EXPECT_TRUE(sm.roll_eom);
  EXPECT_TRUE(smf.roll_eom);
  EXPECT_EQ(sm.roll_dom, 28);

  // (e) a calendar-day frequency cannot roll on month ends, and the guard is on the ROLLING
  //     branch: 2W from a month-end anchor keeps its 14-day grid and reports no EOM term.
  ScheduleSpec w = spec("2026-09-30", "2026-10-28", "2W", StubKind::ShortFront, false);
  w.eom_auto = true;
  const Schedule sw = generate_schedule(w);
  EXPECT_EQ(iso(sw.unadjusted), (std::vector<std::string>{"2026-09-30", "2026-10-14", "2026-10-28"}));
  EXPECT_FALSE(sw.roll_eom);

  // (f) a back stub anchors on the EFFECTIVE date, so that is the date the inference reads.
  ScheduleSpec b = spec("2026-01-31", "2027-04-30", "6M", StubKind::ShortBack, false);
  b.eom_auto = true;
  const Schedule sb = generate_schedule(b);
  EXPECT_EQ(iso(sb.unadjusted), (std::vector<std::string>{"2026-01-31", "2026-07-31", "2027-01-31", "2027-04-30"}));
  EXPECT_TRUE(sb.roll_eom);
  EXPECT_EQ(sb.roll_dom, 31);

  // (g) one period rolls nothing, but the TERM is still reported — it is a property of the trade,
  //     not of how many periods it turned out to have.
  ScheduleSpec one = spec("2026-09-30", "2027-03-31", "1Y", StubKind::ShortFront, false);
  one.term = true;
  one.eom_auto = true;
  const Schedule so = generate_schedule(one);
  EXPECT_EQ(so.periods(), 1u);
  EXPECT_TRUE(so.roll_eom);
  EXPECT_EQ(so.roll_dom, 31);
}
