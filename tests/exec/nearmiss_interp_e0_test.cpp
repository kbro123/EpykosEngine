// M2/Q4 gate: the tiled interpreter on the near-miss shapes fixture (fixtures/nearmiss_shapes.hpp)
// — every op with a constant in every operand position, select with constant arms, sums of two to
// four members, affine chains with and without a leading constant — bitwise the tape replay at
// B = 1 for every state of a 64-draw ball and at B = 64 lane for lane, over several tiles and
// lane tiles. Complements exec_m1_interp_e0_test (the M1 book) and exec_interp_e0_test (one row
// per shape) with shapes that are classes of several rows and near misses of one another.
//
// This TU is compiled with -ffp-contract=off in every preset (the _e0_test.cpp convention); the
// interpreter's kernels are pinned the same way in libepykos (D25).
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "epykos/exec/interpreter.hpp"
#include "epykos/fixtures/nearmiss_shapes.hpp"
#include "epykos/ir/program.hpp"
#include "epykos/ir/signature.hpp"
#include "epykos/tape/passes.hpp"
#include "epykos/tape/replay.hpp"
#include "epykos/tape/tape.hpp"

#ifndef EPYKOS_FP_CONTRACT_OFF
#error "an _e0_test.cpp TU must see EPYKOS_FP_CONTRACT_OFF"
#endif

namespace fixtures = epykos::fixtures;
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

constexpr int n_ball = 64;

struct Fixture {
  Tape tape;                                 // after the standard passes
  ir::Program program;
  std::vector<std::vector<double>> states;   // record point, then 63 draws
  std::vector<std::vector<double>> replay;   // per state
  std::vector<std::vector<double>> oracle;   // per state, the double shapes in this TU
};

const Fixture& fixture() {
  static const Fixture f = [] {
    Fixture x;
    x.tape = fixtures::record_nearmiss();
    epykos::standard_passes(x.tape);
    x.program = ir::infer(x.tape);
    x.states = fixtures::nearmiss_states(n_ball);
    Replayer rp(x.tape);
    for (const std::vector<double>& s : x.states) {
      std::vector<double> out(x.tape.num_outputs());
      rp.run(s.data(), out.data());
      x.replay.push_back(out);
      x.oracle.push_back(fixtures::nearmiss_oracle(s));
    }
    return x;
  }();
  return f;
}

// out[o·B + b] for the states [first, first + B).
std::vector<double> run_batched(const exec::Interpreter& in, const std::vector<std::vector<double>>& states, std::size_t first, int B) {
  const std::size_t K = static_cast<std::size_t>(in.n_inputs());
  std::vector<double> state(K * static_cast<std::size_t>(B));
  for (std::size_t k = 0; k < K; ++k) {
    for (int b = 0; b < B; ++b) state[k * static_cast<std::size_t>(B) + static_cast<std::size_t>(b)] = states[first + static_cast<std::size_t>(b)][k];
  }
  std::vector<double> out(static_cast<std::size_t>(in.n_outputs()) * static_cast<std::size_t>(B));
  in.run(state.data(), B, out.data());
  return out;
}

}  // namespace

TEST(NearmissInterpE0, ReplayIsTheOracleBitwise) {
  const Fixture& f = fixture();
  std::size_t mismatches = 0;
  for (std::size_t s = 0; s < f.states.size(); ++s) {
    for (std::size_t o = 0; o < f.replay[s].size(); ++o) {
      if (bits(f.replay[s][o]) != bits(f.oracle[s][o])) {
        if (mismatches < 5) ADD_FAILURE() << "state " << s << ", output " << o << ": " << f.replay[s][o] << " vs " << f.oracle[s][o];
        ++mismatches;
      }
    }
  }
  EXPECT_EQ(mismatches, 0u);
}

TEST(NearmissInterpE0, SingleStateAndBatchedMatchTheReplayAcrossTilesAndLaneTiles) {
  const Fixture& f = fixture();
  const int tiles[] = {1, 7, 256};
  const int lane_tiles[] = {1, 3, 8, 64};
  std::size_t mismatches = 0, runs = 0;
  for (int tile : tiles) {
    for (int lane_tile : lane_tiles) {
      exec::Options o;
      o.tile = tile;
      o.lane_tile = lane_tile;
      exec::Interpreter in(f.program, o);
      ASSERT_EQ(in.n_inputs(), fixtures::nearmiss_inputs);
      ASSERT_EQ(static_cast<std::size_t>(in.n_outputs()), f.tape.num_outputs());
      const std::string where = "tile " + std::to_string(tile) + " lane_tile " + std::to_string(lane_tile);
      const std::size_t n_out = static_cast<std::size_t>(in.n_outputs());
      // B = 1 at every state.
      std::vector<double> single(n_out);
      for (std::size_t s = 0; s < f.states.size(); ++s) {
        in.run(f.states[s].data(), 1, single.data());
        for (std::size_t k = 0; k < n_out; ++k) {
          if (bits(single[k]) != bits(f.replay[s][k])) {
            if (mismatches < 5) ADD_FAILURE() << where << ", B=1 state " << s << ", output " << k << ": " << single[k] << " vs " << f.replay[s][k];
            ++mismatches;
          }
        }
      }
      // B = 64 over the whole ball in one call.
      const std::vector<double> batched = run_batched(in, f.states, 0, n_ball);
      for (std::size_t b = 0; b < static_cast<std::size_t>(n_ball); ++b) {
        for (std::size_t k = 0; k < n_out; ++k) {
          if (bits(batched[k * static_cast<std::size_t>(n_ball) + b]) != bits(f.replay[b][k])) {
            if (mismatches < 5) ADD_FAILURE() << where << ", B=64 lane " << b << ", output " << k;
            ++mismatches;
          }
        }
      }
      ++runs;
    }
  }
  EXPECT_EQ(mismatches, 0u);
  std::cout << "[  sweep   ] " << runs << " (tile, lane_tile) configurations x " << n_ball << " states, mismatches " << mismatches << '\n';
}
