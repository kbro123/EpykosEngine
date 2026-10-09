# tools/ — build-time tooling that is not engine API and not a gtest / Google Benchmark target:
# each subdirectory owns its own CMakeLists.txt. tools/costmodel/ is M4/CM's calibration tool;
# tools/catalogue/ is M4/C1's catalogue generator; tools/egraph_scale/ is the e-graph saturation
# scaling harness D62's numbers come from; tools/compare/ is the MX head-to-head driver (D21, D71);
# tools/revcollapse/ measures whether the algebra above the pin collapses invariant I1's emitted
# reverse, and whether the collapsed reverse is still bitwise `adjoint::Adjoint::run` (counts only,
# no timing); tools/ladder/ decomposes the O3 risk ladder's TIME (D90's 531.5 us) and, unlike
# revcollapse, therefore refuses a loaded box itself; tools/pullprobe/ is the paired before/after
# D102 §4 asked for, both arms of `adjoint::Options::seed_pull` interleaved in ONE process, and
# refuses a loaded box for the same reason; tools/exotics/ runs D1's revisit clause against the
# first path-dependent payoff in the tree and, like ladder/ and pullprobe/, refuses a loaded box.
#
# costmodel/, egraph_scale/ and egraph_eval/ measure the quarantined M4 plan-level search and are
# built only with it (EPYKOS_LEGACY_SEARCH, root CMakeLists.txt; PRINCIPLES.md §7.1). This is not a
# glob, so the option is read directly rather than through epykos_drop_legacy_search().
if(EPYKOS_LEGACY_SEARCH AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/costmodel/CMakeLists.txt")
  add_subdirectory(costmodel)
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/catalogue/CMakeLists.txt")
  add_subdirectory(catalogue)
endif()
if(EPYKOS_LEGACY_SEARCH AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/egraph_scale/CMakeLists.txt")
  add_subdirectory(egraph_scale)
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/compare/CMakeLists.txt")
  add_subdirectory(compare)
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/revcollapse/CMakeLists.txt")
  add_subdirectory(revcollapse)
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/ladder/CMakeLists.txt")
  add_subdirectory(ladder)
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/pullprobe/CMakeLists.txt")
  add_subdirectory(pullprobe)
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/exotics/CMakeLists.txt")
  add_subdirectory(exotics)
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/h2h/CMakeLists.txt")
  add_subdirectory(h2h)
endif()
if(EPYKOS_LEGACY_SEARCH AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/egraph_eval/CMakeLists.txt")
  add_subdirectory(egraph_eval)
endif()
