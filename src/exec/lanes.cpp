// EpykosEngine — the chunk cursor both runtimes iterate (include/epykos/exec/lanes.hpp).
//
// Integer addressing only: no floating-point arithmetic lives here, so this file needs none of
// the `*_e0.cpp` contraction pinning (PRINCIPLES.md §5.4). It is nonetheless the single
// definition of the chunking rule, which is what keeps `exec::Interpreter::run` and
// `adjoint::Adjoint::run` from drifting apart — they were identical in shape and separately
// wrong before D103 §6.
#include "epykos/exec/lanes.hpp"

#include <algorithm>

#include "epykos/mutation/mutation.hpp"

namespace epykos::exec {

bool next_lane_chunk(int B, int Lt, bool split, LaneChunk* c) noexcept {
  const int b0 = c->b0 + c->step;
  if (b0 >= B) return false;
  // The widest chunk allowed here: `Lt` is what the value buffers hold, and `B - b0` is what the
  // caller supplied. Nothing requires a chunk to start on a multiple of `Lt` — every buffer is
  // addressed by the chunk's own `b0` and `L` — so the cursor tiles [0, B) greedily rather than
  // splitting each `Lt` group separately. That matters when `Lt` is itself not a variant: at
  // Lt = 7 a 1,000-lane run becomes 250 chunks of 4, where splitting group by group would give
  // 125 of 4+1+1+1.
  const int rem = std::min(Lt, B - b0);
  int L = rem;
  if (split) {
    // The largest variant that fits. The table is ascending and variant 0 is the fallback, so the
    // first hit walking down is the widest kernel available for `rem`; `rem` itself is kept when
    // none fits, which is variant 0 and the only path that still needs the runtime-L kernels.
    for (int v = n_lane_variants - 1; v >= 1; --v) {
      if (lane_variants[v] <= rem) {
        L = lane_variants[v];
        break;
      }
    }
  }
  c->b0 = b0;
  c->L = L;
  // Mutant lanes.drop_split_remainder: the cursor advances past the whole GROUP instead of past
  // the chunk just produced, so every lane a split left for a later chunk is never evaluated and
  // the caller reads back whatever its output buffer held. A no-op on a group whose width is a
  // variant (there L == rem), which is what makes it a mutant of the decomposition and not of the
  // chunking: it bites at B = 2, 3, 5, 6, 7 and at any B whose last group is one of those.
  c->step = mutant("lanes.drop_split_remainder") ? rem : L;
  return true;
}

}  // namespace epykos::exec
