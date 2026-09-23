#include "epykos/conventions/registry.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <initializer_list>
#include <set>

#ifndef EPYKOS_BLUEPRINTS_DIR
#define EPYKOS_BLUEPRINTS_DIR ""
#endif

namespace epykos::conventions {

using json::Value;

namespace {

[[noreturn]] void fail(const Value& at, const std::string& what) {
  throw RegistryError("conventions: " + at.where() + ": " + what);
}

[[noreturn]] void fail(const std::string& what) { throw RegistryError("conventions: " + what); }

// Strict object: every key must be in `allowed`, every key in `required` must be present.
void check_keys(const Value& obj, std::initializer_list<const char*> allowed,
                std::initializer_list<const char*> required, const char* what) {
  if (!obj.is_object()) fail(obj, std::string(what) + ": expected an object, found " + obj.describe());
  for (const auto& m : obj.as_object()) {
    bool ok = false;
    for (const char* a : allowed) {
      if (m.first == a) {
        ok = true;
        break;
      }
    }
    if (!ok) fail(m.second, std::string(what) + ": unknown key \"" + m.first + "\"");
  }
  for (const char* r : required) {
    if (!obj.has(r)) fail(obj, std::string(what) + ": missing required key \"" + r + "\"");
  }
}

std::string str(const Value& obj, const char* key) { return obj.at(key).as_string(); }

std::string str_or(const Value& obj, const char* key, const std::string& dflt = "") {
  const Value* v = obj.find(key);
  return v ? v->as_string() : dflt;
}

int integer(const Value& obj, const char* key) {
  const std::int64_t v = obj.at(key).as_int();
  if (v < -1000000 || v > 1000000) fail(obj.at(key), std::string("\"") + key + "\" out of range");
  return static_cast<int>(v);
}

int int_or(const Value& obj, const char* key, int dflt) { return obj.has(key) ? integer(obj, key) : dflt; }

bool bool_or(const Value& obj, const char* key, bool dflt) {
  const Value* v = obj.find(key);
  return v ? v->as_bool() : dflt;
}

std::vector<std::string> string_list(const Value& v, const char* what) {
  std::vector<std::string> out;
  if (v.is_string()) {
    out.push_back(v.as_string());
    return out;
  }
  if (!v.is_array()) fail(v, std::string(what) + ": expected a string or an array of strings");
  for (const Value& s : v.as_array()) out.push_back(s.as_string());
  return out;
}

Sources parse_sources(const Value& obj) {
  Sources out;
  const Value* v = obj.find("sources");
  if (!v) return out;
  if (!v->is_object()) fail(*v, "\"sources\": expected an object { topic: citation | [citations] }");
  for (const auto& m : v->as_object()) out.emplace_back(m.first, string_list(m.second, "sources"));
  return out;
}

Provenance parse_provenance(const Value& obj) {
  Provenance p;
  const Value* v = obj.find("provenance");
  if (!v) return p;
  check_keys(*v, {"imported_from", "imported_on", "imported_key", "cross_check", "notes"},
             {"imported_from", "imported_on", "cross_check"}, "provenance");
  p.imported_from = str(*v, "imported_from");
  p.imported_on = str(*v, "imported_on");
  p.imported_key = str_or(*v, "imported_key");
  p.cross_check = str(*v, "cross_check");
  static const std::set<std::string> kinds = {"agree", "disagree", "partial", "not_in_source", "own_research"};
  if (!kinds.count(p.cross_check)) {
    fail(v->at("cross_check"), "\"cross_check\" must be agree | disagree | partial | not_in_source | own_research");
  }
  if (v->has("notes")) p.notes = string_list(v->at("notes"), "provenance.notes");
  return p;
}

Period period_of(const Value& v) {
  try {
    return Period::parse(v.as_string());
  } catch (const DateError& e) {
    fail(v, e.what());
  }
}

Date date_of(const Value& v) {
  try {
    return parse_iso(v.as_string());
  } catch (const DateError& e) {
    fail(v, e.what());
  }
}

template <class F>
auto named(const Value& v, F&& f) -> decltype(f(v.as_string())) {
  try {
    return f(v.as_string());
  } catch (const DateError& e) {
    fail(v, e.what());
  }
}

}  // namespace

struct Registry::CalendarDef {
  std::string name;
  std::string origin;
  std::string description;
  std::vector<int> weekend;
  Observance observance = Observance::None;
  int first_year = 2020;
  int last_year = 2080;
  std::vector<HolidayRule> rules;
  std::vector<Date> extra;
  std::vector<Date> not_holidays;
  std::vector<std::string> join;
  Sources sources;
  Provenance provenance;
};

std::string Registry::default_dir() {
  if (const char* env = std::getenv("EPYKOS_BLUEPRINTS")) {
    if (*env) return std::string(env) + "/conventions";
  }
  const std::string compiled = EPYKOS_BLUEPRINTS_DIR;
  if (compiled.empty()) fail("no blueprints directory: set EPYKOS_BLUEPRINTS or build from the source tree");
  return compiled + "/conventions";
}

Registry Registry::load(const std::string& dir) {
  std::vector<std::string> paths;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
    if (entry.path().extension() == ".json" && entry.path().filename().string()[0] != '.') {
      paths.push_back(entry.path().string());
    }
  }
  if (ec) fail("cannot read directory \"" + dir + "\": " + ec.message());
  if (paths.empty()) fail("no *.json files in \"" + dir + "\"");
  std::sort(paths.begin(), paths.end());
  return load_files(paths);
}

Registry Registry::load_files(const std::vector<std::string>& paths) {
  Registry r;
  for (const std::string& p : paths) r.add_document(json::parse_file(p), p);
  r.finalize();
  return r;
}

void Registry::add_file(const std::string& path) {
  add_document(json::parse_file(path), path);
  finalize();
}

void Registry::add_document(const Value& doc, const std::string& origin) {
  check_keys(doc, {"meta", "currencies", "calendars", "day_counts", "indices", "instruments", "cb_schedules"}, {},
             "document");
  FileMeta meta;
  meta.path = origin;
  if (const Value* m = doc.find("meta")) {
    check_keys(*m, {"title", "description", "sources", "notes", "imported_from"}, {"title"}, "meta");
    meta.title = str(*m, "title");
    meta.description = str_or(*m, "description");
    if (m->has("sources")) meta.sources = string_list(m->at("sources"), "meta.sources");
    if (m->has("notes")) meta.notes = string_list(m->at("notes"), "meta.notes");
    if (m->has("imported_from")) {
      Value wrap = Value::object();
      wrap.set("provenance", m->at("imported_from"));
      meta.imported_from = parse_provenance(wrap);
    }
  }
  files_.push_back(meta);

  if (const Value* cs = doc.find("currencies")) {
    if (!cs->is_object()) fail(*cs, "\"currencies\": expected an object");
    for (const auto& kv : cs->as_object()) {
      const Value& o = kv.second;
      check_keys(o, {"name", "minor_units", "settlement_calendar", "discount_index", "default_instrument", "sources",
                     "provenance"},
                 {"name", "minor_units", "settlement_calendar", "discount_index", "default_instrument"},
                 ("currency " + kv.first).c_str());
      if (currencies_.count(kv.first)) fail(o, "currency \"" + kv.first + "\" defined twice");
      CurrencyDef c;
      c.code = kv.first;
      c.name = str(o, "name");
      c.minor_units = integer(o, "minor_units");
      c.settlement_calendar = str(o, "settlement_calendar");
      c.discount_index = str(o, "discount_index");
      c.default_instrument = str(o, "default_instrument");
      c.sources = parse_sources(o);
      c.provenance = parse_provenance(o);
      currencies_[kv.first] = std::move(c);
    }
  }

  if (const Value* cs = doc.find("calendars")) {
    if (!cs->is_object()) fail(*cs, "\"calendars\": expected an object");
    for (const auto& kv : cs->as_object()) {
      const Value& o = kv.second;
      const std::string what = "calendar " + kv.first;
      if (calendar_defs_.count(kv.first)) fail(o, what + " defined twice");
      auto def = std::make_shared<CalendarDef>();
      def->name = kv.first;
      def->origin = o.where();
      if (o.has("join")) {
        check_keys(o, {"description", "join", "sources", "provenance"}, {"join"}, what.c_str());
        def->join = string_list(o.at("join"), "join");
        if (def->join.size() < 2) fail(o.at("join"), what + ": a joint calendar needs at least two members");
      } else {
        check_keys(o, {"description", "weekend", "observance", "first_year", "last_year", "holidays", "extra_holidays",
                       "not_holidays", "sources", "provenance"},
                   {"weekend", "holidays"}, what.c_str());
        for (const Value& w : o.at("weekend").as_array()) {
          const std::int64_t wd = w.as_int();
          if (wd < 0 || wd > 6) fail(w, what + ": weekend weekday must be 0 (Monday) .. 6 (Sunday)");
          def->weekend.push_back(static_cast<int>(wd));
        }
        if (o.has("observance")) def->observance = named(o.at("observance"), observance_from_string);
        def->first_year = int_or(o, "first_year", 2020);
        def->last_year = int_or(o, "last_year", 2080);
        if (def->first_year < 1600 || def->last_year > 3000 || def->first_year > def->last_year) {
          fail(o, what + ": first_year / last_year out of range");
        }
        for (const Value& rv : o.at("holidays").as_array()) {
          check_keys(rv, {"label", "rule", "month", "day", "weekday", "n", "days", "skip_if_first_weekday_of_month",
                          "from_year", "to_year", "observance", "source"},
                     {"label", "rule"}, (what + " rule").c_str());
          HolidayRule r;
          r.label = str(rv, "label");
          r.source = str_or(rv, "source");
          const std::string kind = str(rv, "rule");
          const std::string rw = what + " rule \"" + r.label + "\"";
          if (kind == "fixed") {
            r.kind = HolidayRule::Kind::Fixed;
            r.month = integer(rv, "month");
            r.day = integer(rv, "day");
            if (r.month < 1 || r.month > 12 || r.day < 1 || r.day > 31) fail(rv, rw + ": month/day out of range");
            for (const char* k : {"weekday", "n", "days", "skip_if_first_weekday_of_month"}) {
              if (rv.has(k)) fail(rv.at(k), rw + ": \"" + k + "\" does not belong to a fixed rule");
            }
          } else if (kind == "nth_weekday") {
            r.kind = HolidayRule::Kind::NthWeekday;
            r.month = integer(rv, "month");
            r.weekday = integer(rv, "weekday");
            r.n = integer(rv, "n");
            if (r.month < 1 || r.month > 12) fail(rv, rw + ": month out of range");
            if (r.weekday < 0 || r.weekday > 6) fail(rv, rw + ": weekday must be 0 (Monday) .. 6 (Sunday)");
            if (r.n == 0 || r.n > 5 || r.n < -1) fail(rv, rw + ": n must be 1..5 or -1 (last)");
            for (const char* k : {"day", "days", "skip_if_first_weekday_of_month", "observance"}) {
              if (rv.has(k)) fail(rv.at(k), rw + ": \"" + k + "\" does not belong to an nth_weekday rule");
            }
          } else if (kind == "easter_offset") {
            r.kind = HolidayRule::Kind::EasterOffset;
            r.offset_days = integer(rv, "days");
            r.skip_if_first_weekday_of_month = bool_or(rv, "skip_if_first_weekday_of_month", false);
            for (const char* k : {"month", "day", "weekday", "n", "observance"}) {
              if (rv.has(k)) fail(rv.at(k), rw + ": \"" + k + "\" does not belong to an easter_offset rule");
            }
          } else {
            fail(rv.at("rule"), rw + ": unknown rule kind \"" + kind + "\" (fixed | nth_weekday | easter_offset)");
          }
          r.from_year = int_or(rv, "from_year", 0);
          r.to_year = int_or(rv, "to_year", 0);
          if (rv.has("observance")) {
            r.has_observance = true;
            r.observance = named(rv.at("observance"), observance_from_string);
          }
          def->rules.push_back(std::move(r));
        }
        if (o.has("extra_holidays")) {
          for (const Value& d : o.at("extra_holidays").as_array()) def->extra.push_back(date_of(d));
        }
        if (o.has("not_holidays")) {
          for (const Value& d : o.at("not_holidays").as_array()) def->not_holidays.push_back(date_of(d));
        }
      }
      def->description = str_or(o, "description");
      def->sources = parse_sources(o);
      def->provenance = parse_provenance(o);
      calendar_defs_[kv.first] = std::move(def);
    }
  }

  if (const Value* ds = doc.find("day_counts")) {
    if (!ds->is_object()) fail(*ds, "\"day_counts\": expected an object");
    for (const auto& kv : ds->as_object()) {
      const Value& o = kv.second;
      check_keys(o, {"description", "sources"}, {"description"}, ("day_count " + kv.first).c_str());
      if (day_counts_.count(kv.first)) fail(o, "day_count \"" + kv.first + "\" defined twice");
      DayCountDef d;
      d.name = kv.first;
      try {
        d.day_count = daycount_from_string(kv.first);
      } catch (const DateError& e) {
        fail(o, std::string("day_count \"") + kv.first + "\" is not implemented by the code: " + e.what());
      }
      d.description = str(o, "description");
      d.sources = parse_sources(o);
      day_counts_[kv.first] = std::move(d);
    }
  }

  if (const Value* is = doc.find("indices")) {
    if (!is->is_object()) fail(*is, "\"indices\": expected an object");
    for (const auto& kv : is->as_object()) {
      const Value& o = kv.second;
      const std::string what = "index " + kv.first;
      check_keys(o, {"description", "currency", "type", "tenor", "day_count", "fixing_calendar", "fixing_lag",
                     "publication_lag", "publication", "administrator", "sources", "provenance"},
                 {"currency", "type", "day_count", "fixing_calendar"}, what.c_str());
      if (indices_.count(kv.first)) fail(o, what + " defined twice");
      IndexDef x;
      x.name = kv.first;
      x.description = str_or(o, "description");
      x.currency = str(o, "currency");
      const std::string type = str(o, "type");
      if (type == "overnight") {
        x.kind = IndexDef::Kind::Overnight;
        if (o.has("tenor")) fail(o.at("tenor"), what + ": an overnight index has no tenor");
        x.publication_lag = int_or(o, "publication_lag", 1);
        x.fixing_lag = int_or(o, "fixing_lag", 0);
      } else if (type == "term") {
        x.kind = IndexDef::Kind::Term;
        if (!o.has("tenor")) fail(o, what + ": a term index needs a \"tenor\"");
        x.tenor = period_of(o.at("tenor"));
        if (!o.has("fixing_lag")) fail(o, what + ": a term index needs a \"fixing_lag\"");
        x.fixing_lag = integer(o, "fixing_lag");
        x.publication_lag = int_or(o, "publication_lag", 0);
      } else {
        fail(o.at("type"), what + ": type must be overnight | term");
      }
      x.day_count = named(o.at("day_count"), daycount_from_string);
      x.fixing_calendar = str(o, "fixing_calendar");
      x.publication = str_or(o, "publication");
      x.administrator = str_or(o, "administrator");
      x.sources = parse_sources(o);
      x.provenance = parse_provenance(o);
      indices_[kv.first] = std::move(x);
    }
  }

  if (const Value* ps = doc.find("instruments")) {
    if (!ps->is_object()) fail(*ps, "\"instruments\": expected an object");
    for (const auto& kv : ps->as_object()) {
      const Value& o = kv.second;
      const std::string what = "instrument " + kv.first;
      check_keys(o, {"type", "description", "currency", "calendar", "spot_lag", "payment_lag", "bdc", "eom", "stub",
                     "legs", "discount_index", "tenors", "index", "accrual", "day_count", "period", "quote",
                     "contracts", "sources", "provenance"},
                 {"type", "currency", "calendar", "spot_lag", "bdc"}, what.c_str());
      if (instruments_.count(kv.first)) fail(o, what + " defined twice");
      InstrumentConvention p;
      p.name = kv.first;
      const std::string type = str(o, "type");
      if (type == "ois") {
        p.type = InstrumentConvention::Type::OIS;
      } else if (type == "irs") {
        p.type = InstrumentConvention::Type::IRS;
      } else if (type == "basis") {
        p.type = InstrumentConvention::Type::Basis;
      } else if (type == "deposit") {
        p.type = InstrumentConvention::Type::Deposit;
      } else if (type == "future") {
        p.type = InstrumentConvention::Type::Future;
      } else {
        fail(o.at("type"), what + ": type must be ois | irs | basis | deposit | future");
      }
      p.description = str_or(o, "description");
      p.currency = str(o, "currency");
      p.calendar = str(o, "calendar");
      p.spot_lag = integer(o, "spot_lag");
      p.payment_lag = int_or(o, "payment_lag", 0);
      p.bdc = named(o.at("bdc"), bdc_from_string);
      p.eom = bool_or(o, "eom", false);
      if (o.has("stub")) p.stub = named(o.at("stub"), stub_from_string);
      p.discount_index = str_or(o, "discount_index");
      if (o.has("tenors")) {
        for (const Value& t : o.at("tenors").as_array()) p.tenors.push_back(period_of(t));
      }
      const bool swap = p.type == InstrumentConvention::Type::OIS || p.type == InstrumentConvention::Type::IRS ||
                        p.type == InstrumentConvention::Type::Basis;
      if (swap) {
        if (!o.has("legs")) fail(o, what + ": a swap needs \"legs\"");
        for (const char* k : {"index", "accrual", "period", "quote", "contracts"}) {
          if (o.has(k)) fail(o.at(k), what + ": \"" + k + "\" belongs to a future, not a swap");
        }
        const auto& legs = o.at("legs").as_array();
        if (legs.size() != 2) fail(o.at("legs"), what + ": a swap has exactly two legs");
        for (const Value& lv : legs) {
          check_keys(lv, {"role", "index", "frequency", "term_up_to", "day_count", "compounding", "observation", "spread",
                          "payment_lag", "calendar"},
                     {"role", "frequency", "day_count"}, (what + " leg").c_str());
          LegConvention leg;
          const std::string role = str(lv, "role");
          if (role == "fixed") {
            leg.role = LegConvention::Role::Fixed;
            for (const char* k : {"index", "compounding", "observation"}) {
              if (lv.has(k)) fail(lv.at(k), what + ": \"" + k + "\" does not belong to a fixed leg");
            }
          } else if (role == "float") {
            leg.role = LegConvention::Role::Float;
            if (!lv.has("index")) fail(lv, what + ": a float leg needs an \"index\"");
            leg.index = str(lv, "index");
          } else {
            fail(lv.at("role"), what + ": leg role must be fixed | float");
          }
          const std::string freq = str(lv, "frequency");
          if (freq == "TERM") {
            leg.term_frequency = true;
          } else {
            leg.frequency = period_of(lv.at("frequency"));
            if (leg.frequency.n <= 0) fail(lv.at("frequency"), what + ": frequency must be positive");
          }
          if (lv.has("term_up_to")) leg.term_up_to = period_of(lv.at("term_up_to"));
          leg.day_count = named(lv.at("day_count"), daycount_from_string);
          if (lv.has("compounding")) {
            const std::string c = str(lv, "compounding");
            if (c == "compounded") {
              leg.compounding = LegConvention::Compounding::Compounded;
            } else if (c == "averaged") {
              leg.compounding = LegConvention::Compounding::Averaged;
            } else if (c == "none") {
              leg.compounding = LegConvention::Compounding::None;
            } else {
              fail(lv.at("compounding"), what + ": compounding must be compounded | averaged | none");
            }
          }
          if (lv.has("observation")) {
            const Value& ov = lv.at("observation");
            check_keys(ov, {"method", "lookback_days", "lockout_days"}, {"method"}, (what + " observation").c_str());
            leg.observation.method = named(ov.at("method"), observation_method_from_string);
            leg.observation.lookback_days = int_or(ov, "lookback_days", 0);
            leg.observation.lockout_days = int_or(ov, "lockout_days", 0);
            if (leg.observation.lookback_days < 0 || leg.observation.lockout_days < 0) {
              fail(ov, what + ": lookback_days / lockout_days must be >= 0");
            }
            if (leg.observation.method == ObservationMethod::Plain && leg.observation.lookback_days != 0) {
              fail(ov, what + ": a plain observation has no lookback_days");
            }
          }
          leg.spread = bool_or(lv, "spread", false);
          leg.payment_lag = int_or(lv, "payment_lag", -1);
          leg.calendar = str_or(lv, "calendar");
          p.legs.push_back(std::move(leg));
        }
        if (p.type == InstrumentConvention::Type::Basis) {
          int spread_legs = 0;
          for (const auto& l : p.legs) spread_legs += l.spread ? 1 : 0;
          if (spread_legs != 1) fail(o.at("legs"), what + ": a basis swap has exactly one leg with \"spread\": true");
        }
      } else if (p.type == InstrumentConvention::Type::Future) {
        check_keys(o, {"type", "description", "currency", "calendar", "spot_lag", "bdc", "index", "accrual", "day_count",
                       "period", "quote", "contracts", "sources", "provenance"},
                   {"index", "accrual", "day_count", "period", "quote"}, what.c_str());
        p.index = str(o, "index");
        p.accrual = str(o, "accrual");
        if (p.accrual != "compounded" && p.accrual != "averaged" && p.accrual != "fixing") {
          fail(o.at("accrual"), what + ": accrual must be compounded | averaged | fixing");
        }
        p.day_count = named(o.at("day_count"), daycount_from_string);
        p.period = str(o, "period");
        if (p.period != "imm_quarter" && p.period != "calendar_month" && p.period != "imm_quarter_deposit") {
          fail(o.at("period"), what + ": period must be imm_quarter | calendar_month | imm_quarter_deposit");
        }
        p.quote = str(o, "quote");
        p.contracts = int_or(o, "contracts", 0);
      } else {  // deposit
        check_keys(o, {"type", "description", "currency", "calendar", "spot_lag", "payment_lag", "bdc", "eom", "index",
                       "day_count", "tenors", "sources", "provenance"},
                   {"index", "day_count"}, what.c_str());
        p.index = str(o, "index");
        p.day_count = named(o.at("day_count"), daycount_from_string);
      }
      p.sources = parse_sources(o);
      p.provenance = parse_provenance(o);
      instruments_[kv.first] = std::move(p);
    }
  }

  if (const Value* cb = doc.find("cb_schedules")) {
    if (!cb->is_object()) fail(*cb, "\"cb_schedules\": expected an object");
    for (const auto& kv : cb->as_object()) {
      const Value& o = kv.second;
      check_keys(o, {"bank", "source", "as_of", "meetings"}, {"bank", "source", "as_of", "meetings"},
                 ("cb_schedule " + kv.first).c_str());
      if (cb_schedules_.count(kv.first)) fail(o, "cb_schedule \"" + kv.first + "\" defined twice");
      CbSchedule s;
      s.currency = kv.first;
      s.bank = str(o, "bank");
      s.source = str(o, "source");
      s.as_of = str(o, "as_of");
      for (const Value& d : o.at("meetings").as_array()) s.meetings.push_back(date_of(d));
      for (std::size_t i = 1; i < s.meetings.size(); ++i) {
        if (s.meetings[i] <= s.meetings[i - 1]) fail(o.at("meetings"), "cb_schedule " + kv.first + ": meetings not ascending");
      }
      cb_schedules_[kv.first] = std::move(s);
    }
  }
}

void Registry::finalize() {
  // Calendars: rule calendars first, then joint calendars (members may be joint themselves:
  // resolve iteratively).
  calendars_.clear();
  for (const auto& kv : calendar_defs_) {
    const CalendarDef& d = *kv.second;
    if (!d.join.empty()) continue;
    calendars_[d.name] = std::make_unique<Calendar>(d.name, d.weekend, d.rules, d.observance, d.extra, d.not_holidays,
                                                    d.first_year, d.last_year);
  }
  for (bool progress = true; progress;) {
    progress = false;
    for (const auto& kv : calendar_defs_) {
      const CalendarDef& d = *kv.second;
      if (d.join.empty() || calendars_.count(d.name)) continue;
      std::vector<const Calendar*> members;
      bool ready = true;
      for (const std::string& m : d.join) {
        if (!calendar_defs_.count(m)) fail(d.origin + ": calendar " + d.name + " joins unknown calendar \"" + m + "\"");
        if (m == d.name) fail(d.origin + ": calendar " + d.name + " joins itself");
        auto it = calendars_.find(m);
        if (it == calendars_.end()) {
          ready = false;
          break;
        }
        members.push_back(it->second.get());
      }
      if (!ready) continue;
      calendars_[d.name] = std::make_unique<Calendar>(d.name, members);
      progress = true;
    }
  }
  for (const auto& kv : calendar_defs_) {
    if (!calendars_.count(kv.first)) fail(kv.second->origin + ": calendar " + kv.first + " has a cyclic join");
  }
  // Cross references.
  for (const auto& kv : currencies_) {
    const CurrencyDef& c = kv.second;
    if (!calendars_.count(c.settlement_calendar)) {
      fail("currency " + c.code + ": unknown settlement_calendar \"" + c.settlement_calendar + "\"");
    }
    if (!indices_.count(c.discount_index)) fail("currency " + c.code + ": unknown discount_index \"" + c.discount_index + "\"");
    if (!instruments_.count(c.default_instrument)) {
      fail("currency " + c.code + ": unknown default_instrument \"" + c.default_instrument + "\"");
    }
  }
  for (const auto& kv : indices_) {
    const IndexDef& x = kv.second;
    if (!currencies_.count(x.currency)) fail("index " + x.name + ": unknown currency \"" + x.currency + "\"");
    if (!calendars_.count(x.fixing_calendar)) fail("index " + x.name + ": unknown fixing_calendar \"" + x.fixing_calendar + "\"");
  }
  for (const auto& kv : instruments_) {
    const InstrumentConvention& p = kv.second;
    if (!currencies_.count(p.currency)) fail("instrument " + p.name + ": unknown currency \"" + p.currency + "\"");
    if (!calendars_.count(p.calendar)) fail("instrument " + p.name + ": unknown calendar \"" + p.calendar + "\"");
    if (!p.discount_index.empty() && !indices_.count(p.discount_index)) {
      fail("instrument " + p.name + ": unknown discount_index \"" + p.discount_index + "\"");
    }
    if (!p.index.empty() && !indices_.count(p.index)) fail("instrument " + p.name + ": unknown index \"" + p.index + "\"");
    for (const LegConvention& l : p.legs) {
      if (l.role == LegConvention::Role::Float) {
        auto it = indices_.find(l.index);
        if (it == indices_.end()) fail("instrument " + p.name + ": unknown leg index \"" + l.index + "\"");
        if (it->second.kind == IndexDef::Kind::Overnight && l.compounding == LegConvention::Compounding::None) {
          fail("instrument " + p.name + ": an overnight leg needs \"compounding\": compounded | averaged");
        }
        if (it->second.kind == IndexDef::Kind::Term && l.compounding != LegConvention::Compounding::None) {
          fail("instrument " + p.name + ": a term-rate leg has no compounding");
        }
      }
      if (!l.calendar.empty() && !calendars_.count(l.calendar)) {
        fail("instrument " + p.name + ": unknown leg calendar \"" + l.calendar + "\"");
      }
    }
  }
  for (const auto& kv : cb_schedules_) {
    if (!currencies_.count(kv.first)) fail("cb_schedule " + kv.first + ": unknown currency");
  }
}

template <class M>
static const typename M::mapped_type& lookup(const M& m, const std::string& key, const char* what) {
  auto it = m.find(key);
  if (it == m.end()) fail(std::string("no ") + what + " named \"" + key + "\"");
  return it->second;
}

const CurrencyDef& Registry::currency(const std::string& code) const { return lookup(currencies_, code, "currency"); }
const Calendar& Registry::calendar(const std::string& name) const { return *lookup(calendars_, name, "calendar"); }
const DayCountDef& Registry::day_count(const std::string& name) const { return lookup(day_counts_, name, "day_count"); }
const IndexDef& Registry::index(const std::string& name) const { return lookup(indices_, name, "index"); }
const InstrumentConvention& Registry::instrument(const std::string& name) const {
  return lookup(instruments_, name, "instrument");
}
const CbSchedule& Registry::cb_schedule(const std::string& ccy) const { return lookup(cb_schedules_, ccy, "cb_schedule"); }

template <class M>
static std::vector<std::string> keys_of(const M& m) {
  std::vector<std::string> out;
  for (const auto& kv : m) out.push_back(kv.first);
  return out;
}

std::vector<std::string> Registry::currency_codes() const { return keys_of(currencies_); }
std::vector<std::string> Registry::calendar_names() const { return keys_of(calendars_); }
std::vector<std::string> Registry::day_count_names() const { return keys_of(day_counts_); }
std::vector<std::string> Registry::index_names() const { return keys_of(indices_); }
std::vector<std::string> Registry::instrument_names() const { return keys_of(instruments_); }
std::vector<std::string> Registry::cb_schedule_currencies() const { return keys_of(cb_schedules_); }

const Calendar& Registry::leg_calendar(const InstrumentConvention& inst, const LegConvention& leg) const {
  if (!leg.calendar.empty()) return calendar(leg.calendar);
  if (leg.role == LegConvention::Role::Float) return calendar(index(leg.index).fixing_calendar);
  return calendar(inst.calendar);
}

}  // namespace epykos::conventions
