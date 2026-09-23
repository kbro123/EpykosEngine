#include "epykos/conventions/rfr.hpp"

namespace epykos::conventions {

const char* to_string(ObservationMethod m) noexcept {
  switch (m) {
    case ObservationMethod::Plain: return "plain";
    case ObservationMethod::Lookback: return "lookback";
    case ObservationMethod::ObservationShift: return "observation_shift";
  }
  return "?";
}

ObservationMethod observation_method_from_string(const std::string& s) {
  if (s == "plain") return ObservationMethod::Plain;
  if (s == "lookback") return ObservationMethod::Lookback;
  if (s == "observation_shift") return ObservationMethod::ObservationShift;
  throw DateError("rfr: unknown observation method \"" + s + "\" (plain | lookback | observation_shift)");
}

int ObservationPeriod::weight_total() const noexcept {
  int n = 0;
  for (const ObservationDay& d : days) n += d.weight_days;
  return n;
}

ObservationPeriod observation_days(Date accrual_start, Date accrual_end, const ObservationSpec& spec,
                                   const Calendar& cal) {
  if (accrual_end <= accrual_start) {
    throw DateError("rfr: accrual end " + to_iso(accrual_end) + " is not after start " + to_iso(accrual_start));
  }
  if (spec.lookback_days < 0 || spec.lockout_days < 0) throw DateError("rfr: negative lookback / lockout");
  if (spec.method == ObservationMethod::Plain && spec.lookback_days != 0) {
    throw DateError("rfr: a plain observation has no lookback days");
  }
  ObservationPeriod p;
  p.accrual_start = accrual_start;
  p.accrual_end = accrual_end;
  const int k = spec.lookback_days;
  if (spec.method == ObservationMethod::ObservationShift && k > 0) {
    p.obs_start = cal.add_business_days(accrual_start, -k);
    p.obs_end = cal.add_business_days(accrual_end, -k);
  } else {
    p.obs_start = accrual_start;
    p.obs_end = accrual_end;
  }
  // Business days of the observation period; a non-business start carries the preceding
  // business day's rate up to the first business day.
  Date d = p.obs_start;
  while (d < p.obs_end) {
    Date next = cal.next_business_day(d + 1);
    if (next > p.obs_end) next = p.obs_end;
    ObservationDay od;
    od.accrual_day = d;
    od.rate_date = cal.is_business_day(d) ? d : cal.previous_business_day(d);
    if (spec.method == ObservationMethod::Lookback && k > 0) od.rate_date = cal.add_business_days(od.rate_date, -k);
    od.weight_days = next - d;
    p.days.push_back(od);
    d = next;
  }
  if (spec.lockout_days > 0) {
    const std::size_t n = p.days.size();
    const std::size_t m = static_cast<std::size_t>(spec.lockout_days);
    if (m >= n) {
      throw DateError("rfr: lockout of " + std::to_string(spec.lockout_days) + " days covers the whole period " +
                      to_iso(accrual_start) + " to " + to_iso(accrual_end));
    }
    const Date frozen = p.days[n - m - 1].rate_date;
    for (std::size_t i = n - m; i < n; ++i) p.days[i].rate_date = frozen;
  }
  return p;
}

RealisedObservations realised_observations(const ObservationPeriod& p, const FixingsHistory& history,
                                           const std::string& index, double basis) {
  RealisedObservations r;
  bool gap = false;
  for (const ObservationDay& od : p.days) {
    auto rate = history.get(index, od.rate_date);
    if (rate && !gap) {
      r.factor *= 1.0 + *rate * static_cast<double>(od.weight_days) / basis;
      r.weighted_sum += *rate * static_cast<double>(od.weight_days);
      ++r.fixed_count;
      r.fixed_weight += od.weight_days;
    } else {
      if (rate && gap) {
        throw DateError("rfr: " + index + " fixing for " + to_iso(od.rate_date) + " follows a gap in the history");
      }
      gap = true;
      r.unfixed.push_back(od);
    }
  }
  return r;
}

double compounded_rate(const ObservationPeriod& p, const FixingsHistory& history, const std::string& index,
                       double basis) {
  RealisedObservations r = realised_observations(p, history, index, basis);
  if (!r.complete()) {
    throw DateError("rfr: " + index + " has no fixing for " + to_iso(r.unfixed.front().rate_date));
  }
  return (r.factor - 1.0) * basis / static_cast<double>(p.weight_total());
}

double average_rate(const ObservationPeriod& p, const FixingsHistory& history, const std::string& index) {
  RealisedObservations r = realised_observations(p, history, index, 360.0);
  if (!r.complete()) {
    throw DateError("rfr: " + index + " has no fixing for " + to_iso(r.unfixed.front().rate_date));
  }
  return r.weighted_sum / static_cast<double>(p.weight_total());
}

}  // namespace epykos::conventions
