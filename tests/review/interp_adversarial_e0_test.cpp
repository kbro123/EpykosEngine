// M1/P7 review: the tiled interpreter under configurations the P4 gate does not try.
//   * allocations inside run() counted with a global operator new;
//   * odd B (3, 17, 33) with every lane tile (chunk mixes: 8+8+1, 16+1, 32+1, a lone 17 through
//     the runtime kernel with buffers sized for 32, so emit / inline decisions made for 32 lanes
//     execute at 17);
//   * tile sizes that do not divide the row counts (2, 3, 5, 13, 100, 333, 1000, 5000);
//   * every combination of fuse_reductions / fuse_pairs / inline_producers, on and off;
//   * a raw (no-pass) program; a program whose Input domain is fused into a whole-domain Affine.
// Everything must equal the tape replay bitwise (this TU is contraction-free; the kernels are
// contraction-free in their own TUs).
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <new>
#include <string>
#include <vector>

#include "epykos/exec/interpreter.hpp"
#include "epykos/ir/program.hpp"
#include "epykos/ir/signature.hpp"
#include "epykos/maths/m1/book.hpp"
#include "epykos/scalar/rec.hpp"
#include "epykos/tape/passes.hpp"
#include "epykos/tape/record_m1.hpp"
#include "epykos/tape/replay.hpp"
#include "epykos/tape/tape.hpp"
#include "ir/ir_test_helpers.hpp"
#include "tape/mini_book.hpp"

#ifndef EPYKOS_FP_CONTRACT_OFF
#error "an _e0_test.cpp TU must see EPYKOS_FP_CONTRACT_OFF"
#endif

namespace {
std::uint64_t g_allocations = 0;
}
void* operator new(std::size_t n) {
  ++g_allocations;
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) {
  ++g_allocations;
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace m1 = epykos::m1;
namespace ir = epykos::ir;
namespace exec = epykos::exec;
using epykos::Rec;
using epykos::Replayer;
using epykos::Tape;

namespace {

std::uint64_t bits(double v) {
  std::uint64_t u;
  std::memcpy(&u, &v, sizeof u);
  return u;
}

struct Case {
  Tape tape;
  ir::Program program;
  std::vector<std::vector<double>> states;   // 65: record point + batch
  std::vector<std::vector<double>> expect;   // replay per state
};

Case make_case(const Tape& tape) {
  Case c;
  c.tape = tape;
  c.program = ir::infer(c.tape);
  const m1::Batch batch = m1::make_m1_batch();
  c.states.push_back(c.tape.input_values());
  for (int b = 0; b < batch.n_states; ++b) {
    std::vector<double> z(static_cast<std::size_t>(m1::n_knots));
    batch.state(b, z.data());
    c.states.push_back(z);
  }
  Replayer rp(c.tape);
  for (const auto& z : c.states) {
    std::vector<double> o(c.tape.num_outputs());
    rp.run(z.data(), o.data());
    c.expect.push_back(o);
  }
  return c;
}

const Case& full_case() {
  static const Case c = make_case(m1::record_m1(m1::make_m1_book()));
  return c;
}
const Case& sub_case() {
  static const Case c = [] {
    const m1::Book full = m1::make_m1_book();
    std::vector<int> swaps;
    for (int i = 0; i < 60; ++i) swaps.push_back(i * 16 + 3);  // 60 swaps, seasoned and not
    return make_case(m1::record_m1(epykos::test::sub_book(full, swaps)));
  }();
  return c;
}

// Runs B states starting at `first` and compares every lane with the replay. Returns mismatches.
std::size_t check(const exec::Interpreter& in, const Case& c, int first, int B, const std::string& where) {
  const std::size_t K = static_cast<std::size_t>(in.n_inputs()), n_out = static_cast<std::size_t>(in.n_outputs());
  std::vector<double> state(K * static_cast<std::size_t>(B)), out(n_out * static_cast<std::size_t>(B));
  for (std::size_t k = 0; k < K; ++k) for (int b = 0; b < B; ++b) state[k * static_cast<std::size_t>(B) + static_cast<std::size_t>(b)] = c.states[static_cast<std::size_t>(first + b)][k];
  in.run(state.data(), B, out.data());
  std::size_t bad = 0;
  for (int b = 0; b < B; ++b) {
    for (std::size_t o = 0; o < n_out; ++o) {
      if (bits(out[o * static_cast<std::size_t>(B) + static_cast<std::size_t>(b)]) != bits(c.expect[static_cast<std::size_t>(first + b)][o])) {
        if (bad < 3) ADD_FAILURE() << where << ": B " << B << " lane " << b << " output " << o;
        ++bad;
      }
    }
  }
  return bad;
}

}  // namespace

TEST(ReviewInterp, RunDoesNotAllocate) {
  const Case& c = full_case();
  for (int lane_tile : {1, 8, 32, 64}) {
    for (int tile : {7, 256}) {
      exec::Options o;
      o.tile = tile;
      o.lane_tile = lane_tile;
      exec::Interpreter in(c.program, o);
      std::vector<double> state(12 * 64), out(1001 * 64);
      for (int k = 0; k < 12; ++k) for (int b = 0; b < 64; ++b) state[static_cast<std::size_t>(k * 64 + b)] = c.states[static_cast<std::size_t>(b + 1)][static_cast<std::size_t>(k)];
      const std::uint64_t before = g_allocations;
      in.run(state.data(), 1, out.data());
      in.run(state.data(), 64, out.data());
      in.run(state.data(), 3, out.data());
      in.run(state.data(), 17, out.data());
      EXPECT_EQ(g_allocations - before, 0u) << "tile " << tile << " lane_tile " << lane_tile;
    }
  }
}

TEST(ReviewInterp, OddBatchWidthsAcrossLaneTilesAndOddTiles) {
  const Case& c = sub_case();
  std::size_t bad = 0, runs = 0;
  for (int lane_tile : {1, 3, 8, 16, 32, 64}) {
    for (int tile : {2, 3, 5, 13, 100, 333, 1000, 5000}) {
      exec::Options o;
      o.tile = tile;
      o.lane_tile = lane_tile;
      exec::Interpreter in(c.program, o);
      const std::string where = "tile " + std::to_string(tile) + " lane_tile " + std::to_string(lane_tile);
      for (int B : {1, 3, 17, 33, 64}) {
        bad += check(in, c, 1, B, where);
        ++runs;
      }
    }
  }
  EXPECT_EQ(bad, 0u);
  std::cout << "[  review  ] odd B/tile sweep on a 60-swap book: " << runs << " runs, mismatches " << bad << '\n';
}

TEST(ReviewInterp, EveryFusionOptionCombinationIsBitIdentical) {
  const Case& c = full_case();
  std::size_t bad = 0, runs = 0;
  for (int mask = 0; mask < 8; ++mask) {
    for (int lane_tile : {1, 8, 32}) {
      for (int tile : {13, 256, 512}) {
        exec::Options o;
        o.tile = tile;
        o.lane_tile = lane_tile;
        o.fuse_reductions = (mask & 1) != 0;
        o.fuse_pairs = (mask & 2) != 0;
        o.inline_producers = (mask & 4) != 0;
        exec::Interpreter in(c.program, o);
        const std::string where = "fuse_reductions " + std::to_string(o.fuse_reductions) + " fuse_pairs " + std::to_string(o.fuse_pairs) +
                                  " inline_producers " + std::to_string(o.inline_producers) + " tile " + std::to_string(tile) + " lane_tile " + std::to_string(lane_tile);
        bad += check(in, c, 0, 1, where);
        bad += check(in, c, 1, 17, where);
        bad += check(in, c, 1, 64, where);
        runs += 3;
      }
    }
  }
  EXPECT_EQ(bad, 0u);
  std::cout << "[  review  ] fusion option sweep on the M1 book: " << runs << " runs, mismatches " << bad << '\n';
}

TEST(ReviewInterp, ExpPolyModeIsBitIdenticalAcrossBatchWidthsAndLaneTiles) {
  // E1 mode: not compared to the replay, but B = 64 must equal the B = 1 runs lane for lane, and
  // every lane tile must agree (exp_poly's fma steps are claimed bit-identical scalar vs vector).
  const Case& c = sub_case();
  exec::Options base;
  base.exp = exec::ExpMode::poly;
  base.lane_tile = 1;
  exec::Interpreter ref(c.program, base);
  const std::size_t n_out = static_cast<std::size_t>(ref.n_outputs());
  std::vector<std::vector<double>> single;
  for (int b = 1; b <= 64; ++b) {
    std::vector<double> o(n_out);
    ref.run(c.states[static_cast<std::size_t>(b)].data(), 1, o.data());
    single.push_back(o);
  }
  std::size_t bad = 0;
  for (int lane_tile : {3, 4, 8, 16, 32, 64}) {
    for (int tile : {5, 256}) {
      exec::Options o = base;
      o.lane_tile = lane_tile;
      o.tile = tile;
      exec::Interpreter in(c.program, o);
      for (int B : {3, 17, 64}) {
        std::vector<double> state(12 * static_cast<std::size_t>(B)), out(n_out * static_cast<std::size_t>(B));
        for (int k = 0; k < 12; ++k) for (int b = 0; b < B; ++b) state[static_cast<std::size_t>(k * B + b)] = c.states[static_cast<std::size_t>(b + 1)][static_cast<std::size_t>(k)];
        in.run(state.data(), B, out.data());
        for (int b = 0; b < B; ++b) for (std::size_t k = 0; k < n_out; ++k) {
          if (bits(out[k * static_cast<std::size_t>(B) + static_cast<std::size_t>(b)]) != bits(single[static_cast<std::size_t>(b)][k])) {
            if (bad < 3) ADD_FAILURE() << "exp_poly lane_tile " << lane_tile << " tile " << tile << " B " << B << " lane " << b << " output " << k;
            ++bad;
          }
        }
      }
    }
  }
  EXPECT_EQ(bad, 0u);
}

TEST(ReviewInterp, RawRecordingWithoutPassesRunsBitIdentically) {
  const m1::Book full = m1::make_m1_book();
  const Case c = make_case(m1::record_m1_raw(epykos::test::sub_book(full, {0, 1, 2, 300, 301, 302})));
  std::size_t bad = 0;
  for (int lane_tile : {1, 8, 32}) {
    exec::Options o;
    o.lane_tile = lane_tile;
    o.tile = 37;
    exec::Interpreter in(c.program, o);
    bad += check(in, c, 0, 1, "raw lane_tile " + std::to_string(lane_tile));
    bad += check(in, c, 1, 17, "raw lane_tile " + std::to_string(lane_tile));
    bad += check(in, c, 1, 64, "raw lane_tile " + std::to_string(lane_tile));
  }
  EXPECT_EQ(bad, 0u);
}

// Every input used exactly once as a scaled addend: the Input domain is fused into the affine
// (its rows are never materialised), an Input step then runs in the reduction block.
TEST(ReviewInterp, InputDomainFusedIntoAWholeDomainAffine) {
  Tape t;
  {
    Tape::Scope s(t);
    std::vector<Rec> x;
    for (int k = 0; k < 6; ++k) x.push_back(epykos::make_input(t, 0.01 * (k + 1)));
    epykos::register_output(t, 1.5 * x[0] + 2.5 * x[1] - x[2]);
    epykos::register_output(t, x[3] * 0.25 + 3.0 + x[4] * 4.0 - 7.0 * x[5]);
  }
  epykos::standard_passes(t);
  const ir::Program p = ir::infer(t);
  std::vector<std::vector<double>> states;
  for (int b = 0; b < 9; ++b) {
    std::vector<double> z(6);
    for (int k = 0; k < 6; ++k) z[static_cast<std::size_t>(k)] = 0.01 * (k + 1) + 0.001 * b;
    states.push_back(z);
  }
  Replayer rp(t);
  std::size_t bad = 0;
  for (int lane_tile : {1, 4, 9}) {
    exec::Options o;
    o.lane_tile = lane_tile;
    o.max_batch = 9;
    exec::Interpreter in(p, o);
    EXPECT_NE(in.describe().find("fused into"), std::string::npos) << in.describe();
    std::vector<double> state(6 * 9), out(2 * 9), expect(2);
    for (int k = 0; k < 6; ++k) for (int b = 0; b < 9; ++b) state[static_cast<std::size_t>(k * 9 + b)] = states[static_cast<std::size_t>(b)][static_cast<std::size_t>(k)];
    in.run(state.data(), 9, out.data());
    for (int b = 0; b < 9; ++b) {
      rp.run(states[static_cast<std::size_t>(b)].data(), expect.data());
      for (int k = 0; k < 2; ++k) {
        if (bits(out[static_cast<std::size_t>(k * 9 + b)]) != bits(expect[static_cast<std::size_t>(k)])) {
          ADD_FAILURE() << "lane_tile " << lane_tile << " lane " << b << " output " << k << ": " << out[static_cast<std::size_t>(k * 9 + b)] << " vs " << expect[static_cast<std::size_t>(k)];
          ++bad;
        }
      }
    }
  }
  EXPECT_EQ(bad, 0u);
}
