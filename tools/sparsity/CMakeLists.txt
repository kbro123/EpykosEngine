# tools/sparsity/ — the structural pattern of ∂F/∂z per declared block, and the greedy colour
# count on the real fixtures, measured BEFORE a colouring is built (bench/compare/SPARSITY.md).
#
#   sparsity [all|compare_ois|stage_a|stage_a_joint]
#
# Per block: n_r, n_z, the structural nonzero count and density, the shape verdict, the greedy
# colour count and the lane reduction it implies, the dead rows and columns the structural gate
# reads, and the NUMERICAL nonzero count at the record point through the engine's own batched
# adjoint — which is what says whether the structural superset is tight or merely sound.
#
# NO TIMING. Every number is a count, a density or a colour, so it is quotable on a loaded box and
# this tool does not read the load or refuse a box — the position tools/coverage/ and
# tools/revcollapse/ take, and the opposite of tools/ladder/, tools/pullprobe/ and tools/exotics/.
#
# It links `epykos` only and compiles against nothing outside this repository. It reads the
# fixture-only headers under include/epykos/fixtures/ (D28), which is allowed for a tool and not
# for engine API. Built whenever EPYKOS_BUILD_TOOLS is (default ON); like tools/catalogue/ and
# bench/, never run by ctest or CI.
add_executable(sparsity sparsity_main.cpp)
target_link_libraries(sparsity PRIVATE epykos epykos_warnings)
