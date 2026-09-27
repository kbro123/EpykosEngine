// The pin. See include/epykos/compile.hpp for what it is and why it has no options.
//
// Today this is `standard_passes` and nothing more, which makes step 2 of `PRINCIPLES.md` §10 a
// pure refactor: the same five passes in the same order, so every gate below the pin sees the
// identical tape it saw before. Steps 4-6 add canonicalisation, simplification and recurrence
// solving HERE, and every caller picks them up without changing.
#include "epykos/compile.hpp"

namespace epykos {

PassResult compile(Tape& tape) {
  // `standard_passes` is cse, dce, fold_sum, affine_collapse, dce, composing its own remap.
  return standard_passes(tape);
}

}  // namespace epykos
