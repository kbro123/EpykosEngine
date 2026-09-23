// M2/Q3 E0 gate: the adjoint's forward pass reproduces the tape replay, the double oracle and
// the interpreter bitwise on the M1 book; a batched run (B = 64) is lane for lane bitwise the 64
// single-state runs (forward outputs and state adjoints); every tile in {1, 7, 128, 256, 512,
// 4096} and lane tile in {1, 3, 4, 8, 16, 32, 64} gives the same bits; odd batch widths use the
// runtime-lane path bitwise; run() does not allocate.
//
// This TU is compiled with -ffp-contract=off in every preset (the _e0_test.cpp convention); the
// adjoint kernels are pinned the same way in their own TU (src/adjoint/adjoint_e0.cpp, D25), so
// the gate holds under the release preset as well as the reference one.
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
#include "epykos/fixtures/m1_book.hpp"
#include "epykos/fixtures/m1_reference.hpp"
#include "epykos/fixtures/record_m1.hpp"
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

constexpr int n_out = fixtures::n_swaps + 1;

struct Fixture {
  fixtures::Book book;
  fixtures::Batch batch;
  fixtures::ReferenceTable oracle;
  Tape tape;
  ir::Program program;
  std::vector<std::vector<double>> states;   // record point, then the 64 batch states
  std::vector<std::vector<double>> replay;   // tape replay per state
  std::vector<std::vector<double>> seeds;    // one out_bar per state (65 x 1001)
};

// A seed per state: the book PV for the record point, then random unit-scale seeds on every
// output (sub-stream 400000 + s, draw o), so the batched-vs-single comparison covers every path.
std::vector<double> seed_for(int s) {
  std::vector<double> ob(static_cast<std::size_t>(n_out), 0.0);
  if (s == 0) {
    ob[static_cast<std::size_t>(fixtures::n_swaps)] = 1.0;
    return ob;
  }
  epykos::rng::Philox g(fixtures::default_seed, 400000 + static_cast<std::uint64_t>(s));
  for (std::size_t o = 0; o < ob.size(); ++o) ob[o] = g.uniform_range(-1.0, 1.0);
  return ob;
}

const Fixture& fixture() {
  static const Fixture f = [] {
    Fixture x;
    x.book = fixtures::make_m1_book();
    x.batch = fixtures::make_m1_batch();
    x.oracle = fixtures::m1_reference_values(x.book, x.batch);
    x.tape = fixtures::record_m1(x.book);
    x.program = ir::infer(x.tape);
    x.states.push_back(x.tape.input_values());
    for (int b = 0; b < x.batch.n_states; ++b) {
      std::vector<double> z(static_cast<std::size_t>(fixtures::n_knots));
      x.batch.state(b, z.data());
      x.states.push_back(z);
    }
    Replayer rp(x.tape);
    for (const std::vector<double>& z : x.states) {
      std::vector<double> out(x.tape.num_outputs());
      rp.run(z.data(), out.data());
      x.replay.push_back(out);
    }
    for (std::size_t s = 0; s < x.states.size(); ++s) x.seeds.push_back(seed_for(static_cast<int>(s)));
    return x;
  }();
  return f;
}

struct Result {
  std::vector<double> out;        // n_out x B
  std::vector<double> state_bar;  // n_knots x B
};

// One batched run over states[s0 .. s0 + B) with their seeds.
Result run_batch(const adjoint::Adjoint& ad, const Fixture& f, std::size_t s0, int B) {
  const std::size_t Bs = static_cast<std::size_t>(B);
  std::vector<double> state(static_cast<std::size_t>(fixtures::n_knots) * Bs);
  std::vector<double> out_bar(static_cast<std::size_t>(n_out) * Bs);
  for (std::size_t b = 0; b < Bs; ++b) {
    for (std::size_t k = 0; k < static_cast<std::size_t>(fixtures::n_knots); ++k) state[k * Bs + b] = f.states[s0 + b][k];
    for (std::size_t o = 0; o < static_cast<std::size_t>(n_out); ++o) out_bar[o * Bs + b] = f.seeds[s0 + b][o];
  }
  Result r;
  r.out.assign(static_cast<std::size_t>(n_out) * Bs, 0.0);
  r.state_bar.assign(static_cast<std::size_t>(fixtures::n_knots) * Bs, 0.0);
  ad.run(state.data(), B, out_bar.data(), r.out.data(), r.state_bar.data());
  return r;
}

std::size_t count_mismatches(const double* a, const double* b, std::size_t n, const char* what, const std::string& where) {
  std::size_t bad = 0;
  for (std::size_t k = 0; k < n; ++k) {
    if (bits(a[k]) != bits(b[k])) {
      if (bad < 5) ADD_FAILURE() << what << ", " << where << ", entry " << k << ": " << a[k] << " vs " << b[k];
      ++bad;
    }
  }
  return bad;
}

// Compares lane b of a batched result with a B = 1 result.
std::size_t count_lane_mismatches(const Result& batched, int B, int b, const Result& single, const std::string& where) {
  std::size_t bad = 0;
  const std::size_t Bs = static_cast<std::size_t>(B);
  for (std::size_t o = 0; o < static_cast<std::size_t>(n_out); ++o) {
    if (bits(batched.out[o * Bs + static_cast<std::size_t>(b)]) != bits(single.out[o])) {
      if (bad < 5) ADD_FAILURE() << "forward, " << where << ", output " << o;
      ++bad;
    }
  }
  for (std::size_t k = 0; k < static_cast<std::size_t>(fixtures::n_knots); ++k) {
    const double x = batched.state_bar[k * Bs + static_cast<std::size_t>(b)];
    if (bits(x) != bits(single.state_bar[k])) {
      if (bad < 5) ADD_FAILURE() << "state_bar, " << where << ", knot " << k << ": " << x << " vs " << single.state_bar[k];
      ++bad;
    }
  }
  return bad;
}

}  // namespace

TEST(M1AdjointE0, ForwardMatchesReplayOracleAndInterpreterAtEveryState) {
  const Fixture& f = fixture();
  adjoint::Adjoint ad(f.program);
  epykos::exec::Interpreter in(f.program);
  ASSERT_EQ(ad.n_inputs(), 12);
  ASSERT_EQ(ad.n_outputs(), 1001);
  std::size_t mismatches = 0;
  std::vector<double> interp(static_cast<std::size_t>(n_out));
  for (std::size_t s = 0; s < f.states.size(); ++s) {
    const std::string where = s == 0 ? "record point" : "state " + std::to_string(s - 1);
    const Result r = run_batch(ad, f, s, 1);
    mismatches += count_mismatches(r.out.data(), f.replay[s].data(), r.out.size(), "adjoint forward vs replay", where);
    const double* oracle = s == 0 ? f.oracle.record.data() : f.oracle.state(static_cast<int>(s - 1));
    mismatches += count_mismatches(r.out.data(), oracle, r.out.size(), "adjoint forward vs price_book<double>", where);
    in.run(f.states[s].data(), 1, interp.data());
    mismatches += count_mismatches(r.out.data(), interp.data(), r.out.size(), "adjoint forward vs interpreter", where);
  }
  EXPECT_EQ(mismatches, 0u);
  std::cout << "[  states  ] " << f.states.size() << " states x " << n_out << " outputs at B=1, mismatches " << mismatches
            << '\n';
}

TEST(M1AdjointE0, ForwardOutputsMayBeNull) {
  const Fixture& f = fixture();
  adjoint::Adjoint ad(f.program);
  const Result with = run_batch(ad, f, 0, 1);
  std::vector<double> state_bar(static_cast<std::size_t>(fixtures::n_knots), 0.0);
  ad.run(f.states[0].data(), 1, f.seeds[0].data(), nullptr, state_bar.data());
  EXPECT_EQ(count_mismatches(state_bar.data(), with.state_bar.data(), state_bar.size(), "state_bar with out == null", "record point"), 0u);
}

TEST(M1AdjointE0, BatchOf64EqualsThe64SingleStateRuns) {
  const Fixture& f = fixture();
  adjoint::Adjoint ad(f.program);
  const Result batched = run_batch(ad, f, 1, 64);
  std::size_t mismatches = 0;
  for (int b = 0; b < 64; ++b) {
    const Result single = run_batch(ad, f, 1 + static_cast<std::size_t>(b), 1);
    mismatches += count_lane_mismatches(batched, 64, b, single, "B=64 lane " + std::to_string(b) + " vs B=1");
  }
  EXPECT_EQ(mismatches, 0u);
}

TEST(M1AdjointE0, EveryTileAndLaneTileIsBitIdentical) {
  const Fixture& f = fixture();
  const Result ref_rec = run_batch(adjoint::Adjoint(f.program), f, 0, 1);
  const Result ref_batch = run_batch(adjoint::Adjoint(f.program), f, 1, 64);
  const int tiles[] = {1, 7, 128, 256, 512, 4096};
  const int lane_tiles[] = {1, 3, 4, 8, 16, 32, 64};
  std::size_t mismatches = 0;
  std::size_t runs = 0;
  for (int tile : tiles) {
    for (int lane_tile : lane_tiles) {
      adjoint::Options o;
      o.tile = tile;
      o.lane_tile = lane_tile;
      adjoint::Adjoint ad(f.program, o);
      const std::string where = "tile " + std::to_string(tile) + " lane_tile " + std::to_string(lane_tile);
      const Result rec = run_batch(ad, f, 0, 1);
      mismatches += count_mismatches(rec.out.data(), ref_rec.out.data(), rec.out.size(), "B=1 forward", where);
      mismatches += count_mismatches(rec.state_bar.data(), ref_rec.state_bar.data(), rec.state_bar.size(), "B=1 state_bar", where);
      const Result batched = run_batch(ad, f, 1, 64);
      mismatches += count_mismatches(batched.out.data(), ref_batch.out.data(), batched.out.size(), "B=64 forward", where);
      mismatches += count_mismatches(batched.state_bar.data(), ref_batch.state_bar.data(), batched.state_bar.size(), "B=64 state_bar", where);
      ++runs;
    }
  }
  EXPECT_EQ(mismatches, 0u);
  std::cout << "[  sweep   ] " << runs << " (tile, lane_tile) configurations, mismatches " << mismatches << '\n';
}

TEST(M1AdjointE0, OddBatchWidthsUseTheRuntimeLanePathBitIdentically) {
  const Fixture& f = fixture();
  adjoint::Options o;
  o.lane_tile = 64;
  adjoint::Adjoint ad(f.program, o);
  std::size_t mismatches = 0;
  for (int B : {2, 3, 5, 7, 13, 31, 63}) {
    const Result batched = run_batch(ad, f, 1, B);
    for (int b = 0; b < B; ++b) {
      const Result single = run_batch(ad, f, 1 + static_cast<std::size_t>(b), 1);
      mismatches += count_lane_mismatches(batched, B, b, single, "B=" + std::to_string(B) + " lane " + std::to_string(b));
    }
  }
  EXPECT_EQ(mismatches, 0u);
}

TEST(M1AdjointE0, RunDoesNotAllocate) {
  const Fixture& f = fixture();
  adjoint::Adjoint ad(f.program);
  const int B = 64;
  const std::size_t Bs = static_cast<std::size_t>(B);
  std::vector<double> state(static_cast<std::size_t>(fixtures::n_knots) * Bs), out_bar(static_cast<std::size_t>(n_out) * Bs);
  std::vector<double> out(static_cast<std::size_t>(n_out) * Bs), state_bar(static_cast<std::size_t>(fixtures::n_knots) * Bs);
  for (std::size_t b = 0; b < Bs; ++b) {
    for (std::size_t k = 0; k < static_cast<std::size_t>(fixtures::n_knots); ++k) state[k * Bs + b] = f.states[1 + b][k];
    for (std::size_t o = 0; o < static_cast<std::size_t>(n_out); ++o) out_bar[o * Bs + b] = f.seeds[1 + b][o];
  }
  // The first call after construction must not allocate either.
  const std::uint64_t before = g_allocations;
  ad.run(state.data(), B, out_bar.data(), out.data(), state_bar.data());
  EXPECT_EQ(g_allocations - before, 0u);
  const std::uint64_t before2 = g_allocations;
  for (int rep = 0; rep < 3; ++rep) {
    ad.run(state.data(), B, out_bar.data(), out.data(), state_bar.data());
    ad.run(state.data(), 1, out_bar.data(), nullptr, state_bar.data());
  }
  EXPECT_EQ(g_allocations - before2, 0u);
  std::cout << "[  buffers ] values " << ad.value_bytes() << " B, edges " << ad.edge_bytes() << " B, scratch "
            << ad.scratch_bytes() << " B, tables " << ad.table_bytes() << " B\n";
}

TEST(M1AdjointE0, BadBatchWidthThrows) {
  const Fixture& f = fixture();
  adjoint::Options o;
  o.max_batch = 4;
  adjoint::Adjoint ad(f.program, o);
  std::vector<double> state(48), out_bar(static_cast<std::size_t>(n_out) * 8), out(static_cast<std::size_t>(n_out) * 8), sb(96);
  EXPECT_THROW(ad.run(state.data(), 0, out_bar.data(), out.data(), sb.data()), std::invalid_argument);
  EXPECT_THROW(ad.run(state.data(), 5, out_bar.data(), out.data(), sb.data()), std::invalid_argument);
  EXPECT_NO_THROW(ad.run(f.states[0].data(), 1, out_bar.data(), out.data(), sb.data()));
}

TEST(M1AdjointE0, DescribeNamesThePlan) {
  const Fixture& f = fixture();
  adjoint::Adjoint ad(f.program);
  const std::string d = ad.describe();
  EXPECT_NE(d.find("adjoint plan: 10 domains, 42314 values"), std::string::npos) << d.substr(0, 400);
  EXPECT_NE(d.find("affine: coef * segbar"), std::string::npos);
  std::cout << d.substr(0, d.find('\n')) << '\n' << d.substr(d.find('\n') + 1, d.find("\n  reverse domain 8") - d.find('\n')) ;
}
