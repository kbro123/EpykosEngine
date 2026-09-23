// M3/G3 E0 gate: the adjoint on the scan fixtures (D37) — the RFR compounding book and the
// affine scan — its forward pass bitwise the tape replay and the interpreter at the record
// point and 64 states; a batched run (B = 64) lane for lane bitwise the 64 single-state runs
// (forward outputs and state adjoints); every tile in {1, 7, 256} × lane tile in {1, 3, 8, 64}
// the same bits; run() allocation-free.
//
// This TU is compiled with -ffp-contract=off in every preset (the _e0_test.cpp convention); the
// adjoint kernels are pinned the same way in their own TU (src/adjoint/adjoint_e0.cpp, D25).
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <new>
#include <string>
#include <vector>

#include "epykos/adjoint/adjoint.hpp"
#include "epykos/exec/interpreter.hpp"
#include "epykos/fixtures/affine_scan.hpp"
#include "epykos/fixtures/m1_book.hpp"
#include "epykos/fixtures/rfr_book.hpp"
#include "epykos/fixtures/rfr_price.hpp"
#include "epykos/ir/program.hpp"
#include "epykos/ir/signature.hpp"
#include "epykos/rng/philox.hpp"
#include "epykos/tape/replay.hpp"
#include "epykos/tape/tape.hpp"

#ifndef EPYKOS_FP_CONTRACT_OFF
#error "an _e0_test.cpp TU must see EPYKOS_FP_CONTRACT_OFF"
#endif

namespace fixtures = epykos::fixtures;
namespace ir = epykos::ir;
namespace adjoint = epykos::adjoint;
namespace exec = epykos::exec;
using epykos::Replayer;
using epykos::Tape;

// ---- allocation counter: every operator new in this program passes through here.
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

namespace {

std::uint64_t bits(double v) {
  std::uint64_t u;
  std::memcpy(&u, &v, sizeof u);
  return u;
}

struct Case {
  std::string name;
  Tape tape;
  ir::Program program;
  int n_in = 0, n_out = 0;
  std::vector<std::vector<double>> states;   // record point, then 64
  std::vector<std::vector<double>> replay;
  std::vector<std::vector<double>> seeds;    // out_bar per state
};

std::vector<double> seed_for(int s, int n_out) {
  std::vector<double> ob(static_cast<std::size_t>(n_out), 0.0);
  if (s == 0) {
    ob.back() = 1.0;  // the book PV / the mean
    return ob;
  }
  epykos::rng::Philox g(fixtures::default_seed, 540000 + static_cast<std::uint64_t>(s));
  for (double& v : ob) v = g.uniform_range(-1.0, 1.0);
  return ob;
}

void finish(Case& c) {
  c.program = ir::infer(c.tape);
  c.n_in = static_cast<int>(c.tape.num_inputs());
  c.n_out = static_cast<int>(c.tape.num_outputs());
  Replayer rp(c.tape);
  for (const std::vector<double>& z : c.states) {
    std::vector<double> out(c.tape.num_outputs());
    rp.run(z.data(), out.data());
    c.replay.push_back(out);
  }
  for (std::size_t s = 0; s < c.states.size(); ++s) c.seeds.push_back(seed_for(static_cast<int>(s), c.n_out));
}

const std::vector<Case>& cases() {
  static const std::vector<Case> all = [] {
    std::vector<Case> v;
    {
      Case c;
      c.name = "RFR book";
      const fixtures::RfrBook book = fixtures::make_rfr_book();
      c.tape = fixtures::record_rfr(book);
      const fixtures::Batch batch = fixtures::make_m1_batch();
      c.states.push_back(c.tape.input_values());
      for (int b = 0; b < batch.n_states; ++b) {
        std::vector<double> z(static_cast<std::size_t>(fixtures::n_knots));
        batch.state(b, z.data());
        c.states.push_back(z);
      }
      finish(c);
      v.push_back(std::move(c));
    }
    for (const bool output_path : {false, true}) {
      Case c;
      c.name = std::string("affine scan (") + (output_path ? "path" : "finals") + ")";
      const fixtures::AffineScanFixture f = fixtures::make_affine_scan();
      c.tape = fixtures::record_affine_scan(f, output_path);
      c.states = fixtures::affine_scan_states(f, 65);
      finish(c);
      v.push_back(std::move(c));
    }
    return v;
  }();
  return all;
}

// Forward outputs and state adjoints of one state at B = 1.
struct Single {
  std::vector<double> out, state_bar;
};
Single run_single(const adjoint::Adjoint& ad, const std::vector<double>& z, const std::vector<double>& ob) {
  Single s;
  s.out.resize(static_cast<std::size_t>(ad.n_outputs()));
  s.state_bar.resize(static_cast<std::size_t>(ad.n_inputs()));
  ad.run(z.data(), 1, ob.data(), s.out.data(), s.state_bar.data());
  return s;
}

std::size_t count_mismatches(const double* a, const double* b, std::size_t n, const std::string& what) {
  std::size_t bad = 0;
  for (std::size_t k = 0; k < n; ++k) {
    if (bits(a[k]) != bits(b[k])) {
      if (bad < 5) ADD_FAILURE() << what << ", entry " << k << ": " << a[k] << " vs " << b[k];
      ++bad;
    }
  }
  return bad;
}

}  // namespace

TEST(ScanAdjointE0, ForwardIsTheReplayAndTheInterpreterBitwise) {
  for (const Case& c : cases()) {
    adjoint::Adjoint ad(c.program);
    exec::Interpreter in(c.program);
    std::cout << "[  plan    ] " << c.name << "\n" << ad.describe();
    std::size_t bad = 0;
    std::vector<double> interp(static_cast<std::size_t>(c.n_out));
    for (std::size_t s = 0; s < c.states.size(); ++s) {
      const Single r = run_single(ad, c.states[s], c.seeds[s]);
      bad += count_mismatches(r.out.data(), c.replay[s].data(), r.out.size(), c.name + ": adjoint forward vs replay, state " + std::to_string(s));
      in.run(c.states[s].data(), 1, interp.data());
      bad += count_mismatches(r.out.data(), interp.data(), r.out.size(), c.name + ": adjoint forward vs interpreter, state " + std::to_string(s));
    }
    EXPECT_EQ(bad, 0u) << c.name;
  }
}

TEST(ScanAdjointE0, BatchOf64IsLaneForLaneTheSingleStateRuns) {
  for (const Case& c : cases()) {
    adjoint::Adjoint ad(c.program);
    const int B = 64;
    const std::size_t Bs = 64, K = static_cast<std::size_t>(c.n_in), O = static_cast<std::size_t>(c.n_out);
    std::vector<double> state(K * Bs), ob(O * Bs), out(O * Bs), sb(K * Bs);
    for (std::size_t b = 0; b < Bs; ++b) {
      for (std::size_t k = 0; k < K; ++k) state[k * Bs + b] = c.states[b + 1][k];
      for (std::size_t o = 0; o < O; ++o) ob[o * Bs + b] = c.seeds[b + 1][o];
    }
    ad.run(state.data(), B, ob.data(), out.data(), sb.data());
    std::size_t bad = 0;
    for (std::size_t b = 0; b < Bs; ++b) {
      const Single r = run_single(ad, c.states[b + 1], c.seeds[b + 1]);
      std::vector<double> lane_out(O), lane_sb(K);
      for (std::size_t o = 0; o < O; ++o) lane_out[o] = out[o * Bs + b];
      for (std::size_t k = 0; k < K; ++k) lane_sb[k] = sb[k * Bs + b];
      bad += count_mismatches(lane_out.data(), r.out.data(), O, c.name + ": B=64 outputs lane " + std::to_string(b));
      bad += count_mismatches(lane_sb.data(), r.state_bar.data(), K, c.name + ": B=64 state adjoints lane " + std::to_string(b));
    }
    EXPECT_EQ(bad, 0u) << c.name;
  }
}

TEST(ScanAdjointE0, EveryTileAndLaneTileIsBitIdentical) {
  for (const Case& c : cases()) {
    adjoint::Adjoint reference(c.program);
    std::vector<Single> expect;
    for (std::size_t s = 0; s < c.states.size(); s += 8) expect.push_back(run_single(reference, c.states[s], c.seeds[s]));
    std::size_t bad = 0, runs = 0;
    for (int tile : {1, 7, 256}) {
      for (int lane_tile : {1, 3, 8, 64}) {
        adjoint::Options o;
        o.tile = tile;
        o.lane_tile = lane_tile;
        adjoint::Adjoint ad(c.program, o);
        const std::string where = c.name + ": tile " + std::to_string(tile) + " lane_tile " + std::to_string(lane_tile);
        std::size_t e = 0;
        for (std::size_t s = 0; s < c.states.size(); s += 8, ++e) {
          const Single r = run_single(ad, c.states[s], c.seeds[s]);
          bad += count_mismatches(r.out.data(), expect[e].out.data(), r.out.size(), where + " outputs, state " + std::to_string(s));
          bad += count_mismatches(r.state_bar.data(), expect[e].state_bar.data(), r.state_bar.size(), where + " state adjoints, state " + std::to_string(s));
        }
        // And a batch of the same states, lane for lane.
        const int B = static_cast<int>(expect.size());
        const std::size_t Bs = static_cast<std::size_t>(B), K = static_cast<std::size_t>(c.n_in), O = static_cast<std::size_t>(c.n_out);
        std::vector<double> state(K * Bs), ob(O * Bs), out(O * Bs), sb(K * Bs);
        for (std::size_t b = 0; b < Bs; ++b) {
          for (std::size_t k = 0; k < K; ++k) state[k * Bs + b] = c.states[b * 8][k];
          for (std::size_t oo = 0; oo < O; ++oo) ob[oo * Bs + b] = c.seeds[b * 8][oo];
        }
        ad.run(state.data(), B, ob.data(), out.data(), sb.data());
        for (std::size_t b = 0; b < Bs; ++b) {
          for (std::size_t oo = 0; oo < O; ++oo) bad += bits(out[oo * Bs + b]) != bits(expect[b].out[oo]);
          for (std::size_t k = 0; k < K; ++k) bad += bits(sb[k * Bs + b]) != bits(expect[b].state_bar[k]);
        }
        ++runs;
      }
    }
    EXPECT_EQ(bad, 0u) << c.name;
    std::cout << "[  sweep   ] " << c.name << ": " << runs << " (tile, lane_tile) configurations, mismatches " << bad << '\n';
  }
}

TEST(ScanAdjointE0, RunDoesNotAllocate) {
  for (const Case& c : cases()) {
    adjoint::Adjoint ad(c.program);
    const std::size_t K = static_cast<std::size_t>(c.n_in), O = static_cast<std::size_t>(c.n_out);
    std::vector<double> state(K * 8), ob(O * 8), out(O * 8), sb(K * 8);
    for (std::size_t b = 0; b < 8; ++b) {
      for (std::size_t k = 0; k < K; ++k) state[k * 8 + b] = c.states[b + 1][k];
      for (std::size_t o = 0; o < O; ++o) ob[o * 8 + b] = c.seeds[b + 1][o];
    }
    ad.run(state.data(), 8, ob.data(), out.data(), sb.data());  // warm
    const std::uint64_t before = g_allocations;
    ad.run(state.data(), 8, ob.data(), out.data(), sb.data());
    ad.run(c.states[0].data(), 1, c.seeds[0].data(), out.data(), sb.data());
    EXPECT_EQ(g_allocations, before) << c.name << ": run() allocated";
  }
}
