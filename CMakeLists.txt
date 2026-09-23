# EpykosEngine — root build (owned by M1/P0).
#
# Layout (docs/RESUME.md §2):
#   include/epykos/**   public headers, namespace epykos, macros EPY_* (include/epykos/fixtures/ is
#                       test-only: seeded books, oracles, record helpers; not engine API, D28)
#   src/**              non-template implementation, compiled into the static library `epykos`
#                       (src/fixtures/ = the seeded book generator, test-only)
#   src/catalogue/generated/   committed generated kernels (M3); globbed like any other source
#   tests/**/*_test.cpp one gtest executable each (tests/CMakeLists.txt)
#   bench/**/*_bench.cpp one Google Benchmark executable each (bench/CMakeLists.txt)
#   bench/hand/         the hand-fused reference kernel, static library `epykos_hand` (D9, D28):
#                       built whenever tests or benches are, linked by tests/hand and bench/hand
#   third_party/        vendored by scripts/bootstrap.sh (gitignored)
#
# Every source list is a file(GLOB ... CONFIGURE_DEPENDS): adding a header, a source, a test
# or a benchmark never requires editing a CMake file. Re-running the build re-globs.
#
# Configure only through a preset (CMakePresets.json): release | reference | debug.

cmake_minimum_required(VERSION 3.25)

project(EpykosEngine
  VERSION 0.1.0
  DESCRIPTION "Generic, fast financial computation from a recorded graph"
  LANGUAGES CXX)

if(NOT CMAKE_BUILD_TYPE)
  message(FATAL_ERROR "Configure through a preset: cmake --preset release | reference | debug")
endif()
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "^(Clang|AppleClang|GNU)$")
  message(FATAL_ERROR "Unsupported compiler '${CMAKE_CXX_COMPILER_ID}' (D12: Apple clang or GCC 13+)")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(EPYKOS_REFERENCE   "Reference build: every TU gets -ffp-contract=off (E0 gates, D8/D13)" OFF)
option(EPYKOS_BUILD_TESTS "Build the gtest executables under tests/"                          ON)
option(EPYKOS_BUILD_BENCH "Build the Google Benchmark executables under bench/"               ON)
option(EPYKOS_WERROR      "Treat warnings as errors in epykos targets (not third_party)"       OFF)
option(EPYKOS_MUTATIONS   "Compile the registered mutants (include/epykos/mutation) into libepykos; EPYKOS_MUTANT=<name> selects one at run time. The mutation preset only, never a gated build (D32)" OFF)

# ---------------------------------------------------------------------------------------------
# Flags (D13).  Gated build: -O3 -march=x86-64-v3 -fno-math-errno, never -ffast-math.
# The ISA flag only exists on x86-64; on other hosts (the arm64 macOS CI runner) it is dropped
# and the fingerprint records the flags actually used.  Override with -DEPYKOS_ISA_FLAGS=...
# (e.g. -march=native for an informational row under its own fingerprint, D9).
# ---------------------------------------------------------------------------------------------
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64|x64)$")
  set(_epykos_isa_default "-march=x86-64-v3")
else()
  set(_epykos_isa_default "")
endif()
set(EPYKOS_ISA_FLAGS "${_epykos_isa_default}" CACHE STRING
    "ISA flag for optimised builds (D13: -march=x86-64-v3 on x86-64; empty elsewhere)")

set(_epykos_release_list -O3 ${EPYKOS_ISA_FLAGS} -fno-math-errno)
list(JOIN _epykos_release_list " " EPYKOS_RELEASE_FLAGS)

# Replace CMake's per-config defaults so the gated flags are exactly D13's (plus NDEBUG).
set(CMAKE_CXX_FLAGS_RELEASE "${EPYKOS_RELEASE_FLAGS} -DNDEBUG")
set(CMAKE_CXX_FLAGS_DEBUG   "-O0 -g")

if(CMAKE_BUILD_TYPE STREQUAL "Release")
  set(EPYKOS_CXX_FLAGS "${EPYKOS_RELEASE_FLAGS}")
elseif(CMAKE_BUILD_TYPE STREQUAL "Debug")
  set(EPYKOS_CXX_FLAGS "${CMAKE_CXX_FLAGS_DEBUG}")
else()
  message(FATAL_ERROR "Unknown CMAKE_BUILD_TYPE '${CMAKE_BUILD_TYPE}': use a preset")
endif()

if(EPYKOS_REFERENCE)
  # Applies to every target configured below, third_party included.
  add_compile_options(-ffp-contract=off)
  add_compile_definitions(EPYKOS_FP_CONTRACT_OFF=1)
  string(APPEND EPYKOS_CXX_FLAGS " -ffp-contract=off")
endif()

# The effective flags, one line, read by scripts/fingerprint.sh and baked into epykos::build_flags().
file(WRITE "${CMAKE_BINARY_DIR}/epykos_flags.txt" "${EPYKOS_CXX_FLAGS}\n")
message(STATUS "EpykosEngine ${PROJECT_VERSION}: ${CMAKE_BUILD_TYPE} build, flags: ${EPYKOS_CXX_FLAGS}")

# Warnings for our own targets only (third_party is added with SYSTEM).
add_library(epykos_warnings INTERFACE)
target_compile_options(epykos_warnings INTERFACE
  -Wall -Wextra
  $<$<BOOL:${EPYKOS_WERROR}>:-Werror>)

# ---------------------------------------------------------------------------------------------
# Third party (D12): vendored under third_party/ by scripts/bootstrap.sh.
# ---------------------------------------------------------------------------------------------
set(EPYKOS_THIRD_PARTY_DIR "${CMAKE_SOURCE_DIR}/third_party")
foreach(_dep IN ITEMS eigen googletest benchmark)
  if(NOT EXISTS "${EPYKOS_THIRD_PARTY_DIR}/${_dep}/.epykos-stamp")
    message(FATAL_ERROR "third_party/${_dep} is missing or stale: run scripts/bootstrap.sh")
  endif()
endforeach()

find_package(Threads REQUIRED)

# Eigen 3.4.0: include path only, double side only (D14); unused until M4.
# Link Eigen3::Eigen to a target that needs it. Its CMake project is deliberately not added.
add_library(epykos_eigen INTERFACE)
add_library(Eigen3::Eigen ALIAS epykos_eigen)
target_include_directories(epykos_eigen SYSTEM INTERFACE "${EPYKOS_THIRD_PARTY_DIR}/eigen")

if(EPYKOS_BUILD_TESTS)
  set(BUILD_GMOCK ON CACHE BOOL "" FORCE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  set(gtest_build_tests OFF CACHE BOOL "" FORCE)
  set(gtest_build_samples OFF CACHE BOOL "" FORCE)
  add_subdirectory("${EPYKOS_THIRD_PARTY_DIR}/googletest" EXCLUDE_FROM_ALL SYSTEM)
endif()

if(EPYKOS_BUILD_BENCH)
  set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_USE_BUNDLED_GTEST OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_INSTALL_DOCS OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_WERROR OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_DOXYGEN OFF CACHE BOOL "" FORCE)
  add_subdirectory("${EPYKOS_THIRD_PARTY_DIR}/benchmark" EXCLUDE_FROM_ALL SYSTEM)
endif()

# ---------------------------------------------------------------------------------------------
# The engine: static library `epykos` (alias epykos::epykos).
# ---------------------------------------------------------------------------------------------
file(GLOB_RECURSE EPYKOS_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/*.cpp")
file(GLOB_RECURSE EPYKOS_HEADERS CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/include/epykos/*.hpp")
# Ignore dot-prefixed files (editor backups, macOS "._" AppleDouble sidecars on copied trees).
list(FILTER EPYKOS_SOURCES EXCLUDE REGEX "/\\.[^/]*$")
list(FILTER EPYKOS_HEADERS EXCLUDE REGEX "/\\.[^/]*$")

add_library(epykos STATIC ${EPYKOS_SOURCES} ${EPYKOS_HEADERS})
add_library(epykos::epykos ALIAS epykos)

# E0 sources (D13, D25): every src/**/*_e0.cpp is compiled with -ffp-contract=off in EVERY preset,
# on both compilers, and sees EPYKOS_FP_CONTRACT_OFF=1 (the TU #errors without it). Their bits
# therefore never depend on the preset or on the compiler's contraction default: GCC contracts
# multiply-adds in C++ even with -std=c++20 (its ISO-mode -ffp-contract=off default is for C
# only), clang within a statement. They are the fixture generators (book, RNG draws) and the
# interpreter kernels; bench/hand/CMakeLists.txt pins the hand-fused reference kernel the same way.
# Same convention as tests/**/*_e0_test.cpp.
set(EPYKOS_E0_SOURCES ${EPYKOS_SOURCES})
list(FILTER EPYKOS_E0_SOURCES INCLUDE REGEX "_e0\\.cpp$")
set_source_files_properties(${EPYKOS_E0_SOURCES} PROPERTIES
  COMPILE_OPTIONS "-ffp-contract=off"
  COMPILE_DEFINITIONS "EPYKOS_FP_CONTRACT_OFF=1")
list(LENGTH EPYKOS_E0_SOURCES _n_e0)
message(STATUS "EpykosEngine: ${_n_e0} E0 source(s) pinned to -ffp-contract=off")

target_include_directories(epykos PUBLIC "${CMAKE_SOURCE_DIR}/include")
target_compile_features(epykos PUBLIC cxx_std_20)
target_link_libraries(epykos
  PUBLIC  Threads::Threads
  PRIVATE epykos_warnings)
# Mutation testing (D32): with EPYKOS_MUTATIONS=ON the deliberate defects guarded by
# epykos::mutant("<pass>.<defect>") are compiled in and EPYKOS_MUTANT selects one per process;
# scripts/mutation_test.sh asserts that the gate tests catch every one. PUBLIC so that every TU
# of the build (tests included) sees the same selector. OFF: epykos::mutant() is constexpr false.
if(EPYKOS_MUTATIONS)
  target_compile_definitions(epykos PUBLIC EPYKOS_MUTATIONS=1)
  message(STATUS "EpykosEngine: mutants compiled in (EPYKOS_MUTATIONS=ON); EPYKOS_MUTANT=<name> selects one")
endif()
target_compile_definitions(epykos PRIVATE
  EPYKOS_BUILD_CONFIG="${CMAKE_BUILD_TYPE}"
  EPYKOS_BUILD_FLAGS="${EPYKOS_CXX_FLAGS}")

if(EPYKOS_BUILD_TESTS)
  enable_testing()
endif()
# bench/ first: it always defines the hand-fused reference library epykos_hand (bench/hand), which
# tests/hand links; the benchmark executables themselves are only added with EPYKOS_BUILD_BENCH.
if(EPYKOS_BUILD_TESTS OR EPYKOS_BUILD_BENCH)
  add_subdirectory(bench)
endif()
if(EPYKOS_BUILD_TESTS)
  add_subdirectory(tests)
endif()
