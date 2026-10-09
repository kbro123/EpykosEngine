// EpykosEngine — the lane-width variants the runtimes specialise for, and the decomposition of a
// batch into chunks of those widths.
//
// `exec::Interpreter` and `adjoint::Adjoint` both split a run's B lanes into chunks of at most
// `lane_tile` lanes and dispatch each chunk to a kernel instantiated for its lane count. Only the
// widths in `lane_variants` have such a kernel; every other width falls to variant 0, the
// runtime-L fallback, whose inner `for (l = 0; l < L; ++l)` the compiler cannot vectorise against
// a width it does not know.
//
// Both runtimes drive their chunk loop with `next_lane_chunk` below, so the chunking rule has one
// definition and the two cannot drift. See docs/DESIGN.md §5 "Lane chunking".
#pragma once

namespace epykos::exec {

// Lane-width variants the kernels are instantiated for. Variant 0 is the runtime-L fallback.
inline constexpr int lane_variants[] = {0, 1, 4, 8, 16, 32, 64};
inline constexpr int n_lane_variants = 7;

// The variant index of lane count L, or 0 when no kernel is instantiated for it.
inline int lane_variant(int L) noexcept {
  for (int v = 1; v < n_lane_variants; ++v) {
    if (lane_variants[v] == L) return v;
  }
  return 0;
}

// The cursor `next_lane_chunk` walks. `b0` is the first lane of the chunk and `L` its width — the
// two the kernels are handed. `step` is how far the cursor moves past it, which equals `L` in
// every correct run; it is a separate field only so that a decomposition which fails to come back
// for the lanes it split off is a one-line defect the mutation harness can select
// (lanes.drop_split_remainder).
struct LaneChunk {
  int b0 = 0;
  int L = 0;
  int step = 0;
};

// Advances `*c` to the next chunk of a run of `B` lanes in groups of at most `Lt`, and returns
// false when the run is finished. Zero-initialise the cursor, then:
//
//     for (LaneChunk ch{}; next_lane_chunk(B, Lt, split, &ch);) { ... ch.b0, ch.L ... }
//
// With `split` the width of each chunk is the largest variant that fits in `min(Lt, B - b0)`, so
// a run whose remaining lanes do not make up a width with a kernel is executed as a sequence of
// specialised chunks — 2 -> 1+1, 3 -> 1+1+1, 5 -> 4+1, 7 -> 4+1+1+1, and at Lt = 64 a batch of
// 1,000 as 15x64 + 32 + 8 — instead of falling to one generic chunk. Without it each group is one
// chunk of its own width, which is what the engine did before D103 §6's holes were closed, and is
// the reference side of tests/exec/lane_chunking_e0_test.cpp.
//
// A chunk is NOT required to start on a multiple of `Lt`. Every buffer a kernel touches is
// addressed by the chunk's own `b0` and `L` — the value buffer by stride `L`, the caller's state
// and outputs by `ordinal·B + b0 + l` — and `Lt` is only the width the value buffers were sized
// for, so the only constraints are `L <= Lt` and `b0 + L <= B`. The cursor therefore tiles
// [0, B) greedily instead of decomposing each `Lt` group on its own, which is what it would mean
// to "split the remainder": at Lt = 7 a 1,000-lane run comes out as 250 chunks of 4 rather than
// 125 of 4+1+1+1.
//
// WHY THE SPLIT IS SAFE. Lanes are independent: the interpreter evaluates one scenario per lane,
// and the adjoint's pull() is a per-lane zero fill then `dst[l] += src[l]` per reader (and, under
// `adjoint::Options::seed_pull`, a `dst[l] = src[l]` for the first contribution then `+=` for the
// rest), over readers held in a plan order that does not depend on L and a choice of first
// contribution that does not either. So cutting a group into disjoint lane
// ranges reorders nothing and is
// bitwise identical to running it whole — PRINCIPLES.md §5.2a case 1 ("one lane width against
// another ... it stays bitwise"), and the gate above is that test. The one L-dependent quantity
// in either runtime is `acc_rows_in_flight_for` (src/exec/plan.hpp), which blocks a reduction
// epilogue's ROWS; the members summed within a row stay in recorded position order at every
// width, so it changes no fold order. It is also read with the plan-time `Lt` rather than a
// chunk's L, so the segment blocking does not move when a group is split at all.
//
// WHY SPLITTING RATHER THAN PADDING. A kernel reads `state[ordinal·B + b0 + l]` and writes
// `out[ordinal·B + b0 + l]`, so widening a chunk past the lanes the caller supplied would read
// and write outside the caller's buffers. Padding up to the next variant would need a staging
// copy of every input and a masked copy-out; splitting needs neither.
//
// Variant 0 stays the correct path for a width no variant covers: the search below falls back to
// the whole remaining group. With the table above that never fires, because 1 is a variant and so
// every positive width decomposes — which is exactly why `split` is an option rather than
// unconditional, since otherwise nothing would exercise the runtime-L kernels again.
bool next_lane_chunk(int B, int Lt, bool split, LaneChunk* c) noexcept;

}  // namespace epykos::exec
