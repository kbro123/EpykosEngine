# M4/CM cost model validation

Fingerprint `d448afd70180`; 666 measured (case, config, domain) points from 21 capture(s); 42 of 188 coefficients fitted (the rest kept at CostCoefficients::defaults(), unsupported by either workload).

**Mean absolute relative error, every measured domain: 74.4688%** (target < 25%, docs/PROBLEM.md §7 / M4/CM's own gate).

**Mean absolute relative error, domains >= 1% of their config's time (124 of 666 points — the ones an optimisation decision actually turns on): 64.624%.**

Worst domain (every measured domain, including negligible ones): stage_a B=64 tile=256 lane_tile=32 fuse_pairs=1 domain 64 (measured 1.26 us, predicted 11.6664 us, 0.00259415% of that config's time), relative error 825.907%.

Same fitted coefficients, scored over the `fuse_pairs=1` captures ONLY (D48's original grid, for a
like-for-like comparison with the number that entry reported): **75.3446%** over 492 points; **65.0091%** over the 88 of them that are >= 1% of their config's time.

Per case, every domain:

  * `m1`: mean 71.9305% over 130 points
  * `stage_a`: mean 75.0845% over 536 points

Per case, domains >= 1% of their config's time:

  * `m1`: mean 62.4098% over 65 points
  * `stage_a`: mean 67.0634% over 59 points

## Step-pairing contrast (D63)

Measured and predicted whole-config time with `exec::Options::fuse_pairs` off vs on, for every
configuration the grid captured BOTH ways. `1.000` predicted is what a model that prices step
pairing at zero reports whatever the measurement says -- the defect D63 fixed. The gap between the
two ratio columns is the part of the interpreter's own core fusion mechanism the model still does
not see.

| config | measured off/on | predicted off/on | model sees |
|---|---|---|---|
| `m1 B=1 tile=128 lane_tile=8` | 1.02925x | 1.01735x | 59.3266% |
| `m1 B=1 tile=256 lane_tile=8` | 1.01676x | 1.00957x | 57.1044% |
| `m1 B=1 tile=512 lane_tile=8` | 1.05964x | 1.00538x | 9.01292% |
| `m1 B=64 tile=256 lane_tile=8` | 1.04638x | 1.00181x | 3.90222% |
| `stage_a B=1 tile=256 lane_tile=8` | 0.993026x | 1.00603x | 0% |
| `stage_a B=64 tile=256 lane_tile=8` | 0.99811x | 1.00113x | 0% |


What this cannot capture (include/epykos/optimise/cost.hpp's header comment): libm `std::exp` vs `exp_poly` share one Exp coefficient (fitted for whichever ExpMode the capture used); cache effects across domains live at once, not per domain; a scan group's fixed-arity three-operand Sum is one kernel call here and two in the interpreter (D63 priced fused pairs / chain tails, and deliberately left this one); an inlined domain's exact re-fetch count is approximated from IR structure (DomainFacts::gather_refs), though the materialisation decision and `kept_rows` are no longer approximated at all -- D63's infer_plan calls rewrite::planner::default_plan; a domain measuring a few tens of nanoseconds is measuring the profiler's own std::chrono calls as much as its own work.
