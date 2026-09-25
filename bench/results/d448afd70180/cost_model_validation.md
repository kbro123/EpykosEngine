# M4/CM cost model validation

Fingerprint `d448afd70180`; 666 measured (case, config, domain) points from 21 capture(s); 42 of 188 coefficients fitted (the rest kept at CostCoefficients::defaults(), unsupported by either workload).

**Mean absolute relative error, every measured domain: 58.6434%** (target < 25%, docs/PROBLEM.md §7 / M4/CM's own gate).

**Mean absolute relative error, domains >= 1% of their config's time (124 of 666 points — the ones an optimisation decision actually turns on): 49.6842%.**

Worst domain (every measured domain, including negligible ones): stage_a B=1 tile=256 lane_tile=8 fuse_pairs=0 domain 15 (measured 4.47 us, predicted 13.663 us, 0.210746% of that config's time), relative error 205.66%.

Same fitted coefficients, scored over the `fuse_pairs=1` captures ONLY (D48's original grid, for a
like-for-like comparison with the number that entry reported): **58.7343%** over 492 points; **48.9953%** over the 88 of them that are >= 1% of their config's time.

Per case, every domain:

  * `m1`: mean 52.3601% over 130 points
  * `stage_a`: mean 60.1673% over 536 points

Per case, domains >= 1% of their config's time:

  * `m1`: mean 44.2988% over 65 points
  * `stage_a`: mean 55.6172% over 59 points

## Step-pairing contrast (D63)

Measured and predicted whole-config time with `exec::Options::fuse_pairs` off vs on, for every
configuration the grid captured BOTH ways. `1.000` predicted is what a model that prices step
pairing at zero reports whatever the measurement says -- the defect D63 fixed. The gap between the
two ratio columns is the part of the interpreter's own core fusion mechanism the model still does
not see.

| config | measured off/on | predicted off/on | model sees |
|---|---|---|---|
| `m1 B=1 tile=128 lane_tile=8` | 1.01957x | 1.05389x | 275.341% |
| `m1 B=1 tile=256 lane_tile=8` | 1.02692x | 1.02975x | 110.479% |
| `m1 B=1 tile=512 lane_tile=8` | 1.01996x | 1.01671x | 83.7151% |
| `m1 B=64 tile=256 lane_tile=8` | 1.04842x | 1.00431x | 8.89116% |
| `stage_a B=1 tile=256 lane_tile=8` | 0.962809x | 1.01168x | 0% |
| `stage_a B=64 tile=256 lane_tile=8` | 1.02869x | 1.00247x | 8.62702% |


What this cannot capture (include/epykos/optimise/cost.hpp's header comment): libm `std::exp` vs `exp_poly` share one Exp coefficient (fitted for whichever ExpMode the capture used); cache effects across domains live at once, not per domain; a scan group's fixed-arity three-operand Sum is one kernel call here and two in the interpreter (D63 priced fused pairs / chain tails, and deliberately left this one); an inlined domain's exact re-fetch count is approximated from IR structure (DomainFacts::gather_refs), though the materialisation decision and `kept_rows` are no longer approximated at all -- D63's infer_plan calls rewrite::planner::default_plan; a domain measuring a few tens of nanoseconds is measuring the profiler's own std::chrono calls as much as its own work.
