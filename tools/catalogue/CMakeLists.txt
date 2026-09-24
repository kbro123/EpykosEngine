# tools/catalogue/ — M4/C1's catalogue generator (docs/PROBLEM.md §7).
#
#   catalogue_generate <output-dir> [--small]   runs the reference workloads (the Stage A tape,
#                        the M1 book) through record -> ir::infer, collects the canonical
#                        Signature of every catalogue-eligible domain either produces
#                        (include/epykos/catalogue/signature.hpp) and writes
#                        <output-dir>/{kernels_e0.cpp,registry.cpp} — normally
#                        src/catalogue/generated/, via scripts/catalogue_regen.sh.
#
# Built whenever EPYKOS_BUILD_TOOLS is (default ON); like bench/ and tools/costmodel/, never run
# by ctest or CI directly — scripts/catalogue_regen.sh runs it and diffs the result.
add_executable(catalogue_generate generate_main.cpp codegen.cpp)
target_link_libraries(catalogue_generate PRIVATE epykos epykos_warnings)
