# M4/CM cost model validation

Fingerprint `d448afd70180`; 492 measured (case, config, domain) points from 15 capture(s); 41 of 187 coefficients fitted (the rest kept at CostCoefficients::defaults(), unsupported by either workload).

**Mean absolute relative error, every measured domain: 84.4241%** (target < 25%, docs/PROBLEM.md §7 / M4/CM's own gate).

**Mean absolute relative error, domains >= 1% of their config's time (88 of 492 points — the ones an optimisation decision actually turns on): 69.2692%.**

Worst domain (every measured domain, including negligible ones): stage_a B=64 tile=256 lane_tile=8 domain 11 (measured 6.77 us, predicted 38.409 us, 0.013612% of that config's time), relative error 467.341%.

Per case, every domain:

  * `m1`: mean 74.3842% over 90 points
  * `stage_a`: mean 86.6718% over 402 points

Per case, domains >= 1% of their config's time:

  * `m1`: mean 57.5033% over 44 points
  * `stage_a`: mean 81.035% over 44 points

What this cannot capture (include/epykos/optimise/cost.hpp's header comment): libm `std::exp` vs `exp_poly` share one Exp coefficient (fitted for whichever ExpMode the capture used); cache effects across domains live at once, not per domain; fused pairs / chain tails are priced as two ops and two dispatches; a fused-into-reduction domain's `kept_rows` and an inlined domain's exact re-fetch count are approximated from IR structure (infer_plan), not from a live interpreter plan; a domain measuring a few tens of nanoseconds is measuring the profiler's own std::chrono calls as much as its own work.
