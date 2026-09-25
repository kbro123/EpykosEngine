// M3/G3 E0 gate: the tiled interpreter on the scan fixtures (D41) — the RFR compounding book
// (fixtures/rfr_book.hpp) and the affine scan (fixtures/affine_scan.hpp) — bitwise the templated
// double maths and the tape replay at the record point and at 64 batch states at B = 1; a
// batched run (B = 64) lane for lane bitwise the 64 single-state runs; every tile in {1, 7, 256,
// 4096} × lane tile in {1, 3, 8, 32, 64} the same bits; odd batch widths too. A scan is
// evaluated wave by wave: sequential along a chain, parallel across chains and batch lanes.
//
// This TU is compiled with -ffp-contract=off in every preset (the _e0_test.cpp convention); the
// interpreter's kernels are pinned the same way in libepykos (D25), so the gate holds under the
// release preset as well as the reference one. A gate of scripts/mutation_test.sh: the
// interpreter's scan mutant is caught here.
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "epykos/exec/interpreter.hpp"
#include "epykos/fixtures/affine_scan.hpp"
#include "epykos/fixtures/m1_book.hpp"
#include "epykos/fixtures/rfr_book.hpp"
#include "epykos/fixtures/rfr_price.hpp"
#include "epykos/ir/program.hpp"
#include "epykos/ir/signature.hpp"
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

// A recorded fixture with its states, the replay and the double oracle per state.
struct Case {
  std::string name;
  Tape tape;
  ir::Program program;
  std::vector<std::vector<double>> states;   // record point, then 64 draws
  std::vector<std::vector<double>> replay;
  std::vector<std::vector<double>> oracle;
};

Case rfr_case() {
  Case c;
  c.name = "RFR book";
  static const fixtures::RfrBook book = fixtures::make_rfr_book();
  c.tape = fixtures::record_rfr(book);
  c.program = ir::infer(c.tape);
  const fixtures::Batch batch = fixtures::make_m1_batch();  // 64 states around the same record point
  c.states.push_back(c.tape.input_values());
  for (int b = 0; b < batch.n_states; ++b) {
    std::vector<double> z(static_cast<std::size_t>(fixtures::n_knots));
    batch.state(b, z.data());
    c.states.push_back(z);
  }
  Replayer rp(c.tape);
  for (const std::vector<double>& z : c.states) {
    std::vector<double> out(c.tape.num_outputs());
    rp.run(z.data(), out.data());
    c.replay.push_back(out);
    std::vector<double> o(static_cast<std::size_t>(book.n_swaps) + 1);
    fixtures::rfr_reference_state(book, z.data(), o.data());
    c.oracle.push_back(o);
  }
  return c;
}

Case affine_case(bool output_path) {
  Case c;
  c.name = std::string("affine scan (") + (output_path ? "path" : "finals") + ")";
  static const fixtures::AffineScanFixture f = fixtures::make_affine_scan();
  c.tape = fixtures::record_affine_scan(f, output_path);
  c.program = ir::infer(c.tape);
  c.states = fixtures::affine_scan_states(f, 65);
  Replayer rp(c.tape);
  for (const std::vector<double>& z : c.states) {
    std::vector<double> out(c.tape.num_outputs());
    rp.run(z.data(), out.data());
    c.replay.push_back(out);
    c.oracle.push_back(fixtures::affine_scan_oracle(f, z, output_path));
  }
  return c;
}

const std::vector<Case>& cases() {
  static const std::vector<Case> all = [] {
    std::vector<Case> v;
    v.push_back(rfr_case());
    v.push_back(affine_case(false));
    v.push_back(affine_case(true));
    return v;
  }();
  return all;
}

std::size_t count_mismatches(const double* a, const double* b, std::size_t n, const std::string& what) {
  std::size_t bad = 0;
  for (std::size_t k = 0; k < n; ++k) {
    if (bits(a[k]) != bits(b[k])) {
      if (bad < 5) ADD_FAILURE() << what << ", output " << k << ": " << a[k] << " vs " << b[k];
      ++bad;
    }
  }
  return bad;
}

std::vector<std::vector<double>> run_single(const exec::Interpreter& in, const std::vector<std::vector<double>>& states) {
  std::vector<std::vector<double>> out;
  for (const std::vector<double>& z : states) {
    std::vector<double> o(static_cast<std::size_t>(in.n_outputs()));
    in.run(z.data(), 1, o.data());
    out.push_back(o);
  }
  return out;
}

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

TEST(ScanInterpE0, ReplayIsTheOracleBitwise) {
  for (const Case& c : cases()) {
    std::size_t bad = 0;
    for (std::size_t s = 0; s < c.states.size(); ++s) {
      bad += count_mismatches(c.replay[s].data(), c.oracle[s].data(), c.oracle[s].size(), c.name + " replay vs oracle, state " + std::to_string(s));
    }
    EXPECT_EQ(bad, 0u) << c.name;
  }
}

TEST(ScanInterpE0, SingleStateMatchesReplayAndOracleAtEveryState) {
  for (const Case& c : cases()) {
    exec::Interpreter in(c.program);
    ASSERT_FALSE(ir::scan_domains(c.program).empty()) << c.name;
    std::cout << "[  plan    ] " << c.name << "\n" << in.describe();
    std::size_t bad = 0;
    const std::vector<std::vector<double>> out = run_single(in, c.states);
    for (std::size_t s = 0; s < c.states.size(); ++s) {
      const std::string where = c.name + (s == 0 ? ", record point" : ", state " + std::to_string(s - 1));
      bad += count_mismatches(out[s].data(), c.replay[s].data(), out[s].size(), where + ": interpreter B=1 vs replay");
      bad += count_mismatches(out[s].data(), c.oracle[s].data(), out[s].size(), where + ": interpreter B=1 vs double");
    }
    EXPECT_EQ(bad, 0u) << c.name;
    std::cout << "[  states  ] " << c.name << ": " << c.states.size() << " states x " << in.n_outputs()
              << " outputs at B=1, mismatches " << bad << '\n';
  }
}

TEST(ScanInterpE0, BatchOf64EqualsThe64SingleStateRuns) {
  for (const Case& c : cases()) {
    const std::vector<std::vector<double>> batch(c.states.begin() + 1, c.states.end());
    ASSERT_EQ(batch.size(), 64u);
    exec::Interpreter in(c.program);
    const std::vector<std::vector<double>> single = run_single(in, batch);
    const std::vector<double> batched = run_batched(in, batch);
    const std::size_t n_out = static_cast<std::size_t>(in.n_outputs());
    std::size_t bad = 0;
    for (std::size_t b = 0; b < 64; ++b) {
      std::vector<double> lane(n_out);
      for (std::size_t o = 0; o < n_out; ++o) lane[o] = batched[o * 64 + b];
      bad += count_mismatches(lane.data(), single[b].data(), n_out, c.name + ": B=64 lane vs B=1 run, state " + std::to_string(b));
      bad += count_mismatches(lane.data(), c.oracle[b + 1].data(), n_out, c.name + ": B=64 lane vs oracle, state " + std::to_string(b));
    }
    EXPECT_EQ(bad, 0u) << c.name;
  }
}

TEST(ScanInterpE0, EveryTileAndLaneTileIsBitIdentical) {
  const int tiles[] = {1, 7, 256, 4096};
  const int lane_tiles[] = {1, 3, 8, 32, 64};
  for (const Case& c : cases()) {
    const std::vector<std::vector<double>> batch(c.states.begin() + 1, c.states.end());
    std::size_t bad = 0, runs = 0;
    for (int tile : tiles) {
      for (int lane_tile : lane_tiles) {
        exec::Options o;
        o.tile = tile;
        o.lane_tile = lane_tile;
        exec::Interpreter in(c.program, o);
        const std::string where = c.name + ": tile " + std::to_string(tile) + " lane_tile " + std::to_string(lane_tile);
        std::vector<double> rec(static_cast<std::size_t>(in.n_outputs()));
        in.run(c.states[0].data(), 1, rec.data());
        bad += count_mismatches(rec.data(), c.replay[0].data(), rec.size(), where + ", B=1 record point vs replay");
        const std::vector<double> batched = run_batched(in, batch);
        const std::size_t n_out = static_cast<std::size_t>(in.n_outputs());
        for (std::size_t b = 0; b < 64; ++b) {
          for (std::size_t k = 0; k < n_out; ++k) {
            if (bits(batched[k * 64 + b]) != bits(c.replay[b + 1][k])) {
              if (bad < 5) ADD_FAILURE() << where << ", state " << b << ", output " << k;
              ++bad;
            }
          }
        }
        ++runs;
      }
    }
    EXPECT_EQ(bad, 0u) << c.name;
    std::cout << "[  sweep   ] " << c.name << ": " << runs << " (tile, lane_tile) configurations, mismatches " << bad << '\n';
  }
}

TEST(ScanInterpE0, OddBatchWidthsUseTheRuntimeLaneKernelsBitIdentically) {
  for (const Case& c : cases()) {
    exec::Options o;
    o.lane_tile = 64;
    exec::Interpreter in(c.program, o);
    std::size_t bad = 0;
    for (int B : {2, 5, 13, 63}) {
      const std::vector<std::vector<double>> states(c.states.begin() + 1, c.states.begin() + 1 + B);
      const std::vector<double> batched = run_batched(in, states);
      const std::size_t n_out = static_cast<std::size_t>(in.n_outputs());
      for (std::size_t b = 0; b < static_cast<std::size_t>(B); ++b) {
        for (std::size_t k = 0; k < n_out; ++k) {
          if (bits(batched[k * static_cast<std::size_t>(B) + b]) != bits(c.replay[b + 1][k])) {
            if (bad < 5) ADD_FAILURE() << c.name << ": B " << B << ", state " << b << ", output " << k;
            ++bad;
          }
        }
      }
    }
    EXPECT_EQ(bad, 0u) << c.name;
  }
}
