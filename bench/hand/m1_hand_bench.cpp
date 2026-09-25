// M1/P5: timing of the hand-fused reference kernel on the M1 book, single state (B = 1) and
// batched (B = 64), per docs/WORKLOADS.md §M1 "Measurement": tables prebuilt, state inputs written
// fresh each repetition, warm. P6 reuses this as the denominator of the M1 gate.
//
// Variants (informational, D9): shared reciprocal vs per-row division for DF(s)/DF(e); exp_poly
// (vectorised) vs std::exp (scalar libm) so that a comparison against an interpreter using either
// exp can be made like for like; and the reference-arithmetic mode (the oracle's operation order,
// no fma, ÷) which is not a fast kernel but shows what the fused arithmetic buys.
//
// Run, e.g.:
//   ./build/release/bench/hand_m1_hand_bench --benchmark_repetitions=20 --benchmark_min_time=0.2s \
//       --benchmark_report_aggregates_only=true --benchmark_out_format=json \
//       --benchmark_out=bench/results/<fingerprint>/m1_hand.json
// and record scripts/fingerprint.sh (with its load average) alongside.
#include <benchmark/benchmark.h>

#include <cstdint>
#include <vector>

#include "hand/m1_hand_kernel.hpp"
#include "epykos/fixtures/m1_book.hpp"

namespace {

namespace fixtures = epykos::fixtures;
using epykos::hand::HandArith;
using epykos::hand::HandExp;
using epykos::hand::M1HandKernel;
using epykos::hand::M1HandOptions;

const fixtures::Book& book() {
  static const fixtures::Book b = fixtures::make_m1_book();
  return b;
}
const fixtures::Batch& batch() {
  static const fixtures::Batch b = fixtures::make_m1_batch();
  return b;
}

// Variant index -> options.
enum Variant : int {
  fused_shared_poly = 0,
  fused_div_poly = 1,
  fused_shared_stdexp = 2,
  reference_arith = 3,
};
M1HandOptions options_for(int v) {
  M1HandOptions o;
  switch (v) {
    case fused_div_poly: o.shared_reciprocal = false; break;
    case fused_shared_stdexp: o.exp = HandExp::std_exp; break;
    case reference_arith: o.arith = HandArith::reference; break;
    default: break;
  }
  return o;
}
const char* variant_name(int v) {
  switch (v) {
    case fused_shared_poly: return "fused/shared-recip/exp_poly";
    case fused_div_poly: return "fused/per-row-div/exp_poly";
    case fused_shared_stdexp: return "fused/shared-recip/std::exp";
    case reference_arith: return "reference-arith/std::exp";
    default: return "?";
  }
}

void set_counters(benchmark::State& state, const M1HandKernel& k, int B) {
  state.SetLabel(variant_name(static_cast<int>(state.range(0))));
  state.counters["B"] = static_cast<double>(B);
  state.counters["times"] = static_cast<double>(k.n_times());
  state.counters["rows"] = static_cast<double>(k.n_plain_rows() + k.n_float_rows());
  // Time per state: the per-iteration time divided by B.
  state.counters["ns_per_state"] = benchmark::Counter(
      static_cast<double>(B), benchmark::Counter::kIsIterationInvariantRate | benchmark::Counter::kInvert);
  state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()) * B * k.n_swaps());
}

// B = 1: the state written fresh each repetition (a tiny drift so the compiler cannot hoist it).
void BM_HandEval(benchmark::State& state) {
  const M1HandKernel k(book(), options_for(static_cast<int>(state.range(0))));
  std::vector<double> pv(1000);
  double bpv = 0.0;
  double z[12];
  double bump = 0.0;
  for (auto _ : state) {
    for (int kk = 0; kk < 12; ++kk) z[kk] = book().z0[static_cast<std::size_t>(kk)] + bump;
    bump += 1e-9;
    k.eval(z, pv.data(), &bpv);
    benchmark::DoNotOptimize(pv.data());
    benchmark::DoNotOptimize(bpv);
    benchmark::ClobberMemory();
  }
  set_counters(state, k, 1);
}
BENCHMARK(BM_HandEval)
    ->Arg(fused_shared_poly)
    ->Arg(fused_div_poly)
    ->Arg(fused_shared_stdexp)
    ->Arg(reference_arith)
    ->Unit(benchmark::kMicrosecond);

// B = 64: the 64 batch states, SoA, written fresh each repetition.
void BM_HandEvalBatch(benchmark::State& state) {
  constexpr int B = 64;
  const M1HandKernel k(book(), options_for(static_cast<int>(state.range(0))));
  std::vector<double> z(12u * B), pv(1000u * B), bpv(B);
  double bump = 0.0;
  for (auto _ : state) {
    for (std::size_t j = 0; j < z.size(); ++j) z[j] = batch().z[j] + bump;
    bump += 1e-9;
    k.eval_batch(z.data(), B, pv.data(), bpv.data());
    benchmark::DoNotOptimize(pv.data());
    benchmark::DoNotOptimize(bpv.data());
    benchmark::ClobberMemory();
  }
  set_counters(state, k, B);
}
BENCHMARK(BM_HandEvalBatch)
    ->Arg(fused_shared_poly)
    ->Arg(fused_div_poly)
    ->Arg(fused_shared_stdexp)
    ->Arg(reference_arith)
    ->Unit(benchmark::kMicrosecond);

// Table build (constructor), informational: structure churn cost (DESIGN.md §9).
void BM_HandBuild(benchmark::State& state) {
  for (auto _ : state) {
    M1HandKernel k(book());
    benchmark::DoNotOptimize(k);
  }
}
BENCHMARK(BM_HandBuild)->Unit(benchmark::kMicrosecond);

}  // namespace
