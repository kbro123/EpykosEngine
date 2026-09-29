# tools/costmodel/ — M4/CM's cost-model calibration tool (docs/PROBLEM.md §7).
#
#   costmodel_collect   builds ONE (case, B, tile, lane_tile) interpreter and runs it `--reps`
#                        times; exec::Interpreter's global profile table (compiled in only under
#                        the `profile` preset, EPYKOS_EXEC_PROFILE=ON) prints per-domain
#                        microseconds to stderr at process exit. One process per configuration —
#                        the profile table is a single process-lifetime global (D45's "each
#                        (B, lane_tile) its own filtered process", now a reusable tool).
#   costmodel_fit        reads the raw stderr of one or more costmodel_collect runs (a config
#                        file naming each one's case/B/tile/lane_tile), rebuilds the same
#                        programs (an ordinary, non-profiled link: deterministic from the seed,
#                        D16/D44), fits CostCoefficients by linear least squares (Eigen, D12) and
#                        writes bench/results/<fingerprint>/cost_model.json, plus a validation
#                        report of predicted-vs-measured per-domain time.
#
# tools/costmodel/calibrate.py drives both across a grid and computes the fingerprint (D29:
# Python for scripts). Built whenever EPYKOS_BUILD_TOOLS is (default ON); like bench/, never run
# by ctest or CI.
add_executable(costmodel_collect collect_main.cpp)
target_link_libraries(costmodel_collect PRIVATE epykos epykos_warnings)

add_executable(costmodel_fit fit_main.cpp)
target_link_libraries(costmodel_fit PRIVATE epykos epykos_warnings Eigen3::Eigen)
