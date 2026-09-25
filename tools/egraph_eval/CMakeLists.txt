# tools/egraph_eval/ — the "write or wrap an e-graph" evaluation's own measurements (D75).
#
#   egraph_eval_regions   measures the maximal pure-field regions of the `compare_ois` fixture,
#                         at the TAPE level and at the IR level, and can dump one region as JSON
#                         for the out-of-tree scratch harness that drives a candidate library.
#
# This tool links only the engine. The candidate libraries themselves are NOT vendored into this
# repository and NOT in this build: D75 is an evaluation, and the scratch harness that actually
# runs egraphs.cpp lives outside the tree. Like everything under tools/, a measurement tool: not
# engine API, not a gate, never run by ctest or CI.
add_executable(egraph_eval_regions region_main.cpp)
target_link_libraries(egraph_eval_regions PRIVATE epykos epykos_warnings)
