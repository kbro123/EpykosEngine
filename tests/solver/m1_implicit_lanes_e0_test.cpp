// M3/G4 E0 gate: batch lanes through the implicit node (PROBLEM.md §6 O4). Eight lanes of
// shocked quotes through ImplicitProgram::run and ::adjoint equal the eight single-lane runs
// bitwise — every output, the solved curve, the diagnostics and the quote adjoints — under the
// per-iteration policy and under the chord policy (the record-point factorisation shared across
// lanes); identical lanes served by one de-duplicated solve are bitwise the un-deduplicated
// solves; the whole-program interpreter at the solved state is bitwise price_book<double>
// instantiated in this contraction-free TU.
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <vector>

#include "epykos/fixtures/m1_book.hpp"
#include "epykos/fixtures/m1_calibration.hpp"
#include "epykos/fixtures/m1_price.hpp"
#include "epykos/rng/philox.hpp"
#include "epykos/solver/implicit_program.hpp"

#ifndef EPYKOS_FP_CONTRACT_OFF
#error "an _e0_test.cpp TU must see EPYKOS_FP_CONTRACT_OFF"
#endif

namespace fixtures = epykos::fixtures;
namespace solver = epykos::solver;

namespace {

constexpr int n_knots = fixtures::n_knots;
constexpr int B = 8;

struct Fixture {
  fixtures::Book book;
  std::vector<double> quotes;
  fixtures::CalibratedM1 cal;
  std::vector<double> shocked;  // n_knots × B, batch innermost: lane b = quotes + N(0, 10 bp) from sub-stream 600000 + b
};

const Fixture& fixture() {
  static const Fixture f = [] {
    Fixture x;
    x.book = fixtures::make_m1_book();
    x.quotes = fixtures::m1_par_quotes(x.book.z0.data());
    std::vector<double> start(n_knots, 0.03);
    x.cal = fixtures::record_m1_calibrated(x.book, x.quotes, start);
    x.shocked.assign(static_cast<std::size_t>(n_knots) * B, 0.0);
    for (int b = 0; b < B; ++b) {
      epykos::rng::Philox g(fixtures::default_seed, 600000 + static_cast<std::uint64_t>(b));
      for (std::size_t k = 0; k < static_cast<std::size_t>(n_knots); ++k) {
        x.shocked[k * B + static_cast<std::size_t>(b)] = x.quotes[k] + (b == 0 ? 0.0 : 0.0010 * g.gaussian());
      }
    }
    return x;
  }();
  return f;
}

std::vector<double> lane_state(const Fixture& f, int b) {
  std::vector<double> q(static_cast<std::size_t>(n_knots));
  for (std::size_t k = 0; k < q.size(); ++k) q[k] = f.shocked[k * B + static_cast<std::size_t>(b)];
  return q;
}

void check_lanes(solver::ProgramOptions opts, const char* what) {
  const Fixture& f = fixture();
  solver::ImplicitProgram prog(f.cal.tape, f.cal.registry, opts);
  const int n_out = prog.n_outputs();
  const std::size_t Bs = B;
  std::vector<double> out(static_cast<std::size_t>(n_out) * Bs), out1(static_cast<std::size_t>(n_out));
  std::vector<double> out_bar(static_cast<std::size_t>(n_out) * Bs, 0.0), out_bar1(static_cast<std::size_t>(n_out), 0.0);
  std::vector<double> qbar(static_cast<std::size_t>(n_knots) * Bs), qbar1(static_cast<std::size_t>(n_knots));
  // Seeds: the book PV in even lanes, a swap PV in odd lanes.
  for (std::size_t b = 0; b < Bs; ++b) out_bar[static_cast<std::size_t>(b % 2 == 0 ? f.cal.book_pv : f.cal.swap_pv_begin + 100 * static_cast<int>(b)) * Bs + b] = 1.0;
  prog.run(f.shocked.data(), B, out.data());
  std::vector<double> full(static_cast<std::size_t>(prog.n_inputs()) * Bs);
  for (std::size_t b = 0; b < Bs; ++b) {
    for (int o = 0; o < prog.n_inputs(); ++o) full[static_cast<std::size_t>(o) * Bs + b] = prog.full_state(static_cast<int>(b))[o];
  }
  std::vector<solver::SolveReport> reports;
  for (int b = 0; b < B; ++b) reports.push_back(prog.report(0, b));
  std::size_t mismatches = 0;
  int iterations_sum = 0;
  for (int b = 0; b < B; ++b) {
    const std::vector<double> q = lane_state(f, b);
    prog.run(q.data(), 1, out1.data());
    EXPECT_TRUE(prog.report(0, 0).converged) << what << " lane " << b;
    iterations_sum += prog.report(0, 0).iterations;
    EXPECT_EQ(prog.report(0, 0).iterations, reports[static_cast<std::size_t>(b)].iterations) << what << " lane " << b;
    for (int o = 0; o < n_out; ++o) {
      if (out1[static_cast<std::size_t>(o)] != out[static_cast<std::size_t>(o) * Bs + static_cast<std::size_t>(b)]) ++mismatches;
    }
    for (int o = 0; o < prog.n_inputs(); ++o) {
      if (prog.full_state(0)[o] != full[static_cast<std::size_t>(o) * Bs + static_cast<std::size_t>(b)]) ++mismatches;
    }
  }
  EXPECT_EQ(mismatches, 0u) << what << ": forward lanes differ from single runs";
  // The adjoint: lanes vs single runs.
  prog.adjoint(f.shocked.data(), B, out_bar.data(), out.data(), qbar.data());
  std::size_t adj_mismatches = 0;
  for (int b = 0; b < B; ++b) {
    const std::vector<double> q = lane_state(f, b);
    for (int o = 0; o < n_out; ++o) out_bar1[static_cast<std::size_t>(o)] = out_bar[static_cast<std::size_t>(o) * Bs + static_cast<std::size_t>(b)];
    prog.adjoint(q.data(), 1, out_bar1.data(), out1.data(), qbar1.data());
    for (int k = 0; k < n_knots; ++k) {
      if (qbar1[static_cast<std::size_t>(k)] != qbar[static_cast<std::size_t>(k) * Bs + static_cast<std::size_t>(b)]) ++adj_mismatches;
    }
    for (int o = 0; o < n_out; ++o) {
      if (out1[static_cast<std::size_t>(o)] != out[static_cast<std::size_t>(o) * Bs + static_cast<std::size_t>(b)]) ++adj_mismatches;
    }
  }
  EXPECT_EQ(adj_mismatches, 0u) << what << ": adjoint lanes differ from single runs";
  std::cout << "[  " << what << " ] " << prog.last_run().to_string() << "; iterations over the 8 lanes " << iterations_sum
            << "; forward mismatches " << mismatches << ", adjoint mismatches " << adj_mismatches << '\n';
}

}  // namespace

TEST(M1ImplicitLanes, EightShockedLanesEqualEightSingleRunsBitwisePerIterationPolicy) {
  solver::ProgramOptions o;
  o.jacobian = static_cast<int>(solver::JacobianPolicy::per_iteration);
  check_lanes(o, "newton");
}

TEST(M1ImplicitLanes, EightShockedLanesEqualEightSingleRunsBitwiseChordPolicy) {
  solver::ProgramOptions o;
  o.jacobian = static_cast<int>(solver::JacobianPolicy::chord);
  check_lanes(o, "chord ");
}

TEST(M1ImplicitLanes, DeduplicatedIdenticalLanesAreBitwiseTheSeparateSolves) {
  const Fixture& f = fixture();
  solver::ProgramOptions on, off;
  off.dedup_lanes = false;
  solver::ImplicitProgram a(f.cal.tape, f.cal.registry, on), b(f.cal.tape, f.cal.registry, off);
  const std::size_t Bs = B;
  std::vector<double> state(static_cast<std::size_t>(n_knots) * Bs);
  for (std::size_t k = 0; k < static_cast<std::size_t>(n_knots); ++k) {
    for (std::size_t l = 0; l < Bs; ++l) state[k * Bs + l] = f.shocked[k * Bs + (l % 2 == 0 ? 1 : 2)];  // two distinct states
  }
  const int n_out = a.n_outputs();
  std::vector<double> oa(static_cast<std::size_t>(n_out) * Bs), ob(static_cast<std::size_t>(n_out) * Bs);
  std::vector<double> out_bar(static_cast<std::size_t>(n_out) * Bs, 0.0), qa(static_cast<std::size_t>(n_knots) * Bs), qb(static_cast<std::size_t>(n_knots) * Bs);
  for (std::size_t l = 0; l < Bs; ++l) out_bar[static_cast<std::size_t>(f.cal.book_pv) * Bs + l] = 1.0;
  a.adjoint(state.data(), B, out_bar.data(), oa.data(), qa.data());
  b.adjoint(state.data(), B, out_bar.data(), ob.data(), qb.data());
  EXPECT_EQ(a.last_run().solves, 2);
  EXPECT_EQ(a.last_run().shared_lanes, B - 2);
  EXPECT_EQ(b.last_run().solves, B);
  EXPECT_EQ(oa, ob);
  EXPECT_EQ(qa, qb);
  std::cout << "[  dedup   ] " << a.last_run().to_string() << " vs " << b.last_run().to_string() << '\n';
}

TEST(M1ImplicitLanes, WholeProgramAtTheSolvedStateIsBitwiseTheOracle) {
  const Fixture& f = fixture();
  solver::ImplicitProgram prog(f.cal.tape, f.cal.registry);
  const int n_out = prog.n_outputs();
  std::vector<double> out(static_cast<std::size_t>(n_out) * B);
  prog.run(f.shocked.data(), B, out.data());
  std::vector<double> ref(static_cast<std::size_t>(f.book.n_swaps) + 1);
  std::size_t mismatches = 0;
  for (int b = 0; b < B; ++b) {
    const double* z = prog.full_state(b) + f.cal.registry.blocks[0].unknowns[0];
    fixtures::price_book<double>(f.book, z, ref.data(), ref.data() + f.book.n_swaps);
    for (int i = 0; i < f.book.n_swaps; ++i) {
      if (out[static_cast<std::size_t>(f.cal.swap_pv_begin + i) * B + static_cast<std::size_t>(b)] != ref[static_cast<std::size_t>(i)]) ++mismatches;
    }
    if (out[static_cast<std::size_t>(f.cal.book_pv) * B + static_cast<std::size_t>(b)] != ref[static_cast<std::size_t>(f.book.n_swaps)]) ++mismatches;
  }
  EXPECT_EQ(mismatches, 0u);
}
