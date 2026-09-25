# tools/ — build-time tooling that is not engine API and not a gtest / Google Benchmark target:
# each subdirectory owns its own CMakeLists.txt. tools/costmodel/ is M4/CM's calibration tool;
# tools/catalogue/ is M4/C1's catalogue generator; tools/egraph_scale/ is the e-graph saturation
# scaling harness D62's numbers come from; tools/compare/ is the MX head-to-head driver (D21, D71).
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/costmodel/CMakeLists.txt")
  add_subdirectory(costmodel)
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/catalogue/CMakeLists.txt")
  add_subdirectory(catalogue)
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/egraph_scale/CMakeLists.txt")
  add_subdirectory(egraph_scale)
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/compare/CMakeLists.txt")
  add_subdirectory(compare)
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/egraph_eval/CMakeLists.txt")
  add_subdirectory(egraph_eval)
endif()
