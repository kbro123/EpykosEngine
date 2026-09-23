// EpykosEngine — the conventions registry (M3/G0, D36, D37).
//
// Definitions are DATA: currencies, calendars (rule instances), day-count names, index
// definitions, instrument conventions and central-bank meeting schedules are read from
// blueprints/conventions/*.json. Rule kinds and mechanics are CODE (calendar.hpp, daycount.hpp,
// schedule.hpp, rfr.hpp). Adding a currency, an index or an instrument convention is a JSON
// edit (or an extra file): no C++ changes.
//
// The schema is strict: every object's keys are checked against the set below, required
// fields must be present, names must resolve (a leg's index, an index's calendar, a joint
// calendar's members), and every error names the file, line and column of the offending value.
//
// File layout (any file may carry any of the sections; the registry merges every *.json of
// the directory in name order; a name defined twice is an error):
//
//   { "meta": { "title", "description"?, "sources"?: [..], "imported_from"?: {..}, "notes"?: [..] },
//     "currencies":  { CODE: { "name", "minor_units", "settlement_calendar", "discount_index",
//                              "default_instrument", "sources"?, "provenance"? } },
//     "calendars":   { NAME: { "description"?, "weekend": [0..6 ...], "observance"?, "first_year"?,
//                              "last_year"?, "holidays": [ RULE... ], "extra_holidays"?: ["YYYY-MM-DD"],
//                              "not_holidays"?: [..], "sources"?, "provenance"? }
//                    | NAME: { "description"?, "join": [NAME...], "sources"?, "provenance"? } },
//       RULE = { "label", "rule": "fixed" | "nth_weekday" | "easter_offset", "month"?, "day"?,
//                "weekday"?, "n"?, "days"?, "skip_if_first_weekday_of_month"?, "from_year"?,
//                "to_year"?, "observance"?, "source"? }
//     "day_counts":  { NAME: { "description", "sources"? } }       (NAME must be one the code implements)
//     "indices":     { NAME: { "description"?, "currency", "type": "overnight" | "term", "tenor"?,
//                              "day_count", "fixing_calendar", "fixing_lag"?, "publication_lag"?,
//                              "publication"?, "administrator"?, "sources"?, "provenance"? } },
//     "instruments": { NAME: { "type": "ois" | "irs" | "basis" | "deposit" | "future", "description"?,
//                              "currency", "calendar", "spot_lag", "payment_lag"?, "bdc", "eom"?, "stub"?,
//                              "legs"?: [ LEG... ], "discount_index"?, "tenors"?: [..],
//                              "index"?, "accrual"?, "day_count"?, "period"?, "quote"?, "contracts"?,
//                              "sources"?, "provenance"? } },
//       LEG = { "role": "fixed" | "float", "index"?, "frequency", "term_up_to"?, "day_count",
//               "compounding"?: "compounded" | "averaged" | "none", "observation"?: { "method",
//               "lookback_days"?, "lockout_days"? }, "spread"?: bool, "payment_lag"?, "calendar"? }
//     "cb_schedules": { CCY: { "bank", "source", "as_of", "meetings": ["YYYY-MM-DD"...] } } }
//
//   "sources"    is an object { field-or-topic: citation | [citations] } — the citation of each
//                value, matching docs/G4_BUNDLE.md.
//   "provenance" is { "imported_from", "imported_on", "imported_key"?, "cross_check":
//                "agree" | "disagree" | "partial" | "not_in_source" | "own_research", "notes"?: [..] }.
//
// Structure only: nothing here depends on a Scalar.
#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "epykos/conventions/calendar.hpp"
#include "epykos/conventions/daycount.hpp"
#include "epykos/conventions/rfr.hpp"
#include "epykos/conventions/schedule.hpp"
#include "epykos/util/json.hpp"

namespace epykos::conventions {

class RegistryError : public std::runtime_error {
 public:
  explicit RegistryError(const std::string& what) : std::runtime_error(what) {}
};

// The per-value citations of an entry, as written in the JSON ("sources" object).
using Sources = std::vector<std::pair<std::string, std::vector<std::string>>>;

struct Provenance {
  std::string imported_from;   // e.g. the SwapEngine conventions file (D37)
  std::string imported_on;     // ISO date
  std::string imported_key;    // the key in that file
  std::string cross_check;     // agree | disagree | partial | not_in_source | own_research
  std::vector<std::string> notes;
};

struct CurrencyDef {
  std::string code;
  std::string name;
  int minor_units = 2;
  std::string settlement_calendar;
  std::string discount_index;
  std::string default_instrument;
  Sources sources;
  Provenance provenance;
};

struct DayCountDef {
  std::string name;
  DayCount day_count = DayCount::Act360;
  std::string description;
  Sources sources;
};

struct IndexDef {
  enum class Kind : unsigned char { Overnight, Term };
  std::string name;
  std::string description;
  std::string currency;
  Kind kind = Kind::Overnight;
  Period tenor{0, 'D'};          // Term only
  DayCount day_count = DayCount::Act360;
  std::string fixing_calendar;
  int fixing_lag = 0;            // Term: business days before the period start; Overnight: 0
  int publication_lag = 0;       // Overnight: 1 = the rate for day d is published on the next business day
  std::string publication;       // free text: "08:00 ET on the next U.S. Government Securities Business Day"
  std::string administrator;
  Sources sources;
  Provenance provenance;
};

struct LegConvention {
  enum class Role : unsigned char { Fixed, Float };
  enum class Compounding : unsigned char { None, Compounded, Averaged };
  Role role = Role::Fixed;
  std::string index;             // Float
  Period frequency{1, 'Y'};
  bool term_frequency = false;   // frequency "TERM": one period
  Period term_up_to{0, 'D'};     // tenors up to this pay once at the end (0 = never)
  DayCount day_count = DayCount::Act360;
  Compounding compounding = Compounding::None;
  ObservationSpec observation;   // Float overnight legs
  bool spread = false;           // the leg that carries the quoted spread (basis swaps)
  int payment_lag = -1;          // -1 = the instrument's
  std::string calendar;          // empty = the instrument's
};

struct InstrumentConvention {
  enum class Type : unsigned char { OIS, IRS, Basis, Deposit, Future };
  std::string name;
  Type type = Type::OIS;
  std::string description;
  std::string currency;
  std::string calendar;          // spot / roll / payment calendar
  int spot_lag = 0;
  int payment_lag = 0;
  BusinessDayConvention bdc = BusinessDayConvention::ModifiedFollowing;
  bool eom = false;
  StubKind stub = StubKind::ShortFront;
  std::vector<LegConvention> legs;
  std::string discount_index;
  std::vector<Period> tenors;    // the standard quoted tenor set
  // futures
  std::string index;
  std::string accrual;           // "compounded" | "averaged" | "fixing"
  DayCount day_count = DayCount::Act360;
  std::string period;            // "imm_quarter" | "calendar_month" | "imm_quarter_deposit"
  std::string quote;             // "100 - rate"
  int contracts = 0;             // how many are quoted on the front end
  Sources sources;
  Provenance provenance;
};

struct CbSchedule {
  std::string currency;
  std::string bank;
  std::string source;
  std::string as_of;
  std::vector<Date> meetings;
};

struct FileMeta {
  std::string path;
  std::string title;
  std::string description;
  std::vector<std::string> sources;
  std::vector<std::string> notes;
  Provenance imported_from;
};

class Registry {
 public:
  Registry() = default;
  Registry(Registry&&) = default;
  Registry& operator=(Registry&&) = default;

  // The blueprints/conventions directory: $EPYKOS_BLUEPRINTS/conventions if the variable is
  // set, else the source tree's blueprints/conventions compiled in at build time.
  static std::string default_dir();
  // Loads every *.json in `dir` (name order) and validates the whole.
  static Registry load(const std::string& dir = default_dir());
  static Registry load_files(const std::vector<std::string>& paths);

  // Merges one more file and re-validates (an extra convention file needs no C++ change).
  void add_file(const std::string& path);
  // Merges a parsed document (origin names it in errors).
  void add_document(const json::Value& doc, const std::string& origin);

  const std::vector<FileMeta>& files() const noexcept { return files_; }

  const CurrencyDef& currency(const std::string& code) const;
  const Calendar& calendar(const std::string& name) const;
  const DayCountDef& day_count(const std::string& name) const;
  const IndexDef& index(const std::string& name) const;
  const InstrumentConvention& instrument(const std::string& name) const;
  const CbSchedule& cb_schedule(const std::string& currency) const;

  bool has_currency(const std::string& c) const noexcept { return currencies_.count(c) != 0; }
  bool has_calendar(const std::string& c) const noexcept { return calendars_.count(c) != 0; }
  bool has_index(const std::string& c) const noexcept { return indices_.count(c) != 0; }
  bool has_instrument(const std::string& c) const noexcept { return instruments_.count(c) != 0; }

  std::vector<std::string> currency_codes() const;
  std::vector<std::string> calendar_names() const;
  std::vector<std::string> day_count_names() const;
  std::vector<std::string> index_names() const;
  std::vector<std::string> instrument_names() const;
  std::vector<std::string> cb_schedule_currencies() const;

  // The fixing calendar of a leg's index, or the instrument's calendar for a fixed leg.
  const Calendar& leg_calendar(const InstrumentConvention& inst, const LegConvention& leg) const;

 private:
  struct CalendarDef;   // raw, resolved in finalize()
  void finalize();

  std::vector<FileMeta> files_;
  std::map<std::string, CurrencyDef> currencies_;
  std::map<std::string, std::shared_ptr<CalendarDef>> calendar_defs_;
  std::map<std::string, std::unique_ptr<Calendar>> calendars_;
  std::map<std::string, DayCountDef> day_counts_;
  std::map<std::string, IndexDef> indices_;
  std::map<std::string, InstrumentConvention> instruments_;
  std::map<std::string, CbSchedule> cb_schedules_;
};

}  // namespace epykos::conventions
