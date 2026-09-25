# tests/ — every tests/**/*_test.cpp is its own gtest executable, registered with ctest.
#
# Conventions (owned by M1/P0; packages only add files):
#   * Drop a file named <name>_test.cpp anywhere under tests/. It becomes target and ctest entry
#     "<subdir>_<name>_test" (path separators become '_'; e.g. tests/tape/cse_test.cpp -> tape_cse_test).
#     It links epykos, gtest_main and gmock, and may #include helper headers from tests/.
#   * Files ending in _e0_test.cpp are E0 (bit-identity) gates: the TU is compiled with
#     -ffp-contract=off in EVERY preset and sees EPYKOS_FP_CONTRACT_OFF=1, so the templated maths
#     it instantiates carries no fused multiply-adds. Library sources named src/**/*_e0.cpp are
#     pinned the same way (root CMakeLists.txt, D25); every other TU of libepykos keeps the
#     preset's flags, so an E0 gate that crosses into such code runs under the reference preset.
#     Label "e0": ctest -L e0.

#   * ctest labels: the first subdirectory under tests/ (if any) and "e0".
#   * tests/hand/ tests the hand-fused reference kernel of bench/hand/ (D9, D28): those executables
#     also link the static library epykos_hand (bench/hand/CMakeLists.txt, always configured).
#   * Tests run with the current directory set to their binary dir, so debug dumps land in build/.

file(GLOB_RECURSE EPYKOS_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*_test.cpp")
list(FILTER EPYKOS_TEST_SOURCES EXCLUDE REGEX "/\\.[^/]*$")  # no dot-prefixed files

foreach(_src IN LISTS EPYKOS_TEST_SOURCES)
  file(RELATIVE_PATH _rel "${CMAKE_CURRENT_SOURCE_DIR}" "${_src}")
  string(REGEX REPLACE "\\.cpp$" "" _name "${_rel}")
  string(REPLACE "/" "_" _name "${_name}")
  if(TARGET "${_name}")
    message(FATAL_ERROR "tests: two files map to the same target name '${_name}' (${_rel})")
  endif()

  add_executable("${_name}" "${_src}")
  if(_rel MATCHES "^hand/")
    # The hand-fused reference (bench/hand/), listed before epykos so the static link order is
    # epykos_hand then epykos once (listed after, CMake would repeat libepykos.a and ld warns).
    target_link_libraries("${_name}" PRIVATE epykos_hand)
  endif()
  target_link_libraries("${_name}" PRIVATE epykos epykos_warnings GTest::gtest_main GTest::gmock)
  target_include_directories("${_name}" PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

  set(_labels "")
  string(FIND "${_rel}" "/" _slash)
  if(_slash GREATER -1)
    string(SUBSTRING "${_rel}" 0 ${_slash} _label)
    list(APPEND _labels "${_label}")
  endif()
  if(_rel MATCHES "_e0_test\\.cpp$")
    target_compile_options("${_name}" PRIVATE -ffp-contract=off)
    target_compile_definitions("${_name}" PRIVATE EPYKOS_FP_CONTRACT_OFF=1)
    list(APPEND _labels "e0")
  endif()

  add_test(NAME "${_name}" COMMAND "${_name}" WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
  if(_labels)
    set_tests_properties("${_name}" PROPERTIES LABELS "${_labels}")
  endif()
endforeach()

list(LENGTH EPYKOS_TEST_SOURCES _n)
message(STATUS "EpykosEngine: ${_n} test executable(s)")
