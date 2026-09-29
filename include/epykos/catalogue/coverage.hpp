// EpykosEngine — the catalogue coverage report both engines' `coverage()` return (M4/C1).
#pragma once

#include <cstddef>

namespace epykos::catalogue {

// groups_total / groups_catalogued: over domains this engine instance ATTEMPTED to catalogue
// (each engine has its own additional eligibility on top of is_cataloguable(); a domain outside
// even that — an Input domain, or, for exec::Interpreter only, one already fused/inlined/scan/
// whole-segment — counts in neither number: it was never a candidate, not a candidate the
// catalogue "missed"). rows_total / rows_catalogued: the same, weighted by ir::Domain::rows — a
// coarse, always-available proxy for the time fraction below.
struct Coverage {
  std::size_t groups_total = 0;
  std::size_t groups_catalogued = 0;
  std::size_t rows_total = 0;
  std::size_t rows_catalogued = 0;

  double group_fraction() const noexcept {
    return groups_total == 0 ? 0.0 : static_cast<double>(groups_catalogued) / static_cast<double>(groups_total);
  }
  double row_fraction() const noexcept {
    return rows_total == 0 ? 0.0 : static_cast<double>(rows_catalogued) / static_cast<double>(rows_total);
  }

  // The fraction of this instance's OWN measured wall-clock evaluation time (summed since
  // construction, over every run()/run_chunk() call so far) spent in catalogued-domain
  // execution. Only ever populated in a build compiled with -DEPYKOS_EXEC_PROFILE (the `profile`
  // preset, CLAUDE.md's Build section) AND after at least one run has completed; -1.0 otherwise —
  // callers must treat -1.0 as "not measured", never as "0% covered".
  double time_fraction = -1.0;
};

}  // namespace epykos::catalogue
