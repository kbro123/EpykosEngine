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
//   BM_Calibrate/policy  (M3/G6) ONE calibration: the four block solves of one lane from the
//                        blueprint's flat start, quotes at the record point, no book — the
//                        residual programs and solvers of solver/residual.hpp driven as
//                        ImplicitProgram drives them; policy 0 = per_iteration (Newton / LM,
//                        the Jacobian at every iterate), 1 = chord (the record-point
//                        factorisation drives the steps: the scenario grid's policy)
//   BM_Evaluate/B/L      (M3/G6) O2 evaluation: the whole-program interpreter alone at the
//                        record point (the solved full state), B identical lanes at lane tile
//                        L — what a scenario lane costs after its solves
//
// Run, e.g.:
//   bench/run.sh build/release/bench/stage_a_stage_a_bench --benchmark_repetitions=5 --benchmark_min_time=1s
#include <benchmark/benchmark.h>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "epykos/fixtures/stage_a.hpp"
#include "epykos/solver/implicit_program.hpp"
#include "epykos/solver/residual.hpp"

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

// One lane's calibration as ImplicitProgram::run performs it, without the whole-program run:
// per block in registry order, the parameters from the lane's full state (quotes and earlier
// blocks' solutions), the solve from the block's start, the solution written back.
struct CalibrateRig {
  std::vector<std::unique_ptr<solver::ResidualProgram>> rp;
  std::vector<std::unique_ptr<solver::BlockSolver>> bs;
  std::vector<solver::Factors> record_factors;   // J at the record point (the chord policy's shared factorisation)
  std::vector<solver::Factors> lane_factors;
  std::vector<double> lane;                      // the full input vector of the lane
  solver::JacobianPolicy policy = solver::JacobianPolicy::per_iteration;

  CalibrateRig(const Fixture& f, solver::JacobianPolicy p) : policy(p) {
    const std::vector<double> all = f.t.tape.input_values();
    lane = all;
    for (const solver::ImplicitBlock& b : f.t.registry.blocks) {
      rp.push_back(std::make_unique<solver::ResidualProgram>(f.t.tape, b, true, 64));
      solver::SolveOptions o = b.options;
      o.jacobian = p;
      bs.push_back(std::make_unique<solver::BlockSolver>(*rp.back(), o));
      const solver::ResidualProgram& r = *rp.back();
      std::vector<double> z, pp, F(static_cast<std::size_t>(r.n_residuals())), Jz(static_cast<std::size_t>(r.n_residuals()) * static_cast<std::size_t>(r.n_unknowns())),
          Jp(static_cast<std::size_t>(r.n_residuals()) * static_cast<std::size_t>(std::max(1, r.n_params())));
      for (int o2 : r.unknown_ordinals()) z.push_back(all[static_cast<std::size_t>(o2)]);
      for (int o2 : r.param_ordinals()) pp.push_back(all[static_cast<std::size_t>(o2)]);
      rp.back()->jacobian(z.data(), pp.data(), F.data(), Jz.data(), r.n_params() > 0 ? Jp.data() : nullptr);
      record_factors.emplace_back();
      record_factors.back().compute(r.n_residuals(), r.n_unknowns(), r.n_params(), Jz.data(), r.n_params() > 0 ? Jp.data() : nullptr);
      lane_factors.emplace_back();
    }
  }
  // Returns the iteration count over the blocks; every block must converge.
  int solve(const Fixture& f, int* jacobians = nullptr) {
    int iterations = 0, jac = 0;
    std::vector<double> z, p;
    for (std::size_t k = 0; k < rp.size(); ++k) {
      const solver::ImplicitBlock& b = f.t.registry.blocks[k];
      const solver::ResidualProgram& r = *rp[k];
      p.clear();
      for (int o : r.param_ordinals()) p.push_back(lane[static_cast<std::size_t>(o)]);
      z = b.start;
      const solver::SolveReport rep = bs[k]->solve(p.data(), z.data(), lane_factors[k], policy == solver::JacobianPolicy::chord ? &record_factors[k] : nullptr);
      if (!rep.converged) throw std::runtime_error("BM_Calibrate: block " + b.name + " did not converge");
      iterations += rep.iterations;
      jac += rep.jacobians;
      for (std::size_t j = 0; j < z.size(); ++j) lane[static_cast<std::size_t>(b.unknowns[j])] = z[j];
      lane[static_cast<std::size_t>(b.diag_jtr_input)] = rep.jtr_inf;
      lane[static_cast<std::size_t>(b.diag_iterations_input)] = static_cast<double>(rep.iterations);
    }
    if (jacobians != nullptr) *jacobians = jac;
    return iterations;
  }
};

void BM_Calibrate(benchmark::State& state) {
  const Fixture& f = fixture();
  const solver::JacobianPolicy policy = state.range(0) == 1 ? solver::JacobianPolicy::chord : solver::JacobianPolicy::per_iteration;
  CalibrateRig rig(f, policy);
  int iterations = 0, jacobians = 0;
  for (auto _ : state) {
    iterations = rig.solve(f, &jacobians);
    benchmark::DoNotOptimize(rig.lane.data());
    benchmark::ClobberMemory();
  }
  state.counters["policy"] = static_cast<double>(state.range(0));
  state.counters["iterations"] = static_cast<double>(iterations);
  state.counters["jacobians"] = static_cast<double>(jacobians);
  state.counters["blocks"] = static_cast<double>(rig.rp.size());
}
BENCHMARK(BM_Calibrate)->Arg(0)->Arg(1)->Unit(benchmark::kMillisecond);

// (B, lane_tile): lane_tile 8 is the grid's plan (stage_a_program_options' default); 1 and 32
// are the M1 best points at B = 1 and B = 64, where the inliner and the exp tails fire.
void BM_Evaluate(benchmark::State& state) {
  const Fixture& f = fixture();
  const int B = static_cast<int>(state.range(0));
  const int lane_tile = static_cast<int>(state.range(1));
  solver::ProgramOptions po = fixtures::stage_a_program_options(f.s, 64);
  po.interpreter.lane_tile = lane_tile;
  solver::ImplicitProgram prog(f.t.tape, f.t.registry, po);
  const std::vector<double> all = f.t.tape.input_values();   // the record point: quotes, solved knots, diagnostics
  const std::size_t n_in = all.size(), n_out = static_cast<std::size_t>(prog.n_outputs());
  std::vector<double> st(n_in * static_cast<std::size_t>(B)), out(n_out * static_cast<std::size_t>(B));
  for (std::size_t k = 0; k < n_in; ++k) {
    for (int b = 0; b < B; ++b) st[k * static_cast<std::size_t>(B) + static_cast<std::size_t>(b)] = all[k];
  }
  for (auto _ : state) {
    prog.interpreter().run(st.data(), B, out.data());
    benchmark::DoNotOptimize(out.data());
    benchmark::ClobberMemory();
  }
  state.counters["B"] = static_cast<double>(B);
  state.counters["lane_tile"] = static_cast<double>(lane_tile);
  state.counters["fused_rows"] = static_cast<double>(prog.interpreter().num_fused_values());
  state.counters["values"] = static_cast<double>(prog.program().num_values());
  state.counters["domains"] = static_cast<double>(prog.program().domains.size());
}
BENCHMARK(BM_Evaluate)->Args({1, 8})->Args({1, 1})->Args({8, 8})->Args({64, 8})->Args({64, 32})->Args({64, 64})->Unit(benchmark::kMillisecond);

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
