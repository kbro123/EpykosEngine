# tools/egraph_scale/ — the e-graph saturation scaling harness (docs/DECISIONS.md D62).
#
#   egraph_scale   saturates a Stage A recording of `--trades` trades with the full rule set
#                  (R1-R7 + fma + the five planner rules), ONE ROUND AT A TIME, and prints per
#                  round: program nodes, plan nodes, wall-clock milliseconds, peak resident set
#                  and the per-rule (sites matched, nodes added, proposals skipped by the
#                  application memo) breakdown. `--extract` additionally runs extraction under
#                  this fingerprint's REAL fitted cost model (bench/results/<fp>/cost_model.json,
#                  D48) and prints the winning history and its ratio against the M1-shaped
#                  default plan.
#
# This is a measurement tool, not a gate and not a Google Benchmark target: it reports the growth
# numbers D54 / D62 quote, in a form a human can re-run. Like bench/, never run by ctest or CI.
add_executable(egraph_scale scale_main.cpp)
target_link_libraries(egraph_scale PRIVATE epykos epykos_warnings)
