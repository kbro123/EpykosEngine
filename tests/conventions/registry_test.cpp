// M3/G0: the conventions registry loads and validates blueprints/conventions/*.json (D36, D37):
// unknown keys and missing fields are errors with a location; every value docs/G4_BUNDLE.md
// states is in the registry; a synthetic extra convention file (a new currency, calendar,
// index and instrument) loads with no C++ change.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "epykos/conventions/registry.hpp"
#include "epykos/conventions/rfr.hpp"
#include "epykos/conventions/schedule.hpp"

using namespace epykos::conventions;
using epykos::json::parse;

namespace {

const Registry& reg() {
  static Registry r = Registry::load();
  return r;
}

std::string error_of(const char* doc) {
  Registry r;
  try {
    r.add_document(parse(doc, "t.json"), "t.json");
    return "";
  } catch (const RegistryError& e) {
    return e.what();
  } catch (const epykos::json::JsonError& e) {
    return e.what();
  }
}

// Errors caught at finalize (cross references): load the document as a file.
std::string finalize_error_of(const char* doc) {
  const std::string path = (std::filesystem::temp_directory_path() / "epykos_g0_finalize.json").string();
  std::ofstream(path) << doc;
  std::string what;
  try {
    Registry q = Registry::load_files({path});
  } catch (const RegistryError& e) {
    what = e.what();
  }
  std::remove(path.c_str());
  return what;
}

std::vector<std::string> tenors_of(const InstrumentConvention& p) {
  std::vector<std::string> out;
  for (const Period& t : p.tenors) out.push_back(t.to_string());
  return out;
}

}  // namespace

TEST(Registry, LoadsTheBlueprints) {
  const Registry& r = reg();
  EXPECT_GE(r.files().size(), 4u);
  EXPECT_EQ(r.currency_codes(), (std::vector<std::string>{"EUR", "USD"}));
  EXPECT_EQ(r.calendar_names(), (std::vector<std::string>{"EUR-TARGET", "EURUSD", "NONE", "USD-FED", "USD-SIFMA", "USD-SOFR", "USD-SOFR-SWAP"}));
  EXPECT_EQ(r.day_count_names(), (std::vector<std::string>{"30/360", "30E/360", "ACT/360", "ACT/365F", "ACT/ACT ISDA"}));
  EXPECT_EQ(r.index_names(), (std::vector<std::string>{"EUR-ESTR", "EUR-EURIBOR-3M", "EUR-EURIBOR-6M", "USD-SOFR"}));
  EXPECT_EQ(r.cb_schedule_currencies(), (std::vector<std::string>{"EUR", "USD"}));
  for (const char* name : {"USD-SOFR-OIS", "USD-SOFR-OIS-SHIFT2", "USD-SOFR-OIS-SHIFT2-LOCKOUT2", "USD-SOFR-AVG-SWAP",
                           "USD-SOFR-ON-DEPOSIT", "USD-SOFR-3M-FUTURE", "USD-SOFR-1M-FUTURE", "EUR-ESTR-OIS",
                           "EUR-ESTR-ON-DEPOSIT", "EUR-EURIBOR-3M-DEPOSIT", "EUR-EURIBOR-6M-DEPOSIT", "EUR-EURIBOR-3M-IRS",
                           "EUR-EURIBOR-6M-IRS", "EUR-3S6S-BASIS", "EUR-EURIBOR-3M-FUTURE"}) {
    EXPECT_TRUE(r.has_instrument(name)) << name;
  }
  EXPECT_THROW(r.instrument("GBP-SONIA-OIS"), RegistryError);
  EXPECT_THROW(r.calendar("GBP"), RegistryError);
  // provenance is recorded on imported entries (D37)
  EXPECT_NE(r.instrument("USD-SOFR-OIS").provenance.imported_from.find("SwapEngine"), std::string::npos);
  EXPECT_EQ(r.instrument("USD-SOFR-OIS").provenance.imported_on, "2026-09-23");
  EXPECT_EQ(r.instrument("USD-SOFR-OIS").provenance.cross_check, "agree");
  EXPECT_EQ(r.instrument("EUR-EURIBOR-3M-IRS").provenance.cross_check, "disagree");
  EXPECT_FALSE(r.instrument("USD-SOFR-OIS").sources.empty());
  for (const auto& f : r.files()) EXPECT_FALSE(f.title.empty()) << f.path;
}

TEST(Registry, UsdConventionsAsInG4Bundle) {
  const Registry& r = reg();
  // USD.2: the index
  const IndexDef& sofr = r.index("USD-SOFR");
  EXPECT_EQ(sofr.kind, IndexDef::Kind::Overnight);
  EXPECT_EQ(sofr.day_count, DayCount::Act360);
  EXPECT_EQ(sofr.fixing_calendar, "USD-SOFR");
  EXPECT_EQ(sofr.publication_lag, 1);
  EXPECT_EQ(sofr.currency, "USD");
  // USD.3: SOFR OIS
  const InstrumentConvention& ois = r.instrument("USD-SOFR-OIS");
  EXPECT_EQ(ois.type, InstrumentConvention::Type::OIS);
  EXPECT_EQ(ois.calendar, "USD-SOFR-SWAP");
  EXPECT_EQ(ois.spot_lag, 2);
  EXPECT_EQ(ois.payment_lag, 2);
  EXPECT_EQ(ois.bdc, BusinessDayConvention::ModifiedFollowing);
  EXPECT_EQ(ois.stub, StubKind::ShortFront);
  EXPECT_TRUE(ois.eom);
  ASSERT_EQ(ois.legs.size(), 2u);
  EXPECT_EQ(ois.legs[0].role, LegConvention::Role::Fixed);
  EXPECT_EQ(ois.legs[0].frequency, Period::parse("1Y"));
  EXPECT_EQ(ois.legs[0].term_up_to, Period::parse("1Y"));
  EXPECT_EQ(ois.legs[0].day_count, DayCount::Act360);
  EXPECT_EQ(ois.legs[1].role, LegConvention::Role::Float);
  EXPECT_EQ(ois.legs[1].index, "USD-SOFR");
  EXPECT_EQ(ois.legs[1].compounding, LegConvention::Compounding::Compounded);
  EXPECT_EQ(ois.legs[1].frequency, Period::parse("1Y"));
  EXPECT_EQ(ois.legs[1].day_count, DayCount::Act360);
  EXPECT_EQ(ois.legs[1].observation.method, ObservationMethod::Plain);
  EXPECT_EQ(ois.legs[1].observation.lookback_days, 0);
  EXPECT_EQ(ois.legs[1].observation.lockout_days, 0);
  EXPECT_EQ(ois.discount_index, "USD-SOFR");
  const auto tenors = tenors_of(ois);
  for (const char* t : {"2Y", "3Y", "4Y", "5Y", "6Y", "7Y", "10Y", "12Y", "15Y", "20Y", "30Y"}) {   // CFTC MAT
    EXPECT_NE(std::find(tenors.begin(), tenors.end(), t), tenors.end()) << t;
  }
  EXPECT_EQ(&r.leg_calendar(ois, ois.legs[1]), &r.calendar("USD-SOFR"));
  EXPECT_EQ(&r.leg_calendar(ois, ois.legs[0]), &r.calendar("USD-SOFR-SWAP"));
  // the shift / lockout variants and the averaging swap
  const InstrumentConvention& sh = r.instrument("USD-SOFR-OIS-SHIFT2");
  EXPECT_EQ(sh.legs[1].observation.method, ObservationMethod::ObservationShift);
  EXPECT_EQ(sh.legs[1].observation.lookback_days, 2);
  EXPECT_EQ(sh.legs[1].observation.lockout_days, 0);
  const InstrumentConvention& lo = r.instrument("USD-SOFR-OIS-SHIFT2-LOCKOUT2");
  EXPECT_EQ(lo.legs[1].observation.method, ObservationMethod::ObservationShift);
  EXPECT_EQ(lo.legs[1].observation.lookback_days, 2);
  EXPECT_EQ(lo.legs[1].observation.lockout_days, 2);
  EXPECT_EQ(r.instrument("USD-SOFR-AVG-SWAP").legs[1].compounding, LegConvention::Compounding::Averaged);
  // USD.4: futures
  const InstrumentConvention& sr3 = r.instrument("USD-SOFR-3M-FUTURE");
  EXPECT_EQ(sr3.type, InstrumentConvention::Type::Future);
  EXPECT_EQ(sr3.index, "USD-SOFR");
  EXPECT_EQ(sr3.accrual, "compounded");
  EXPECT_EQ(sr3.period, "imm_quarter");
  EXPECT_EQ(sr3.day_count, DayCount::Act360);
  EXPECT_EQ(sr3.quote, "100 - rate");
  const InstrumentConvention& sr1 = r.instrument("USD-SOFR-1M-FUTURE");
  EXPECT_EQ(sr1.accrual, "averaged");
  EXPECT_EQ(sr1.period, "calendar_month");
  // USD.5: the front-end deposit
  const InstrumentConvention& dep = r.instrument("USD-SOFR-ON-DEPOSIT");
  EXPECT_EQ(dep.type, InstrumentConvention::Type::Deposit);
  EXPECT_EQ(dep.spot_lag, 0);
  EXPECT_EQ(tenors_of(dep), (std::vector<std::string>{"1D"}));
  // currency
  EXPECT_EQ(r.currency("USD").discount_index, "USD-SOFR");
  EXPECT_EQ(r.currency("USD").default_instrument, "USD-SOFR-OIS");
  EXPECT_EQ(r.cb_schedule("USD").meetings.size(), 16u);
  EXPECT_EQ(to_iso(r.cb_schedule("USD").meetings[1]), "2026-03-18");
}

TEST(Registry, EurConventionsAsInG4Bundle) {
  const Registry& r = reg();
  // EUR.2: indices
  const IndexDef& estr = r.index("EUR-ESTR");
  EXPECT_EQ(estr.kind, IndexDef::Kind::Overnight);
  EXPECT_EQ(estr.day_count, DayCount::Act360);
  EXPECT_EQ(estr.fixing_calendar, "EUR-TARGET");
  EXPECT_EQ(estr.publication_lag, 1);
  const IndexDef& e3 = r.index("EUR-EURIBOR-3M");
  EXPECT_EQ(e3.kind, IndexDef::Kind::Term);
  EXPECT_EQ(e3.tenor, Period::parse("3M"));
  EXPECT_EQ(e3.fixing_lag, 2);
  EXPECT_EQ(e3.day_count, DayCount::Act360);
  EXPECT_EQ(e3.fixing_calendar, "EUR-TARGET");
  EXPECT_EQ(r.index("EUR-EURIBOR-6M").tenor, Period::parse("6M"));
  EXPECT_EQ(r.index("EUR-EURIBOR-6M").fixing_lag, 2);
  // EUR.3: €STR OIS
  const InstrumentConvention& ois = r.instrument("EUR-ESTR-OIS");
  EXPECT_EQ(ois.calendar, "EUR-TARGET");
  EXPECT_EQ(ois.spot_lag, 2);
  EXPECT_EQ(ois.payment_lag, 2);   // recorded disagreement (TP ICAP / LCH: 1) in G4_BUNDLE.md EUR.3
  EXPECT_EQ(ois.bdc, BusinessDayConvention::ModifiedFollowing);
  EXPECT_EQ(ois.legs[0].day_count, DayCount::Act360);
  EXPECT_EQ(ois.legs[0].frequency, Period::parse("1Y"));
  EXPECT_EQ(ois.legs[1].index, "EUR-ESTR");
  EXPECT_EQ(ois.legs[1].compounding, LegConvention::Compounding::Compounded);
  EXPECT_EQ(ois.legs[1].frequency, Period::parse("1Y"));
  EXPECT_EQ(ois.legs[1].observation.method, ObservationMethod::Plain);
  EXPECT_EQ(ois.provenance.cross_check, "partial");
  // EUR.4: EURIBOR swaps
  const InstrumentConvention& s3 = r.instrument("EUR-EURIBOR-3M-IRS");
  EXPECT_EQ(s3.type, InstrumentConvention::Type::IRS);
  EXPECT_EQ(s3.spot_lag, 2);
  EXPECT_EQ(s3.payment_lag, 0);
  EXPECT_EQ(s3.legs[0].role, LegConvention::Role::Fixed);
  EXPECT_EQ(s3.legs[0].day_count, DayCount::Thirty360US);   // 30/360 Bond Basis (CFTC MAT, Bloomberg SEF, Strata); SwapEngine 30E/360 recorded
  EXPECT_EQ(s3.legs[0].frequency, Period::parse("1Y"));
  EXPECT_EQ(s3.legs[1].index, "EUR-EURIBOR-3M");
  EXPECT_EQ(s3.legs[1].frequency, Period::parse("3M"));
  EXPECT_EQ(s3.legs[1].day_count, DayCount::Act360);
  EXPECT_EQ(s3.legs[1].compounding, LegConvention::Compounding::None);
  EXPECT_EQ(s3.discount_index, "EUR-ESTR");
  const InstrumentConvention& s6 = r.instrument("EUR-EURIBOR-6M-IRS");
  EXPECT_EQ(s6.legs[0].day_count, DayCount::Thirty360US);
  EXPECT_EQ(s6.legs[1].index, "EUR-EURIBOR-6M");
  EXPECT_EQ(s6.legs[1].frequency, Period::parse("6M"));
  const auto tenors = tenors_of(s6);
  for (const char* t : {"2Y", "3Y", "4Y", "5Y", "6Y", "7Y", "10Y", "15Y", "20Y", "30Y"}) {   // CFTC MAT
    EXPECT_NE(std::find(tenors.begin(), tenors.end(), t), tenors.end()) << t;
  }
  // EUR.5: 3s6s basis: the spread on the 3M leg
  const InstrumentConvention& b = r.instrument("EUR-3S6S-BASIS");
  EXPECT_EQ(b.type, InstrumentConvention::Type::Basis);
  EXPECT_TRUE(b.legs[0].spread);
  EXPECT_EQ(b.legs[0].index, "EUR-EURIBOR-3M");
  EXPECT_EQ(b.legs[0].frequency, Period::parse("3M"));
  EXPECT_FALSE(b.legs[1].spread);
  EXPECT_EQ(b.legs[1].index, "EUR-EURIBOR-6M");
  EXPECT_EQ(b.legs[1].frequency, Period::parse("6M"));
  EXPECT_EQ(b.discount_index, "EUR-ESTR");
  // EUR.6: Euribor futures and deposits
  const InstrumentConvention& fut = r.instrument("EUR-EURIBOR-3M-FUTURE");
  EXPECT_EQ(fut.index, "EUR-EURIBOR-3M");
  EXPECT_EQ(fut.accrual, "fixing");
  EXPECT_EQ(fut.period, "imm_quarter_deposit");
  EXPECT_EQ(fut.spot_lag, 2);
  EXPECT_EQ(r.instrument("EUR-EURIBOR-3M-DEPOSIT").spot_lag, 2);
  EXPECT_EQ(tenors_of(r.instrument("EUR-EURIBOR-6M-DEPOSIT")), (std::vector<std::string>{"6M"}));
  EXPECT_EQ(r.currency("EUR").settlement_calendar, "EUR-TARGET");
  EXPECT_EQ(to_iso(r.cb_schedule("EUR").meetings[15]), "2027-12-16");
}

TEST(Registry, RejectsUnknownKeysAndMissingFields) {
  // unknown top-level key
  EXPECT_NE(error_of(R"({"meta": {"title": "t"}, "foo": {}})").find("unknown key \"foo\""), std::string::npos);
  // unknown key inside an instrument, with its location
  const std::string e1 = error_of(R"({"instruments": {"X": {"type": "ois", "currency": "USD", "calendar": "C", "spot_lag": 2, "bdc": "Following", "legs": [], "colour": 1}}})");
  EXPECT_NE(e1.find("unknown key \"colour\""), std::string::npos) << e1;
  EXPECT_NE(e1.find("t.json:1:"), std::string::npos) << e1;
  // missing required field
  const std::string e2 = error_of(R"({"instruments": {"X": {"type": "ois", "currency": "USD", "calendar": "C", "spot_lag": 2}}})");
  EXPECT_NE(e2.find("missing required key \"bdc\""), std::string::npos) << e2;
  // a rule with a field of another kind
  const std::string e3 = error_of(R"({"calendars": {"C": {"weekend": [5, 6], "holidays": [{"label": "x", "rule": "fixed", "month": 1, "day": 1, "days": -2}]}}})");
  EXPECT_NE(e3.find("does not belong to a fixed rule"), std::string::npos) << e3;
  // an unknown rule kind, observance, day count, bdc
  EXPECT_NE(error_of(R"({"calendars": {"C": {"weekend": [5, 6], "holidays": [{"label": "x", "rule": "lunar"}]}}})").find("unknown rule kind"), std::string::npos);
  EXPECT_NE(error_of(R"({"calendars": {"C": {"weekend": [5, 6], "observance": "maybe", "holidays": []}}})").find("unknown observance"), std::string::npos);
  EXPECT_NE(error_of(R"({"day_counts": {"BUS/252": {"description": "x"}}})").find("not implemented"), std::string::npos);
  EXPECT_NE(error_of(R"({"indices": {"I": {"currency": "USD", "type": "overnight", "day_count": "ACT/366", "fixing_calendar": "C"}}})").find("unknown day count"), std::string::npos);
  EXPECT_NE(error_of(R"({"instruments": {"X": {"type": "ois", "currency": "USD", "calendar": "C", "spot_lag": 2, "bdc": "Nearest", "legs": []}}})").find("unknown business day convention"), std::string::npos);
  // wrong types
  EXPECT_NE(error_of(R"({"instruments": {"X": {"type": "ois", "currency": "USD", "calendar": "C", "spot_lag": "2", "bdc": "Following", "legs": []}}})").find("expected integer"), std::string::npos);
  // a term index without a tenor; an overnight index with one
  EXPECT_NE(error_of(R"({"indices": {"I": {"currency": "USD", "type": "term", "day_count": "ACT/360", "fixing_calendar": "C"}}})").find("needs a \"tenor\""), std::string::npos);
  EXPECT_NE(error_of(R"({"indices": {"I": {"currency": "USD", "type": "overnight", "tenor": "1D", "day_count": "ACT/360", "fixing_calendar": "C"}}})").find("has no tenor"), std::string::npos);
  // a swap with one leg; a basis swap without a spread leg
  EXPECT_NE(error_of(R"({"instruments": {"X": {"type": "irs", "currency": "EUR", "calendar": "C", "spot_lag": 2, "bdc": "Following", "legs": [{"role": "fixed", "frequency": "1Y", "day_count": "30/360"}]}}})").find("exactly two legs"), std::string::npos);
  // duplicate definitions
  const std::string e4 = error_of(R"({"currencies": {"USD": {"name": "a", "minor_units": 2, "settlement_calendar": "C", "discount_index": "I", "default_instrument": "P"}}, "calendars": {"C": {"weekend": [5,6], "holidays": []}, "C": {"weekend": [5,6], "holidays": []}}})");
  EXPECT_NE(e4.find("duplicate key"), std::string::npos) << e4;
  // provenance cross_check vocabulary
  EXPECT_NE(error_of(R"({"currencies": {"USD": {"name": "a", "minor_units": 2, "settlement_calendar": "C", "discount_index": "I", "default_instrument": "P", "provenance": {"imported_from": "x", "imported_on": "2026-09-23", "cross_check": "sure"}}}})").find("cross_check"), std::string::npos);
  // dangling references are caught at finalize
  const std::string e5 = finalize_error_of(R"({"currencies": {"USD": {"name": "a", "minor_units": 2, "settlement_calendar": "NOPE", "discount_index": "I", "default_instrument": "P"}}})");
  EXPECT_NE(e5.find("unknown settlement_calendar \"NOPE\""), std::string::npos) << e5;
  const std::string e6 = finalize_error_of(R"({"calendars": {"J": {"join": ["A", "B"]}}})");
  EXPECT_NE(e6.find("unknown calendar"), std::string::npos) << e6;
  const std::string e7 = finalize_error_of(R"({"calendars": {"A": {"join": ["B", "A"]}, "B": {"join": ["A", "B"]}}})");
  EXPECT_FALSE(e7.empty()) << "a cyclic join must fail";
}

TEST(Registry, AnExtraConventionFileNeedsNoCode) {
  // A SYNTHETIC currency with its own calendar, overnight index and OIS convention, written to a
  // temporary file and merged into the registry: schedules, observation windows and payment
  // dates come out of the same code.
  const char* doc = R"({
    "meta": {"title": "synthetic XYZ conventions - a test fixture"},
    "currencies": {"XYZ": {"name": "Synthetic", "minor_units": 2, "settlement_calendar": "XYZ-CAL",
                           "discount_index": "XYZ-ON", "default_instrument": "XYZ-ON-OIS"}},
    "calendars": {"XYZ-CAL": {"weekend": [4, 5], "observance": "next_weekday", "first_year": 2025, "last_year": 2030,
                              "holidays": [{"label": "Founding Day", "rule": "fixed", "month": 3, "day": 6},
                                           {"label": "Second Tuesday of May", "rule": "nth_weekday", "month": 5, "weekday": 1, "n": 2},
                                           {"label": "Easter Tuesday", "rule": "easter_offset", "days": 2}]},
                  "XYZ-USD": {"join": ["XYZ-CAL", "USD-SIFMA"]}},
    "indices": {"XYZ-ON": {"currency": "XYZ", "type": "overnight", "day_count": "ACT/365F", "fixing_calendar": "XYZ-CAL"}},
    "instruments": {"XYZ-ON-OIS": {"type": "ois", "currency": "XYZ", "calendar": "XYZ-CAL", "spot_lag": 1, "payment_lag": 1,
                                   "bdc": "ModifiedFollowing", "eom": true, "stub": "ShortBack",
                                   "legs": [{"role": "fixed", "frequency": "6M", "day_count": "ACT/365F"},
                                            {"role": "float", "index": "XYZ-ON", "compounding": "compounded", "frequency": "6M", "day_count": "ACT/365F",
                                             "observation": {"method": "lookback", "lookback_days": 5, "lockout_days": 1}}],
                                   "tenors": ["6M", "1Y", "2Y"]}}
  })";
  const std::string path = (std::filesystem::temp_directory_path() / "epykos_g0_xyz.json").string();
  std::ofstream(path) << doc;
  Registry r = Registry::load();
  ASSERT_NO_THROW(r.add_file(path));
  std::remove(path.c_str());
  EXPECT_TRUE(r.has_currency("XYZ"));
  const Calendar& c = r.calendar("XYZ-CAL");
  // weekend Friday/Saturday: Sundays are business days; 2026-03-06 (Founding Day) is a Friday,
  // i.e. on the weekend, and the observance rules speak of Saturday / Sunday, so it is simply
  // not a weekday close; in 2027 it is a Saturday -> next_weekday -> Monday 2027-03-08.
  EXPECT_TRUE(c.is_business_day(parse_iso("2026-03-01")));   // a Sunday
  EXPECT_FALSE(c.is_business_day(parse_iso("2026-03-06")));  // a Friday: weekend
  EXPECT_TRUE(c.is_business_day(parse_iso("2026-03-08")));   // a Sunday, not shifted onto
  EXPECT_FALSE(c.is_business_day(parse_iso("2027-03-08")));  // Founding Day 2027 observed on the Monday
  EXPECT_FALSE(c.is_business_day(parse_iso("2026-05-12")));  // second Tuesday of May 2026
  EXPECT_FALSE(c.is_business_day(parse_iso("2026-04-07")));  // Easter Tuesday 2026
  EXPECT_TRUE(r.calendar("XYZ-USD").is_joint());
  EXPECT_FALSE(r.calendar("XYZ-USD").is_business_day(parse_iso("2026-01-19")));  // MLK from the USD side
  const InstrumentConvention& p = r.instrument("XYZ-ON-OIS");
  EXPECT_EQ(p.legs[1].observation.method, ObservationMethod::Lookback);
  EXPECT_EQ(p.legs[1].observation.lookback_days, 5);
  EXPECT_EQ(p.legs[1].observation.lockout_days, 1);
  EXPECT_EQ(p.legs[1].day_count, DayCount::Act365F);
  // and the mechanics run on it
  ScheduleSpec s;
  s.effective = spot_date(parse_iso("2026-06-01"), p.spot_lag, c);
  s.termination = add_period(s.effective, Period::parse("1Y"));
  s.frequency = p.legs[1].frequency;
  s.bdc = p.bdc;
  s.calendar = &c;
  s.stub = p.stub;
  s.eom = p.eom;
  const Schedule sched = generate_schedule(s);
  EXPECT_EQ(sched.periods(), 2u);
  const auto pay = payment_dates(sched, p.payment_lag, c);
  EXPECT_EQ(pay.size(), 2u);
  EXPECT_GT(pay[0], sched.adjusted[1]);
  const ObservationPeriod op = observation_days(sched.adjusted[0], sched.adjusted[1], p.legs[1].observation, r.leg_calendar(p, p.legs[1]));
  EXPECT_EQ(op.weight_total(), sched.adjusted[1] - sched.adjusted[0]);
  EXPECT_EQ(op.days.back().rate_date, op.days[op.days.size() - 2].rate_date);   // 1-day lockout
  // the original entries are still there
  EXPECT_TRUE(r.has_instrument("USD-SOFR-OIS"));
  // a second load of the same file is a duplicate
  std::ofstream(path) << doc;
  EXPECT_THROW(r.add_file(path), RegistryError);
  std::remove(path.c_str());
}

TEST(Registry, DefaultDirectoryAndEnvironmentOverride) {
  const std::string d = Registry::default_dir();
  EXPECT_NE(d.find("blueprints/conventions"), std::string::npos) << d;
  EXPECT_TRUE(std::filesystem::exists(d)) << d;
  EXPECT_THROW(Registry::load((std::filesystem::temp_directory_path() / "epykos_g0_no_such_dir").string()), RegistryError);
}
