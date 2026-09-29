# tools/compare/ — the MX head-to-head driver (docs/DECISIONS.md D21, D71; bench/compare/README.md).
#
#   compare_ois   builds the fixtures::compare_ois problem (one USD SOFR OIS curve, a square
#                 par-rate calibration, an OIS book), records it as one tape, calibrates through
#                 the implicit node, prices the book and takes the O3 ladder through the IFT;
#                 writes the problem in the neutral exchange form and this engine's answers to it.
#
# It links nothing of any other engine and reads no other checkout (D11). Like everything under
# tools/, a measurement tool: never a gate, never run by ctest or CI.
add_executable(compare_ois compare_main.cpp)
target_link_libraries(compare_ois PRIVATE epykos epykos_warnings)
