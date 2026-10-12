// THE FIRST INDEPENDENT ORACLE IN THIS REPOSITORY. Every other gate here compares the engine to
// the engine: to its own templated `double`, to its replay, to a differential test of its own
// passes. That is what the external review which commissioned D106 objected to — "everything to
// date is self-verified against your own templated `double` path" — and it is what D2 deferred.
// This file compares `epykos::curve`'s interpolation schemes to QuantLib 1.35, number for number.
//
// WHY AGREEMENT WITH SWAPENGINE IS NOT THIS. D109 reports our curves agreeing with the other
// engine to 6.717e-15 and book NPV to 7.829e-15. A shared convention error produces perfect
// agreement and two wrong engines, and nothing in a head-to-head can tell the difference. A third
// implementation can.
//
// ---- provenance, and what was and was not taken (D107, D108) ---------------------------------
//
// The CASES are lifted from SwapEngine's T1 oracle tests, which D108 permits: "the inputs, the
// conventions, the schedules, the market data shapes, and above all the expected numbers".
//
//   tests/scheme_value_oracle_test.cpp   case A's knots, values and 0.045 grid step, and the
//                                        correspondence below between a scheme name and a
//                                        QuantLib class — which is a CONVENTION, and the single
//                                        most valuable thing in the lift.
//   tests/monotone_cubic_oracle_test.cpp case B's two datasets (one monotone, one wiggly where
//                                        the Hyman filter actively clamps) and its knots.
//
// What was NOT taken is any routine that computes an expected output (D108 §2): no residual, no
// curve build, no interpolant. The expectations below come from QuantLib, which is neither
// engine, so they are literals here and need no dependency — D12 is untouched, exactly as D108 §1
// anticipated. `ql_oracle_data.hpp` is the table; it is generated once and checked in, and the
// "fixtures are generated, no data files" rule does not reach it: an oracle table is not a
// fixture, it is the other implementation's answer, which is the whole point of having one.
//
// TO REGENERATE: a ~90-line program that includes <ql/math/interpolations/...>, builds the
// classes named below on the knots and values below, and prints `%a` hex floats over the grids.
// It links QuantLib 1.35 and lives nowhere in this tree; the correspondence is the specification:
//
//   epykos::curve::Flat          QuantLib ForwardFlatInterpolation
//   epykos::curve::Linear        QuantLib LinearInterpolation
//   epykos::curve::NaturalCubic  QuantLib CubicNaturalSpline
//   epykos::curve::Hermite       QuantLib CubicInterpolation(Parabolic, monotonic = false, ...)
//   epykos::curve::MonotoneCubic QuantLib CubicInterpolation(..., monotonic = true, ...)
//
// The last two have a boundary condition and a tangent source that the name does not fix, and
// both turn out to matter. The tests below measure rather than assume, and §MonotoneCubic records
// a real divergence instead of tolerating it.
//
// Hex-float literals are exact, so the only error in a comparison is ours. An E0 TU
// (-ffp-contract=off in every preset), which also puts this oracle in the mutation gate set.
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <string>
#include <vector>

#include "epykos/maths/curve/scheme.hpp"
#include "curve/ql_oracle_data.hpp"

#ifndef EPYKOS_FP_CONTRACT_OFF
#error "an _e0_test.cpp TU must see EPYKOS_FP_CONTRACT_OFF"
#endif

namespace curve = epykos::curve;

namespace {

// SwapEngine tests/scheme_value_oracle_test.cpp: "wiggly: tangents matter".
const std::vector<double>& knots_a() {
  static const std::vector<double> t{1.0, 1.5, 2.25, 3.0, 5.0, 7.5, 10.0};
  return t;
}
const std::vector<double>& values_a() {
  static const std::vector<double> y{0.030, 0.034, 0.031, 0.037, 0.040, 0.036, 0.041};
  return y;
}

// SwapEngine tests/monotone_cubic_oracle_test.cpp, its node set [kT0, kBack...] flattened.
const std::vector<double>& knots_b() {
  static const std::vector<double> t{0.5, 1.0, 2.0, 3.0, 5.0, 7.0, 10.0};
  return t;
}
const std::vector<double>& values_b_monotone() {
  static const std::vector<double> y{0.030, 0.033, 0.036, 0.038, 0.041, 0.043, 0.045};
  return y;
}
const std::vector<double>& values_b_wiggly() {
  static const std::vector<double> y{0.030, 0.045, 0.030, 0.048, 0.032, 0.050, 0.031};
  return y;
}

// Their tolerance for this comparison, lifted with the case: `swaps::tol::literal`.
constexpr double kTol = 1e-12;

struct Worst {
  double gap = 0.0;
  double at = 0.0;
  std::size_t index = 0;
};

// The worst absolute gap between a scheme's value and a QuantLib column, over a whole grid.
template <class Scheme>
Worst worst_value_gap(const Scheme& s, const std::vector<double>& v, const double* grid, const double* ql,
                      std::size_t n) {
  const auto coef = s.prepare(v.data());
  Worst w;
  for (std::size_t i = 0; i < n; ++i) {
    const double gap = std::abs(s.value(v.data(), coef, grid[i]) - ql[i]);
    if (gap > w.gap) w = {gap, grid[i], i};
  }
  return w;
}

// Same, for the definite integral from the first knot — which is what QuantLib's primitive() is.
template <class Scheme>
Worst worst_primitive_gap(const Scheme& s, const std::vector<double>& v, const double* grid, const double* ql,
                          std::size_t n) {
  Worst w;
  for (std::size_t i = 0; i < n; ++i) {
    const double gap = std::abs(s.integral(v.data(), grid[0], grid[i]) - ql[i]);
    if (gap > w.gap) w = {gap, grid[i], i};
  }
  return w;
}

void report(const char* what, const Worst& w) {
  std::cout << "  [ql-oracle] " << what << ": worst |gap| = " << w.gap << " at t = " << w.at << '\n';
}

constexpr std::size_t kNa = std::size(kGridA);
constexpr std::size_t kNb = std::size(kGridB);

}  // namespace

// ---- the two schemes the engine can both evaluate and integrate ------------------------------

// `Flat` and `Linear` are the only schemes with an `integral`, and they are exactly the two that
// `curve::Variable::forward` permits (the static_assert in composite.hpp): a forward curve's
// discount factor IS the integral, so this pair of comparisons is the one that stands behind a DF.
TEST(SchemeOracleE0, FlatMatchesQuantLibForwardFlatInValueAndIntegral) {
  const curve::Flat s(knots_a().data(), static_cast<int>(knots_a().size()));
  const Worst v = worst_value_gap(s, values_a(), kGridA, kQlAFlatValue, kNa);
  const Worst p = worst_primitive_gap(s, values_a(), kGridA, kQlAFlatPrimitive, kNa);
  report("Flat value vs ForwardFlatInterpolation", v);
  report("Flat integral vs ForwardFlatInterpolation::primitive", p);
  EXPECT_LT(v.gap, kTol) << "worst at t = " << v.at;
  EXPECT_LT(p.gap, kTol) << "worst at t = " << p.at;
}

TEST(SchemeOracleE0, LinearMatchesQuantLibLinearInValueAndIntegral) {
  const curve::Linear s(knots_a().data(), static_cast<int>(knots_a().size()));
  const Worst v = worst_value_gap(s, values_a(), kGridA, kQlALinearValue, kNa);
  const Worst p = worst_primitive_gap(s, values_a(), kGridA, kQlALinearPrimitive, kNa);
  report("Linear value vs LinearInterpolation", v);
  report("Linear integral vs LinearInterpolation::primitive", p);
  EXPECT_LT(v.gap, kTol) << "worst at t = " << v.at;
  EXPECT_LT(p.gap, kTol) << "worst at t = " << p.at;
}

// ---- the C2 spline ---------------------------------------------------------------------------

// The strongest single assertion in this file: `NaturalCubic` solves a tridiagonal system by the
// Thomas algorithm over `Scalar` with structural pivots, and QuantLib solves the same system its
// own way. Nothing but the mathematics makes them agree.
TEST(SchemeOracleE0, NaturalCubicMatchesQuantLibCubicNaturalSpline) {
  const curve::NaturalCubic s(knots_a().data(), static_cast<int>(knots_a().size()));
  const Worst v = worst_value_gap(s, values_a(), kGridA, kQlANaturalCubicValue, kNa);
  report("NaturalCubic value vs CubicNaturalSpline", v);
  EXPECT_LT(v.gap, kTol) << "worst at t = " << v.at;
}

// ---- Hermite --------------------------------------------------------------------------------

// `Hermite` uses Bessel (parabolic) tangents at the interior knots and the parabolic END
// EXTRAPOLATION formula at the two ends, m_0 = ((2h_0+h_1)d_0 - h_0 d_1)/(h_0+h_1).
//
// A fact about QuantLib's API, established here and worth knowing before reading its arguments:
// the boundary condition is applied only in the `da == Spline` branch of
// `CubicInterpolation`'s coefficient update, so for `Parabolic` it is INERT. The two columns
// generated with `SecondDerivative, 0.0` and with `Lagrange` ends are bit-identical over all 201
// points, checked. SwapEngine's own oracle passes `SecondDerivative, 0.0` here and those
// arguments do nothing; the comparison is still valid, and the correspondence it documents holds
// for the derivative rule alone. So there is one column to match, not two.
TEST(SchemeOracleE0, HermiteMatchesQuantLibParabolicCubic) {
  const curve::Hermite s(knots_a().data(), static_cast<int>(knots_a().size()));
  const Worst v = worst_value_gap(s, values_a(), kGridA, kQlAHermiteSecondDeriv0Value, kNa);
  report("Hermite vs CubicInterpolation(Parabolic)", v);
  EXPECT_LT(v.gap, kTol) << "worst at t = " << v.at
                         << ". Our interior tangents are Bessel and our end tangents are the parabolic "
                            "extrapolation; a gap confined to the first and last segment is the latter.";
  // The end conditions are inert for a non-Spline derivative rule: pinned, because the next
  // reader of this file will otherwise assume the arguments mean something.
  for (std::size_t i = 0; i < kNa; ++i) {
    ASSERT_EQ(kQlAHermiteSecondDeriv0Value[i], kQlAHermiteLagrangeValue[i])
        << "QuantLib's boundary condition is no longer inert for da = Parabolic at index " << i
        << " -- then the two end conditions are distinguishable and this test must choose between them";
  }
}

// ---- MonotoneCubic: the divergence D110 could not adjudicate ----------------------------------

// D110 found that our `monotone_cubic` and the other engine's differ in TANGENT SOURCE and could
// not say which, if either, is QuantLib's. Agreement between two engines cannot settle that. This
// is the question a third implementation answers, and the answer has two halves.
//
// THE CONSTRUCTIONS.
//   theirs  QuantLib `MonotonicCubicNaturalSpline` == `CubicInterpolation(da = Spline,
//           monotonic = true)`: the C2 natural-spline derivatives, then the Hyman filter. Their
//           own oracle says it is "transcribed to match QuantLib EXACTLY".
//   ours    Bessel (parabolic) tangents, then a limiter that zeroes the tangent at any secant
//           SIGN CHANGE and clips it to 3*min(|d_left|, |d_right|) — every clamp a `select`
//           (scheme.hpp, `MonotoneCubic::limited`).
//
// WHAT THE ORACLE SHOWS, in the two places it can look.
//
//   (1) The MONOTONE dataset is the control: neither filter clamps there — our values equal
//       QuantLib's UNFILTERED parabolic cubic to 1.4e-17 — so that dataset isolates the tangent
//       source, and it says ours is parabolic and theirs is not: our gap to
//       MonotonicCubicNaturalSpline is 7.2e-5, which is also exactly our gap to the plain
//       CubicNaturalSpline. The tangent source is settled, and D110 was right.
//
//   (2) The WIGGLY dataset is where a filter must act, and here QuantLib's does NOT.
//       `CubicInterpolation(Parabolic, monotonic = true)` is bit-identical to the same with
//       `monotonic = false` on both datasets. That is structural, not a coincidence: QuantLib's
//       Hyman filter tests `tmp[i]*pm > 0` and clips to `min(|tmp[i]|, M)` with
//       `M >= 3*min(|S[i-1]|, |S[i]|, |pm|)`, where `pm` is the Bessel tangent — so when the
//       derivative rule IS Parabolic, `tmp[i] == pm`, the sign test can never fail and the min is
//       vacuous. **QuantLib's monotonicity filter is a no-op on parabolic tangents.**
//
//   (3) QuantLib has SIX local derivative rules and a monotonicity flag, and the engine matches
//       none of the combinations. `Parabolic`, `Kruger`, `FritschButland`, `Harmonic` and `Akima`
//       were each evaluated with the flag on and off. One looks like a hit and is not:
//       `Harmonic` agrees with us to 1.4e-17 on the WIGGLY dataset and misses by 1.2e-4 on the
//       monotone one and 5.0e-4 on case A's knots. The coincidence has a reason — wiggly-B
//       alternates secant sign at EVERY interior knot, so both schemes zero every interior
//       tangent and both reduce to the unique cubic with zero end tangents on each segment — and
//       it is pinned below precisely so that nobody reads one dataset and concludes we are
//       QuantLib's Harmonic scheme.
//
// So QuantLib has no counterpart to this scheme: its only monotone cubic is the spline-derivative
// one, and `Parabolic` is documented in its own enum as "local, non-monotonic". The oracle
// therefore establishes what ours is NOT and what its tangents ARE, and cannot validate the
// limiter — which is stated here rather than papered over, and is why the limiter gets the
// independent shape-preservation check below instead.
TEST(SchemeOracleE0, MonotoneCubicHasParabolicTangentsAndIsNotQuantLibsMonotonicNaturalSpline) {
  struct Case {
    const char* name;
    const std::vector<double>& v;
    const double* ql_monotonic_natural;  // MonotonicCubicNaturalSpline: theirs
    const double* ql_parabolic;          // CubicInterpolation(Parabolic): our tangent source
    const double* ql_natural;            // CubicNaturalSpline: the unfiltered C2 spline
    const double* ql_harmonic;           // CubicInterpolation(Harmonic): the near-miss, see (3)
    bool filter_clamps;                  // whether OUR limiter acts on this dataset
    bool harmonic_coincides;             // whether (3)'s degenerate coincidence holds here
  };
  const Case cases[] = {
      {"monotone", values_b_monotone(), kQlBMonotoneMonoValue, kQlBPlainParabolicMonoValue, kQlBNaturalMonoValue,
       kQlBHarmonicMonoValue, false, false},
      {"wiggly", values_b_wiggly(), kQlBMonotoneWigglyValue, kQlBPlainParabolicWigglyValue, kQlBNaturalWigglyValue,
       kQlBHarmonicWigglyValue, true, true},
  };
  const curve::MonotoneCubic s(knots_b().data(), static_cast<int>(knots_b().size()));
  for (const Case& c : cases) {
    const Worst vs_theirs = worst_value_gap(s, c.v, kGridB, c.ql_monotonic_natural, kNb);
    const Worst vs_para = worst_value_gap(s, c.v, kGridB, c.ql_parabolic, kNb);
    const Worst vs_nat = worst_value_gap(s, c.v, kGridB, c.ql_natural, kNb);
    const Worst vs_harm = worst_value_gap(s, c.v, kGridB, c.ql_harmonic, kNb);
    std::cout << "  [ql-oracle] MonotoneCubic (" << c.name << "): vs MonotonicCubicNaturalSpline " << vs_theirs.gap
              << ", vs CubicInterpolation(Parabolic) " << vs_para.gap << ", vs CubicNaturalSpline " << vs_nat.gap
              << ", vs CubicInterpolation(Harmonic) " << vs_harm.gap << '\n';

    // (3): the Harmonic coincidence is DATASET-LOCAL, and both halves are pinned. Without the
    // second half a reader who runs only the wiggly case concludes we are QuantLib's Harmonic
    // scheme, which case A refutes by 5.0e-4.
    if (c.harmonic_coincides) {
      EXPECT_LT(vs_harm.gap, kTol) << c.name << ": the degenerate coincidence with QuantLib's Harmonic scheme is "
                                      "gone. Every interior secant pair on this dataset changes sign, which forces "
                                      "both schemes to zero every interior tangent, so losing it means one of the "
                                      "two limiters no longer zeroes at a sign change.";
    } else {
      EXPECT_GT(vs_harm.gap, kTol) << c.name << ": MonotoneCubic now matches QuantLib's Harmonic scheme on data "
                                      "where the two constructions differ. That would be an identification, not a "
                                      "coincidence -- record it in a decision entry.";
    }

    // QuantLib's own monotonic flag is inert for a parabolic rule, so this column is both the
    // filtered and the unfiltered parabolic cubic.
    for (std::size_t i = 0; i < kNb; ++i) {
      ASSERT_EQ(kQlBPlainParabolicMonoValue[i], kQlBHymanParabolicMonoValue[i]) << "index " << i;
      ASSERT_EQ(kQlBPlainParabolicWigglyValue[i], kQlBHymanParabolicWigglyValue[i]) << "index " << i;
    }

    if (!c.filter_clamps) {
      // The control: our limiter does nothing here, so the tangent source stands exposed and must
      // be QuantLib's parabolic one exactly.
      EXPECT_LT(vs_para.gap, kTol)
          << c.name << ": our tangents are not QuantLib's parabolic ones (worst " << vs_para.gap << " at t = "
          << vs_para.at << "), and no limiter clamps on monotone data, so scheme.hpp's claim that the tangents are "
             "Bessel estimates would be wrong.";
    } else {
      // Where our limiter acts, it moves the curve off the unfiltered parabolic cubic. That is
      // the limiter being real, and it is the only thing the oracle can say about it.
      EXPECT_GT(vs_para.gap, kTol) << c.name << ": our limiter did not clamp on data chosen because it must. Then "
                                      "this dataset no longer discriminates and a new one is needed.";
    }

    // The finding, asserted as a FLOOR on both datasets so that loosening a tolerance cannot make
    // it pass quietly: we are not the scheme that shares our name.
    EXPECT_GT(vs_theirs.gap, kTol)
        << c.name << ": MonotoneCubic now AGREES with QuantLib's MonotonicCubicNaturalSpline to " << vs_theirs.gap
        << ". If that is intended, the tangent source changed and D110's finding is superseded -- say so in a "
           "decision entry and delete this expectation. Do not loosen it.";
  }
  // Every scheme interpolates its knots, whatever its tangents: the one thing that must agree.
  for (const Case& c : cases) {
    const auto coef = s.prepare(c.v.data());
    for (std::size_t k = 0; k < knots_b().size(); ++k) {
      EXPECT_EQ(s.value(c.v.data(), coef, knots_b()[k]), c.v[k]) << c.name << " knot " << k;
    }
  }

  // And the same refutation on case A's knots, where no coincidence is available: we match
  // neither the scheme that shares our name nor the one that happened to agree on wiggly-B. Two
  // knot sets, because one would be a coincidence of the kind this test just had to unpick.
  const curve::MonotoneCubic sa(knots_a().data(), static_cast<int>(knots_a().size()));
  const Worst a_theirs = worst_value_gap(sa, values_a(), kGridA, kQlAMonotoneCubicValue, kNa);
  const Worst a_harm = worst_value_gap(sa, values_a(), kGridA, kQlAHarmonicValue, kNa);
  std::cout << "  [ql-oracle] MonotoneCubic (case A knots): vs MonotonicCubicNaturalSpline " << a_theirs.gap
            << ", vs CubicInterpolation(Harmonic) " << a_harm.gap << '\n';
  EXPECT_GT(a_theirs.gap, kTol) << "case A: see the wiggly message above";
  EXPECT_GT(a_harm.gap, kTol) << "case A: MonotoneCubic now matches QuantLib's Harmonic scheme on a second knot set "
                                 "too -- that IS an identification. Record it and replace this expectation.";
}

// The limiter has no oracle, so it gets the property its name promises instead, measured against
// the same QuantLib columns: on each segment the interpolant stays inside the bracket of that
// segment's two knot values.
//
// READ THE COMPARISON CAREFULLY, because it is easy to misreport. QuantLib's documented guarantee
// is NARROWER than the property measured here — its header says the filter ensures monotonicity
// "in the regions of local monotoniticity of the input (three successive increasing or decreasing
// values)". The wiggly dataset's last three values are 0.032, 0.050, 0.031, so segment [7, 10] is
// not such a region and QuantLib promises nothing there. Its 2.7e-3 excursion is therefore
// CONSISTENT with its own contract and is not a defect. What the numbers show is that the two
// schemes implement different contracts: ours zeroes the tangent at every secant sign change and
// so brackets every segment unconditionally, where Hyman–Dougherty's filter relaxes its bound by
// 1.5*min(|pm|, |pd|) and keeps the bracket only where the input is locally monotone. Ours is the
// more aggressive of the two and pays for it in smoothness, not in correctness.
TEST(SchemeOracleE0, MonotoneCubicDoesNotOvershootWhereQuantLibsParabolicCubicDoes) {
  const curve::MonotoneCubic s(knots_b().data(), static_cast<int>(knots_b().size()));
  const std::vector<double>& t = knots_b();

  // The worst excursion outside [min(v_k, v_k+1), max(v_k, v_k+1)] over the grid.
  const auto overshoot_of = [&t](const std::vector<double>& v, const double* col) {
    double worst = 0.0;
    for (std::size_t i = 0; i < kNb; ++i) {
      const double u = kGridB[i];
      std::size_t k = 0;
      while (k + 2 < t.size() && u >= t[k + 1]) ++k;
      const double lo = std::min(v[k], v[k + 1]), hi = std::max(v[k], v[k + 1]);
      worst = std::max(worst, std::max(lo - col[i], col[i] - hi));
    }
    return std::max(worst, 0.0);
  };
  // Ours, evaluated rather than tabulated.
  const auto overshoot_ours = [&](const std::vector<double>& v) {
    std::vector<double> col;
    const auto coef = s.prepare(v.data());
    for (std::size_t i = 0; i < kNb; ++i) col.push_back(s.value(v.data(), coef, kGridB[i]));
    return overshoot_of(v, col.data());
  };

  const double ours = overshoot_ours(values_b_wiggly());
  const double ql_para = overshoot_of(values_b_wiggly(), kQlBPlainParabolicWigglyValue);
  const double ql_mono = overshoot_of(values_b_wiggly(), kQlBMonotoneWigglyValue);
  const double ql_nat = overshoot_of(values_b_wiggly(), kQlBNaturalWigglyValue);
  std::cout << "  [ql-oracle] wiggly overshoot: epykos MonotoneCubic " << ours << ", QuantLib Parabolic " << ql_para
            << ", MonotonicCubicNaturalSpline " << ql_mono << ", CubicNaturalSpline " << ql_nat << '\n';

  EXPECT_EQ(ours, 0.0) << "MonotoneCubic overshoots its own segment bracket by " << ours
                       << " -- the limiter does not do what the scheme is named for";
  EXPECT_GT(ql_para, 0.0) << "QuantLib's parabolic cubic no longer overshoots this dataset, so the dataset has "
                             "stopped discriminating a shape-preserving scheme from a plain one";
  EXPECT_GT(ql_mono, 0.0) << "QuantLib's MonotonicCubicNaturalSpline no longer leaves this segment's bracket. That "
                             "is not a defect either way -- see the note above on the width of its guarantee -- but "
                             "if it holds now, the contrast this test draws has gone and the text must change.";
  // And on monotone data, where no filter acts, nobody overshoots.
  EXPECT_EQ(overshoot_ours(values_b_monotone()), 0.0);
}
