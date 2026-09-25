// MX: the head-to-head risk ladder (fixtures/compare_ois.hpp; D21, D71).
//
// This engine's side of the comparison `bench/compare/README.md` describes. The fixture is one
// USD SOFR OIS curve, sixteen par-rate calibration instruments on sixteen knots, and a book of
// OIS trades; the quantity timed is O3, the bucketed delta ladder d(book PV)/d(quote) through
// the adjoint and the IFT — the same quantity the other engine reports a `risk_us` for, on the
// identical problem, verified to agree first (that verification is the gate on quoting any of
// these numbers at all).
//
//   BM_LadderChord/T  ONE book-level ladder row, T trades, warm start AND the chord Jacobian
//                     policy: the lane starts from the tape's record-point solution, the ladder
//                     is taken at the record quotes, so the solve begins AT its solution and the
//                     record-point factorisation it reuses is the exact one the IFT needs. This
//                     is the row matched against the other engine's `risk_us`, which is taken on
//                     a curve that is already calibrated and reuses its calibration Jacobian.
//   BM_LadderWarm/T   the same, but each lane builds its own Jacobian (the default policy). The
//                     difference between this row and the chord row IS the calibration Jacobian.
//   BM_LadderCold/T   the same ladder from the block's own flat start, i.e. a full
//                     recalibration and then the ladder. The honest upper bound: nothing about
//                     the warm row is hidden by leaving this one out.
//   BM_Evaluate/T     the whole-program interpreter alone at the solved state, one lane — the
//                     forward half, for context on where the ladder's time goes.
//   BM_Calibrate/T    O1 + O2 with no ladder: one full recalibration from the flat start and the
//                     forward pass. The closest analogue to a "curve build", and OURS ONLY — the
//                     other engine's stateless response reports no calibration time, so there is
//                     no matched number to put beside it.
//   BM_Build/T        solver::ImplicitProgram construction (inference, residual programs,
//                     interpreter, adjoint): structure time, paid once, never per ladder.
//
// Run, e.g.:
//   bench/run.sh build/release/bench/compare_ois_ladder_bench --benchmark_repetitions=20
#include <benchmark/benchmark.h>

#include <cstddef>
#include <map>
#include <memory>
#include <vector>

#include "epykos/fixtures/compare_ois.hpp"
#include "epykos/solver/implicit_program.hpp"

namespace {

namespace fixtures = epykos::fixtures;
namespace solver = epykos::solver;

// The book sizes the comparison reports. Sixteen, sixty-four and two hundred and fifty-six
// trades on the `compare_ois` fixture (D64), all against the same sixteen-quote curve.
constexpr int kTrades[] = {16, 64, 256};

struct Fixture {
  fixtures::CompareOis s;
  fixtures::CompareOisTape tape;
  std::unique_ptr<solver::ImplicitProgram> chord;   // warm start + the record-point factorisation
  std::unique_ptr<solver::ImplicitProgram> warm;    // warm start, its own Jacobian
  std::unique_ptr<solver::ImplicitProgram> cold;    // the block's flat start

  explicit Fixture(int trades) {
    fixtures::CompareOisOptions o;
    o.trades = trades;
    s = fixtures::make_compare_ois(o);
    tape = fixtures::record_compare_ois(s);
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

// Built once per trade count and shared by every benchmark that needs it: the fixture's own
// build is structure time (BM_Build times it on purpose) and does not belong in a ladder row.
Fixture& fixture(int trades) {
  static std::map<int, std::unique_ptr<Fixture>> cache;
  std::unique_ptr<Fixture>& f = cache[trades];
  if (!f) f = std::make_unique<Fixture>(trades);
  return *f;
}

enum class Which { Chord, Warm, Cold };

void ladder_row(benchmark::State& state, Which which) {
  const int trades = static_cast<int>(state.range(0));
  Fixture& f = fixture(trades);
  solver::ImplicitProgram& prog =
      which == Which::Chord ? *f.chord : (which == Which::Warm ? *f.warm : *f.cold);
  const std::vector<int> ordinals = {f.tape.book_output};
  std::vector<double> out;
  for (auto _ : state) {
    const std::vector<double> rows = fixtures::compare_ois_ladder(prog, f.s.quotes, ordinals, &out);
    benchmark::DoNotOptimize(rows.data());
    benchmark::ClobberMemory();
  }
  const solver::ImplicitProgram::RunStats& r = prog.last_run();
  state.counters["trades"] = trades;
  state.counters["quotes"] = f.s.n_quotes();
  state.counters["solves"] = r.solves;
  state.counters["residual_evals"] = static_cast<double>(r.residual_evaluations);
  state.counters["jacobians"] = static_cast<double>(r.jacobians);
}

void BM_LadderChord(benchmark::State& state) { ladder_row(state, Which::Chord); }
void BM_LadderWarm(benchmark::State& state) { ladder_row(state, Which::Warm); }
void BM_LadderCold(benchmark::State& state) { ladder_row(state, Which::Cold); }

void BM_Evaluate(benchmark::State& state) {
  const int trades = static_cast<int>(state.range(0));
  Fixture& f = fixture(trades);
  solver::ImplicitProgram& prog = *f.warm;
  const int n_in = prog.n_inputs(), n_out = prog.n_outputs();
  // One solved run first: the interpreter is timed at the SOLVED full state, which only exists
  // after a run has produced it (full_state throws otherwise).
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
  state.counters["trades"] = trades;
}

// O1 + O2 with no ladder: one full recalibration from the block's flat start, then the forward
// pass. This is the closest analogue to a "curve build", and it is reported for THIS ENGINE ONLY
// -- the other engine's stateless JSON response carries `risk_us` and `price_us` but no
// calibration time, so unlike the ladder there is no matched number to put beside it
// (bench/compare/README.md §6). Context, not a head-to-head row.
void BM_Calibrate(benchmark::State& state) {
  const int trades = static_cast<int>(state.range(0));
  Fixture& f = fixture(trades);
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
  state.counters["trades"] = trades;
  state.counters["solves"] = r.solves;
  state.counters["residual_evals"] = static_cast<double>(r.residual_evaluations);
  state.counters["jacobians"] = static_cast<double>(r.jacobians);
}

void BM_Build(benchmark::State& state) {
  const int trades = static_cast<int>(state.range(0));
  Fixture& f = fixture(trades);
  for (auto _ : state) {
    solver::ImplicitProgram p(f.tape.tape, f.tape.registry, fixtures::compare_ois_program_options(f.s));
    benchmark::DoNotOptimize(&p);
  }
  state.counters["trades"] = trades;
}

void register_all() {
  for (int t : kTrades) {
    benchmark::RegisterBenchmark("BM_LadderChord", BM_LadderChord)->Arg(t)->Unit(benchmark::kMicrosecond);
    benchmark::RegisterBenchmark("BM_LadderWarm", BM_LadderWarm)->Arg(t)->Unit(benchmark::kMicrosecond);
    benchmark::RegisterBenchmark("BM_LadderCold", BM_LadderCold)->Arg(t)->Unit(benchmark::kMicrosecond);
    benchmark::RegisterBenchmark("BM_Evaluate", BM_Evaluate)->Arg(t)->Unit(benchmark::kMicrosecond);
    benchmark::RegisterBenchmark("BM_Calibrate", BM_Calibrate)->Arg(t)->Unit(benchmark::kMicrosecond);
  }
  benchmark::RegisterBenchmark("BM_Build", BM_Build)->Arg(kTrades[0])->Unit(benchmark::kMillisecond);
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
