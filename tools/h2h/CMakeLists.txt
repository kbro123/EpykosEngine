# tools/h2h/ — the representative head-to-head: both engines in one process (D78).
#
# OPT-IN AND OFF BY DEFAULT. This is the ONE target in the repository that compiles against
# another engine's headers, which D78 permits for this directory alone and for no other purpose.
# Without -DEPYKOS_H2H=ON it is not configured at all, so the repository builds, tests and CIs
# exactly as before on a machine that has no such checkout.
#
#   cmake --preset release -DEPYKOS_H2H=ON -DSWAPENGINE_ROOT=/path/to/that/checkout
#   cmake --build --preset release --target h2h
#
# SWAPENGINE_ROOT must be a CONFIGURED BUILD of it: the harness needs its api static library and
# the simd_config.hpp its build generates. Flag parity is checked below and is the reason the
# comparison is worth running -- both engines are -O3 -march=x86-64-v3 -fno-math-errno under the
# same Apple clang, so neither side is measured under kinder flags than the other.
if(NOT EPYKOS_H2H)
  return()
endif()
if(NOT SWAPENGINE_ROOT)
  message(FATAL_ERROR "tools/h2h: -DEPYKOS_H2H=ON needs -DSWAPENGINE_ROOT=<that checkout>")
endif()

set(_se_inc "${SWAPENGINE_ROOT}/include")
set(_se_gen "${SWAPENGINE_ROOT}/build/generated")
set(_se_eigen "${SWAPENGINE_ROOT}/third_party/eigen")
set(_se_boost "${SWAPENGINE_ROOT}/third_party/boost")
find_library(SWAPENGINE_API_LIB NAMES swaps_api HINTS "${SWAPENGINE_ROOT}/build/api" NO_DEFAULT_PATH)
foreach(_p "${_se_inc}" "${_se_gen}" "${_se_eigen}")
  if(NOT EXISTS "${_p}")
    message(FATAL_ERROR "tools/h2h: ${_p} does not exist — is SWAPENGINE_ROOT configured and built?")
  endif()
endforeach()
if(NOT SWAPENGINE_API_LIB)
  message(FATAL_ERROR "tools/h2h: no libswaps_api under ${SWAPENGINE_ROOT}/build/api — build it first")
endif()

add_executable(h2h h2h_main.cpp h2h_problem.cpp h2h_bridge.cpp)
# Its include directories are SYSTEM so that this repository's own warning set (epykos_warnings,
# -Werror) is not applied to another project's headers: a warning there is not ours to fix, and
# silencing it by loosening OUR flags would change the code under measurement.
target_include_directories(h2h SYSTEM PRIVATE "${_se_inc}" "${_se_gen}" "${_se_eigen}")
if(EXISTS "${_se_boost}")
  target_include_directories(h2h SYSTEM PRIVATE "${_se_boost}")
endif()
target_compile_definitions(h2h PRIVATE EIGEN_MPL2_ONLY)
target_link_libraries(h2h PRIVATE epykos "${SWAPENGINE_API_LIB}")
message(STATUS "EpykosEngine: h2h ON against ${SWAPENGINE_ROOT} (D78)")
