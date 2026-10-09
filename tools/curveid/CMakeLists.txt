# tools/curveid/ — the curve-identity probe (bench/compare/CURVEID.md).
#
# OPT-IN AND OFF BY DEFAULT, behind the same -DEPYKOS_H2H=ON as tools/h2h/, and never in CI. D107
# (2026-10-09) lifted D11's prohibition on READING that checkout; the prohibition on copying,
# porting or adapting anything of it into include/epykos/ or src/ is untouched, and this target
# changes nothing there. It calls the other engine's public curve and nothing else.
#
#   cmake --preset release -DEPYKOS_H2H=ON -DSWAPENGINE_ROOT=/path/to/that/checkout
#   cmake --build --preset release --target curveid
#   ./build/release/tools/curveid/curveid
#
# UNLIKE tools/h2h/ this needs only their HEADERS: swaps/curve/curve_module.hpp and
# swaps/curve/regions.hpp are header-only templates, so SWAPENGINE_ROOT does NOT have to be a
# configured build and no library of theirs is linked. Eigen is needed because curve_module.hpp
# includes <Eigen/Core>; their pinned copy is preferred so their header compiles exactly as it does
# in their tree, with our own pinned Eigen 3.4.0 as the fallback (both are 3.4.0).
if(NOT EPYKOS_H2H)
  return()
endif()
if(NOT SWAPENGINE_ROOT)
  message(FATAL_ERROR "tools/curveid: -DEPYKOS_H2H=ON needs -DSWAPENGINE_ROOT=<that checkout>")
endif()

set(_se_inc "${SWAPENGINE_ROOT}/include")
if(NOT EXISTS "${_se_inc}/swaps/curve/curve_module.hpp")
  message(FATAL_ERROR "tools/curveid: ${_se_inc}/swaps/curve/curve_module.hpp not found — is SWAPENGINE_ROOT that checkout's root?")
endif()
set(_se_eigen "${SWAPENGINE_ROOT}/third_party/eigen")
if(NOT EXISTS "${_se_eigen}/Eigen/Core")
  set(_se_eigen "${CMAKE_SOURCE_DIR}/third_party/eigen")
endif()

add_executable(curveid curveid_main.cpp)
# SYSTEM, for the reason tools/h2h/ gives: this repository's -Werror warning set is not applied to
# another project's headers, and loosening OUR flags to silence one would change the code measured.
target_include_directories(curveid SYSTEM PRIVATE "${_se_inc}" "${_se_eigen}")
target_compile_definitions(curveid PRIVATE EIGEN_MPL2_ONLY)
target_link_libraries(curveid PRIVATE epykos)
message(STATUS "EpykosEngine: curveid ON against ${SWAPENGINE_ROOT} (D107)")
