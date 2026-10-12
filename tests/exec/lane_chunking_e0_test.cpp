// E0 gate for the lane-chunk decomposition (D103 §6; include/epykos/exec/lanes.hpp).
//
// `exec::Interpreter` and `adjoint::Adjoint` split a run's B lanes into groups of at most
// `lane_tile` and dispatch each group to a kernel instantiated for its lane count. Only the
// widths in `lane_variants` have one, so before this gate existed a group of 2, 3, 5, 6 or 7
// lanes fell to variant 0, the runtime-L fallback — the last group of any B whose remainder mod
// `lane_tile` was one of those, and EVERY group when `lane_tile` itself was. Three such widths
// are now run as a sequence of specialised chunks instead: 2 -> 1+1, 5 -> 4+1, 6 -> 4+1+1. Those
// three are the ones whose decomposition is measured faster than the generic kernel in BOTH
// runtimes (D113 §2); every other non-variant width — 3, 7, and everything above 6 — is left
// whole, because another chunk is another whole pass over the program and that is what the
// measurement says costs more.
//
// What this pins, in two independent layers:
//
//  1. STRUCTURE, mechanically. Every chunk the cursor emits under `split_lane_chunks` is either a
//     width with a kernel of its own (`lane_variant(L) != 0`) or a width `prefer_generic_width`
//     leaves whole, the chunks are a partition of [0, B), and none is wider than the lane tile.
//     These are read off the cursor and checked against `lane_variants`, `decomposed_widths` and
//     `prefer_generic_width` themselves, so a future edit to any of them fails here rather than
//     regressing silently. `decomposed_widths` is pinned in both directions: a width outside it
//     is never decomposed, and every width inside it must actually BE decomposed somewhere in the
//     matrix — a measured exception that stops being reached is a dead branch, and this is how
//     D104's own regression could ship (its decomposition was never measured against the generic
//     kernel it replaced, only against itself).
//
//  2. VALUES, bitwise. The split results are bit-for-bit the results of the old single-remainder
//     chunking, which `split_lane_chunks = false` still runs, for both runtimes over a matrix of
//     B and `lane_tile`. That is the whole licence for the change: lanes are independent — the
//     interpreter evaluates one scenario per lane and the adjoint's pull() is `dst[l] += src[l]`
//     per lane — so cutting a group into disjoint lane ranges reorders nothing. PRINCIPLES.md
//     §5.2a case 1 ("one lane width against another ... it stays bitwise") is why this is an
//     equality and not a tolerance, and why a failure here would be a real defect rather than a
//     reason to loosen anything.
//
// The output buffers are pre-filled with a sentinel before every run, so a lane that no chunk
// wrote is a mismatch rather than a coincidence.
//
// This TU is compiled with -ffp-contract=off in every preset (the _e0_test.cpp convention).
#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "epykos/adjoint/adjoint.hpp"
#include "epykos/exec/interpreter.hpp"
#include "epykos/exec/lanes.hpp"
#include "epykos/fixtures/m1_book.hpp"
#include "epykos/fixtures/record_m1.hpp"
#include "epykos/ir/program.hpp"
#include "epykos/ir/signature.hpp"
#include "epykos/rng/philox.hpp"
#include "epykos/tape/tape.hpp"
#include "ir/ir_test_helpers.hpp"

#ifndef EPYKOS_FP_CONTRACT_OFF
#error "an _e0_test.cpp TU must see EPYKOS_FP_CONTRACT_OFF"
#endif

namespace adjoint = epykos::adjoint;
namespace exec = epykos::exec;
namespace fixtures = epykos::fixtures;
namespace ir = epykos::ir;
using epykos::Tape;

namespace {

std::uint64_t bits(double v) {
  std::uint64_t u;
  std::memcpy(&u, &v, sizeof u);
  return u;
}

// A lane that no chunk wrote keeps this, which no output of the fixture takes.
constexpr double sentinel = -1.2345678901234567e+300;

// ---- layer 1: the chunk sequence, as a fact about the cursor ------------------------------

struct Chunk {
  int b0 = 0;
  int L = 0;
};

std::vector<Chunk> chunks_of(int B, int Lt, bool split) {
  std::vector<Chunk> out;
  for (exec::LaneChunk c{}; exec::next_lane_chunk(B, Lt, split, &c);) out.push_back({c.b0, c.L});
  return out;
}

// Every B this gate exercises. The awkward ones are the remainders with no kernel (2, 3, 5, 6, 7
// and the B whose last group is one of those at lane_tile 8: 63, 100 -> 4 is a variant, 1000 ->
// exact); the ordinary ones are there to pin that nothing moved on the fast path.
const std::vector<int>& batches() {
  static const std::vector<int> v = {1, 2, 3, 4, 5, 6, 7, 8, 9, 11, 12, 13, 15, 16, 17, 31, 33, 40, 63, 64, 65, 100, 128, 1000};
  return v;
}

const std::vector<int>& lane_tiles() {
  static const std::vector<int> v = {1, 2, 3, 4, 5, 7, 8, 16, 32, 64};
  return v;
}

// ---- the fixtures the value layers run on -------------------------------------------------

struct Program {
  Tape tape;
  ir::Program program;
  int n_in = 0;
  int n_out = 0;
  std::vector<std::vector<double>> states;   // one per lane, as many as the widest B needs
  std::vector<std::vector<double>> seeds;    // one out_bar per lane
};

// `swaps` of the M1 book, with `n_lanes` deterministic states and adjoint seeds. The small
// sub-book is what the wide batches run on: the chunking is a property of B and lane_tile, not of
// the program, and a 1,000-lane run of the whole book buys nothing for the time it costs.
Program make_program(const std::vector<int>& swaps, int n_lanes, std::uint64_t stream) {
  Program p;
  const fixtures::Book full = fixtures::make_m1_book();
  const fixtures::Book book = swaps.empty() ? full : epykos::test::sub_book(full, swaps);
  p.tape = fixtures::record_m1(book);
  p.program = ir::infer(p.tape);
  p.n_in = static_cast<int>(p.program.inputs.size());
  p.n_out = static_cast<int>(p.program.outputs.size());
  const std::vector<double> record = p.tape.input_values();
  for (int b = 0; b < n_lanes; ++b) {
    epykos::rng::Philox g(fixtures::default_seed, stream + static_cast<std::uint64_t>(b));
    std::vector<double> z(static_cast<std::size_t>(p.n_in));
    // A perturbation of the record point, so every lane is a different scenario and still lands
    // in the domain the fixture's curve is built for.
    for (std::size_t k = 0; k < z.size(); ++k) z[k] = record[k] * (1.0 + 0.05 * g.uniform_range(-1.0, 1.0));
    p.states.push_back(z);
    std::vector<double> ob(static_cast<std::size_t>(p.n_out));
    for (std::size_t o = 0; o < ob.size(); ++o) ob[o] = g.uniform_range(-1.0, 1.0);
    p.seeds.push_back(ob);
  }
  return p;
}

const Program& whole_book() {
  static const Program p = make_program({}, 128, 700000);
  return p;
}

const Program& small_book() {
  static const Program p = make_program({0, 1, 2, 500, 999}, 1000, 800000);
  return p;
}

const Program& program_for(int B) { return B <= 128 ? whole_book() : small_book(); }

std::vector<double> run_interp(const Program& p, int B, int Lt, bool split) {
  exec::Options o;
  o.max_batch = B;
  o.lane_tile = Lt;
  o.split_lane_chunks = split;
  exec::Interpreter in(p.program, o);
  const std::size_t Bs = static_cast<std::size_t>(B);
  std::vector<double> state(static_cast<std::size_t>(p.n_in) * Bs);
  for (std::size_t b = 0; b < Bs; ++b) {
    for (std::size_t k = 0; k < static_cast<std::size_t>(p.n_in); ++k) state[k * Bs + b] = p.states[b][k];
  }
  std::vector<double> out(static_cast<std::size_t>(p.n_out) * Bs, sentinel);
  in.run(state.data(), B, out.data());
  return out;
}

struct AdjResult {
  std::vector<double> out;
  std::vector<double> state_bar;
};

AdjResult run_adjoint(const Program& p, int B, int Lt, bool split) {
  adjoint::Options o;
  o.max_batch = B;
  o.lane_tile = Lt;
  o.split_lane_chunks = split;
  adjoint::Adjoint ad(p.program, o);
  const std::size_t Bs = static_cast<std::size_t>(B);
  std::vector<double> state(static_cast<std::size_t>(p.n_in) * Bs);
  std::vector<double> out_bar(static_cast<std::size_t>(p.n_out) * Bs);
  for (std::size_t b = 0; b < Bs; ++b) {
    for (std::size_t k = 0; k < static_cast<std::size_t>(p.n_in); ++k) state[k * Bs + b] = p.states[b][k];
    for (std::size_t o = 0; o < static_cast<std::size_t>(p.n_out); ++o) out_bar[o * Bs + b] = p.seeds[b][o];
  }
  AdjResult r;
  r.out.assign(static_cast<std::size_t>(p.n_out) * Bs, sentinel);
  r.state_bar.assign(static_cast<std::size_t>(p.n_in) * Bs, sentinel);
  ad.run(state.data(), B, out_bar.data(), r.out.data(), r.state_bar.data());
  return r;
}

std::size_t count_mismatches(const std::vector<double>& a, const std::vector<double>& b, const std::string& what,
                             std::size_t* reported) {
  std::size_t n = 0;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (bits(a[i]) != bits(b[i])) {
      if (*reported < 8) {
        ADD_FAILURE() << what << ": element " << i << " split " << a[i] << " generic " << b[i];
        ++*reported;
      }
      ++n;
    }
  }
  return n;
}

}  // namespace

// ---- layer 1 ------------------------------------------------------------------------------

// The assertion a future edit to `lane_variants` or `prefer_generic_width` has to satisfy: under
// the split, a chunk reaches the runtime-L fallback only at a width the decomposition DECLARED it
// would leave whole. Everything else must have a kernel.
//
// This replaced a strictly stronger-looking assertion — "no chunk is ever generic" — which was
// true only because D104 decomposed every width, including the one it should not have (D113 §2).
// Pinning the declared set in both directions is what keeps that from being a loosening: a width
// outside the set may not reach the generic kernel, and the set itself is checked below to be
// non-empty, disjoint from `lane_variants`, and actually reached.
TEST(LaneChunkingE0, ASplitChunkIsEitherAVariantOrADeclaredGenericWidth) {
  std::size_t chunks = 0, undeclared = 0, declared_generic = 0;
  for (const int B : batches()) {
    for (const int Lt : lane_tiles()) {
      for (const Chunk& c : chunks_of(B, Lt, true)) {
        ++chunks;
        if (exec::lane_variant(c.L) != 0) continue;
        if (exec::prefer_generic_width(c.L)) {
          ++declared_generic;
          continue;
        }
        if (undeclared < 8) {
          ADD_FAILURE() << "B " << B << ", lane_tile " << Lt << ": chunk at lane " << c.b0 << " has width " << c.L
                        << ", which is neither in lane_variants nor a width prefer_generic_width declines -- it "
                           "would run on the generic kernel by accident";
        }
        ++undeclared;
      }
    }
  }
  EXPECT_EQ(undeclared, 0u);
  // The other direction: the widths left whole are reached, so the rule is not a dead branch.
  EXPECT_GT(declared_generic, 0u) << "no configuration in the matrix emits a width left whole -- either the matrix "
                                     "stopped covering one or the predicate is now dead code";
  // And every width the rule DOES decompose is actually decomposed somewhere here, so a listed
  // width that stopped being reachable is a failure rather than a silent no-op.
  for (const int w : exec::decomposed_widths) {
    bool seen = false;
    for (const int Lt : lane_tiles()) {
      if (Lt < w) continue;
      const std::vector<Chunk> cs = chunks_of(w, Lt, true);
      if (cs.size() > 1) seen = true;
    }
    EXPECT_TRUE(seen) << "width " << w << " is listed as decomposed but no lane tile in the matrix decomposes it";
  }
  std::cout << "[  chunks  ] " << chunks << " split chunks over " << batches().size() << " batch widths x "
            << lane_tiles().size() << " lane tiles; left whole " << declared_generic << ", undeclared "
            << undeclared << '\n';
}

// `decomposed_widths` is a performance decision, and these are the properties that make it a
// coherent one rather than a hole. A width it names must NOT be a variant — decomposing INTO a
// kernel that already exists for the whole width cannot be faster — and must be at least 2, since
// 1 is a variant and there is nothing below it to decompose into. The predicate must agree with
// the list in both directions over every width a lane tile can produce.
TEST(LaneChunkingE0, TheDecomposedWidthsAreCoherentAndThePredicateAgreesWithThem) {
  for (const int w : exec::decomposed_widths) {
    EXPECT_EQ(exec::lane_variant(w), 0) << "width " << w << " has a specialised kernel; decomposing INTO it cannot "
                                           "be faster than using it";
    EXPECT_GE(w, 2) << "width " << w << " is below 2; 1 is a variant and has nothing to decompose into";
    EXPECT_FALSE(exec::prefer_generic_width(w)) << "width " << w << " is listed as decomposed but left whole";
  }
  // Every other width in the range a lane tile can produce is left whole unless it is a variant.
  for (int w = 1; w <= 64; ++w) {
    const bool listed = std::find(std::begin(exec::decomposed_widths), std::end(exec::decomposed_widths), w) !=
                        std::end(exec::decomposed_widths);
    const bool variant = exec::lane_variant(w) != 0;
    EXPECT_EQ(exec::prefer_generic_width(w), !listed && !variant)
        << "width " << w << ": the predicate and decomposed_widths disagree";
  }
  std::cout << "[ decomp   ] " << exec::n_decomposed_widths << " decomposed width(s), checked against every width in "
            << "[1, 64]\n";
}

// A decomposition that drops a lane, or runs one twice, is the defect the mutant models; this is
// what makes it visible structurally as well as numerically.
//
// Note what is NOT asserted: that a chunk begins on a multiple of `lane_tile`. It need not, and
// the split deliberately does not — the cursor tiles [0, B) greedily, so at lane_tile 7 a
// 1,000-lane run is 250 chunks of 4 and not 125 of 4 + a generic 3. Every buffer is addressed by
// the chunk's own b0 and L, so the real constraints are the two below: a chunk fits the lane tile
// the value buffers were sized for, and the chunks tile the batch exactly.
TEST(LaneChunkingE0, TheChunksTileTheBatchExactlyAndFitTheLaneTile) {
  for (const int B : batches()) {
    for (const int Lt : lane_tiles()) {
      for (const bool split : {false, true}) {
        const std::vector<Chunk> cs = chunks_of(B, Lt, split);
        ASSERT_FALSE(cs.empty()) << "B " << B << ", lane_tile " << Lt;
        int next = 0;
        for (const Chunk& c : cs) {
          EXPECT_EQ(c.b0, next) << "B " << B << ", lane_tile " << Lt << ", split " << split << ": gap or overlap";
          EXPECT_GT(c.L, 0);
          EXPECT_LE(c.L, Lt) << "a chunk is never wider than the lane tile the buffers are sized for";
          EXPECT_LE(c.b0 + c.L, B) << "a chunk is never wider than the lanes the caller supplied";
          next = c.b0 + c.L;
        }
        EXPECT_EQ(next, B) << "B " << B << ", lane_tile " << Lt << ", split " << split << ": lanes left unevaluated";
      }
    }
  }
}

// `split_lane_chunks = false` must still be exactly the pre-D103 chunking, because that is what
// the value layers below use as their reference.
TEST(LaneChunkingE0, GenericChunkingIsTheOldSingleRemainderShape) {
  for (const int B : batches()) {
    for (const int Lt : lane_tiles()) {
      const std::vector<Chunk> cs = chunks_of(B, Lt, false);
      std::vector<Chunk> want;
      for (int b0 = 0; b0 < B; b0 += Lt) want.push_back({b0, std::min(Lt, B - b0)});
      ASSERT_EQ(cs.size(), want.size()) << "B " << B << ", lane_tile " << Lt;
      for (std::size_t i = 0; i < cs.size(); ++i) {
        EXPECT_EQ(cs[i].b0, want[i].b0);
        EXPECT_EQ(cs[i].L, want[i].L);
      }
    }
  }
}

// Variant 0 is still reachable and still correct for a width no kernel covers. Under the split it
// is now reached deliberately, at the one declared width; with `split` off it is reached at every
// non-variant width, which is what keeps the runtime-L kernels exercised broadly rather than at a
// single L. This pins that the fallback branch is the whole remaining group either way.
TEST(LaneChunkingE0, TheFallbackIsReachableForAWidthNoVariantCovers) {
  // A hypothetical table without the width of 1 is not constructible here, so the property is
  // pinned where it is observable: the generic chunking routes these widths to variant 0 today,
  // and the value layers run that path on every configuration below.
  for (const int L : {2, 3, 5, 6, 7}) EXPECT_EQ(exec::lane_variant(L), 0) << L << " is not a specialised width";
  for (const int L : {1, 4, 8, 16, 32, 64}) EXPECT_NE(exec::lane_variant(L), 0) << L << " must have a kernel";
  // And a width wider than every variant still has a path: the split caps each chunk at 64.
  for (const Chunk& c : chunks_of(200, 200, true)) {
    EXPECT_TRUE(exec::lane_variant(c.L) != 0 || exec::prefer_generic_width(c.L)) << c.L;
  }
  EXPECT_EQ(chunks_of(200, 200, false).front().L, 200) << "the generic path must still take a width of 200 whole";
  // The rule read off the cursor rather than off the predicate, at the five non-variant widths
  // below the widest small variant. These five are the whole of what D103 §6 found and D113 §2
  // measured, so they are written out one by one rather than derived.
  EXPECT_EQ(chunks_of(2, 8, true).size(), 2u) << "2 must be 1+1 -- the adjoint -23%, the interpreter -15%";
  EXPECT_EQ(chunks_of(5, 8, true).size(), 2u) << "5 must be 4+1 -- both runtimes about -20%";
  EXPECT_EQ(chunks_of(6, 8, true).size(), 3u) << "6 must be 4+1+1 -- both runtimes win";
  ASSERT_EQ(chunks_of(3, 8, true).size(), 1u) << "3 must be ONE generic chunk -- 1+1+1 costs the interpreter +7%";
  EXPECT_EQ(chunks_of(3, 8, true).front().L, 3);
  ASSERT_EQ(chunks_of(7, 8, true).size(), 1u) << "7 must be ONE generic chunk -- 4+1+1+1 costs both runtimes";
  EXPECT_EQ(chunks_of(7, 8, true).front().L, 7);
  // And nothing above 6 is decomposed: at a wide lane tile the plain arm is one chunk where the
  // old split was five or seven, which is where the adjoint lost 23-25% (D113 §2).
  for (const int w : {9, 10, 11, 13, 15, 31, 63}) {
    ASSERT_EQ(chunks_of(w, 64, true).size(), 1u) << "width " << w << " must be one generic chunk at lane_tile 64";
    EXPECT_EQ(chunks_of(w, 64, true).front().L, w);
  }
}

// ---- layer 2: the bitwise equality, both runtimes -----------------------------------------

TEST(LaneChunkingE0, InterpreterSplitIsBitwiseTheGenericChunking) {
  std::size_t mismatches = 0, reported = 0, runs = 0;
  for (const int B : batches()) {
    const Program& p = program_for(B);
    for (const int Lt : lane_tiles()) {
      const std::vector<double> split = run_interp(p, B, Lt, true);
      const std::vector<double> generic = run_interp(p, B, Lt, false);
      mismatches += count_mismatches(split, generic, "interp B=" + std::to_string(B) + " lane_tile=" + std::to_string(Lt),
                                     &reported);
      // The sentinel must be gone either way: every lane of every output is written.
      for (const double v : split) {
        if (bits(v) == bits(sentinel)) {
          if (reported < 8) {
            ADD_FAILURE() << "interp B=" << B << " lane_tile=" << Lt << ": an output lane was never written";
            ++reported;
          }
          ++mismatches;
        }
      }
      ++runs;
    }
  }
  EXPECT_EQ(mismatches, 0u);
  std::cout << "[  interp  ] " << runs << " (B, lane_tile) configurations, mismatches " << mismatches << '\n';
}

TEST(LaneChunkingE0, AdjointSplitIsBitwiseTheGenericChunking) {
  std::size_t mismatches = 0, reported = 0, runs = 0;
  // The adjoint costs several times the forward pass, so it takes the same batch widths over the
  // lane tiles that actually produce a split remainder.
  for (const int B : batches()) {
    const Program& p = program_for(B);
    for (const int Lt : {3, 4, 7, 8, 64}) {
      const AdjResult split = run_adjoint(p, B, Lt, true);
      const AdjResult generic = run_adjoint(p, B, Lt, false);
      const std::string what = "adjoint B=" + std::to_string(B) + " lane_tile=" + std::to_string(Lt);
      mismatches += count_mismatches(split.out, generic.out, what + " out", &reported);
      mismatches += count_mismatches(split.state_bar, generic.state_bar, what + " state_bar", &reported);
      for (const double v : split.state_bar) {
        if (bits(v) == bits(sentinel)) {
          if (reported < 8) {
            ADD_FAILURE() << what << ": a state_bar lane was never written";
            ++reported;
          }
          ++mismatches;
        }
      }
      ++runs;
    }
  }
  EXPECT_EQ(mismatches, 0u);
  std::cout << "[ adjoint  ] " << runs << " (B, lane_tile) configurations, mismatches " << mismatches << '\n';
}

// Each specialised width's kernels are a correct path in their own right: one B = 64 batch run
// through chunks of width w, for every w in the table, agrees bitwise with the same batch run as
// one 64-lane chunk. This is what a variant added to the table in future has to satisfy, and it
// is the reason the table and the instantiated kernels cannot drift apart unnoticed.
TEST(LaneChunkingE0, EveryVariantWidthAgreesWithTheWholeBatchRun) {
  const Program& p = whole_book();
  std::size_t mismatches = 0, reported = 0;
  const std::vector<double> ref = run_interp(p, 64, 64, false);
  for (const int w : {1, 4, 8, 16, 32, 64}) {
    const std::vector<double> got = run_interp(p, 64, w, true);
    mismatches += count_mismatches(got, ref, "variant width " + std::to_string(w), &reported);
  }
  EXPECT_EQ(mismatches, 0u);
  std::cout << "[ variants ] 6 widths against the one-chunk run of the same batch, mismatches " << mismatches << '\n';
}
