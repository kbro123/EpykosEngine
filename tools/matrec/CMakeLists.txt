# tools/matrec/ — the STRUCTURAL half of materialise-vs-recompute: whether any signal the engine
# already holds (reader count, cone cost, reuse distance, working-set size) separates the values
# that should be stored from the values that should be recomputed, at both ends of the sharing
# spectrum (the rates book and D106's path grid).
#
#   matrec [all|compare_ois(64)|compare_ois(256)|exotic_path(...)|stage_a] [--lane-tile N] [--tile N]
#
# Per fixture: the reader-count distribution of the always-materialised class (domain values) and
# of the always-recomputed class (intra-chain steps, a count the engine does NOT hold); the cone
# cost back to the nearest live values; the reuse distance under the shipped forward-then-reverse
# schedule; the per-domain and per-tile working set, with what a full materialisation would cost;
# and the same reader counts over Q = adjoint(P) split into its forward and reverse halves.
#
# NO TIMING. Every number is a count, a byte size or a ratio, so it is quotable on a loaded box and
# this tool does not read the load or refuse one — the position tools/sparsity/, tools/coverage/
# and tools/revcollapse/ take, and the opposite of tools/ladder/, tools/pullprobe/ and
# tools/exotics/. The timing half, which DOES refuse a loaded box, is tools/matprobe/.
#
# It links `epykos` only and compiles against nothing outside this repository. It reads the
# fixture-only headers under include/epykos/fixtures/ (D28), which is allowed for a tool and not
# for engine API. Built whenever EPYKOS_BUILD_TOOLS is (default ON); like tools/sparsity/ and
# bench/, never run by ctest or CI.
add_executable(matrec matrec_main.cpp)
target_link_libraries(matrec PRIVATE epykos epykos_warnings)
