// M2/Q3: timing of value + adjoint on the M1 book, single state (B = 1) and batched (B = 64),
// with the conventions of bench/exec/m1_interp_bench.cpp and bench/hand/m1_hand_bench.cpp
// (tables prebuilt in the constructor, the state written fresh each repetition, warm): one
// Adjoint::run computes the 1,001 outputs and d(book pv)/dz (out_bar = e_book) per lane. The
// interpreter's forward-only rows in exec_m1_interp_bench are the value-only reference; the
// ratio adjoint / value is the informational figure (D9), never a gate in M2.
//
// Arguments: {tile, lane_tile}. The tile sweep of D15 (128 / 256 / 512) and a lane-tile sweep
// for B = 64 are registered; EPYKOS_TILE / EPYKOS_LANE_TILE replace a sweep by one value.
//
// Run, e.g.:
//   ./build/release/bench/adjoint_m1_adjoint_bench --benchmark_repetitions=20 --benchmark_min_time=0.2s \
//       --benchmark_report_aggregates_only=false --benchmark_out_format=json \
//       --benchmark_out=bench/results/<fingerprint>/m2_adjoint.json
// and record scripts/fingerprint.sh (with its load average) alongside.
#include <benchmark/benchmark.h>

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "epykos/adjoint/adjoint.hpp"
#include "epykos/fixtures/m1_book.hpp"
#include "epykos/fixtures/record_m1.hpp"
#include "epykos/ir/program.hpp"
#include "epykos/ir/signature.hpp"
#include "epykos/tape/tape.hpp"

namespace {

namespace fixtures = epykos::fixtures;
namespace ir = epykos::ir;
using epykos::adjoint::Adjoint;
using epykos::adjoint::Options;

constexpr int n_out = fixtures::n_swaps + 1;

const fixtures::Book& book() {
  static const fixtures::Book b = fixtures::make_m1_book();
  return b;
}
const fixtures::Batch& batch() {
  static const fixtures::Batch b = fixtures::make_m1_batch();
  return b;
}
const epykos::Tape& tape() {
  static const epykos::Tape t = fixtures::record_m1(book());
  return t;
}
const ir::Program& program() {
  static const ir::Program p = ir::infer(tape());
  return p;
}

Options options_for(const benchmark::State& state) {
  Options o;
  o.tile = static_cast<int>(state.range(0));
  o.lane_tile = static_cast<int>(state.range(1));
  o.max_batch = fixtures::n_states;
  return o;
}

void set_counters(benchmark::State& state, const Adjoint& ad, int B) {
  state.SetLabel(std::string("tile ") + std::to_string(ad.options().tile) + " lane_tile " + std::to_string(ad.options().lane_tile) +
                 " value+adjoint(book)");
  state.counters["B"] = static_cast<double>(B);
  state.counters["tile"] = static_cast<double>(ad.options().tile);
  state.counters["lane_tile"] = static_cast<double>(ad.options().lane_tile);
  state.counters["values"] = static_cast<double>(ad.num_values());
  state.counters["ns_per_state"] = benchmark::Counter(
      static_cast<double>(B), benchmark::Counter::kIsIterationInvariantRate | benchmark::Counter::kInvert);
  state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()) * B * fixtures::n_swaps);
}

// B = 1: value + d(book)/dz, the state written fresh each repetition.
void BM_AdjointEval(benchmark::State& state) {
  const Adjoint ad(program(), options_for(state));
  std::vector<double> out(n_out), out_bar(n_out, 0.0), state_bar(12);
  out_bar[fixtures::n_swaps] = 1.0;
  double z[12];
  double bump = 0.0;
  for (auto _ : state) {
    for (int kk = 0; kk < 12; ++kk) z[kk] = book().z0[static_cast<std::size_t>(kk)] + bump;
    bump += 1e-9;
    ad.run(z, 1, out_bar.data(), out.data(), state_bar.data());
    benchmark::DoNotOptimize(out.data());
    benchmark::DoNotOptimize(state_bar.data());
    benchmark::ClobberMemory();
  }
  set_counters(state, ad, 1);
}

// B = 64: the 64 batch states, SoA, written fresh each repetition; out_bar = e_book in every lane.
void BM_AdjointEvalBatch(benchmark::State& state) {
  constexpr int B = 64;
  const Adjoint ad(program(), options_for(state));
  std::vector<double> z(12u * B), out(static_cast<std::size_t>(n_out) * B), out_bar(static_cast<std::size_t>(n_out) * B, 0.0),
      state_bar(12u * B);
  for (int b = 0; b < B; ++b) out_bar[static_cast<std::size_t>(fixtures::n_swaps) * B + static_cast<std::size_t>(b)] = 1.0;
  double bump = 0.0;
  for (auto _ : state) {
    for (std::size_t j = 0; j < z.size(); ++j) z[j] = batch().z[j] + bump;
    bump += 1e-9;
    ad.run(z.data(), B, out_bar.data(), out.data(), state_bar.data());
    benchmark::DoNotOptimize(out.data());
    benchmark::DoNotOptimize(state_bar.data());
    benchmark::ClobberMemory();
  }
  set_counters(state, ad, B);
}

// Plan build (constructor) from the inferred program: structure churn cost, informational.
void BM_AdjointBuild(benchmark::State& state) {
  for (auto _ : state) {
    Adjoint ad(program());
    benchmark::DoNotOptimize(ad.num_values());
  }
}

std::vector<std::int64_t> env_or(const char* name, std::vector<std::int64_t> fallback) {
  if (const char* v = std::getenv(name)) {
    const long long x = std::atoll(v);
    if (x > 0) return {static_cast<std::int64_t>(x)};
  }
  return fallback;
}

void register_all() {
  const std::vector<std::int64_t> tiles = env_or("EPYKOS_TILE", {128, 256, 512});
  const std::vector<std::int64_t> lane_tiles = env_or("EPYKOS_LANE_TILE", {4, 8, 16, 32, 64});
  for (std::int64_t tile : tiles) {
    benchmark::RegisterBenchmark("BM_AdjointEval", BM_AdjointEval)
        ->Args({tile, 1})
        ->ArgNames({"tile", "lane_tile"})
        ->Unit(benchmark::kMicrosecond);
  }
  for (std::int64_t tile : tiles) {
    for (std::int64_t lane_tile : lane_tiles) {
      benchmark::RegisterBenchmark("BM_AdjointEvalBatch", BM_AdjointEvalBatch)
          ->Args({tile, lane_tile})
          ->ArgNames({"tile", "lane_tile"})
          ->Unit(benchmark::kMicrosecond);
    }
  }
  benchmark::RegisterBenchmark("BM_AdjointBuild", BM_AdjointBuild)->Unit(benchmark::kMicrosecond);
}

struct Registrar {
  Registrar() {
    register_all();
    if (std::getenv("EPYKOS_DESCRIBE") != nullptr) std::cerr << Adjoint(program()).describe();
  }
} registrar;

}  // namespace
