// M1/P4: timing of the tiled interpreter on the M1 book, single state (B = 1) and batched
// (B = 64), with the same book, state and measurement conventions as bench/hand/m1_hand_bench.cpp
// (tables prebuilt, state inputs written fresh each repetition, warm) so P6 can compare like for
// like: the interpreter's std::exp rows against BM_HandEval/2 and BM_HandEvalBatch/2
// (fused/shared-recip/std::exp), its exp_poly rows against /0 (the hand kernel's default).
//
// Arguments: {tile, lane_tile, exp} with exp 0 = std::exp (E0), 1 = exp_poly (E1). The tile
// sweep of D15 (128 / 256 / 512) and a lane-tile sweep for B = 64 are registered; the
// environment variable EPYKOS_TILE, when set, replaces the tile sweep by that one value (and
// EPYKOS_LANE_TILE the lane-tile sweep), e.g. EPYKOS_TILE=64 ./exec_m1_interp_bench.
//
// Run, e.g.:
//   ./build/release/bench/exec_m1_interp_bench --benchmark_repetitions=25 --benchmark_min_time=0.2s \
//       --benchmark_report_aggregates_only=false --benchmark_out_format=json \
//       --benchmark_out=bench/results/<fingerprint>/m1_interp.json
// and record scripts/fingerprint.sh (with its load average) alongside.
#include <benchmark/benchmark.h>

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "epykos/exec/interpreter.hpp"
#include "epykos/ir/program.hpp"
#include "epykos/ir/signature.hpp"
#include "epykos/maths/m1/book.hpp"
#include "epykos/tape/record_m1.hpp"
#include "epykos/tape/tape.hpp"

namespace {

namespace m1 = epykos::m1;
namespace ir = epykos::ir;
using epykos::exec::ExpMode;
using epykos::exec::Interpreter;
using epykos::exec::Options;

const m1::Book& book() {
  static const m1::Book b = m1::make_m1_book();
  return b;
}
const m1::Batch& batch() {
  static const m1::Batch b = m1::make_m1_batch();
  return b;
}
const epykos::Tape& tape() {
  static const epykos::Tape t = m1::record_m1(book());
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
  o.exp = state.range(2) == 0 ? ExpMode::std_exp : ExpMode::poly;
  o.max_batch = m1::n_states;
  return o;
}

void set_counters(benchmark::State& state, const Interpreter& in, int B) {
  state.SetLabel(std::string("tile ") + std::to_string(in.options().tile) + " lane_tile " +
                 std::to_string(in.options().lane_tile) + " " + epykos::exec::to_string(in.options().exp));
  state.counters["B"] = static_cast<double>(B);
  state.counters["tile"] = static_cast<double>(in.options().tile);
  state.counters["lane_tile"] = static_cast<double>(in.options().lane_tile);
  state.counters["values"] = static_cast<double>(in.num_values());
  state.counters["domains"] = static_cast<double>(program().domains.size());
  state.counters["ns_per_state"] = benchmark::Counter(
      static_cast<double>(B), benchmark::Counter::kIsIterationInvariantRate | benchmark::Counter::kInvert);
  state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()) * B * m1::n_swaps);
}

// B = 1: the state written fresh each repetition (a tiny drift so the compiler cannot hoist it).
void BM_InterpEval(benchmark::State& state) {
  const Interpreter in(program(), options_for(state));
  std::vector<double> out(1001);
  double z[12];
  double bump = 0.0;
  for (auto _ : state) {
    for (int kk = 0; kk < 12; ++kk) z[kk] = book().z0[static_cast<std::size_t>(kk)] + bump;
    bump += 1e-9;
    in.run(z, 1, out.data());
    benchmark::DoNotOptimize(out.data());
    benchmark::ClobberMemory();
  }
  set_counters(state, in, 1);
}

// B = 64: the 64 batch states, SoA, written fresh each repetition.
void BM_InterpEvalBatch(benchmark::State& state) {
  constexpr int B = 64;
  const Interpreter in(program(), options_for(state));
  std::vector<double> z(12u * B), out(1001u * B);
  double bump = 0.0;
  for (auto _ : state) {
    for (std::size_t j = 0; j < z.size(); ++j) z[j] = batch().z[j] + bump;
    bump += 1e-9;
    in.run(z.data(), B, out.data());
    benchmark::DoNotOptimize(out.data());
    benchmark::ClobberMemory();
  }
  set_counters(state, in, B);
}

// Plan build (constructor) from the inferred program, and the whole pipeline from the book
// (record + passes + infer + plan): structure churn cost (DESIGN.md §9), informational.
void BM_InterpBuild(benchmark::State& state) {
  for (auto _ : state) {
    Interpreter in(program());
    benchmark::DoNotOptimize(in.num_values());
  }
}
void BM_InterpPipeline(benchmark::State& state) {
  for (auto _ : state) {
    const epykos::Tape t = m1::record_m1(book());
    const ir::Program p = ir::infer(t);
    Interpreter in(p);
    benchmark::DoNotOptimize(in.num_values());
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
  for (std::int64_t exp : {0, 1}) {
    for (std::int64_t tile : tiles) {
      benchmark::RegisterBenchmark("BM_InterpEval", BM_InterpEval)
          ->Args({tile, 1, exp})
          ->ArgNames({"tile", "lane_tile", "exp"})
          ->Unit(benchmark::kMicrosecond);
    }
    for (std::int64_t tile : tiles) {
      for (std::int64_t lane_tile : lane_tiles) {
        benchmark::RegisterBenchmark("BM_InterpEvalBatch", BM_InterpEvalBatch)
            ->Args({tile, lane_tile, exp})
            ->ArgNames({"tile", "lane_tile", "exp"})
            ->Unit(benchmark::kMicrosecond);
      }
    }
  }
  benchmark::RegisterBenchmark("BM_InterpBuild", BM_InterpBuild)->Unit(benchmark::kMicrosecond);
  benchmark::RegisterBenchmark("BM_InterpPipeline", BM_InterpPipeline)->Unit(benchmark::kMillisecond);
}

struct Registrar {
  Registrar() {
    register_all();
    if (std::getenv("EPYKOS_DESCRIBE") != nullptr) std::cerr << Interpreter(program()).describe();
  }
} registrar;

}  // namespace
