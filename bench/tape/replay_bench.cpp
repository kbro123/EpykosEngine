// M1/P2: cost of recording, of the passes, and of a scalar replay of the mini book.
//
// Informational only (D9): the M1 gate compares P4's interpreter with P5's hand-fused kernel on
// P1's book. This bench gives the scalar-replay baseline that both must beat, per fingerprint.
// Run: ./build/release/bench/tape_replay_bench --benchmark_out=bench/results/<id>/tape_replay.json
//                                              --benchmark_out_format=json
#include <benchmark/benchmark.h>

#include <array>
#include <vector>

#include "epykos/tape/passes.hpp"
#include "epykos/tape/replay.hpp"
#include "epykos/tape/tape.hpp"
#include "tape/mini_book.hpp"

namespace {

using epykos::Tape;
using epykos::test::kMiniKnots;
using epykos::test::mini_book;
using epykos::test::MiniSwap;
using epykos::test::record_mini_book;

void BM_Record(benchmark::State& state) {
  const std::vector<MiniSwap> book = mini_book(static_cast<int>(state.range(0)));
  std::size_t nodes = 0;
  for (auto _ : state) {
    Tape t = record_mini_book(book);
    nodes = t.size();
    benchmark::DoNotOptimize(t);
  }
  state.counters["nodes"] = static_cast<double>(nodes);
  state.counters["swaps"] = static_cast<double>(book.size());
}
BENCHMARK(BM_Record)->Arg(50)->Arg(1000)->Unit(benchmark::kMillisecond);

void BM_StandardPasses(benchmark::State& state) {
  const std::vector<MiniSwap> book = mini_book(static_cast<int>(state.range(0)));
  const Tape recorded = record_mini_book(book);
  std::size_t before = recorded.size();
  std::size_t after = 0;
  for (auto _ : state) {
    state.PauseTiming();
    Tape t = recorded;
    state.ResumeTiming();
    epykos::standard_passes(t);
    after = t.size();
    benchmark::DoNotOptimize(t);
  }
  state.counters["nodes_before"] = static_cast<double>(before);
  state.counters["nodes_after"] = static_cast<double>(after);
}
BENCHMARK(BM_StandardPasses)->Arg(50)->Arg(1000)->Unit(benchmark::kMillisecond);

// Single-state replay; state inputs written fresh each repetition (WORKLOADS.md measurement).
void replay_bench(benchmark::State& state, bool optimised) {
  const std::vector<MiniSwap> book = mini_book(static_cast<int>(state.range(0)));
  Tape t = record_mini_book(book);
  if (optimised) epykos::standard_passes(t);
  epykos::Replayer rp(t);
  std::vector<double> out(t.num_outputs());
  const std::array<double, kMiniKnots>& z = epykos::test::kMiniRecordState;
  std::array<double, kMiniKnots> in{};
  double bump = 0.0;
  for (auto _ : state) {
    for (int k = 0; k < kMiniKnots; ++k) in[k] = z[k] + bump;
    bump += 1e-7;
    rp.run(in.data(), out.data());
    benchmark::DoNotOptimize(out.data());
    benchmark::ClobberMemory();
  }
  state.counters["nodes"] = static_cast<double>(t.size());
  state.counters["ns_per_node"] = benchmark::Counter(
      static_cast<double>(t.size()), benchmark::Counter::kIsIterationInvariantRate | benchmark::Counter::kInvert);
  state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()) * static_cast<std::int64_t>(book.size()));
}
void BM_ReplayRaw(benchmark::State& state) { replay_bench(state, false); }
void BM_ReplayOptimised(benchmark::State& state) { replay_bench(state, true); }
BENCHMARK(BM_ReplayRaw)->Arg(50)->Arg(1000)->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_ReplayOptimised)->Arg(50)->Arg(1000)->Unit(benchmark::kMicrosecond);

// The templated double instantiation: what the recording reproduces, scalar and unfused.
void BM_DoubleOracle(benchmark::State& state) {
  const std::vector<MiniSwap> book = mini_book(static_cast<int>(state.range(0)));
  std::array<double, kMiniKnots> in = epykos::test::kMiniRecordState;
  double bump = 0.0;
  for (auto _ : state) {
    for (double& z : in) z += bump;
    bump = 1e-7;
    const std::vector<double> out = epykos::test::eval_mini_book_double(in, book);
    benchmark::DoNotOptimize(out.data());
  }
  state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()) * static_cast<std::int64_t>(book.size()));
}
BENCHMARK(BM_DoubleOracle)->Arg(50)->Arg(1000)->Unit(benchmark::kMicrosecond);

}  // namespace
