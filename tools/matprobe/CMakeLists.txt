# tools/matprobe/ — the TIMING half of materialise-vs-recompute: both arms of
# `adjoint::Options::materialise_steps` interleaved in ONE process, A,B,B,A, on the same
# ir::Program (D105's pattern, which exists because a two-binary comparison of this very subsystem
# carried a 6.6% codegen confound that collapsed to 0.2% once both arms sat behind one option).
#
#   matprobe [all|compare_ois|stage_a|exotic_path] [--mode paired|evict] [--batch N]...
#            [--reps N] [--trades N] [--paths N] [--steps N] [--lane-tile N] [--tile N]
#            [--swap] [--ignore-load]
#
# Per fixture and batch: the structural census (intra-chain instances and how many have exactly one
# in-group reader — a theorem of ir::infer's boundary rule, so always 100%; recomputed
# transcendentals, which is the signal that actually differs between the rates book and the path
# grid; the recompute's share of the forward's arithmetic; both arms' footprints), then the paired
# medians, the reverse-pass bound A−F0, and the verdict. `--mode evict` repeats it under D91 §3's
# forced cache flush at 0/1/4/16/64 MB, which is how residency is separated from arithmetic.
#
# IT TIMES, so like tools/ladder/, tools/pullprobe/ and tools/exotics/ it reads the 1-minute load
# itself and REFUSES above cores/2 (bench/run.sh's rule, D29). The counts-only half, which refuses
# no box, is tools/matrec/.
#
# It links `epykos` only and compiles against nothing outside this repository. It reads the
# fixture-only headers under include/epykos/fixtures/ (D28), which is allowed for a tool and not
# for engine API. Built whenever EPYKOS_BUILD_TOOLS is (default ON); never run by ctest or CI.
add_executable(matprobe matprobe_main.cpp)
target_link_libraries(matprobe PRIVATE epykos epykos_warnings)
