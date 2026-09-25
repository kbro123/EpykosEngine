# bench/hand — the hand-fused reference kernel (M1/P5; D9): a benchmark reference, not engine API.
#
# Built as the static library `epykos_hand` whenever tests or benches are built, and linked by
# tests/hand/*_test.cpp (tests/CMakeLists.txt) and bench/hand/*_bench.cpp (bench/CMakeLists.txt).
# Its headers are included as "hand/<name>.hpp" (bench/ is the include root). Sources named
# *_e0.cpp are pinned to -ffp-contract=off with EPYKOS_FP_CONTRACT_OFF=1 in every preset, the same
# convention as src/**/*_e0.cpp (root CMakeLists.txt, D25), so the kernel's bits never depend on
# the preset or on the compiler's contraction default.

file(GLOB EPYKOS_HAND_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
file(GLOB EPYKOS_HAND_HEADERS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp")
list(FILTER EPYKOS_HAND_SOURCES EXCLUDE REGEX "_bench\\.cpp$")  # the benchmarks are bench/'s
list(FILTER EPYKOS_HAND_SOURCES EXCLUDE REGEX "/\\.[^/]*$")     # no dot-prefixed files
list(FILTER EPYKOS_HAND_HEADERS EXCLUDE REGEX "/\\.[^/]*$")

add_library(epykos_hand STATIC ${EPYKOS_HAND_SOURCES} ${EPYKOS_HAND_HEADERS})
target_include_directories(epykos_hand PUBLIC "${CMAKE_SOURCE_DIR}/bench")
target_link_libraries(epykos_hand
  PUBLIC  epykos
  PRIVATE epykos_warnings)

set(EPYKOS_HAND_E0_SOURCES ${EPYKOS_HAND_SOURCES})
list(FILTER EPYKOS_HAND_E0_SOURCES INCLUDE REGEX "_e0\\.cpp$")
set_source_files_properties(${EPYKOS_HAND_E0_SOURCES} PROPERTIES
  COMPILE_OPTIONS "-ffp-contract=off"
  COMPILE_DEFINITIONS "EPYKOS_FP_CONTRACT_OFF=1")
list(LENGTH EPYKOS_HAND_E0_SOURCES _n_e0)
message(STATUS "EpykosEngine: bench/hand library epykos_hand, ${_n_e0} E0 source(s) pinned to -ffp-contract=off")
