// EpykosEngine — THE PIN (`docs/PRINCIPLES.md` §1), and the only front door to it.
//
// The contract splits the pipeline in two. Above the pin the engine chooses WHICH expression to
// evaluate — canonicalisation, algebraic simplification, recurrence closed forms — and is judged
// against the recorded expression in exact real arithmetic. Below the pin it chooses HOW to
// evaluate that expression — inference, planning, execution, the adjoint — and is bit-identical
// to what came out of the pin.
//
// `compile` IS everything above the pin. A recording goes in; the pinned tape comes out; the
// pinned tape is what `ir::infer` is handed and what every gate below the pin compares against.
//
// Why this file exists at all. Until now the tape passes were called by whoever happened to
// record the tape — `src/solver/residual.cpp` and eight fixtures, each calling `standard_passes`
// for itself — so there was no place to add a phase to, and no single answer to "what did the
// engine actually run". `PRINCIPLES.md` §7.2 item 6 names that as a structure to remove and §10
// step 2 removes it. Passes are added INSIDE this function and every caller gets them without
// changing a line.
//
// Deliberately absent, and each absence is a decision:
//
//   * NO options struct and no way to run a subset. One pipeline, so that what is measured, what
//     is gated and what ships are the same program. The moment there are two, a number can be
//     quoted from one and a gate met by the other. If a caller ever genuinely needs less, the
//     passes themselves are still public in `tape/passes.hpp` — that is the escape hatch, and it
//     is honest about being one.
//   * NO new result type. It returns `PassResult`, the same struct every pass returns, with the
//     remap composed across the whole pipeline exactly as `standard_passes` composes it across
//     its five. `nodes_before` is the recording, `nodes_after` is the pinned tape.
//   * NO call to `ir::infer`. That is below the pin; keeping it out is what makes the boundary a
//     boundary rather than a comment.
#pragma once

#include "epykos/tape/passes.hpp"
#include "epykos/tape/tape.hpp"

namespace epykos {

// Everything above the pin, in order, to a fixpoint where the passes interact. Rewrites `tape` in
// place and returns the composed result; `remap[old_id]` is the node the recording's node became,
// or `invalid_node` if it no longer exists.
PassResult compile(Tape& tape);

}  // namespace epykos
