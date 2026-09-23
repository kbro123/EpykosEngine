// M1/P4 E0 gate: the tiled interpreter reproduces the tape replay (P2) and the templated double
// maths (P1) bitwise on the M1 book — at B = 1 for the record point and each of the 64 batch
// states; at B = 64 in one call, lane for lane equal to the 64 single-state runs; for every tile
// in {1, 7, 128, 256, 512, 4096} and every lane tile in {1, 3, 4, 8, 16, 32, 64}; and on the three
// smaller books of the P3 gate. Odd batch widths (the runtime-lane-count kernels) are covered too.
//
// This TU is compiled with -ffp-contract=off in every preset (the _e0_test.cpp convention), so
// the oracle, the record-point values and the replay carry no fused multiply-adds. The
// interpreter's kernels are compiled into libepykos with contraction pinned off in their own
// TUs (src/exec/kernels_l*.cpp), so this gate holds under the release preset as well as the
// reference one; CI runs both.
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "epykos/exec/interpreter.hpp"
#include "epykos/ir/program.hpp"
#include "epykos/ir/signature.hpp"
#include "epykos/maths/m1/book.hpp"
#include "epykos/maths/m1/reference.hpp"
#include "epykos/tape/record_m1.hpp"
#include "epykos/tape/replay.hpp"
#include "epykos/tape/tape.hpp"
#include "ir/ir_test_helpers.hpp"

#ifndef EPYKOS_FP_CONTRACT_OFF
#error "an _e0_test.cpp TU must see EPYKOS_FP_CONTRACT_OFF"
#endif

namespace m1 = epykos::m1;
namespace ir = epykos::ir;
namespace exec = epykos::exec;
using epykos::Replayer;
using epykos::Tape;

namespace {

std::uint64_t bits(double v) {
  std::uint64_t u;
  std::memcpy(&u, &v, sizeof u);
  return u;
}

struct Fixture {
  m1::Book book;
  m1::Batch batch;
  m1::ReferenceTable oracle;       // price_book<double> in this TU: the P1 oracle
  Tape tape;                       // record_m1: the P2 tape after the E0 passes
  ir::Program program;             // infer: the P3 IR
  std::vector<std::vector<double>> states;   // record point, then the 64 batch states
  std::vector<std::vector<double>> replay;   // replay of the tape per state (1001 outputs)
};

const Fixture& fixture() {
  static const Fixture f = [] {
    Fixture x;
    x.book = m1::make_m1_book();
    x.batch = m1::make_m1_batch();
    x.oracle = m1::m1_reference_values(x.book, x.batch);
    x.tape = m1::record_m1(x.book);
    x.program = ir::infer(x.tape);
    x.states.push_back(x.tape.input_values());
    for (int b = 0; b < x.batch.n_states; ++b) {
      std::vector<double> z(static_cast<std::size_t>(m1::n_knots));
      x.batch.state(b, z.data());
      x.states.push_back(z);
    }
    Replayer rp(x.tape);
    for (const std::vector<double>& z : x.states) {
      std::vector<double> out(x.tape.num_outputs());
      rp.run(z.data(), out.data());
      x.replay.push_back(out);
    }
    return x;
  }();
  return f;
}

std::size_t count_mismatches(const double* a, const double* b, std::size_t n, const char* what, const std::string& where) {
  std::size_t bad = 0;
  for (std::size_t k = 0; k < n; ++k) {
    if (bits(a[k]) != bits(b[k])) {
      if (bad < 5) ADD_FAILURE() << what << ", " << where << ", output " << k << ": " << a[k] << " vs " << b[k];
      ++bad;
    }
  }
  return bad;
}

// The interpreter at B = 1 on every state: outputs per state.
std::vector<std::vector<double>> run_single(const exec::Interpreter& in, const std::vector<std::vector<double>>& states) {
  std::vector<std::vector<double>> out;
  for (const std::vector<double>& z : states) {
    std::vector<double> o(static_cast<std::size_t>(in.n_outputs()));
    in.run(z.data(), 1, o.data());
    out.push_back(o);
  }
  return out;
}

// The interpreter at B = states.size() in one call: out[o·B + b].
std::vector<double> run_batched(const exec::Interpreter& in, const std::vector<std::vector<double>>& states) {
  const int B = static_cast<int>(states.size());
  const std::size_t K = static_cast<std::size_t>(in.n_inputs());
  std::vector<double> state(K * static_cast<std::size_t>(B));
  for (std::size_t k = 0; k < K; ++k) {
    for (int b = 0; b < B; ++b) state[k * static_cast<std::size_t>(B) + static_cast<std::size_t>(b)] = states[static_cast<std::size_t>(b)][k];
  }
  std::vector<double> out(static_cast<std::size_t>(in.n_outputs()) * static_cast<std::size_t>(B));
  in.run(state.data(), B, out.data());
  return out;
}

}  // namespace

TEST(M1InterpE0, SingleStateMatchesReplayAndOracleAtEveryState) {
  const Fixture& f = fixture();
  exec::Interpreter in(f.program);
  ASSERT_EQ(in.n_inputs(), 12);
  ASSERT_EQ(in.n_outputs(), 1001);
  std::size_t mismatches = 0;
  const std::vector<std::vector<double>> out = run_single(in, f.states);
  for (std::size_t s = 0; s < f.states.size(); ++s) {
    const std::string where = s == 0 ? "record point" : "state " + std::to_string(s - 1);
    mismatches += count_mismatches(out[s].data(), f.replay[s].data(), out[s].size(), "interpreter B=1 vs replay", where);
    const double* oracle = s == 0 ? f.oracle.record.data() : f.oracle.state(static_cast<int>(s - 1));
    mismatches += count_mismatches(out[s].data(), oracle, out[s].size(), "interpreter B=1 vs price_book<double>", where);
  }
  EXPECT_EQ(mismatches, 0u);
  std::cout << "[  states  ] " << f.states.size() << " states x " << in.n_outputs() << " outputs at B=1, mismatches "
            << mismatches << '\n';
}

TEST(M1InterpE0, BatchOf64EqualsThe64SingleStateRuns) {
  const Fixture& f = fixture();
  const std::vector<std::vector<double>> batch(f.states.begin() + 1, f.states.end());
  ASSERT_EQ(batch.size(), 64u);
  exec::Interpreter in(f.program);
  const std::vector<std::vector<double>> single = run_single(in, batch);
  const std::vector<double> batched = run_batched(in, batch);
  std::size_t mismatches = 0;
  const std::size_t n_out = static_cast<std::size_t>(in.n_outputs());
  for (std::size_t b = 0; b < 64; ++b) {
    std::vector<double> lane(n_out);
    for (std::size_t o = 0; o < n_out; ++o) lane[o] = batched[o * 64 + b];
    mismatches += count_mismatches(lane.data(), single[b].data(), n_out, "B=64 lane vs B=1 run", "state " + std::to_string(b));
    mismatches += count_mismatches(lane.data(), f.oracle.state(static_cast<int>(b)), n_out, "B=64 lane vs oracle",
                                   "state " + std::to_string(b));
  }
  EXPECT_EQ(mismatches, 0u);
}

TEST(M1InterpE0, EveryTileAndLaneTileIsBitIdentical) {
  const Fixture& f = fixture();
  const std::vector<std::vector<double>> batch(f.states.begin() + 1, f.states.end());
  const int tiles[] = {1, 7, 128, 256, 512, 4096};
  const int lane_tiles[] = {1, 3, 4, 8, 16, 32, 64};
  std::size_t mismatches = 0;
  std::size_t runs = 0;
  for (int tile : tiles) {
    for (int lane_tile : lane_tiles) {
      exec::Options o;
      o.tile = tile;
      o.lane_tile = lane_tile;
      exec::Interpreter in(f.program, o);
      const std::string where = "tile " + std::to_string(tile) + " lane_tile " + std::to_string(lane_tile);
      // B = 1 at the record point and B = 64 over the batch.
      std::vector<double> rec(static_cast<std::size_t>(in.n_outputs()));
      in.run(f.states[0].data(), 1, rec.data());
      mismatches += count_mismatches(rec.data(), f.replay[0].data(), rec.size(), "B=1 record point vs replay", where);
      const std::vector<double> batched = run_batched(in, batch);
      const std::size_t n_out = static_cast<std::size_t>(in.n_outputs());
      for (std::size_t b = 0; b < 64; ++b) {
        for (std::size_t o = 0; o < n_out; ++o) {
          if (bits(batched[o * 64 + b]) != bits(f.replay[b + 1][o])) {
            if (mismatches < 5) ADD_FAILURE() << where << ", state " << b << ", output " << o;
            ++mismatches;
          }
        }
      }
      ++runs;
    }
  }
  EXPECT_EQ(mismatches, 0u);
  std::cout << "[  sweep   ] " << runs << " (tile, lane_tile) configurations, mismatches " << mismatches << '\n';
}

TEST(M1InterpE0, OddBatchWidthsUseTheRuntimeLaneKernelsBitIdentically) {
  const Fixture& f = fixture();
  exec::Options o;
  o.lane_tile = 64;
  exec::Interpreter in(f.program, o);
  std::size_t mismatches = 0;
  for (int B : {2, 3, 5, 7, 13, 31, 63}) {
    const std::vector<std::vector<double>> states(f.states.begin() + 1, f.states.begin() + 1 + B);
    const std::vector<double> batched = run_batched(in, states);
    const std::size_t n_out = static_cast<std::size_t>(in.n_outputs());
    for (std::size_t b = 0; b < static_cast<std::size_t>(B); ++b) {
      for (std::size_t o = 0; o < n_out; ++o) {
        if (bits(batched[o * static_cast<std::size_t>(B) + b]) != bits(f.replay[b + 1][o])) {
          if (mismatches < 5) ADD_FAILURE() << "B " << B << ", state " << b << ", output " << o;
          ++mismatches;
        }
      }
    }
  }
  EXPECT_EQ(mismatches, 0u);
}

TEST(M1InterpE0, SmallerBooksAgreeWithReplayAndOracle) {
  const Fixture& f = fixture();
  const std::vector<std::vector<int>> cases = {{300}, {3}, {0, 1, 2, 3, 4, 200, 201, 202, 203, 204}};
  for (const std::vector<int>& swaps : cases) {
    const m1::Book sub = epykos::test::sub_book(f.book, swaps);
    const Tape tape = m1::record_m1(sub);
    const ir::Program program = ir::infer(tape);
    Replayer rp(tape);
    exec::Interpreter in(program);
    std::vector<double> expect(tape.num_outputs()), oracle(static_cast<std::size_t>(sub.n_swaps) + 1);
    std::size_t mismatches = 0;
    const std::vector<std::vector<double>> single = run_single(in, f.states);
    for (std::size_t s = 0; s < f.states.size(); ++s) {
      rp.run(f.states[s].data(), expect.data());
      m1::m1_reference_state(sub, f.states[s].data(), oracle.data());
      mismatches += count_mismatches(single[s].data(), expect.data(), expect.size(), "sub-book B=1 vs replay", "");
      mismatches += count_mismatches(single[s].data(), oracle.data(), oracle.size(), "sub-book B=1 vs oracle", "");
    }
    const std::vector<std::vector<double>> batch(f.states.begin() + 1, f.states.end());
    const std::vector<double> batched = run_batched(in, batch);
    for (std::size_t b = 0; b < 64; ++b) {
      for (std::size_t o = 0; o < expect.size(); ++o) {
        if (bits(batched[o * 64 + b]) != bits(single[b + 1][o])) {
          if (mismatches < 5) ADD_FAILURE() << swaps.size() << " swaps, B=64 state " << b << ", output " << o;
          ++mismatches;
        }
      }
    }
    EXPECT_EQ(mismatches, 0u) << swaps.size() << " swaps";
  }
}
