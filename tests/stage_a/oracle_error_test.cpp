// P0/oracle: the Stage A book's NAIVE `double` path measured against ground truth
// (PRINCIPLES.md §4; D72). The second half of the package's headline measurement; the first is
// tests/maths/m1_oracle_error_test.cpp.
//
// SCOPE, which is a contract question and not a convenience. The Stage A problem is
// quotes -> calibration solve -> knots -> book. What is measured here is the second arrow: given
// the solved knots, the book. fixtures/stage_a_oracle.hpp argues why at length; in short, D7 makes
// the solve an implicit node whose solution the tape carries as INPUTS, so "the recorded
// expression evaluated without rounding" downstream of it means exactly this; PRINCIPLES.md §3
// puts the solver out of scope; and `solver::CurveSet` type-erases every residual to `Rec` and
// `double` only, so a third Scalar cannot reach the calibration at all. That last point is a
// finding of this package, recorded in D72, not worked around here.
//
// Not a mutation gate: `stage_a_oracle_error_test` does not match D33's gate regex. Not pinned, so
// the naive side carries the preset's flags (contracted under `release`, not under `reference`)
// while the truth side does not move between them.
#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

#include "epykos/fixtures/stage_a.hpp"
#include "epykos/maths/instrument/instrument.hpp"
#include "epykos/fixtures/stage_a_oracle.hpp"
#include "epykos/scalar/wide.hpp"
#include "epykos/verify/differential.hpp"
#include "epykos/verify/oracle.hpp"
#include "epykos/version.hpp"
#include "stage_a/stage_a_test_helpers.hpp"

namespace fixtures = epykos::fixtures;
namespace verify = epykos::verify;
using epykos::Wide;
using epykos::test::stage_a;
using epykos::test::stage_a_tape;

namespace {

constexpr int kStates = 16;  // each state is a 106-bit pricing of 2,000 trades

// The record-point knot state, flattened in curve order: the centre of the ball.
const std::vector<double>& centre() {
  static const std::vector<double> z = fixtures::stage_a_flat_knots(stage_a_tape().record_knots);
  return z;
}

}  // namespace

// Before measuring anything with it, the fixture's own `double` side must be the reference every
// other Stage A gate already uses. Otherwise a difference between them would be silently reported
// as the naive path's error against truth.
TEST(StageAOracleError, TheNaiveSideIsBitwiseThePriceStageAAtReferenceEveryOtherGateUses) {
  const fixtures::StageA& s = stage_a();
  const fixtures::StageATape& t = stage_a_tape();
  const int n_out = fixtures::stage_a_oracle_outputs(s);

  std::vector<double> mine(static_cast<std::size_t>(n_out), 0.0);
  fixtures::stage_a_naive_fn(s, *t.set)(centre().data(), mine.data());

  const fixtures::StageABook<double> ref = fixtures::price_stage_a_at(s, *t.set, t.record_knots);
  std::vector<double> theirs(static_cast<std::size_t>(n_out), 0.0);
  fixtures::stage_a_flatten<double>(s, ref, theirs.data());

  int mismatches = 0;
  for (int o = 0; o < n_out; ++o) {
    if (epykos::test::bits(mine[static_cast<std::size_t>(o)]) != epykos::test::bits(theirs[static_cast<std::size_t>(o)])) {
      ++mismatches;
    }
  }
  EXPECT_EQ(mismatches, 0);
  std::cout << "[ anchor   ] " << n_out
            << " outputs bitwise identical to fixtures::price_stage_a_at, the reference "
               "tests/stage_a/gate_differential_e0_test.cpp holds the compiled program equal to -- so the "
               "error measured below is the compiled program's too, by transfer\n";
}

TEST(StageAOracleError, NaivePathValuationErrorAgainstTruth) {
  const fixtures::StageA& s = stage_a();
  const fixtures::StageATape& t = stage_a_tape();
  const int n_out = fixtures::stage_a_oracle_outputs(s);
  const int n_in = t.set->n_knots_total();

  std::cout << "[ build    ] Stage A valuation: " << epykos::build_config() << " [" << epykos::build_flags() << "]"
            << ", oracle " << epykos::oracle_mantissa_bits_v<epykos::Oracle> << " bits\n";
  std::cout << "[ fixture  ] the Stage A desk problem of docs/PROBLEM.md §4: " << s.n_trades() << " trades, "
            << s.n_quotes() << " quotes, " << s.n_curves() << " curves, " << n_in << " curve knots, " << n_out
            << " book outputs; " << kStates << " states on a ball around the record-point knots (rho 0.005)\n";

  verify::BallOptions bo;
  bo.draws = kStates;
  const verify::StateBall ball = verify::make_state_ball(centre().data(), n_in, bo);

  const verify::BatchFn naive = verify::batch_of(fixtures::stage_a_naive_fn(s, *t.set), n_in, n_out);
  const verify::OracleFn truth = fixtures::stage_a_oracle_fn(s, *t.set);

  verify::TruthOptions opt;
  opt.classes = fixtures::stage_a_output_classes(s);
  // D26's scale. Measured without it the trade-PV class reads 2.4e-08 while the leg-PV class,
  // which does not cancel, reads 1.6e-13 -- five decades apart on the same arithmetic. The first
  // number is the book's CONDITIONING and reporting it as the naive path's error would be wrong.
  // `max self` in the table below is still the unscaled reading, so both are visible.
  opt.scale = fixtures::stage_a_scale_fn(s, *t.set);

  const auto t0 = std::chrono::steady_clock::now();
  const verify::TruthReport rep = verify::error_against_truth(ball, n_out, naive, truth, opt);
  const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

  std::cout << "[ measured ] " << rep.summary() << "\n";
  std::cout << rep.per_class_table();
  std::cout << "[ measured ] " << (static_cast<long>(n_out) * kStates) << " (output, state) pairs in "
            << std::fixed << std::setprecision(1) << seconds << " s\n";

  EXPECT_EQ(rep.degraded, 0u);
  // As on M1: no tolerance is asserted, because PRINCIPLES.md §4 puts the per-class numbers in
  // PROBLEM.md and they should be set FROM this. Sanity rails only -- an all-zero report would
  // mean the oracle had collapsed onto the thing it judges.
  EXPECT_GT(rep.max_rel, 0.0) << "the oracle resolved no difference at all: suspect the oracle";
  EXPECT_LT(rep.max_rel, 1e-12) << "the naive path is far worse than any previous gate suggested";
}

// The oracle Jacobian must be h-STABLE, and this test exists because the fixture failed it.
//
// A finite difference at 106 bits is roundoff-limited at ~u/h = 4e-22 and truncation-limited at
// O(h^4) after Richardson, so over any sane range of h the answer must not move. A DOUBLE-precision
// difference, by contrast, is roundoff-limited at ~1e-16/h and gets visibly better as h grows.
// That signature is how the real defect here was found: an earlier draft of
// fixtures::stage_a_oracle_fn rounded the perturbed Wide state back to double
// (`z[k].value()`), quantising the step, and the resulting "naive path sensitivity error" of
// 1.33e-07 was the instrument's error rather than the engine's. The valuation channel was
// unaffected and gave no hint, because a ball's states are exactly representable doubles; nothing
// in verify/oracle.hpp could have caught it either, since the values were finite, normal and at
// full precision and the loss was in the INPUT. So the property is pinned here.
TEST(StageAOracleError, TheOracleJacobianIsStepIndependentWhichIsWhatMakesItTheOracles) {
  const fixtures::StageA& s = stage_a();
  const fixtures::StageATape& t = stage_a_tape();
  const int n_out = fixtures::stage_a_oracle_outputs(s);
  const int n_in = t.set->n_knots_total();

  const verify::OracleFn truth_fn = fixtures::stage_a_oracle_fn(s, *t.set);
  const std::vector<double> steps = {1e-13, 1e-11, 1e-9, 1e-7, 1e-5};
  std::vector<std::vector<Wide>> jac;
  for (double h : steps) {
    verify::JacobianOptions jo;
    jo.inputs = {0};
    jo.h = h;
    jac.push_back(verify::oracle_jacobian(truth_fn, centre().data(), n_in, n_out, jo));
  }

  // Movement against the largest h, which is the least roundoff-sensitive of them.
  const std::vector<Wide>& ref = jac.back();
  std::vector<double> worst(steps.size(), 0.0);
  for (std::size_t i = 0; i + 1 < steps.size(); ++i) {
    for (int o = 0; o < n_out; ++o) {
      const std::size_t os = static_cast<std::size_t>(o);
      const double mag = std::fabs(ref[os].hi);
      if (mag < 1.0) continue;  // a structurally-zero entry has no scale to be stable against
      worst[i] = std::max(worst[i], std::fabs(((jac[i][os] - ref[os]) / Wide(mag)).hi));
    }
  }
  std::cout << "[ h-stable ] oracle Jacobian movement against h = " << std::scientific << std::setprecision(0)
            << steps.back() << ", over all " << n_out << " outputs:\n";
  for (std::size_t i = 0; i + 1 < steps.size(); ++i) {
    std::cout << std::scientific << std::setprecision(0) << "[ h-stable ]   h = " << steps[i] << " : "
              << std::setprecision(3) << worst[i] << "\n";
  }

  // Over h in [1e-11, 1e-5] -- six decades -- the answer must not move beyond double's own noise.
  // A double-precision difference moves by ~1e-5 over the same range (measured in the diagnosis
  // that produced this test).
  for (std::size_t i = 1; i + 1 < steps.size(); ++i) {
    EXPECT_LT(worst[i], 1e-13) << "the oracle Jacobian is step-dependent at h = " << steps[i]
                               << ": something on the truth path is not at the oracle's precision";
  }
  // h = 1e-13 is deliberately past the useful range and is reported rather than asserted tightly:
  // at that step the oracle's own roundoff, |f|/|f'| * u/h, is visible for outputs whose value is
  // large and whose sensitivity to knot 0 is small. It must still be nowhere near the ~1e-3 a
  // double difference would show there, which is the discriminating comparison.
  EXPECT_LT(worst[0], 1e-9) << "even at an absurdly small step the oracle must beat a double difference by decades";
  std::cout << "[ h-stable ] at h = 1e-13 a DOUBLE central difference moves by ~1e-3; the oracle moves by "
            << std::scientific << std::setprecision(3) << worst[0] << "\n";
}

TEST(StageAOracleError, NaivePathSensitivityErrorAgainstTruth) {
  const fixtures::StageA& s = stage_a();
  const fixtures::StageATape& t = stage_a_tape();
  const int n_out = fixtures::stage_a_oracle_outputs(s);
  const int n_in = t.set->n_knots_total();
  const int n = s.n_trades();

  std::cout << "[ build    ] Stage A sensitivities: " << epykos::build_config() << " [" << epykos::build_flags()
            << "]\n";

  // The sensitivities in scope are d(book output) / d(curve knot): the part of the risk ladder the
  // optimiser can see. The rest of the ladder goes through the IFT on the calibration solve, which
  // PRINCIPLES.md §3 puts out of scope and which no Scalar but `double` can reach anyway.
  //
  // A SAMPLE of the knots, not all 70, and stated as one: the oracle Jacobian costs 4 evaluations
  // of the whole 2,000-trade book per knot. Every 7th knot spreads the sample across all four
  // curves rather than taking it from one.
  std::vector<int> take;
  for (int k = 0; k < n_in; k += 7) take.push_back(k);
  const std::size_t nt = take.size();

  const verify::OracleFn truth_fn = fixtures::stage_a_oracle_fn(s, *t.set);
  verify::JacobianOptions jo;
  jo.inputs = take;
  const auto t0 = std::chrono::steady_clock::now();
  const std::vector<Wide> truth = verify::oracle_jacobian(truth_fn, centre().data(), n_in, n_out, jo);
  const double jac_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  auto J = [&](int o, std::size_t j) -> const Wide& { return truth[static_cast<std::size_t>(o) * nt + j]; };

  // THE SCALE, which is D26 one derivative up and is what makes these numbers mean anything.
  //
  // d(pv_i)/dz = d(leg0_i)/dz - d(leg1_i)/dz is a difference of cancelling terms exactly as pv_i is,
  // and d(book)/dz is a 2,000-term fold of long and short trade sensitivities. Measured against the
  // entries themselves the worst reading on this fixture is 1.05e+12 at a true value of -1.4e-14 --
  // a report about a cell that is structurally empty. So each entry is scaled by the magnitude of
  // the TERMS that produced it, all of which are already in the Jacobian above:
  //
  //   d pv_i      |d leg0_i/dz_k| + |d leg1_i/dz_k|
  //   d pv_usd_i  the same, times the trade's FX placeholder
  //   d leg_i     itself: a leg is a same-signed sum of coupons and does not cancel
  //   aggregates  the sum of |d pv/dz_k| (or |d pv_usd/dz_k|) over the trades they fold
  //
  // An entry whose scale is below 1e-8 of the output's own value is a STRUCTURAL ZERO -- a trade
  // that does not reference that knot at all -- and is excluded and counted rather than divided by.
  // The separation is not delicate: a real dependency is of order the trade's duration, 1 to 30
  // times its PV, and a structural zero is around 1e-15 of it.
  std::vector<double> value(static_cast<std::size_t>(n_out), 0.0);
  fixtures::stage_a_naive_fn(s, *t.set)(centre().data(), value.data());

  std::vector<double> fx(static_cast<std::size_t>(n), 1.0);
  for (int i = 0; i < n; ++i) {
    const epykos::instrument::Instrument& in = s.instruments[static_cast<std::size_t>(i)];
    fx[static_cast<std::size_t>(i)] =
        in.currency == s.def.reporting_currency ? 1.0 : std::fabs(s.def.fx_placeholder(in.currency));
  }
  // Per sampled knot: the absolute-sum scales of the aggregates.
  std::vector<std::vector<double>> ccy_scale(nt, std::vector<double>(static_cast<std::size_t>(s.n_currencies()), 0.0));
  std::vector<std::vector<double>> net_scale(nt, std::vector<double>(static_cast<std::size_t>(s.n_netting_sets()), 0.0));
  std::vector<double> book_scale(nt, 0.0);
  for (std::size_t j = 0; j < nt; ++j) {
    for (int i = 0; i < n; ++i) {
      ccy_scale[j][static_cast<std::size_t>(s.def.currency_index(s.instruments[static_cast<std::size_t>(i)].currency))] +=
          std::fabs(J(i, j).hi);
      net_scale[j][static_cast<std::size_t>(s.trades[static_cast<std::size_t>(i)].netting_set)] +=
          std::fabs(J(n + i, j).hi);
      book_scale[j] += std::fabs(J(n + i, j).hi);
    }
  }
  auto scale_of = [&](int o, std::size_t j) -> double {
    if (o < n) return std::fabs(J(2 * n + o, j).hi) + std::fabs(J(3 * n + o, j).hi);
    if (o < 2 * n) {
      const int i = o - n;
      return (std::fabs(J(2 * n + i, j).hi) + std::fabs(J(3 * n + i, j).hi)) * fx[static_cast<std::size_t>(i)];
    }
    if (o < 4 * n) return std::fabs(J(o, j).hi);
    const int a = o - 4 * n;
    if (a < s.n_currencies()) return ccy_scale[j][static_cast<std::size_t>(a)];
    if (a < s.n_currencies() + s.n_netting_sets()) {
      return net_scale[j][static_cast<std::size_t>(a - s.n_currencies())];
    }
    return book_scale[j];
  };

  // TWO naive derivative paths, because they are different things and only the first is what the
  // engine produces:
  //   analytic  price_stage_a<Dual<1>> -- the recorded expression's exact derivative evaluated in
  //             double, which is what the mechanical adjoint computes by the other route (M3
  //             measured the two agreeing to 2.79e-15 on this tape, D35/D44);
  //   bumped    a central difference in double, a DIFFERENT algorithm carrying a truncation error
  //             of its own. Included as a contrast because it is what a desk means by a risk
  //             number, and the gap between the columns is why this engine has an adjoint.
  const double h_rel = std::cbrt(3.0 * 0x1p-53);
  std::vector<double> zp = centre(), zm = centre();
  std::vector<double> op(static_cast<std::size_t>(n_out)), om(static_cast<std::size_t>(n_out));
  std::vector<double> tang(static_cast<std::size_t>(n_out));
  const verify::ScalarFn naive_fn = fixtures::stage_a_naive_fn(s, *t.set);

  struct Acc {
    std::string name;
    int first = 0, count = 0;
    double max_analytic = 0.0, sum_sq_analytic = 0.0;
    double max_bumped = 0.0;
    long n = 0, skipped = 0;
    int worst_o = -1, worst_k = -1;
    double worst_truth = 0.0, worst_scale = 0.0;
    double rms_analytic() const { return n > 0 ? std::sqrt(sum_sq_analytic / static_cast<double>(n)) : 0.0; }
  };
  const std::vector<verify::OutputClass> classes = fixtures::stage_a_output_classes(s);
  std::vector<Acc> acc;
  for (const verify::OutputClass& c : classes) acc.push_back(Acc{c.name, c.first, c.count});

  int degraded = 0;
  const auto t1 = std::chrono::steady_clock::now();
  for (std::size_t j = 0; j < nt; ++j) {
    const std::size_t k = static_cast<std::size_t>(take[j]);
    fixtures::stage_a_tangent(s, *t.set, centre().data(), take[j], tang.data());

    const double hk = std::max(std::fabs(centre()[k]), 1.0) * h_rel;
    zp[k] = centre()[k] + hk;
    zm[k] = centre()[k] - hk;
    naive_fn(zp.data(), op.data());
    naive_fn(zm.data(), om.data());
    zp[k] = centre()[k];
    zm[k] = centre()[k];

    for (Acc& A : acc) {
      for (int o = A.first; o < A.first + A.count; ++o) {
        const std::size_t os = static_cast<std::size_t>(o);
        const Wide& tv = J(o, j);
        if (!tv.has_full_precision()) ++degraded;
        const double scale = scale_of(o, j);
        if (scale <= 1e-8 * std::fabs(value[os])) {
          ++A.skipped;  // a structural zero: this output does not reference this knot
          continue;
        }
        const double e_a = std::fabs(((Wide(tang[os]) - tv) / Wide(scale)).hi);
        const double e_b = std::fabs(((Wide((op[os] - om[os]) / (2.0 * hk)) - tv) / Wide(scale)).hi);
        A.sum_sq_analytic += e_a * e_a;
        ++A.n;
        A.max_bumped = std::max(A.max_bumped, e_b);
        if (e_a > A.max_analytic) {
          A.max_analytic = e_a;
          A.worst_o = o;
          A.worst_k = static_cast<int>(k);
          A.worst_truth = tv.hi;
          A.worst_scale = scale;
        }
      }
    }
  }
  const double naive_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count();
  EXPECT_EQ(degraded, 0);

  std::cout << "[ measured ] oracle Jacobian: " << n_out << " outputs x " << nt << " of the " << n_in
            << " knots (every 7th), Richardson central differences at 106 bits (" << 4 * nt
            << " oracle evaluations of the whole book), " << std::fixed << std::setprecision(1) << jac_seconds
            << " s; the two naive paths " << naive_seconds << " s\n";
  std::cout << "[ measured ] each entry is scaled by the magnitude of the terms that produced it (D26 one "
               "derivative up); structural zeros are excluded and counted\n";
  double worst_analytic = 0.0, worst_bumped = 0.0;
  for (const Acc& A : acc) {
    std::cout << std::scientific << std::setprecision(3) << "[ measured ] " << A.name << ": analytic (Dual) max "
              << A.max_analytic << " rms " << A.rms_analytic() << " | bumped (double FD) max " << A.max_bumped
              << " | " << A.n << " entries, " << A.skipped << " structural zeros";
    if (A.worst_o >= 0) {
      std::cout << "; worst at (output " << A.worst_o << (A.worst_o >= 3 * n && A.worst_o < 4 * n ? " = leg1, the float leg" : "")
                << ", knot " << A.worst_k << ") truth " << A.worst_truth << " scale " << A.worst_scale;
    }
    std::cout << "\n";
    worst_analytic = std::max(worst_analytic, A.max_analytic);
    worst_bumped = std::max(worst_bumped, A.max_bumped);
  }
  std::cout << std::scientific << std::setprecision(3) << "[ measured ] worst overall: analytic " << worst_analytic
            << ", bumped " << worst_bumped << " ("
            << (worst_analytic > 0.0 ? worst_bumped / worst_analytic : 0.0)
            << "x the analytic path's error, which is why the engine has an adjoint)\n";

  EXPECT_GT(worst_analytic, 0.0) << "the oracle resolved no difference at all: suspect the oracle";
  // Sanity rails only; PRINCIPLES.md §4's numbers belong in PROBLEM.md and should be set from this.
  //
  // The rail is looser than the aggregates class needs (1.2e-13) because the LEG class carries a
  // cancellation the others do not, and it is worth naming rather than averaging away. A float
  // leg's sensitivity is d/dz of N*tau*fwd*DF with fwd = (DF(s)/DF(e) - 1)/tau: the bracket is
  // ~r*tau ~ 1e-4 of the discount factors that formed it, so d(leg)/dz is a cancelling quantity in
  // its own right and dividing by the entry itself (which is what this class does, having no
  // per-coupon derivative to build a D26 scale from) reads that conditioning back. The trade-PV
  // class, which IS scaled by |d leg0| + |d leg1|, sits at 5.2e-11 and the aggregates at 1.2e-13.
  EXPECT_LT(worst_analytic, 1e-8);
  EXPECT_GT(worst_bumped, worst_analytic) << "a bumped difference should be the worse instrument";
}
