# tools/exotics/ — THE EXOTICS KILL TEST: D1's revisit clause run against a path-dependent payoff.
#
#   exoticsprobe [--mode all|shape|time|oracle|coverage] [--paths 64,256,1024] [--steps 12,52,252]
#                [--reps N] [--batch N] [--ignore-load]
#
# Every fixture before `include/epykos/fixtures/exotic_path.hpp` was an interest-rate swap, so D1
# ("no JIT -- a build-time catalogue plus a tiled interpreter"; revisit only if M1/M3 show it
# cannot reach the gates) had only ever been tested on the easiest asset class there is. This tool
# runs it on a scripted payoff over a path grid and prints the pass/fail criterion
# `docs/EXOTICS_KILL_TEST.md` committed BEFORE the tool was written, so the number is never handed
# over without it.
#
# Times only public API calls (`exec::Interpreter::run` against the templated-`double` evaluation of
# the same Scalar source), so what it measures is the shipped path; nothing under include/epykos/ or
# src/ is touched.
#
# Like tools/ladder/ it DOES time, so its output is reportable only on a reserved box: it reads the
# 1-minute load itself and refuses above cores/2, which is bench/run.sh's rule (D29,
# docs/WORKLOADS.md M1 Measurement). It also knows which build it is and REFUSES to print a timing
# from the `profile` preset, where `catalogue::Coverage::time_fraction` is the only thing available
# and every Interpreter::run() carries instrumentation (CLAUDE.md Build). Coverage from `profile`,
# timings from `release`, never mixed.
#
# Built whenever EPYKOS_BUILD_TOOLS is (default ON); like tools/catalogue/, tools/ladder/ and
# bench/, never run by ctest or CI.
#
# No -ffp-contract=off here, deliberately and like tools/ladder/: every timing this tool produces is
# under the shipped release flags, and pinning the contraction in this TU would measure a build
# nobody ships. The agreement it prints is a tolerance comparison across the pin (PRINCIPLES.md
# §5.2a case 2), never a bit comparison, so the pinning would buy nothing there either.
add_executable(exoticsprobe exotics_main.cpp)
target_link_libraries(exoticsprobe PRIVATE epykos epykos_warnings)
