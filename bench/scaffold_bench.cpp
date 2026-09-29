// M1/P0: verifies the *_bench.cpp wiring (Google Benchmark links and runs). Not a measurement.
#include <benchmark/benchmark.h>

#include "epykos/version.hpp"

static void BM_ScaffoldVersion(benchmark::State& state) {
  for (auto _ : state) {
    benchmark::DoNotOptimize(epykos::version());
  }
}
BENCHMARK(BM_ScaffoldVersion);
