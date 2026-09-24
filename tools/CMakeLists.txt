# tools/ — build-time tooling that is not engine API and not a gtest / Google Benchmark target:
# each subdirectory owns its own CMakeLists.txt. tools/catalogue/ is M3's still-unbuilt generator
# (empty, .gitkeep only); tools/costmodel/ is M4/CM's calibration tool.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/costmodel/CMakeLists.txt")
  add_subdirectory(costmodel)
endif()
