# tools/revcollapse/ — the measurement behind D102: does the algebra above the pin collapse the
# EMITTED REVERSE (invariant I1's `adjoint(P)`, D98-D100), and is the collapsed reverse still the
# same answer?
#
#   revcollapse [fixture|all]      prints, per fixture, the forward tape's own collapse, Q's size,
#                        the reverse tape raw -> standard_passes -> compile, the halves of that
#                        tape (the forward half of Q IS the pinned forward tape), the ARITHMETIC
#                        (not node) counts, the per-pass attribution, the leave-one-out, the
#                        simplify rule-site census, the cse duplicate census, and the bitwise
#                        comparison of all three tapes against `adjoint::Adjoint::run` over five
#                        out_bar seeds -- through `Replayer`, `ir::Evaluator` and
#                        `exec::Interpreter` at three batch sizes.
#
# NO TIMING. Every number here is a count or a size, which is what makes it quotable on a loaded
# box (D95 §4). Built whenever EPYKOS_BUILD_TOOLS is (default ON); like tools/catalogue/ and
# bench/, never run by ctest or CI.
#
# -ffp-contract=off, for the reason tests/adjoint/i1_gate_e0_test.cpp's header gives: `Replayer`
# and `ir::Evaluator` are header-only and take THIS TU's flags, so without it they would contract
# `acc + coef*member` into an FMA here while src/adjoint/adjoint_e0.cpp did not, and the bitwise
# comparison would fail for a reason that has nothing to do with the collapse.
add_executable(revcollapse revcollapse_main.cpp)
target_link_libraries(revcollapse PRIVATE epykos epykos_warnings)
target_compile_options(revcollapse PRIVATE -ffp-contract=off)
