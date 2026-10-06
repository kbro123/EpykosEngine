// EpykosEngine — the reverse of a Program AS a Program (PRINCIPLES.md §1b, invariant I1; M5/C2,
// C3a).
//
// I1 says the IR is closed under differentiation: `adjoint(P)` yields an `ir::Program`. Until this
// file existed it did not — `adjoint::Adjoint` was a compiled artifact whose derivative arithmetic
// was a `switch` over `Op` in `src/adjoint/adjoint_e0.cpp`, so no pass above or below the pin could
// see a single operation of it. §1b named one cause: `SlotKind::Gather` is an operand ADDRESSING
// mode and "the reverse of an addressing mode is a scatter-accumulate, which the IR cannot
// express".
//
// That last clause is wrong, and this file is the demonstration. **`Op::Affine` over a `Segment`
// IS a scatter-accumulate, read backwards.** `plan.hpp`'s four "who reads me" CSR lists already
// transpose every index array of the program — they are exactly the reverse adjacency a scatter
// needs — and `src/adjoint/adjoint_e0.cpp`'s `pull()` already consumes them as
//
//     v̄ = 0 + Σ out_bar[o] + Σ gbar[g, r] + Σ segbar[s, r] + Σ coefs[m]·segbar[s, r],
//
// left to right. That fold, member by member, is `Op::Affine`'s own fold (`acc = konst;
// p = coef·m; acc = acc + p`), written identically in `include/epykos/ir/evaluate.hpp`,
// `src/exec/kernels_impl.hpp`, `include/epykos/tape/replay.hpp` and the pull itself. So the pull
// emits as ONE `Affine` step over ONE segment whose members are the concatenated CSR lists. **No
// new op, no change to the `Op` enum, no new dispatch site.** `Op::Gather`, `Op::SegmentSum` and
// `Op::Linmap` stay reserved and unused.
//
// What the emitted program is
// ---------------------------
// `adjoint_to_program(P)` returns a Program Q with
//
//   inputs   P's inputs, in P's input order, then one input per OUTPUT ordinal of P: the seed
//            out_bar. So Q takes (state, out_bar) as one flat input vector.
//   outputs  P's outputs, in P's output order, then one output per INPUT ordinal of P: the
//            state adjoint. So Q returns (out, state_bar) as one flat output vector.
//
// and Q is bit-identical to `adjoint::Adjoint::run(state, B, out_bar, out, state_bar)` on the
// same P — not within a tolerance, bitwise, which is what §1b's gate asks for and what
// `tests/adjoint/adjoint_to_program_e0_test.cpp` asserts with `memcmp` under every fusion,
// tiling, lane and batch configuration of `exec::Interpreter`.
//
// Shape. Q's forward half is one domain per (P domain, step): intermediates are MATERIALISED
// rather than recomputed per target, because recomputing exploded a 37-domain fixture to ~44k
// steps while materialising gives 819 small domains. Q's reverse half walks P's domains
// backwards and emits, per domain, the `Affine` pull, then one domain per step adjoint, per
// segment edge slot and per gather edge slot — each the accumulation chain of that target, in
// `adjoint_e0.cpp`'s order (steps last to first; an op's operands a, b, c).
//
// Why the emitted arithmetic looks redundant, and must stay so (all five are bitwise, not style):
//
//   1. the pull's `konst` is a Literal `+0.0` and is never elided: `0.0 + (-0.0)` is `+0.0` and
//      the runtime memsets the accumulator to `+0.0` before the first `+=`;
//   2. output, gather and Sum readers carry coefficient `1.0`, because the pull multiplies for
//      affine readers and `1.0 * x` is exact for every non-NaN double;
//   3. the four CSR lists concatenate in plan order — outputs, gathers, sums, affines;
//   4. every step-adjoint and edge-slot chain starts `Add(Lit +0.0, t)` or `Sub(Lit +0.0, t)`,
//      again because of signed zero (the buffers are memset, then `+=` / `-=`);
//   5. an aliased `Div` — both operand slots the same Step or the same Gather — emits
//      `(ȳ/b)·(1 − y)` as ONE contribution, reproducing `acc_div_aliased`'s dispatch
//      structurally. Two separate accumulations are NOT bitwise (D84's defect, inverted).
//
// Scope. Elementwise ops, gathers and segments (`Sum` / `Affine`). A **scan** domain is refused
// with `std::logic_error`: the reverse scan is stage C3b. The forward half needs no transcendental
// policy beyond `ExpMode::std_exp`, and the reverse needs none at all — `acc_exp` and `acc_sqrt`
// consume the already-materialised value `y`, so no `exp`, `log` or `sqrt` appears in the reverse
// half of Q.
#pragma once

#include "epykos/adjoint/plan.hpp"
#include "epykos/ir/program.hpp"

namespace epykos::adjoint {

// The reverse of `program` as a Program (see the file header for its input / output layout).
//
// Throws std::logic_error when `program` contains a scan domain (or any other recurrent domain,
// or a fixed-arity Sum, which only scan groups carry): the reverse scan is stage C3b.
// Throws std::invalid_argument when `program` has no outputs (there would be nothing to seed),
// or std::runtime_error / std::invalid_argument from `build_plan` for a program the adjoint
// itself refuses.
//
// The two-argument form takes a plan already built from the SAME program (it is not re-derived).
ir::Program adjoint_to_program(const ir::Program& program);
ir::Program adjoint_to_program(const ir::Program& program, const AdjointPlan& plan);

}  // namespace epykos::adjoint
