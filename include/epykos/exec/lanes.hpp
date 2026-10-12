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

// The non-variant widths whose DECOMPOSITION is measured faster than running the width whole on
// the generic kernel. There are three, and the list is a measurement rather than a rule — D113,
// paired arms in one binary (`compare_ois` 64 trades, three replicates, both runtimes, at
// `lane_tile` 8 and 64).
//
// What is being traded. A non-variant width w decomposes into the largest variant below it plus
// the decomposition of the rest, so every extra piece is another whole pass over the program —
// `exec::Interpreter::run`'s chunk loop, and in the adjoint `run_chunk`'s forward() AND reverse().
// Against that sits the generic kernel's inner `for (l = 0; l < L; ++l)`, which the compiler
// cannot vectorise against a width it does not know. The first cost scales with the number of
// pieces; the second with how badly the generic kernel does at that one width. Measured, the
// second is large only at small widths — the generic kernel at L >= 8 is within a few percent of
// the specialised one per lane in the adjoint — so decomposition pays at a handful of small
// widths and loses everywhere else:
//
//   w   split emits   adjoint        interpreter     verdict
//   2   1+1           -23 to -24%    -15 to -17%     decompose
//   3   1+1+1         -0.3 to -2.5%  +7.2 to +10.3%  LEAVE WHOLE
//   5   4+1           -19 to -22%    -23 to -25%     decompose
//   6   4+1+1         -3.8 to -6.5%  -7.1 to -9.0%   decompose
//   7   4+1+1+1       +2.8 to +5.2%  +2.5 to +4.0%   LEAVE WHOLE
//
// and above the widest small variant, where the plain arm is one generic chunk and the split is
// three to seven, the adjoint loses by 3% (w = 10), 8% (11), 9% (13), 13% (36 within B = 100),
// 23% (15) and 25% (63), against interpreter gains that are smaller in absolute microseconds
// because the adjoint costs 4-7x the forward pass per lane. So no width above 6 is decomposed.
//
// This is the whole of the regression D104 shipped (ROADMAP, "Lane widths and chunk tiling",
// item 3, whose own account of it — "B = 3, 7, 11, 15 by 4-13%", fixed by one condition on a tail
// of 3 — is corrected by D113 §2: it measured `lane_tile` 8 only, where the plain arm is already
// chunked, and the defect is chunk count rather than a tail width).
//
// Do NOT grow this into a cost model over decomposition choices — that is D86's retired cost
// model again, and the lever is worth ~0% on anything this project benchmarks. A width earns a
// place here by a paired measurement in BOTH runtimes, or not at all.
inline constexpr int decomposed_widths[] = {2, 5, 6};
inline constexpr int n_decomposed_widths = 3;

// Whether width `w` is handed to the generic kernel whole rather than decomposed. False for a
// width that has its own kernel — there is nothing to decide, and the decomposition of a variant
// is the variant itself.
inline bool prefer_generic_width(int w) noexcept {
  if (lane_variant(w) != 0) return false;
  for (int i = 0; i < n_decomposed_widths; ++i) {
    if (decomposed_widths[i] == w) return false;
  }
  return true;
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
// With `split`, a width in `decomposed_widths` is executed as a sequence of specialised chunks —
// 2 -> 1+1, 5 -> 4+1, 6 -> 4+1+1 — instead of falling to one generic chunk, and every other width
// is emitted whole exactly as it would be without `split`. At Lt = 64 a batch of 1,000 is
// 15x64 + 32 + 8 either way, because 40 = 32 + 8 is two variants. So what `split` changes is
// confined to the three widths above and to any run whose remainder is one of them; a batch of 15
// at Lt = 64 is one generic chunk under both settings.
//
// Without `split` each group is one chunk of its own width, which is what the engine did before
// D103 §6's holes were closed, and is the reference side of
// tests/exec/lane_chunking_e0_test.cpp. It remains the only setting under which the runtime-L
// kernels are exercised at EVERY non-variant width rather than at the ones left whole by default.
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
// the whole remaining group, and `prefer_generic_width` sends most non-variant widths there
// directly without searching at all. So the generic kernels are live on the default path — they
// are what runs a batch of 3, 7, 11 or 15 — and `split` being an option is what additionally
// exercises them at the three widths the default decomposes.
bool next_lane_chunk(int B, int Lt, bool split, LaneChunk* c) noexcept;

}  // namespace epykos::exec
