# bench/ — every bench/**/*_bench.cpp is its own Google Benchmark executable.
#
# Conventions (owned by M1/P0; packages only add files):
#   * Drop a file named <name>_bench.cpp anywhere under bench/. It becomes target
#     "<subdir>_<name>_bench" (path separators become '_'), linked against epykos and
#     benchmark_main, and may #include helper headers from bench/ and tests/.
#   * Benchmarks are never run by ctest or CI. Run them by hand and write results as JSON to
#     bench/results/<fingerprint>/ (see bench/results/README.md); results are committed.
#   * The build-all target builds them; pass -DEPYKOS_BUILD_BENCH=OFF to skip.

file(GLOB_RECURSE EPYKOS_BENCH_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*_bench.cpp")
list(FILTER EPYKOS_BENCH_SOURCES EXCLUDE REGEX "/\\.[^/]*$")  # no dot-prefixed files

foreach(_src IN LISTS EPYKOS_BENCH_SOURCES)
  file(RELATIVE_PATH _rel "${CMAKE_CURRENT_SOURCE_DIR}" "${_src}")
  string(REGEX REPLACE "\\.cpp$" "" _name "${_rel}")
  string(REPLACE "/" "_" _name "${_name}")
  if(TARGET "${_name}")
    message(FATAL_ERROR "bench: two files map to the same target name '${_name}' (${_rel})")
  endif()

  add_executable("${_name}" "${_src}")
  target_link_libraries("${_name}" PRIVATE epykos epykos_warnings benchmark::benchmark_main)
  target_include_directories("${_name}" PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_SOURCE_DIR}/tests")
endforeach()

list(LENGTH EPYKOS_BENCH_SOURCES _n)
message(STATUS "EpykosEngine: ${_n} benchmark executable(s)")
