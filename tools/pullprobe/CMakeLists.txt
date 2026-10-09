# tools/pullprobe/ — what `adjoint::Options::seed_pull` is worth, paired, in one process.
#
#   pullprobe [--fixture all|stage_a|compare_ois] [--batch N ...] [--trades N]
#             [--lane-tile N] [--tile N] [--reps N] [--ignore-load]
#
# D102 §4 item 1 priced `pull()`'s zero start at 26% of Stage A's reverse-only arithmetic and ended
# by saying only a paired before/after settles it; D103 §3 showed why an op count does not become a
# time. This is that before/after. Both arms are one option on one `ir::Program` in ONE binary,
# interleaved call by call — never two builds, whose inlining differs in ways that have already
# produced a −6.6% that was not there. The source header states the method, the A,B,B,A bounce and
# why `A − F0` is a BOUND on the reverse pass rather than an estimate.
#
# Times, so like tools/ladder/ it reads the 1-minute load itself and refuses above cores/2
# (bench/run.sh's rule, D29). Built whenever EPYKOS_BUILD_TOOLS is; never run by ctest or CI.
#
# No -ffp-contract=off here, deliberately: every number is a time under the shipped release flags.
# It compares no bits — that is tests/adjoint/seed_pull_e0_test.cpp's job, and that file IS
# `_e0_test`.
add_executable(pullprobe pullprobe_main.cpp)
target_link_libraries(pullprobe PRIVATE epykos epykos_warnings)
