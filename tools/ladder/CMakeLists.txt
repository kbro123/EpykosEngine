# tools/ladder/ — the decomposition of the O3 risk ladder: where D90's 531.5 us go.
#
#   ladderprobe [--mode all|shape|phases|sweep|batch|evict|unit|toggle] [--trades N]
#               [--tenors a,b,c] [--rows 1,8,...] [--batch N] [--reps N] [--evict-mb 0,1,4,...]
#
# Times only PUBLIC API calls on the same `solver::ImplicitProgram` the ladder runs on, so what it
# measures is the shipped path; nothing under include/epykos/ or src/ is touched. The source file's
# header states the subtraction ladder, why `A - F` is an UPPER BOUND on the reverse pass rather
# than an estimate, and why the exit-Jacobian toggle is an attribution device and never a proposal.
#
# UNLIKE tools/revcollapse/ this tool DOES time, so its output is reportable only on a reserved
# box: it reads the 1-minute load itself and refuses above cores/2, which is bench/run.sh's rule
# (D29, docs/WORKLOADS.md M1 Measurement). Built whenever EPYKOS_BUILD_TOOLS is (default ON); like
# tools/catalogue/ and bench/, never run by ctest or CI.
#
# No -ffp-contract=off here, deliberately and unlike tools/revcollapse/: every number this tool
# produces is a TIME under the shipped release flags, and pinning the contraction in this TU would
# measure a build nobody ships. It compares no bits.
add_executable(ladderprobe ladder_main.cpp)
target_link_libraries(ladderprobe PRIVATE epykos epykos_warnings)
