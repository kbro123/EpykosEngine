#include "epykos/conventions/fixings.hpp"

#include <cmath>

namespace epykos::conventions {

void FixingsHistory::set(const std::string& index, Date d, double rate) { table_[index][d] = rate; }

std::optional<double> FixingsHistory::get(const std::string& index, Date d) const noexcept {
  auto it = table_.find(index);
  if (it == table_.end()) return std::nullopt;
  auto jt = it->second.find(d);
  if (jt == it->second.end()) return std::nullopt;
  return jt->second;
}

double FixingsHistory::at(const std::string& index, Date d) const {
  auto v = get(index, d);
  if (!v) throw DateError("fixings: no " + index + " fixing for " + to_iso(d));
  return *v;
}

std::optional<Date> FixingsHistory::last_on_or_before(const std::string& index, Date d) const noexcept {
  auto it = table_.find(index);
  if (it == table_.end() || it->second.empty()) return std::nullopt;
  auto jt = it->second.upper_bound(d);
  if (jt == it->second.begin()) return std::nullopt;
  --jt;
  return jt->first;
}

std::size_t FixingsHistory::size(const std::string& index) const noexcept {
  auto it = table_.find(index);
  return it == table_.end() ? 0 : it->second.size();
}

std::vector<std::string> FixingsHistory::indices() const {
  std::vector<std::string> out;
  for (const auto& kv : table_) out.push_back(kv.first);
  return out;
}

const std::map<Date, double>* FixingsHistory::series(const std::string& index) const noexcept {
  auto it = table_.find(index);
  return it == table_.end() ? nullptr : &it->second;
}

namespace {

// splitmix64: a tiny, well-mixed 64-bit generator; adequate for a synthetic fixture and
// independent of the engine's Philox (whose draws belong to the MC fixtures).
struct SplitMix64 {
  std::uint64_t s;
  std::uint64_t next() noexcept {
    std::uint64_t z = (s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  }
  // uniform in [-1, 1)
  double symmetric() noexcept { return static_cast<double>(next() >> 11) * (2.0 / 9007199254740992.0) - 1.0; }
};

std::uint64_t hash_string(const std::string& s) noexcept {
  std::uint64_t h = 1469598103934665603ull;  // FNV-1a
  for (unsigned char c : s) {
    h ^= c;
    h *= 1099511628211ull;
  }
  return h;
}

}  // namespace

void synthetic_fixings(FixingsHistory& out, const std::string& index, const Calendar& fixing_calendar,
                       Date from, Date to, const SyntheticFixingsSpec& spec) {
  SplitMix64 rng{spec.seed ^ hash_string(index) ^ (static_cast<std::uint64_t>(from) << 32)};
  double r = spec.level;
  for (Date d = from; d <= to; ++d) {
    if (!fixing_calendar.is_business_day(d)) continue;
    // mean reversion, then a bounded uniform step (no normal draw: this is a fixture, not a model)
    r += spec.reversion * (spec.level - r) + spec.daily_vol * rng.symmetric() * 1.7320508075688772;
    if (r < spec.floor) r = spec.floor;
    if (r > spec.cap) r = spec.cap;
    // published to 5 decimals of a percent (SOFR: 2 dp of a percent; €STR: 3 dp) — keep 6 dp of a rate
    const double published = std::round(r * 1e6) / 1e6;
    out.set(index, d, published);
  }
}

}  // namespace epykos::conventions
