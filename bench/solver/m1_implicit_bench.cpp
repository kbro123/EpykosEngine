// M3/G4: timing of the implicit node on the M1 book — the one tape quotes -> implicit ->
// price_book (fixtures/m1_calibration.hpp) through solver::ImplicitProgram — with the
// conventions of bench/exec/m1_interp_bench.cpp (tables built in the constructor, the state
// written fresh each repetition, warm). Informational (D9): the numbers behind the choices
// stated in RESUME.md §5 (M3/G4) — Jacobian policy per lane, the shared record-point
// factorisation (chord), warm starts, lane de-duplication, and the two-curve solve order
// (sequential vs joint). Counters: iterations, Jacobians and residual evaluations per lane.
//
//   BM_Run/B/policy/warm        forward: B lanes of shocked quotes (N(0, 10 bp) per lane,
//                               sub-stream 700000 + b), policy 0 = per_iteration, 1 = chord,
//                               warm 0 = from the flat 3% start, 1 = from the record solution
//   BM_RunSameState/B           forward: B identical lanes (the risk-ladder pattern: one solve)
//   BM_Adjoint/B/policy/warm    forward + IFT adjoint of the book PV per lane
//   BM_TwoCurves/mode/warm      forward at B = 1 on the two-curve tape, mode 0 sequential, 1 joint
//   BM_Build                    ImplicitProgram construction from the calibrated tape
//
// Run, e.g.:
//   bench/run.sh build/release/bench/solver_m1_implicit_bench --benchmark_repetitions=20 --benchmark_min_time=0.2s
#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdint>
#include <vector>

#include "epykos/fixtures/m1_book.hpp"
#include "epykos/fixtures/m1_calibration.hpp"
#include "epykos/rng/philox.hpp"
#include "epykos/solver/implicit_program.hpp"

namespace {

namespace fixtures = epykos::fixtures;
namespace solver = epykos::solver;

constexpr int n_knots = fixtures::n_knots;

struct Fixture {
  fixtures::Book book;
  std::vector<double> quotes;
  fixtures::CalibratedM1 cal;
  std::vector<double> shocked;  // n_knots × 64, batch innermost
  std::vector<double> two_quotes;
  fixtures::CalibratedTwoCurves seq, joint;
};

const Fixture& fixture() {
  static const Fixture f = [] {
    Fixture x;
    x.book = fixtures::make_m1_book();
    x.quotes = fixtures::m1_par_quotes(x.book.z0.data());
    std::vector<double> start(n_knots, 0.03);
    x.cal = fixtures::record_m1_calibrated(x.book, x.quotes, start);
    x.shocked.assign(static_cast<std::size_t>(n_knots) * 64, 0.0);
    for (int b = 0; b < 64; ++b) {
      epykos::rng::Philox g(fixtures::default_seed, 700000 + static_cast<std::uint64_t>(b));
      for (std::size_t k = 0; k < static_cast<std::size_t>(n_knots); ++k) x.shocked[k * 64 + static_cast<std::size_t>(b)] = x.quotes[k] + 0.0010 * g.gaussian();
    }
    std::vector<double> zp0(fixtures::proj_record_state.begin(), fixtures::proj_record_state.end());
    x.two_quotes = fixtures::two_curve_quotes(x.book.z0.data(), zp0.data());
    std::vector<double> ps(fixtures::n_proj_knots, 0.04);
    x.seq = fixtures::record_two_curves(x.two_quotes, start, ps, solver::CurveSet::Mode::sequential);
    x.joint = fixtures::record_two_curves(x.two_quotes, start, ps, solver::CurveSet::Mode::joint);
    return x;
  }();
  return f;
}

solver::ProgramOptions options_for(int policy, int warm) {
  solver::ProgramOptions o;
  o.jacobian = policy;
  o.warm_start = warm != 0;
  o.interpreter.tile = 256;
  o.interpreter.lane_tile = 32;
  return o;
}

void set_counters(benchmark::State& state, const solver::ImplicitProgram& prog, int B) {
  const solver::ImplicitProgram::RunStats& s = prog.last_run();
  state.counters["solves"] = static_cast<double>(s.solves);
  state.counters["jacobians"] = static_cast<double>(s.jacobians);
  state.counters["residual_evals"] = static_cast<double>(s.residual_evaluations);
  state.counters["refreshed"] = static_cast<double>(s.refreshed);
  int iterations = 0;
  for (int b = 0; b < B; ++b) iterations += prog.report(0, b).iterations;
  state.counters["iterations"] = static_cast<double>(iterations);
  state.counters["B"] = static_cast<double>(B);
}

void BM_Run(benchmark::State& state) {
  const Fixture& f = fixture();
  const int B = static_cast<int>(state.range(0));
  solver::ImplicitProgram prog(f.cal.tape, f.cal.registry, options_for(static_cast<int>(state.range(1)), static_cast<int>(state.range(2))));
  std::vector<double> in(static_cast<std::size_t>(n_knots) * static_cast<std::size_t>(B)), out(static_cast<std::size_t>(prog.n_outputs()) * static_cast<std::size_t>(B));
  for (auto _ : state) {
    for (std::size_t k = 0; k < static_cast<std::size_t>(n_knots); ++k) {
      for (std::size_t b = 0; b < static_cast<std::size_t>(B); ++b) in[k * static_cast<std::size_t>(B) + b] = f.shocked[k * 64 + b];
    }
    prog.run(in.data(), B, out.data());
    benchmark::DoNotOptimize(out.data());
    benchmark::ClobberMemory();
  }
  set_counters(state, prog, B);
}
BENCHMARK(BM_Run)->ArgsProduct({{1, 8, 64}, {0, 1}, {0, 1}})->Unit(benchmark::kMicrosecond);

void BM_RunSameState(benchmark::State& state) {
  const Fixture& f = fixture();
  const int B = static_cast<int>(state.range(0));
  solver::ImplicitProgram prog(f.cal.tape, f.cal.registry, options_for(0, 1));
  std::vector<double> in(static_cast<std::size_t>(n_knots) * static_cast<std::size_t>(B)), out(static_cast<std::size_t>(prog.n_outputs()) * static_cast<std::size_t>(B));
  for (auto _ : state) {
    for (std::size_t k = 0; k < static_cast<std::size_t>(n_knots); ++k) {
      for (std::size_t b = 0; b < static_cast<std::size_t>(B); ++b) in[k * static_cast<std::size_t>(B) + b] = f.shocked[k * 64 + 1];
    }
    prog.run(in.data(), B, out.data());
    benchmark::DoNotOptimize(out.data());
    benchmark::ClobberMemory();
  }
  set_counters(state, prog, B);
}
BENCHMARK(BM_RunSameState)->Arg(64)->Unit(benchmark::kMicrosecond);

void BM_Adjoint(benchmark::State& state) {
  const Fixture& f = fixture();
  const int B = static_cast<int>(state.range(0));
  solver::ImplicitProgram prog(f.cal.tape, f.cal.registry, options_for(static_cast<int>(state.range(1)), static_cast<int>(state.range(2))));
  const std::size_t Bs = static_cast<std::size_t>(B);
  std::vector<double> in(static_cast<std::size_t>(n_knots) * Bs), out(static_cast<std::size_t>(prog.n_outputs()) * Bs),
      out_bar(static_cast<std::size_t>(prog.n_outputs()) * Bs, 0.0), q_bar(static_cast<std::size_t>(n_knots) * Bs);
  for (std::size_t b = 0; b < Bs; ++b) out_bar[static_cast<std::size_t>(f.cal.book_pv) * Bs + b] = 1.0;
  for (auto _ : state) {
    for (std::size_t k = 0; k < static_cast<std::size_t>(n_knots); ++k) {
      for (std::size_t b = 0; b < Bs; ++b) in[k * Bs + b] = f.shocked[k * 64 + b];
    }
    prog.adjoint(in.data(), B, out_bar.data(), out.data(), q_bar.data());
    benchmark::DoNotOptimize(q_bar.data());
    benchmark::ClobberMemory();
  }
  set_counters(state, prog, B);
}
BENCHMARK(BM_Adjoint)->ArgsProduct({{1, 8}, {0, 1}, {1}})->Unit(benchmark::kMicrosecond);

void BM_TwoCurves(benchmark::State& state) {
  const Fixture& f = fixture();
  const bool joint = state.range(0) != 0;
  const fixtures::CalibratedTwoCurves& c = joint ? f.joint : f.seq;
  solver::ImplicitProgram prog(c.tape, c.registry, options_for(0, static_cast<int>(state.range(1))));
  std::vector<double> in(f.two_quotes.size()), out(static_cast<std::size_t>(prog.n_outputs()));
  for (auto _ : state) {
    for (std::size_t k = 0; k < in.size(); ++k) in[k] = f.two_quotes[k] + (k == 3 ? 0.0001 : 0.0);
    prog.run(in.data(), 1, out.data());
    benchmark::DoNotOptimize(out.data());
    benchmark::ClobberMemory();
  }
  const solver::ImplicitProgram::RunStats& s = prog.last_run();
  state.counters["blocks"] = static_cast<double>(prog.n_blocks());
  state.counters["jacobians"] = static_cast<double>(s.jacobians);
  state.counters["residual_evals"] = static_cast<double>(s.residual_evaluations);
  int iterations = 0;
  for (int k = 0; k < prog.n_blocks(); ++k) iterations += prog.report(k, 0).iterations;
  state.counters["iterations"] = static_cast<double>(iterations);
}
BENCHMARK(BM_TwoCurves)->ArgsProduct({{0, 1}, {0, 1}})->Unit(benchmark::kMicrosecond);

void BM_Build(benchmark::State& state) {
  const Fixture& f = fixture();
  for (auto _ : state) {
    solver::ImplicitProgram prog(f.cal.tape, f.cal.registry);
    benchmark::DoNotOptimize(prog.n_outputs());
  }
}
BENCHMARK(BM_Build)->Unit(benchmark::kMillisecond);

}  // namespace
