// SPIKE, MEASUREMENT ONLY — what telescoping the compounded coupon is worth END TO END, in wall
// clock, on the `compare_ois` fixture (include/epykos/fixtures/spike_telescoped.hpp; D74).
//
// Read the header of that fixture first: the telescoped coupon here is hand-written test-only
// code that must never become production maths (PRINCIPLES.md §2), and it exists so the prize of
// finding the rewrite AUTOMATICALLY can be priced before the machinery to find it is built.
//
// Every row is registered twice, once per coupon form, with the SAME fixture, the same quotes,
// the same book and the same ProgramOptions, so the pair differs in the coupon form and nothing
// else:
//
//   BM_Evaluate/<form>/T   the whole-program interpreter at the solved state, one lane. The
//                          forward half alone: what "evaluate" costs.
//   BM_Calibrate/<form>/T  O1 + O2, one full recalibration from the block's flat start plus the
//                          forward pass. The closest analogue to a curve build.
//   BM_LadderChord/<form>/T, BM_LadderWarm/<form>/T, BM_LadderCold/<form>/T
//                          O3, d(book PV)/d(quote) through the adjoint and the IFT, on the three
//                          Jacobian policies of bench/compare/ois_ladder_bench.cpp. The reverse
//                          risk ladder is PRINCIPLES.md §3's "about a quarter of the recurring
//                          work" and does not move by the same factor as the other two.
//   BM_Build/<form>/T      ImplicitProgram construction: inference, residual programs, the
//                          interpreter and the adjoint. Structure time, paid once.
//   BM_Record/<form>/T     recording the tape and running the E0 passes. Paid once per book.
//
// This benchmark is EXPLORATORY and is deliberately not given a `bench/results/` baseline: a
// spike's hand-written maths has no business gating the engine's performance (D34 — an
// un-baselined name is not gated). Run it on a quiet machine, per bench/run.sh:
//
//   uptime; ps -Ao %cpu,command -r | head        # load lags; check what is actually running
//   bench/run.sh build/release/bench/spike_telescoping_bench
#include <benchmark/benchmark.h>

#include <cstddef>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "epykos/fixtures/compare_ois.hpp"
#include "epykos/fixtures/spike_telescoped.hpp"
#include "epykos/solver/implicit_program.hpp"

namespace {

namespace fixtures = epykos::fixtures;
namespace spike = epykos::fixtures::spike;
namespace solver = epykos::solver;

// Sixteen and sixty-four book trades against the sixteen-instrument calibration set, stated in
// long form at the use site per D64. Two hundred and fifty-six is left out: the fixture is
// calibration-dominated (D71 §6(c)) and the extra row costs minutes without changing the
// conclusion.
constexpr int kTrades[] = {16, 64};

struct Fixture {
  fixtures::CompareOis s;
  fixtures::CompareOisTape tape;
  std::unique_ptr<solver::ImplicitProgram> chord;
  std::unique_ptr<solver::ImplicitProgram> warm;
  std::unique_ptr<solver::ImplicitProgram> cold;

  Fixture(int trades, spike::Form form) {
    fixtures::CompareOisOptions o;
    o.trades = trades;
    s = fixtures::make_compare_ois(o);
    tape = spike::record_compare_ois_spike(s, form);
    solver::ProgramOptions po = fixtures::compare_ois_program_options(s);
    po.warm_start = true;
    po.jacobian = static_cast<int>(solver::JacobianPolicy::chord);
    chord = std::make_unique<solver::ImplicitProgram>(tape.tape, tape.registry, po);
    po.jacobian = -1;
    warm = std::make_unique<solver::ImplicitProgram>(tape.tape, tape.registry, po);
    po.warm_start = false;
    cold = std::make_unique<solver::ImplicitProgram>(tape.tape, tape.registry, po);
  }
};

Fixture& fixture(int trades, spike::Form form) {
  static std::map<std::pair<int, int>, std::unique_ptr<Fixture>> cache;
  std::unique_ptr<Fixture>& f = cache[{trades, static_cast<int>(form)}];
  if (!f) f = std::make_unique<Fixture>(trades, form);
  return *f;
}

void common_counters(benchmark::State& state, const Fixture& f) {
  state.counters["trades"] = f.s.n_trades();
  state.counters["quotes"] = f.s.n_quotes();
  state.counters["tape_nodes"] = static_cast<double>(f.tape.nodes_after_passes);
}

enum class Which { Chord, Warm, Cold };

void ladder_row(benchmark::State& state, spike::Form form, Which which) {
  const int trades = static_cast<int>(state.range(0));
  Fixture& f = fixture(trades, form);
  solver::ImplicitProgram& prog = which == Which::Chord ? *f.chord : (which == Which::Warm ? *f.warm : *f.cold);
  const std::vector<int> ordinals = {f.tape.book_output};
  std::vector<double> out;
  for (auto _ : state) {
    const std::vector<double> rows = fixtures::compare_ois_ladder(prog, f.s.quotes, ordinals, &out);
    benchmark::DoNotOptimize(rows.data());
    benchmark::ClobberMemory();
  }
  const solver::ImplicitProgram::RunStats& r = prog.last_run();
  common_counters(state, f);
  state.counters["solves"] = r.solves;
  state.counters["residual_evals"] = static_cast<double>(r.residual_evaluations);
  state.counters["jacobians"] = static_cast<double>(r.jacobians);
}

void evaluate_row(benchmark::State& state, spike::Form form) {
  const int trades = static_cast<int>(state.range(0));
  Fixture& f = fixture(trades, form);
  solver::ImplicitProgram& prog = *f.warm;
  const int n_in = prog.n_inputs(), n_out = prog.n_outputs();
  {
    std::vector<double> st(static_cast<std::size_t>(prog.n_state())), o(static_cast<std::size_t>(n_out));
    for (int k = 0; k < prog.n_state(); ++k) st[static_cast<std::size_t>(k)] = f.s.quotes[static_cast<std::size_t>(k)];
    prog.run(st.data(), 1, o.data());
  }
  std::vector<double> full(static_cast<std::size_t>(n_in));
  for (int k = 0; k < n_in; ++k) full[static_cast<std::size_t>(k)] = prog.full_state(0)[k];
  std::vector<double> o(static_cast<std::size_t>(n_out));
  for (auto _ : state) {
    prog.interpreter().run(full.data(), 1, o.data());
    benchmark::DoNotOptimize(o.data());
    benchmark::ClobberMemory();
  }
  common_counters(state, f);
}

void calibrate_row(benchmark::State& state, spike::Form form) {
  const int trades = static_cast<int>(state.range(0));
  Fixture& f = fixture(trades, form);
  solver::ImplicitProgram& prog = *f.cold;
  const int n_q = prog.n_state(), n_out = prog.n_outputs();
  std::vector<double> st(static_cast<std::size_t>(n_q)), o(static_cast<std::size_t>(n_out));
  for (int k = 0; k < n_q; ++k) st[static_cast<std::size_t>(k)] = f.s.quotes[static_cast<std::size_t>(k)];
  for (auto _ : state) {
    prog.run(st.data(), 1, o.data());
    benchmark::DoNotOptimize(o.data());
    benchmark::ClobberMemory();
  }
  const solver::ImplicitProgram::RunStats& r = prog.last_run();
  common_counters(state, f);
  state.counters["solves"] = r.solves;
  state.counters["residual_evals"] = static_cast<double>(r.residual_evaluations);
  state.counters["jacobians"] = static_cast<double>(r.jacobians);
}

void build_row(benchmark::State& state, spike::Form form) {
  const int trades = static_cast<int>(state.range(0));
  Fixture& f = fixture(trades, form);
  for (auto _ : state) {
    solver::ImplicitProgram p(f.tape.tape, f.tape.registry, fixtures::compare_ois_program_options(f.s));
    benchmark::DoNotOptimize(&p);
  }
  common_counters(state, f);
}

void record_row(benchmark::State& state, spike::Form form) {
  const int trades = static_cast<int>(state.range(0));
  Fixture& f = fixture(trades, form);
  for (auto _ : state) {
    fixtures::CompareOisTape t = spike::record_compare_ois_spike(f.s, form);
    benchmark::DoNotOptimize(t.tape.size());
  }
  common_counters(state, f);
}

// The two forms of a row are registered ADJACENTLY, not all-naive-then-all-telescoped. Google
// Benchmark runs benchmarks in registration order, so a machine that drifts over the run biases
// the ratio directly if the two halves are minutes apart; registering the pair together makes the
// two members of every ratio near neighbours in time. D68 made the same point about interleaving.
void register_all() {
  using benchmark::RegisterBenchmark;
  constexpr spike::Form kForms[] = {spike::Form::naive, spike::Form::telescoped};
  auto pair = [&](const char* name, int arg, benchmark::TimeUnit unit, void (*fn)(benchmark::State&, spike::Form)) {
    for (const spike::Form form : kForms) {
      RegisterBenchmark(std::string(name) + "/form:" + spike::to_string(form),
                        [form, fn](benchmark::State& s) { fn(s, form); })
          ->Arg(arg)
          ->Unit(unit);
    }
  };
  for (int t : kTrades) {
    pair("BM_Evaluate", t, benchmark::kMicrosecond, evaluate_row);
    pair("BM_Calibrate", t, benchmark::kMicrosecond, calibrate_row);
    for (const spike::Form form : kForms) {
      RegisterBenchmark(std::string("BM_LadderChord/form:") + spike::to_string(form),
                        [form](benchmark::State& s) { ladder_row(s, form, Which::Chord); })
          ->Arg(t)
          ->Unit(benchmark::kMicrosecond);
    }
    for (const spike::Form form : kForms) {
      RegisterBenchmark(std::string("BM_LadderWarm/form:") + spike::to_string(form),
                        [form](benchmark::State& s) { ladder_row(s, form, Which::Warm); })
          ->Arg(t)
          ->Unit(benchmark::kMicrosecond);
    }
    for (const spike::Form form : kForms) {
      RegisterBenchmark(std::string("BM_LadderCold/form:") + spike::to_string(form),
                        [form](benchmark::State& s) { ladder_row(s, form, Which::Cold); })
          ->Arg(t)
          ->Unit(benchmark::kMicrosecond);
    }
  }
  pair("BM_Build", kTrades[0], benchmark::kMillisecond, build_row);
  pair("BM_Record", kTrades[0], benchmark::kMillisecond, record_row);
}

}  // namespace

int main(int argc, char** argv) {
  benchmark::Initialize(&argc, argv);
  register_all();
  if (benchmark::ReportUnrecognizedArguments(argc, argv)) return 1;
  benchmark::RunSpecifiedBenchmarks();
  benchmark::Shutdown();
  return 0;
}
