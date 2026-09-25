// P0/oracle: the M1 book's NAIVE `double` path measured against ground truth (PRINCIPLES.md §4; D72).
//
// This is the measurement the package exists to produce. Nobody has ever measured it: every
// numerical gate in M1-M4 compared one `double` evaluation against another, so "agreement" has
// always been agreement with an uncharacterised approximation. What follows is how far the naive
// path actually is from the recorded expression's true value.
//
// WHAT IS THE NAIVE PATH HERE. `price_book<double>` (fixtures/m1_price.hpp), the templated maths
// D3 makes the single source, instantiated on `double`. Ground truth is `price_book<Wide>` — the
// SAME source text at 106 significand bits. Nothing is reimplemented on either side.
//
// WHY THIS FILE IS NOT PINNED. It is deliberately not `*_e0_test.cpp`, so the naive side carries
// the preset's own flags: run under `release` it measures the path with FMA contraction, under
// `reference` the path without it. Those are two different naive paths and the difference between
// the two runs is itself a result (D30 found contraction of the reference alone reaches 13 ulps).
// The truth side does not move between them — scalar/wide.hpp's contraction independence, measured
// in tests/scalar/wide_test.cpp — so the whole difference belongs to the naive path.
//
// Not a mutation gate: `maths_m1_oracle_error_test` does not match D33's gate regex. It is a
// measurement, and it costs about 60x a double evaluation per state.
#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <vector>

#include "epykos/exec/interpreter.hpp"
#include "epykos/fixtures/m1_book.hpp"
#include "epykos/fixtures/m1_differential.hpp"
#include "epykos/fixtures/m1_oracle.hpp"
#include "epykos/fixtures/m1_price.hpp"
#include "epykos/fixtures/m1_tangent.hpp"
#include "epykos/fixtures/record_m1.hpp"
#include "epykos/ir/program.hpp"
#include "epykos/ir/signature.hpp"
#include "epykos/scalar/wide.hpp"
#include "epykos/verify/differential.hpp"
#include "epykos/verify/oracle.hpp"
#include "epykos/version.hpp"

namespace exec = epykos::exec;
namespace fixtures = epykos::fixtures;
namespace ir = epykos::ir;
namespace verify = epykos::verify;
using epykos::Wide;

namespace {

constexpr int kStates = 64;  // 64 of the M2 ball's 256: each state is a full 106-bit book pricing

const fixtures::Book& book() {
  static const fixtures::Book b = fixtures::make_m1_book();
  return b;
}

void print_build(const char* what) {
  std::cout << "[ build    ] " << what << ": " << epykos::build_config() << " [" << epykos::build_flags() << "]"
            << ", oracle " << epykos::oracle_mantissa_bits_v<epykos::Oracle> << " bits\n";
}

}  // namespace

// ------------------------------------------------------------------------------------------------
// Valuation
// ------------------------------------------------------------------------------------------------

TEST(M1OracleError, NaivePathValuationErrorAgainstTruth) {
  const fixtures::Book& b = book();
  print_build("M1 valuation");
  std::cout << "[ fixture  ] the M1 book of docs/WORKLOADS.md §M1, seed " << fixtures::default_seed << ": "
            << b.n_swaps << " swaps, " << b.row_tau.size() << " coupon rows, " << fixtures::n_knots
            << " curve knots; " << kStates << " states on the M2 ball (rho 0.005)\n";

  verify::BallOptions bo;
  bo.draws = kStates;
  const verify::StateBall ball = fixtures::m1_state_ball(b, bo);
  const int n_out = b.n_swaps + 1;

  const verify::BatchFn naive = verify::batch_of(fixtures::m1_reference_fn(b), fixtures::n_knots, n_out);
  const verify::OracleFn truth = fixtures::m1_oracle_fn(b);

  // Two readings of the same measurement (verify/oracle.hpp): against the D26 leg scale, which is
  // how well the ARITHMETIC was done, and against the value itself, which is how wrong the ANSWER
  // is. A swap PV is a difference of legs that can cancel to 1e-6 of the leg size, so the two
  // differ by the conditioning and reporting only one of them would mislead.
  verify::TruthOptions opt;
  opt.classes = fixtures::m1_output_classes(b);
  opt.scale = fixtures::m1_leg_scale_fn(b);

  const auto t0 = std::chrono::steady_clock::now();
  const verify::TruthReport rep = verify::error_against_truth(ball, n_out, naive, truth, opt);
  const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

  std::cout << "[ measured ] " << rep.summary() << "\n";
  std::cout << rep.per_class_table();
  std::cout << "[ measured ] " << (static_cast<long>(n_out) * kStates) << " (output, state) pairs in "
            << std::fixed << std::setprecision(1) << seconds << " s\n";

  const verify::ClassTruth& swaps = rep.classes.at(0);
  const verify::ClassTruth& bookpv = rep.classes.at(1);
  std::cout << std::scientific << std::setprecision(3)
            << "[ measured ] swap PV  : max " << swaps.max_rel << " of the leg scale (" << swaps.max_ulps
            << " ulps), rms " << swaps.rms_rel << ", max vs the value itself " << swaps.max_rel_self << "\n"
            << "[ measured ] book PV  : max " << bookpv.max_rel << " of the leg scale (" << bookpv.max_ulps
            << " ulps), rms " << bookpv.rms_rel << ", max vs the value itself " << bookpv.max_rel_self << "\n";

  // The oracle must have been at full precision everywhere, or the figures above mean less than
  // they claim. This is the guard, not a formality.
  EXPECT_EQ(rep.degraded, 0u);

  // No tolerance is asserted on the naive path: PRINCIPLES.md §4 puts the per-class numbers in
  // PROBLEM.md, and they should be set FROM this measurement rather than before it. What is
  // asserted is that the instrument resolved something -- an all-zero report would mean the
  // oracle had silently collapsed onto the thing it is judging, which is the failure the owner
  // named. Loose bounds, stated as sanity rails and not as a contract.
  EXPECT_GT(swaps.max_rel, 0.0) << "the oracle resolved no difference at all: suspect the oracle";
  EXPECT_LT(swaps.max_rel, 1e-12) << "the naive path is far worse than any previous gate suggested";
  EXPECT_LT(bookpv.max_rel, 1e-12);
}

// The instrument's own headroom, on the real fixture rather than in the abstract. Two quantities
// are easy to confuse here and the distinction is the whole point of the package:
//
//   truth.tail()  is what a DOUBLE cannot see -- the part of the true value below ulp(hi). For a
//                 book PV of 1e7 that is around 1e-9, and it is a property of double, not of the
//                 oracle. It is NOT the oracle's uncertainty and using it as one would understate
//                 the headroom by sixteen decades.
//   |truth|·2^-106 is what the ORACLE cannot see, and that is the instrument's resolution.
TEST(M1OracleError, TheOraclesOwnResolutionIsFarBelowWhatItMeasures) {
  const fixtures::Book& b = book();
  std::vector<Wide> zw(fixtures::n_knots);
  for (int k = 0; k < fixtures::n_knots; ++k) zw[static_cast<std::size_t>(k)] = Wide(b.z0[static_cast<std::size_t>(k)]);
  std::vector<Wide> wout(static_cast<std::size_t>(b.n_swaps) + 1);
  fixtures::price_book<Wide>(b, zw.data(), wout.data(), wout.data() + b.n_swaps);

  std::vector<double> dout(static_cast<std::size_t>(b.n_swaps) + 1);
  fixtures::price_book<double>(b, b.z0.data(), dout.data(), dout.data() + b.n_swaps);

  const Wide& truth = wout[static_cast<std::size_t>(b.n_swaps)];
  const double naive = dout[static_cast<std::size_t>(b.n_swaps)];
  const double naive_err = std::fabs((Wide(naive) - truth).hi);

  // One oracle rounding at this magnitude, and a deliberately pessimistic bound on the whole
  // evaluation: the book is a 1,000-swap fold over 31,806 coupon rows, so no dependency chain is
  // longer than about 1e5 operations and a worst-case all-aligned accumulation is 1e5 of them.
  const double one_rounding = std::fabs(truth.value()) * 0x1p-106;
  const double pessimistic = 1e5 * one_rounding;

  EXPECT_TRUE(truth.has_full_precision());
  EXPECT_GT(naive_err, 1e6 * pessimistic)
      << "the naive error is not comfortably above even a pessimistic bound on the oracle's own";

  std::cout << std::scientific << std::setprecision(6) << "[ headroom ] book PV at the record point: naive "
            << std::setprecision(17) << naive << "\n"
            << "[ headroom ]                            truth " << truth.value() << " + " << std::scientific
            << std::setprecision(3) << truth.tail() << "\n"
            << "[ headroom ] naive absolute error        " << naive_err << "\n"
            << "[ headroom ] one oracle rounding here    " << one_rounding << " (" << (naive_err / one_rounding)
            << "x smaller than the error being measured)\n"
            << "[ headroom ] pessimistic 1e5-op bound    " << pessimistic << " (" << (naive_err / pessimistic)
            << "x smaller)\n"
            << "[ headroom ] for contrast, truth.tail()  " << std::fabs(truth.tail())
            << " -- what DOUBLE cannot see, which is a property of double and not of the oracle\n";
}
// ------------------------------------------------------------------------------------------------
// Sensitivities
// ------------------------------------------------------------------------------------------------

TEST(M1OracleError, NaivePathSensitivityErrorAgainstTruth) {
  const fixtures::Book& b = book();
  print_build("M1 sensitivities");

  // The naive derivative path: the SAME templated maths on Dual<n_knots> (scalar/dual.hpp), whose
  // value channel is bitwise price_book<double> operation for operation. This is what a risk
  // number on this engine is made of. M2 measured the mechanical adjoint agreeing with this Dual
  // pass to 2.1e-13 relative on this book (D30/D31), so the adjoint's distance from truth is
  // within that of what is measured here -- transferred from a green gate, not re-measured.
  const fixtures::Jacobian naive = fixtures::jacobian_forward_wide(b, b.z0.data());
  ASSERT_EQ(naive.n_inputs, fixtures::n_knots);
  ASSERT_EQ(naive.n_outputs, b.n_swaps + 1);

  const auto t0 = std::chrono::steady_clock::now();
  const std::vector<Wide> truth = fixtures::m1_oracle_jacobian(b, b.z0.data());
  const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  ASSERT_EQ(truth.size(), static_cast<std::size_t>(naive.n_outputs) * fixtures::n_knots);

  // Per output class, exactly as the valuation channel reports: the swap sensitivities and the
  // book's. The scale of a derivative is the derivative itself -- a gradient entry is not a
  // difference of cancelling terms the way a swap PV is -- so one reading suffices here, but the
  // worst entry is reported with its magnitude so a reader can see whether it is a small one.
  struct Acc {
    const char* name;
    double max_rel = 0.0;
    double sum_sq = 0.0;
    long n = 0;
    int worst_o = -1, worst_k = -1;
    double worst_truth = 0.0, worst_naive = 0.0;
    void add(int o, int k, double naive_v, const Wide& truth_v) {
      const double mag = std::fabs(truth_v.hi);
      if (mag == 0.0) return;  // an exactly-zero sensitivity has no relative error to report
      const double rel = std::fabs(((Wide(naive_v) - truth_v) / Wide(mag)).hi);
      sum_sq += rel * rel;
      ++n;
      if (rel > max_rel) {
        max_rel = rel;
        worst_o = o;
        worst_k = k;
        worst_truth = truth_v.hi;
        worst_naive = naive_v;
      }
    }
    double rms() const { return n > 0 ? std::sqrt(sum_sq / static_cast<double>(n)) : 0.0; }
  };
  Acc swaps{"sensitivity: d swap PV / d knot"};
  Acc bookpv{"sensitivity: d book PV / d knot"};

  int degraded = 0;
  for (int o = 0; o < naive.n_outputs; ++o) {
    Acc& acc = (o == b.n_swaps) ? bookpv : swaps;
    for (int k = 0; k < fixtures::n_knots; ++k) {
      const Wide& t = truth[static_cast<std::size_t>(o) * fixtures::n_knots + static_cast<std::size_t>(k)];
      if (!t.has_full_precision()) ++degraded;
      acc.add(o, k, naive.at(o, k), t);
    }
  }
  EXPECT_EQ(degraded, 0);

  std::cout << "[ measured ] oracle Jacobian: " << naive.n_outputs << " outputs x " << fixtures::n_knots
            << " knots, Richardson central differences at 106 bits (" << 4 * fixtures::n_knots
            << " oracle evaluations), " << std::fixed << std::setprecision(1) << seconds << " s\n";
  for (const Acc* a : {&swaps, &bookpv}) {
    std::cout << std::scientific << std::setprecision(3) << "[ measured ] " << a->name << ": max " << a->max_rel
              << ", rms " << a->rms() << " over " << a->n << " entries";
    if (a->worst_o >= 0) {
      std::cout << "; worst at (output " << a->worst_o << ", knot " << a->worst_k << ") truth " << a->worst_truth
                << " naive " << a->worst_naive;
    }
    std::cout << "\n";
  }

  EXPECT_GT(swaps.max_rel, 0.0) << "the oracle resolved no difference at all: suspect the oracle";
  EXPECT_LT(swaps.max_rel, 1e-9);
  EXPECT_LT(bookpv.max_rel, 1e-9);
}

// ------------------------------------------------------------------------------------------------
// An execution path against truth (PRINCIPLES.md §4 as rewritten 2026-09-25)
// ------------------------------------------------------------------------------------------------

// §4 retired execution as a tier of its own: "the interpreter, the catalogue kernels and the
// adjoint are implementations of the recorded maths like any rewrite, and are judged the same way."
// It also states the preference this test acts on: "prefer measuring each path against the oracle
// over comparing the two paths to each other, where the oracle is affordable — it is the stronger
// statement and it says which path is wrong, not merely that they differ."
//
// So this measures the COMPILED program, exec::Interpreter over infer(record_m1(book)), against the
// same 106-bit truth the templated `double` path was measured against above. No new harness code is
// needed: `error_against_truth` takes differential.hpp's `BatchFn`, and `interpreter_fn` is already
// the adapter, so an execution path drops in exactly where a reference implementation does.
//
// What the two numbers together say, which neither says alone: the interpreter's error against
// truth is the error a user of this engine actually gets, and the templated path's error is what it
// would be if the engine did not exist. tests/verify/m1_differential_e0_test.cpp separately holds
// the two bitwise equal, so they are expected to coincide here — and if they ever stop coinciding,
// this test says WHICH of them moved away from truth, which a path-against-path comparison cannot.
TEST(M1OracleError, CompiledInterpreterAgainstTruth) {
  const fixtures::Book& b = book();
  print_build("M1 interpreter");

  const epykos::Tape tape = fixtures::record_m1(b);
  const ir::Program program = ir::infer(tape);
  const int n_out = static_cast<int>(tape.num_outputs());
  ASSERT_EQ(n_out, b.n_swaps + 1);

  exec::Options iopt;
  iopt.max_batch = 8;
  exec::Interpreter in(program, iopt);

  verify::BallOptions bo;
  bo.draws = kStates;
  const verify::StateBall ball = fixtures::m1_state_ball(b, bo);

  verify::TruthOptions opt;
  opt.classes = fixtures::m1_output_classes(b);
  opt.scale = fixtures::m1_leg_scale_fn(b);
  opt.batch = 8;

  const verify::TruthReport rep =
      verify::error_against_truth(ball, n_out, verify::interpreter_fn(in), fixtures::m1_oracle_fn(b), opt);

  std::cout << "[ measured ] interpreter (" << program.domains.size() << " IR domains, batch 8) vs truth\n";
  std::cout << rep.per_class_table();
  EXPECT_EQ(rep.degraded, 0u);
  EXPECT_GT(rep.classes.at(0).max_rel, 0.0);
  EXPECT_LT(rep.classes.at(0).max_rel, 1e-12);

  // The same measurement on the templated `double` path, for the comparison §4 asks for: which
  // path is closer to truth, not merely whether they differ.
  const verify::BatchFn naive = verify::batch_of(fixtures::m1_reference_fn(b), fixtures::n_knots, n_out);
  const verify::TruthReport ref =
      verify::error_against_truth(ball, n_out, naive, fixtures::m1_oracle_fn(b), opt);
  std::cout << std::scientific << std::setprecision(3) << "[ measured ] swap PV max rel vs truth: interpreter "
            << rep.classes.at(0).max_rel << ", templated double " << ref.classes.at(0).max_rel << "; book PV "
            << rep.classes.at(1).max_rel << " and " << ref.classes.at(1).max_rel << "\n";
}
