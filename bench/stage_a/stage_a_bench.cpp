// M3/G5: timings of the Stage A tape (fixtures/stage_a.hpp) as the record of what the one
// recording costs before M4 optimises it (informational, D9; G6 states the M4 baseline):
//
//   BM_Record            make_stage_a + record_stage_a (the tables, the record-time solves, the
//                        book, the E0 passes): the structure-time cost
//   BM_Build             solver::ImplicitProgram construction from the recorded tape (inference,
//                        the residual programs, the interpreter and the adjoint)
//   BM_Run/B             O2 / O4: B scenario lanes of the grid (each recalibrated, chord policy)
//   BM_Adjoint/B         O3: the IFT adjoint ladder of B outputs (the book, then trade PVs), one
//                        lane per output
//   BM_ForwardLadder     O3 in forward mode: the Dual<70> pass of the whole problem (every
//                        trade's par delta to every quote)
//
// Run, e.g.:
//   bench/run.sh build/release/bench/stage_a_stage_a_bench --benchmark_repetitions=5 --benchmark_min_time=1s
#include <benchmark/benchmark.h>

#include <cstddef>
#include <memory>
#include <vector>

#include "epykos/fixtures/stage_a.hpp"
#include "epykos/solver/implicit_program.hpp"

namespace {

namespace fixtures = epykos::fixtures;
namespace solver = epykos::solver;

constexpr int N = 70;

struct Fixture {
  fixtures::StageA s;
  fixtures::StageATape t;
};

const Fixture& fixture() {
  static const Fixture f = [] {
    Fixture x;
    x.s = fixtures::make_stage_a();
    x.t = fixtures::record_stage_a(x.s);
    return x;
  }();
  return f;
}

void BM_Record(benchmark::State& state) {
  for (auto _ : state) {
    const fixtures::StageA s = fixtures::make_stage_a();
    const fixtures::StageATape t = fixtures::record_stage_a(s);
    benchmark::DoNotOptimize(t.tape.size());
    state.counters["nodes_raw"] = static_cast<double>(t.stats.nodes_raw);
    state.counters["nodes"] = static_cast<double>(t.stats.nodes_after_passes);
    state.counters["s_passes"] = t.stats.seconds_passes;
    state.counters["s_calibrate"] = t.stats.seconds_calibrate;
  }
}
BENCHMARK(BM_Record)->Unit(benchmark::kMillisecond)->Iterations(1);

void BM_Build(benchmark::State& state) {
  const Fixture& f = fixture();
  for (auto _ : state) {
    solver::ImplicitProgram prog(f.t.tape, f.t.registry, fixtures::stage_a_program_options(f.s, 64));
    benchmark::DoNotOptimize(prog.n_outputs());
    state.counters["values"] = static_cast<double>(prog.program().num_values());
    state.counters["domains"] = static_cast<double>(prog.program().domains.size());
  }
}
BENCHMARK(BM_Build)->Unit(benchmark::kMillisecond)->Iterations(2);

void BM_Run(benchmark::State& state) {
  const Fixture& f = fixture();
  const int B = static_cast<int>(state.range(0));
  solver::ImplicitProgram prog(f.t.tape, f.t.registry, fixtures::stage_a_program_options(f.s, 64));
  std::vector<std::vector<double>> lanes(f.s.scenario_quotes.begin(), f.s.scenario_quotes.begin() + B);
  for (auto _ : state) {
    const std::vector<double> out = fixtures::run_lanes(prog, lanes);
    benchmark::DoNotOptimize(out.data());
    benchmark::ClobberMemory();
  }
  const solver::ImplicitProgram::RunStats& st = prog.last_run();
  state.counters["B"] = static_cast<double>(B);
  state.counters["solves"] = static_cast<double>(st.solves);
  state.counters["residual_evals"] = static_cast<double>(st.residual_evaluations);
  state.counters["jacobians"] = static_cast<double>(st.jacobians);
}
BENCHMARK(BM_Run)->Arg(1)->Arg(8)->Arg(64)->Unit(benchmark::kMillisecond);

void BM_Adjoint(benchmark::State& state) {
  const Fixture& f = fixture();
  const int B = static_cast<int>(state.range(0));
  solver::ImplicitProgram prog(f.t.tape, f.t.registry, fixtures::stage_a_program_options(f.s, 64));
  std::vector<int> ordinals = {f.t.layout.book};
  for (int i = 0; ordinals.size() < static_cast<std::size_t>(B); ++i) ordinals.push_back(f.t.layout.pv(i));
  for (auto _ : state) {
    const std::vector<double> rows = fixtures::ladder(prog, f.s.quotes, ordinals);
    benchmark::DoNotOptimize(rows.data());
    benchmark::ClobberMemory();
  }
  state.counters["B"] = static_cast<double>(B);
  state.counters["solves"] = static_cast<double>(prog.last_run().solves);
}
BENCHMARK(BM_Adjoint)->Arg(1)->Arg(8)->Arg(64)->Unit(benchmark::kMillisecond);

void BM_ForwardLadder(benchmark::State& state) {
  const Fixture& f = fixture();
  solver::SolveOptions o;
  o.tol = f.s.options.solve_tol;
  for (auto _ : state) {
    const fixtures::StageAForward<N> fw = fixtures::forward_stage_a<N>(f.s, *f.t.set, f.s.quotes, f.s.options.mode, o, &f.t.record_knots);
    double total = fw.book.book_total.v;
    benchmark::DoNotOptimize(total);
    benchmark::ClobberMemory();
  }
  state.counters["quotes"] = N;
  state.counters["trades"] = static_cast<double>(f.s.n_trades());
}
BENCHMARK(BM_ForwardLadder)->Unit(benchmark::kMillisecond);

}  // namespace
