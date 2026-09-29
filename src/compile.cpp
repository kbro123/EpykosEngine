// The pin. See include/epykos/compile.hpp for what it is and why it has no options.
//
// `simplify` (the algebra) interleaved with `standard_passes` (the data movement) to a fixpoint.
// That is the whole of PRINCIPLES.md §10 steps 4-6: the peephole rule set in src/tape/passes.cpp
// subsumes canonicalisation, cancellation AND the telescope, so no separate recurrence solver is
// built here. §3a explains why one was expected and is not needed.
#include "epykos/compile.hpp"

namespace epykos {

PassResult compile(Tape& tape) {
  // `simplify` first, on the recording as written: its patterns are over Add / Sub / Mul / Div,
  // and `fold_sum` and `affine_collapse` rewrite exactly those into `Sum` and `Affine`, where the
  // patterns no longer match. Then the four data-movement passes.
  //
  // Then round again while it is still shrinking, because the two halves feed each other: cse
  // merges subtrees that only become identical once simplify has cancelled them, and simplify
  // sees cancellations that only appear once cse has made two operands the SAME node rather than
  // merely equal ones. The loop is bounded because every round that continues strictly removes
  // nodes; the bound is a backstop against a rule pair that ping-pongs, which would be a bug in
  // the rule set rather than a legitimate long run.
  constexpr int kMaxRounds = 8;
  PassResult total = simplify(tape);
  compose(total, standard_passes(tape));
  for (int round = 1; round < kMaxRounds; ++round) {
    const std::size_t before = tape.size();
    compose(total, simplify(tape));
    compose(total, standard_passes(tape));
    if (tape.size() >= before) break;
  }
  return total;
}

}  // namespace epykos
