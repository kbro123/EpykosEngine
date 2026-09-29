// EpykosEngine — the synthetic calendar of docs/WORKLOADS.md ("Common conventions").
//
//   * Day 0 is the valuation date; there are no holidays; days are plain integers.
//   * An annual schedule starting on day d0 has period ends d_j = d0 + round(365.25·j), j = 1..T.
//   * Accrual is ACT/360: τ = (d_end − d_start) / 360.
//   * Discount time is t = d / 365.
//
// Everything here is structure (integers and doubles that never depend on a Scalar), so the
// pricing templates take these values as plain doubles.
#pragma once

namespace epykos::calendar {

inline constexpr int valuation_day = 0;

inline constexpr double accrual_basis = 360.0;   // ACT/360
inline constexpr double discount_basis = 365.0;  // t = d / 365
inline constexpr double schedule_year = 365.25;  // d_j = d0 + round(365.25·j)

// round(365.25·j) for j >= 0, computed exactly in integers: 365.25·j = (1461·j)/4, and rounding
// half away from zero is floor((1461·j + 2) / 4). Identical to std::lround(365.25 * j).
constexpr int schedule_offset(int j) noexcept { return (1461 * j + 2) / 4; }

// d_j = d0 + round(365.25·j). j = 0 gives d0 itself.
constexpr int schedule_day(int d0, int j) noexcept { return d0 + schedule_offset(j); }

// τ = (d_end − d_start) / 360.
constexpr double year_fraction(int d_start, int d_end) noexcept {
  return static_cast<double>(d_end - d_start) / 360.0;
}

// t = d / 365.
constexpr double time_of_day(int d) noexcept { return static_cast<double>(d) / 365.0; }

// Fills days[0..periods] with d0, d_1, ..., d_periods.
inline void annual_schedule(int d0, int periods, int* days) noexcept {
  for (int j = 0; j <= periods; ++j) days[j] = schedule_day(d0, j);
}

}  // namespace epykos::calendar
