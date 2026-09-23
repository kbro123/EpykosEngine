// EpykosEngine — a fixings history table: index name -> date -> published rate (M3/G0).
//
// Realised fixings are DATA of a seasoned trade (PROBLEM.md section 3). The table holds plain
// doubles keyed by the index's publication convention: for an overnight index the key is the
// day the rate is FOR (SOFR for Thursday is keyed on Thursday, whatever day it was published);
// for a term index the key is the fixing date.
//
// There are no data files: histories are seeded and generated in code by synthetic_fixings()
// and are labelled synthetic. Nothing here depends on a Scalar.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "epykos/conventions/calendar.hpp"
#include "epykos/conventions/date.hpp"

namespace epykos::conventions {

class FixingsHistory {
 public:
  void set(const std::string& index, Date d, double rate);
  std::optional<double> get(const std::string& index, Date d) const noexcept;
  bool has(const std::string& index, Date d) const noexcept { return get(index, d).has_value(); }
  // The rate, or a DateError naming the index and date when it is missing.
  double at(const std::string& index, Date d) const;
  // The last published date <= d for the index, if any.
  std::optional<Date> last_on_or_before(const std::string& index, Date d) const noexcept;
  std::size_t size(const std::string& index) const noexcept;
  std::vector<std::string> indices() const;
  const std::map<Date, double>* series(const std::string& index) const noexcept;

 private:
  std::map<std::string, std::map<Date, double>> table_;
};

// SYNTHETIC fixings for an index: one rate per business day of `fixing_calendar` in
// [from, to], a mean-reverting random walk around `level` with daily step `daily_vol`
// (absolute, e.g. 0.0002 = 2 bp) driven by a splitmix64 stream from `seed`, clamped to
// [floor, cap]. Deterministic for a given (seed, index, from, to). The result is labelled
// synthetic: it is a test fixture, not market data (D16).
struct SyntheticFixingsSpec {
  std::uint64_t seed = 1;
  double level = 0.04;
  double daily_vol = 0.0002;
  double reversion = 0.02;   // fraction of the gap to `level` closed per business day
  double floor = 0.0;
  double cap = 0.25;
};

void synthetic_fixings(FixingsHistory& out, const std::string& index, const Calendar& fixing_calendar,
                       Date from, Date to, const SyntheticFixingsSpec& spec);

}  // namespace epykos::conventions
