// P0/oracle: `Wide`, the oracle scalar of PRINCIPLES.md §4 (D72).
//
// This file is the instrument's own gate. An oracle that is wrong is worse than no oracle, so
// everything scalar/wide.hpp claims in prose is measured here:
//   * the error-free transformations are exact;
//   * Wide agrees with `long double` where `long double` is wide enough to judge (x86-64), and
//     SKIPS LOUDLY where it is not (Apple arm64, where `long double` IS `double`);
//   * the identities that pin exp / log / sqrt at a precision no witness type on this machine can
//     reach;
//   * the operator surface is Dual's, so "the maths instantiates at the oracle type" is checkable;
//   * a Wide result does not depend on the TU's contraction setting (D46's failure mode).
//
// NOT pinned to -ffp-contract=off, and deliberately not named *_e0_test.cpp: PRINCIPLES.md §4a
// retires that convention as the rebuild reaches each file, and a new file should not join it. The
// contraction check below does not need it (see the comment on that test).
#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <random>
#include <vector>

#include "epykos/scalar/dual.hpp"
#include "epykos/scalar/select.hpp"
#include "epykos/scalar/wide.hpp"
#include "epykos/version.hpp"
#include "epykos/verify/oracle.hpp"

using epykos::Wide;
using epykos::WideBool;

// `__int128` is a compiler extension, not standard C++20, and CMAKE_CXX_EXTENSIONS is OFF. Both
// toolchains this project builds on have it (GCC 13 and Apple clang on x86-64 and arm64 alike), so
// the integer-exactness proof below normally runs; it is guarded so that a target without it loses
// that one proof rather than the file, and the other two proofs -- normalisation and agreement with
// quick_two_sum -- are unconditional and need no wider type either.
#if defined(__SIZEOF_INT128__)
#define EPYKOS_HAVE_INT128 1
using epykos_int128 = __int128;
#endif

namespace {

// The exact value of a Wide as a long double. Meaningful only where long double is wider than
// double; guarded by every caller.
long double as_ld(const Wide& w) { return static_cast<long double>(w.hi) + static_cast<long double>(w.lo); }

long double rel_ld(long double a, long double b) {
  if (a == b) return 0.0L;
  const long double m = std::fabs(b) > 0.0L ? std::fabs(b) : 1.0L;
  return std::fabs(a - b) / m;
}

// |a| of a Wide as a double, using the low word.
double wide_abs(const Wide& a) { return std::fabs(a.hi + a.lo); }

bool same_bits(double a, double b) {
  std::uint64_t x = 0, y = 0;
  std::memcpy(&x, &a, sizeof x);
  std::memcpy(&y, &b, sizeof y);
  return x == y;
}

}  // namespace

// ------------------------------------------------------------------------------------------------
// The guard PRINCIPLES.md §4 asks for
// ------------------------------------------------------------------------------------------------

TEST(WideScalar, OracleHasStrictlyMoreMantissaBitsThanDouble) {
  // The static_assert in scalar/wide.hpp has already enforced this at compile time; this states
  // the number in the log so a reader of a CI run knows what the figures were taken with.
  static_assert(epykos::oracle_mantissa_bits_v<epykos::Oracle> > std::numeric_limits<double>::digits);
  EXPECT_EQ(epykos::oracle_mantissa_bits_v<epykos::Oracle>, 106);
  EXPECT_EQ(epykos::oracle_headroom_bits, 53);
  EXPECT_EQ(epykos::oracle_mantissa_bits_v<double>, 53);
  std::cout << "[ oracle   ] Wide = " << epykos::oracle_mantissa_bits_v<epykos::Oracle>
            << " significand bits, " << epykos::oracle_headroom_bits << " more than double ("
            << std::scientific << std::setprecision(2) << std::ldexp(1.0, epykos::oracle_headroom_bits)
            << "x finer than a double ulp)\n";
}

// The arm64 degradation, handled where it belongs. `long double` is the oracle's WITNESS, not the
// oracle, so a host where it is useless costs a cross-check and nothing else. The skip is loud.
TEST(WideScalar, LongDoubleWitnessIsAnnouncedOrSkippedLoudly) {
  std::cout << "[ witness  ] long double = " << epykos::witness_mantissa_bits << " significand bits, sizeof "
            << sizeof(long double) << "\n";
  if (!epykos::witness_is_wider_than_double) {
    std::cout << "[ witness  ] SKIPPED: `long double` is `double` on this target (Apple arm64), so it "
                 "cannot witness anything about a 106-bit oracle. Wide itself is UNAFFECTED — it is a "
                 "double-double and carries 106 bits here exactly as it does on x86-64. What is lost is "
                 "one redundant cross-check, not the instrument.\n";
    GTEST_SKIP() << "long double is not wider than double on this target";
  }
  EXPECT_GT(epykos::witness_mantissa_bits, std::numeric_limits<double>::digits);
}

// ------------------------------------------------------------------------------------------------
// The error-free transformations
// ------------------------------------------------------------------------------------------------

TEST(WideScalar, TwoSumAndTwoProdAreExact) {
  // Exactness is proven three ways, none of which needs a wider float type, so the proof holds on
  // Apple arm64 too, where there is no witness:
  //   1. against 128-bit INTEGER arithmetic, for operands that are exact integers scaled by powers
  //      of two - the sum and the product are then computable with no rounding at all;
  //   2. the pair (s, e) is normalised: re-summing it returns (s, e) unchanged;
  //   3. two_sum agrees bitwise with Dekker's quick_two_sum wherever quick_two_sum's precondition
  //      holds (|a| >= |b|), which is two independent algorithms reaching the same answer.
  std::mt19937_64 rng(20260925);
  std::uniform_int_distribution<std::int64_t> mant(-(std::int64_t{1} << 52), std::int64_t{1} << 52);
  std::uniform_int_distribution<int> shift(0, 40);

  int int_checked = 0, quick_checked = 0;
  for (int i = 0; i < 50000; ++i) {
    const std::int64_t ma = mant(rng);
    const std::int64_t mb = mant(rng);
    const int sa = shift(rng);

    // 1a. two_sum against a 128-bit integer: s + e must be the exact integer a + b.
    const double a = std::ldexp(static_cast<double>(ma), sa);
    const double b = static_cast<double>(mb);
    double s = 0.0, e = 0.0;
    epykos::wide_detail::two_sum(a, b, s, e);
#if defined(EPYKOS_HAVE_INT128)
    const epykos_int128 exact_sum = (static_cast<epykos_int128>(ma) << sa) + static_cast<epykos_int128>(mb);
    EXPECT_TRUE(static_cast<epykos_int128>(s) + static_cast<epykos_int128>(e) == exact_sum)
        << "two_sum inexact at i=" << i;
#endif

    // 1b. two_prod: p must be fl(a*b), and p + pe the exact integer a*b.
    const double a2 = static_cast<double>(ma);
    double p = 0.0, pe = 0.0;
    epykos::wide_detail::two_prod(a2, b, p, pe);
#if defined(EPYKOS_HAVE_INT128)
    const epykos_int128 exact_prod = static_cast<epykos_int128>(ma) * static_cast<epykos_int128>(mb);
    EXPECT_TRUE(static_cast<epykos_int128>(p) + static_cast<epykos_int128>(pe) == exact_prod)
        << "two_prod inexact at i=" << i;
#endif
    EXPECT_TRUE(same_bits(p, a2 * b)) << "two_prod's head is not fl(a*b) at i=" << i;
    ++int_checked;

    // 2 + 3, over the full dynamic range rather than the integer family.
    const double x = std::ldexp(static_cast<double>(ma), shift(rng) - 20);
    const double y = std::ldexp(static_cast<double>(mb), shift(rng) - 20);
    double xs = 0.0, xe = 0.0;
    epykos::wide_detail::two_sum(x, y, xs, xe);
    double s2 = 0.0, e2 = 0.0;
    epykos::wide_detail::two_sum(xs, xe, s2, e2);
    EXPECT_TRUE(same_bits(s2, xs)) << "two_sum's pair is not normalised at i=" << i;
    EXPECT_TRUE(same_bits(e2, xe)) << "two_sum's pair is not normalised at i=" << i;
    if (std::fabs(x) >= std::fabs(y)) {
      double qs = 0.0, qe = 0.0;
      epykos::wide_detail::quick_two_sum(x, y, qs, qe);
      EXPECT_TRUE(same_bits(qs, xs)) << "two_sum and quick_two_sum disagree at i=" << i;
      EXPECT_TRUE(same_bits(qe, xe)) << "two_sum and quick_two_sum disagree at i=" << i;
      ++quick_checked;
    }
  }
#if defined(EPYKOS_HAVE_INT128)
  const char* int_note = "exact against 128-bit integer arithmetic on ";
#else
  const char* int_note = "NOT checked against integer arithmetic (no __int128 on this target) over ";
#endif
  std::cout << "[ eft      ] two_sum / two_prod " << int_note << int_checked
            << " operand pairs, normalised on all of them; two_sum == quick_two_sum on the "
            << quick_checked << " where Dekker's precondition holds\n";
}

// ------------------------------------------------------------------------------------------------
// Against the witness, where there is one
// ------------------------------------------------------------------------------------------------

TEST(WideScalar, ArithmeticAgreesWithLongDouble) {
  if (!epykos::witness_is_wider_than_double) {
    std::cout << "[ witness  ] SKIPPED (see LongDoubleWitnessIsAnnouncedOrSkippedLoudly)\n";
    GTEST_SKIP() << "no witness on this target";
  }
  std::mt19937_64 rng(4242);
  std::uniform_real_distribution<double> u(-1.0, 1.0);
  long double w_add = 0, w_sub = 0, w_mul = 0, w_div = 0, w_sqrt = 0;
  const int n = 200000;
  for (int i = 0; i < n; ++i) {
    const double a = u(rng) * std::pow(10.0, u(rng) * 6);
    const double b = u(rng) * std::pow(10.0, u(rng) * 6);
    const long double la = a, lb = b;
    w_add = std::fmax(w_add, rel_ld(as_ld(Wide(a) + Wide(b)), la + lb));
    w_sub = std::fmax(w_sub, rel_ld(as_ld(Wide(a) - Wide(b)), la - lb));
    w_mul = std::fmax(w_mul, rel_ld(as_ld(Wide(a) * Wide(b)), la * lb));
    if (b != 0.0) w_div = std::fmax(w_div, rel_ld(as_ld(Wide(a) / Wide(b)), la / lb));
    if (a > 0.0) w_sqrt = std::fmax(w_sqrt, rel_ld(as_ld(sqrt(Wide(a))), std::sqrt(la)));
  }
  // A single operation on two doubles is exact in a double-double, so the witness sees nothing.
  EXPECT_EQ(w_add, 0.0L);
  EXPECT_EQ(w_sub, 0.0L);
  EXPECT_EQ(w_mul, 0.0L);
  EXPECT_LE(w_div, 1e-19L);
  EXPECT_LE(w_sqrt, 1e-19L);
  std::cout << "[ witness  ] vs long double over " << n
            << " pairs: add/sub/mul exact, div " << std::scientific << std::setprecision(3)
            << static_cast<double>(w_div) << ", sqrt " << static_cast<double>(w_sqrt) << "\n";
}

TEST(WideScalar, TranscendentalsBeatTheWitness) {
  if (!epykos::witness_is_wider_than_double) {
    std::cout << "[ witness  ] SKIPPED (see LongDoubleWitnessIsAnnouncedOrSkippedLoudly)\n";
    GTEST_SKIP() << "no witness on this target";
  }
  std::mt19937_64 rng(77);
  std::uniform_real_distribution<double> u(-1.0, 1.0);
  long double w_exp = 0, w_log = 0;
  for (int i = 0; i < 50000; ++i) {
    const double x = u(rng) * 40.0;
    w_exp = std::fmax(w_exp, rel_ld(as_ld(exp(Wide(x))), std::exp(static_cast<long double>(x))));
    const double y = std::fabs(u(rng)) * 1e5 + 1e-5;
    w_log = std::fmax(w_log, rel_ld(as_ld(log(Wide(y))), std::log(static_cast<long double>(y))));
  }
  // The disagreement is about one ulp OF LONG DOUBLE (1.08e-19), and the identities below show
  // Wide is the more accurate side: libm's 64-bit expl/logl is what is being seen here.
  const long double witness_ulp = std::numeric_limits<long double>::epsilon();
  EXPECT_LE(w_exp, 4.0L * witness_ulp);
  EXPECT_LE(w_log, 4.0L * witness_ulp);
  std::cout << "[ witness  ] vs long double: exp " << std::scientific << std::setprecision(3)
            << static_cast<double>(w_exp) << ", log " << static_cast<double>(w_log)
            << " (long double's own eps is " << static_cast<double>(witness_ulp)
            << ", so the witness is the limiting side)\n";
}

// ------------------------------------------------------------------------------------------------
// The identities, which reach past any witness this machine has
// ------------------------------------------------------------------------------------------------

TEST(WideScalar, TranscendentalIdentitiesHoldAt1e28OrBetterInTheEngineRange) {
  // |x| <= 2 is the whole range this engine evaluates: DF = exp(−z·t) with z·t in roughly [0, 1.5].
  std::mt19937_64 rng(31415);
  std::uniform_real_distribution<double> u(0.0, 1.0);
  double w_exp_id = 0.0, w_log_rt = 0.0, w_sqrt_rt = 0.0;
  for (int i = 0; i < 20000; ++i) {
    const double x = -2.0 + 4.0 * u(rng);
    // exp(x)·exp(−x) = 1
    w_exp_id = std::fmax(w_exp_id, wide_abs(exp(Wide(x)) * exp(Wide(-x)) - 1.0));
    const double y = 1e-3 + 2.0 * u(rng);
    // log(exp(y)) = y
    w_log_rt = std::fmax(w_log_rt, wide_abs(log(exp(Wide(y))) - y) / y);
    // sqrt(y)² = y
    const Wide s = sqrt(Wide(y));
    w_sqrt_rt = std::fmax(w_sqrt_rt, wide_abs(s * s - y) / y);
  }
  EXPECT_LT(w_exp_id, 1e-28);
  EXPECT_LT(w_log_rt, 1e-26);
  EXPECT_LT(w_sqrt_rt, 1e-30);
  std::cout << "[ identity ] |x| <= 2: |exp(x)exp(-x)-1| " << std::scientific << std::setprecision(3) << w_exp_id
            << ", |log(exp(x))-x|/x " << w_log_rt << ", |sqrt(x)^2-x|/x " << w_sqrt_rt << "\n";

  // ln2 is a hardcoded constant pair; exp(ln2) == 2 pins both of its words.
  const Wide two = exp(Wide(epykos::wide_detail::ln2_hi, epykos::wide_detail::ln2_lo));
  EXPECT_EQ(two.hi, 2.0);
  EXPECT_LT(std::fabs(two.lo), 1e-30);
  std::cout << "[ identity ] exp(ln2) = " << std::setprecision(17) << two.hi << " + " << std::scientific
            << std::setprecision(3) << two.lo << "\n";
}

// The one place a double-double degrades, measured rather than argued (scalar/wide.hpp).
TEST(WideScalar, PrecisionGuardCatchesTheSubnormalTail) {
  EXPECT_TRUE(Wide(1.0).has_full_precision());
  EXPECT_TRUE(Wide(0.0).has_full_precision());
  EXPECT_TRUE(exp(Wide(-1.0)).has_full_precision());
  EXPECT_FALSE(Wide(std::numeric_limits<double>::infinity()).has_full_precision());
  EXPECT_FALSE(Wide(std::numeric_limits<double>::quiet_NaN()).has_full_precision());

  // exp(−700) is inside the last 18 decades before the exponent floor: its low word is subnormal,
  // so the pair no longer carries 106 bits and the guard must say so.
  const Wide tiny = exp(Wide(-700.0));
  EXPECT_GT(tiny.hi, 0.0);
  EXPECT_FALSE(tiny.has_full_precision());
  std::cout << "[ guard    ] exp(-700) = " << std::scientific << std::setprecision(6) << tiny.hi << " + " << tiny.lo
            << ", full precision: " << (tiny.has_full_precision() ? "yes" : "no")
            << " (this is why verify/oracle.hpp counts degraded values instead of assuming there are none)\n";

  // Nothing this engine evaluates is anywhere near it: a discount factor is within a few decades
  // of 1, and a PV within a few decades of its notional.
  EXPECT_TRUE(exp(Wide(-1.5)).has_full_precision());
  EXPECT_TRUE(Wide(1e-30).has_full_precision());
  EXPECT_TRUE(Wide(1e9).has_full_precision());
}

// ------------------------------------------------------------------------------------------------
// The operator surface: whatever instantiates on Dual instantiates on Wide
// ------------------------------------------------------------------------------------------------

// Inside `namespace epykos` on purpose: that is the lookup environment the real maths has (it
// lives in epykos::curve, epykos::ois, epykos::instrument), so `select` / `max` / `min` / `recip`
// resolve here exactly as they do there — by ADL for Wide and Dual, by enclosing-namespace lookup
// for double (scalar/select.hpp). Writing it anywhere else would test a different thing.
namespace epykos {
namespace {

// A template exercising every part of the Scalar vocabulary the maths uses (CLAUDE.md's recording
// discipline plus the transcendentals). It must compile at double, Dual<1> and Wide alike; that is
// the checkable form of "the oracle is an instantiation, not a reimplementation".
template <class S>
S vocabulary(const S& a, const S& b) {
  S acc = S(1.0);
  acc = acc + a;
  acc = acc - b;
  acc = acc * a;
  acc = acc / (b + 2.0);
  acc = 1.0 + acc;
  acc = 3.0 - acc;
  acc = 2.0 * acc;
  acc = 8.0 / (acc + 4.0);
  acc += a;
  acc -= 0.5;
  acc *= 1.25;
  acc /= 2.0;
  acc = -acc;
  acc = +acc;
  acc = exp(acc * 0.1);
  acc = sqrt(acc);
  acc = log(acc + 1.0);
  acc = recip(acc + 2.0);
  acc = fma(acc, a, b);
  acc = select(a < b, acc, -acc);
  acc = select(a < 0.0, acc, acc * 2.0);
  acc = select(a < b, 1.0, 2.0) * acc;
  acc = max(acc, b);
  acc = min(acc, S(1e9));
  acc = abs(acc);
  return acc;
}

}  // namespace
}  // namespace epykos

TEST(WideScalar, OperatorSurfaceMatchesDualAndDouble) {
  const double a = 0.37, b = 1.21;
  const double d = epykos::vocabulary<double>(a, b);
  const epykos::Dual<1> du = epykos::vocabulary<epykos::Dual<1>>(epykos::Dual<1>(a), epykos::Dual<1>(b));
  const Wide w = epykos::vocabulary<Wide>(Wide(a), Wide(b));

  // Dual's value channel is `double` operation for operation, so it is bitwise the double path.
  EXPECT_TRUE(same_bits(du.v, d));
  // Wide computes the same expression without the rounding, so it agrees to double's own accuracy
  // and no further — the gap IS the double path's error on this little expression.
  const double rel = wide_abs(Wide(d) - w) / std::fabs(w.hi);
  EXPECT_LT(rel, 1e-14);
  EXPECT_GT(epykos::oracle_mantissa_bits_v<Wide>, 53);
  std::cout << "[ surface  ] vocabulary(): double " << std::setprecision(17) << d << ", truth " << w.hi << " + "
            << std::scientific << std::setprecision(3) << w.lo << ", double's relative error " << rel << "\n";

  static_assert(!std::is_convertible_v<Wide, double>, "no implicit Wide -> double");
  static_assert(!std::is_convertible_v<WideBool, bool>, "a value branch must not compile on Wide");
  static_assert(std::is_trivially_copyable_v<Wide>);
}

TEST(WideScalar, SelectAndComparisonSemanticsMatchScalarSelect) {
  const Wide a(-2.0), b(3.0);
  EXPECT_EQ(max(a, b).hi, 3.0);
  EXPECT_EQ(min(a, b).hi, -2.0);
  EXPECT_EQ(abs(a).hi, 2.0);
  // scalar/select.hpp's documented oddity: abs(-0.0) is -0.0, not +0.0.
  EXPECT_TRUE(same_bits(abs(Wide(-0.0)).hi, -0.0));
  EXPECT_TRUE((a < b).unchecked_value());
  EXPECT_FALSE((b < a).unchecked_value());
  EXPECT_TRUE((a <= a).unchecked_value());
  EXPECT_TRUE((a == a).unchecked_value());
  // The low word participates in the ordering: two Wides with the same head are not equal.
  const Wide c(1.0, 1e-20), e(1.0, 0.0);
  EXPECT_FALSE((c == e).unchecked_value());
  EXPECT_TRUE((e < c).unchecked_value());
}

// ------------------------------------------------------------------------------------------------
// D46's failure mode: a header-only template rounded differently by the TU that instantiated it
// ------------------------------------------------------------------------------------------------

// D46's finding, in its own words, is that "contraction is a per-TU, context-sensitive compiler
// decision", so GCC is free to round one instantiation of a header-only template differently from
// another EVEN WHEN BOTH TUs CARRY THE SAME FLAGS. That is the failure this test reproduces: the
// same `Wide` expression is computed here and in src/verify/wide_probe.cpp, an ordinary library TU,
// and the two must agree to the last bit of the low word. No pinning is involved on either side,
// which is the point — the property being checked is that `Wide` gives one answer whatever the
// compiler decides to do with it, and that property is what lets ONE oracle figure be the figure.
//
// Under PRINCIPLES.md §4 this is a test of convenience rather than a contract: the two paths do
// agree exactly, asserting it costs nothing, and it is a good bug detector. If a future change to
// Wide's kernels makes them legitimately flag-sensitive, this assertion is relaxed and the
// divergence measured instead.
//
// The stronger, real-workload version of the same claim is in D72 §6 and needs no test at all: the
// Stage A oracle produces identical figures under `release` and `reference`, over 8,043 outputs.
TEST(WideScalar, WideResultDoesNotDependOnTheTUThatComputedIt) {
  // The same expression as src/verify/wide_probe.cpp.
  auto here = [](double seed, Wide* out) {
    const Wide x(seed);
    const Wide y = x * 1.0000000001 + 0.5;
    Wide a(1.0);
    for (int i = 0; i < 40; ++i) a = a * y + x * 0.25;
    out[0] = a;
    Wide b(1.0);
    for (int i = 0; i < 20; ++i) b = (b * x + 1.0) / (y + b * 0.125);
    out[1] = b;
    Wide c(0.0);
    for (int i = 0; i < 10; ++i) {
      const Wide t = x * (0.1 * static_cast<double>(i)) - 0.3;
      c = c + exp(t) * 0.5 + log(exp(t) + 1.0) * 0.25;
    }
    out[2] = c;
  };

  int compared = 0;
  for (double seed : {0.25, 0.5, 0.75, 1.0, 1.5}) {
    Wide mine[3], theirs[3];
    here(seed, mine);
    epykos::verify::wide_contraction_probe(seed, theirs);
    for (int k = 0; k < 3; ++k) {
      EXPECT_TRUE(same_bits(mine[k].hi, theirs[k].hi))
          << "hi word differs at seed " << seed << ", chain " << k << ": this TU " << std::setprecision(17)
          << mine[k].hi << " vs the library TU " << theirs[k].hi;
      EXPECT_TRUE(same_bits(mine[k].lo, theirs[k].lo))
          << "LOW word differs at seed " << seed << ", chain " << k << ": this TU " << std::setprecision(17)
          << mine[k].lo << " vs the library TU " << theirs[k].lo;
      ++compared;
    }
  }
  std::cout << "[ contract ] " << compared
            << " (seed, chain) results bitwise identical between this TU and "
               "src/verify/wide_probe.cpp, two separate instantiations under the same preset flags ("
            << epykos::build_flags() << ")\n";
}
