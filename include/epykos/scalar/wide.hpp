// EpykosEngine — `Wide`, the oracle scalar (PRINCIPLES.md §4; D72).
//
// PRINCIPLES.md §4 sets ground truth as "the recorded expression evaluated without rounding", and
// says the oracle is "the templated maths instantiated at wider precision". `Wide` is that wider
// precision. It is a Scalar in exactly the sense D3 means: `price_book<Wide>` is the SAME source
// as `price_book<double>`, so the oracle is an instantiation, never a second copy of the maths.
//
// Representation: an unevaluated double-double, x = hi + lo with |lo| <= ulp(hi)/2, so the pair
// carries 2·53 = 106 significand bits against double's 53. Every operation below is one of the
// standard error-free-transformation algorithms (Dekker 1971, Knuth 1969, Shewchuk 1997, and the
// dd_real kernels of Bailey/Hida/Li's QD); nothing is borrowed from any other checkout (D11).
//
// WHY A DOUBLE-DOUBLE AND NOT `long double` (D72, argued from what the gates must resolve):
//
//   * `long double` is 80-bit extended on x86-64 (64 significand bits, 11 more than double) but
//     is IDENTICAL TO `double` on Apple arm64. An oracle that silently equals the thing it judges
//     is worse than no oracle, and CI runs an arm64 job. A double-double has the same 106 bits on
//     every target this project builds on, so one measured error figure is the figure on every
//     fingerprint — the same discipline D25/D46 already impose on every other number here.
//   * Headroom. Against the naive `double` path alone, `long double`'s 2^11 = 2,048x would do: it
//     resolves a ~1e-13 error to about three significant figures. It does NOT leave room for the
//     measurement this instrument exists to make next. PRINCIPLES.md §8 step 4 makes telescoping
//     the worked example; a telescoped product is expected to be one to three decades MORE
//     accurate than the naive product loop it replaces, and ranking an instrument's own error at
//     1e-16 with 2,048x total headroom leaves single digits of margin. 106 bits leaves 2^53.
//   * Cost. D12 is untouched: this is in-repo, header-only, no new dependency. The price is speed:
//     MEASURED at 62.5x a double evaluation of the M1 book (45.7 ms against 0.731 ms, fingerprint
//     d448afd70180, release flags), which an offline instrument can pay.
//
// `long double` is kept, but as a WITNESS rather than as the oracle: tests/scalar/wide_e0_test.cpp
// cross-checks `Wide` against `long double` on hosts where `long double` is wider than `double`,
// and skips loudly where it is not. The static_assert on `Oracle` below is the compile-time guard
// the owner asked for: it is on the ORACLE alias, so re-pointing the oracle at `long double`
// breaks the build on Apple arm64 instead of degrading in silence.
//
// MEASURED ACCURACY (fingerprint d448afd70180, Apple clang 21, release flags;
// tests/scalar/wide_e0_test.cpp re-measures all of it). + − × ÷ and sqrt agree with `long double`
// EXACTLY on 200,000 random operand pairs — the 64-bit witness cannot see a difference, so the
// bounds below come from identities instead. Over |x| <= 2, which is the whole range this engine
// evaluates (`exp(−z·t)` with z·t in roughly [0, 1.5]): |exp(x)·exp(−x) − 1| <= 2.6e-30 and
// |log(exp(x)) − x| / x <= 4.2e-28. exp(ln2) is 2.0 with a zero tail. Against `long double`,
// `Wide`'s exp and log differ by about one ulp OF LONG DOUBLE (1.1e-19), i.e. libm's 64-bit
// `expl`/`logl` is the less accurate side of that comparison, which is the point.
//
// THE ONE PLACE IT DEGRADES, measured, not assumed. A double-double loses precision gradually as
// |x| approaches the exponent floor, because the low word goes subnormal while the high word is
// still normal: measured, exp(−700) has lo/hi = 8.6e-18 with lo = 8.5e-322 (a subnormal), and
// exp(−740) has lo = 0 exactly — 53 bits, not 106. `has_full_precision()` below is the guard, and
// verify/oracle.hpp refuses to report an error figure computed from a degraded value rather than
// quoting one it cannot stand behind. Discount factors and PVs live nowhere near that floor, so it
// has never fired; it is checked rather than argued.
//
// FLOATING-POINT CONTRACTION. The exactness-critical primitives are immune to it by construction:
// two_sum / quick_two_sum contain no multiply for an FMA to absorb, and two_prod asks for the
// fusion explicitly through `std::fma`, which IEEE 754 defines as a single rounding whatever the
// TU's `-ffp-contract` setting is. Every remaining `a·b + c` in the correction terms is written as
// an explicit `std::fma` for the same reason, so a `Wide` result does not depend on the flags of
// the TU that instantiated it. That is a claim, and tests/scalar/wide_e0_test.cpp measures it.
// `-ffast-math` would break these algorithms outright; D13 forbids it.
//
// Contract (the operator surface of `Dual<N>`, so anything that instantiates on Dual instantiates
// on Wide — that is what makes "the maths instantiates at the oracle type" a checkable statement):
//   * + − * / between Wides, unary minus, mixed with double on either side, compound assignments,
//     exp / log / sqrt / recip / fma;
//   * comparisons return WideBool, which does not convert to bool, so a value branch does not
//     compile here either (CLAUDE.md recording discipline);
//   * select / max / min / abs with the value semantics of scalar/select.hpp;
//   * no implicit Wide -> double: value() is an explicit read and rounds to nearest.
//
// Wide carries no taint and no tangent, so `structural_if` cannot check anything: Rec's taint is
// the authoritative discipline check (D14) and Dual's is the weaker one. Stated, not hidden.
#pragma once

#include <cmath>
#include <limits>
#include <type_traits>

namespace epykos {

// ------------------------------------------------------------------------------------------------
// Error-free transformations. Exact for finite operands with no overflow.
// ------------------------------------------------------------------------------------------------
namespace wide_detail {

// Knuth's two-sum: s = fl(a + b), e = (a + b) − s exactly, for any finite a, b. Additions and
// subtractions only, so no contraction can reach it.
inline void two_sum(double a, double b, double& s, double& e) noexcept {
  s = a + b;
  const double bb = s - a;
  e = (a - (s - bb)) + (b - bb);
}

// Dekker's fast two-sum: the same, valid when |a| >= |b| (or either is zero).
inline void quick_two_sum(double a, double b, double& s, double& e) noexcept {
  s = a + b;
  e = b - (s - a);
}

// p = fl(a·b), e = a·b − p exactly. `std::fma` is a single-rounding operation by IEEE 754-2008
// §5.4.1, so this is exact and flag-independent; a target without hardware FMA gets libm's
// correctly-rounded software one, which is slower and equally exact.
inline void two_prod(double a, double b, double& p, double& e) noexcept {
  p = a * b;
  e = std::fma(a, b, -p);
}

// ln 2 as a double-double: hi is the nearest double, lo the remainder. Checked in
// tests/scalar/wide_e0_test.cpp by exp(ln2) == 2 and by log(2) == ln2.
inline constexpr double ln2_hi = 0x1.62e42fefa39efp-1;
inline constexpr double ln2_lo = 0x1.abc9e3b39803fp-56;

}  // namespace wide_detail

class Wide;

// A comparison of Wides. Not convertible to bool: use select or structural_if.
struct WideBool {
  bool v = false;

  constexpr WideBool() noexcept = default;
  constexpr explicit WideBool(bool value) noexcept : v(value) {}

  // The bit with no check. Debugging and tests only.
  constexpr bool unchecked_value() const noexcept { return v; }
};

static_assert(!std::is_convertible_v<WideBool, bool>, "no implicit WideBool -> bool");
static_assert(!std::is_constructible_v<bool, WideBool>, "no explicit WideBool -> bool either");

class Wide {
 public:
  // The significand width of the representation: two doubles, the low word trailing the high one
  // by a full significand. This is the number the oracle guard below is stated in.
  static constexpr int mantissa_bits = 2 * std::numeric_limits<double>::digits;  // 106

  double hi = 0.0;
  double lo = 0.0;

  constexpr Wide() noexcept = default;
  constexpr Wide(double value) noexcept : hi(value), lo(0.0) {}  // NOLINT: implicit by design
  constexpr Wide(double h, double l) noexcept : hi(h), lo(l) {}

  // The oracle rounded to double (round-to-nearest of hi + lo: hi already is that, because the
  // pair is normalised). Explicit on purpose — there is no implicit Wide -> double.
  constexpr double value() const noexcept { return hi; }
  // The part double cannot see. `hi` plus this is the oracle's answer.
  constexpr double tail() const noexcept { return lo; }

  constexpr bool is_finite() const noexcept { return hi - hi == 0.0; }

  // The smallest |hi| whose low word can still be a NORMAL double: DBL_MIN · 2^53 = 2^-969.
  // Below it the pair carries fewer than 106 bits and slides towards plain double.
  static constexpr double full_precision_floor = 0x1p-969;

  // Does this value still carry the full 106 bits? A value that does not is not fit to be
  // ground truth, and verify/oracle.hpp reports rather than hides it.
  bool has_full_precision() const noexcept {
    if (!(hi - hi == 0.0)) return false;  // inf / nan
    return hi == 0.0 || std::fabs(hi) >= full_precision_floor;
  }

  // ---- addition ------------------------------------------------------------------------------

  friend Wide operator+(const Wide& a, const Wide& b) noexcept {
    double s1 = 0.0, s2 = 0.0, t1 = 0.0, t2 = 0.0;
    wide_detail::two_sum(a.hi, b.hi, s1, s2);
    wide_detail::two_sum(a.lo, b.lo, t1, t2);
    s2 += t1;
    wide_detail::quick_two_sum(s1, s2, s1, s2);
    s2 += t2;
    wide_detail::quick_two_sum(s1, s2, s1, s2);
    return Wide(s1, s2);
  }
  friend Wide operator+(const Wide& a, double b) noexcept {
    double s1 = 0.0, s2 = 0.0;
    wide_detail::two_sum(a.hi, b, s1, s2);
    s2 += a.lo;
    wide_detail::quick_two_sum(s1, s2, s1, s2);
    return Wide(s1, s2);
  }
  friend Wide operator+(double a, const Wide& b) noexcept { return b + a; }

  // ---- subtraction ---------------------------------------------------------------------------

  friend Wide operator-(const Wide& a) noexcept { return Wide(-a.hi, -a.lo); }
  friend Wide operator+(const Wide& a) noexcept { return a; }
  friend Wide operator-(const Wide& a, const Wide& b) noexcept { return a + (-b); }
  friend Wide operator-(const Wide& a, double b) noexcept { return a + (-b); }
  friend Wide operator-(double a, const Wide& b) noexcept { return (-b) + a; }

  // ---- multiplication ------------------------------------------------------------------------

  friend Wide operator*(const Wide& a, const Wide& b) noexcept {
    double p1 = 0.0, p2 = 0.0;
    wide_detail::two_prod(a.hi, b.hi, p1, p2);
    // Explicit fusions: the compiler has nothing left to contract, so the result does not depend
    // on the including TU's -ffp-contract setting.
    p2 = std::fma(a.hi, b.lo, p2);
    p2 = std::fma(a.lo, b.hi, p2);
    wide_detail::quick_two_sum(p1, p2, p1, p2);
    return Wide(p1, p2);
  }
  friend Wide operator*(const Wide& a, double b) noexcept {
    double p1 = 0.0, p2 = 0.0;
    wide_detail::two_prod(a.hi, b, p1, p2);
    p2 = std::fma(a.lo, b, p2);
    wide_detail::quick_two_sum(p1, p2, p1, p2);
    return Wide(p1, p2);
  }
  friend Wide operator*(double a, const Wide& b) noexcept { return b * a; }

  // ---- division ------------------------------------------------------------------------------

  // Three corrected quotient digits: q1 from the leading doubles, then two exact remainders.
  friend Wide operator/(const Wide& a, const Wide& b) noexcept {
    const double q1 = a.hi / b.hi;
    Wide r = a - b * q1;
    const double q2 = r.hi / b.hi;
    r = r - b * q2;
    const double q3 = r.hi / b.hi;
    double s1 = 0.0, s2 = 0.0;
    wide_detail::quick_two_sum(q1, q2, s1, s2);
    return Wide(s1, s2) + q3;
  }
  friend Wide operator/(const Wide& a, double b) noexcept { return a / Wide(b); }
  friend Wide operator/(double a, const Wide& b) noexcept { return Wide(a) / b; }

  // ---- compound assignment -------------------------------------------------------------------

  Wide& operator+=(const Wide& o) noexcept { return *this = *this + o; }
  Wide& operator-=(const Wide& o) noexcept { return *this = *this - o; }
  Wide& operator*=(const Wide& o) noexcept { return *this = *this * o; }
  Wide& operator/=(const Wide& o) noexcept { return *this = *this / o; }
  Wide& operator+=(double o) noexcept { return *this = *this + o; }
  Wide& operator-=(double o) noexcept { return *this = *this - o; }
  Wide& operator*=(double o) noexcept { return *this = *this * o; }
  Wide& operator/=(double o) noexcept { return *this = *this / o; }

  // ---- comparisons -> WideBool -----------------------------------------------------------------

  friend WideBool operator<(const Wide& a, const Wide& b) noexcept {
    return WideBool(a.hi < b.hi || (a.hi == b.hi && a.lo < b.lo));
  }
  friend WideBool operator==(const Wide& a, const Wide& b) noexcept {
    return WideBool(a.hi == b.hi && a.lo == b.lo);
  }
  friend WideBool operator>(const Wide& a, const Wide& b) noexcept { return b < a; }
  friend WideBool operator<=(const Wide& a, const Wide& b) noexcept {
    return WideBool(a.hi < b.hi || (a.hi == b.hi && a.lo <= b.lo));
  }
  friend WideBool operator>=(const Wide& a, const Wide& b) noexcept { return b <= a; }

  friend WideBool operator<(const Wide& a, double b) noexcept { return a < Wide(b); }
  friend WideBool operator<=(const Wide& a, double b) noexcept { return a <= Wide(b); }
  friend WideBool operator>(const Wide& a, double b) noexcept { return a > Wide(b); }
  friend WideBool operator>=(const Wide& a, double b) noexcept { return a >= Wide(b); }
  friend WideBool operator==(const Wide& a, double b) noexcept { return a == Wide(b); }
  friend WideBool operator<(double a, const Wide& b) noexcept { return Wide(a) < b; }
  friend WideBool operator<=(double a, const Wide& b) noexcept { return Wide(a) <= b; }
  friend WideBool operator>(double a, const Wide& b) noexcept { return Wide(a) > b; }
  friend WideBool operator>=(double a, const Wide& b) noexcept { return Wide(a) >= b; }
  friend WideBool operator==(double a, const Wide& b) noexcept { return Wide(a) == b; }

  // ---- branches ------------------------------------------------------------------------------

  friend Wide select(const WideBool& c, const Wide& a, const Wide& b) noexcept { return c.v ? a : b; }
  friend Wide select(const WideBool& c, const Wide& a, double b) noexcept { return c.v ? a : Wide(b); }
  friend Wide select(const WideBool& c, double a, const Wide& b) noexcept { return c.v ? Wide(a) : b; }

  // scalar/select.hpp's value semantics: max(a,b) = a < b ? b : a, min(a,b) = b < a ? b : a,
  // abs(a) = a < 0 ? -a : a (so abs(-0.0) is -0.0, unlike std::fabs).
  friend Wide max(const Wide& a, const Wide& b) noexcept { return select(a < b, b, a); }
  friend Wide min(const Wide& a, const Wide& b) noexcept { return select(b < a, b, a); }
  friend Wide abs(const Wide& a) noexcept { return select(a < 0.0, -a, a); }
  friend Wide max(const Wide& a, double b) noexcept { return max(a, Wide(b)); }
  friend Wide max(double a, const Wide& b) noexcept { return max(Wide(a), b); }
  friend Wide min(const Wide& a, double b) noexcept { return min(a, Wide(b)); }
  friend Wide min(double a, const Wide& b) noexcept { return min(Wide(a), b); }

  // ---- transcendental --------------------------------------------------------------------------

  friend Wide recip(const Wide& a) noexcept { return Wide(1.0) / a; }

  // a·b + c at the oracle's precision. An Fma node of the recorded program means a·b + c with one
  // rounding; evaluated here it gets none that matters.
  friend Wide fma(const Wide& a, const Wide& b, const Wide& c) noexcept { return a * b + c; }

  // sqrt by two Newton steps on x' = (x + a/x)/2 from the double root. Each step squares the
  // relative error (1e-16 -> 1e-32), so the second is already at the representation's floor.
  friend Wide sqrt(const Wide& a) noexcept {
    if (a.hi == 0.0) return Wide(a.hi);  // preserves the sign of zero
    if (a.hi < 0.0) return Wide(std::numeric_limits<double>::quiet_NaN());
    if (!(a.hi - a.hi == 0.0)) return Wide(std::sqrt(a.hi));  // inf / nan
    Wide y(std::sqrt(a.hi));
    y = (y + a / y) * 0.5;  // ·0.5 is exact
    y = (y + a / y) * 0.5;
    return y;
  }

  // exp by range reduction to |r| <= ln2/64, a 16-term Taylor sum in double-double, five
  // squarings and an exact scaling by 2^k. Truncation after r^16/16! is below 1e-40; the
  // dominant residual is the reduction, |x|·2^-106 absolute, which for the |x| <= 1 this engine
  // evaluates is ~1e-32 relative. Measured in tests/scalar/wide_e0_test.cpp.
  friend Wide exp(const Wide& a) noexcept {
    if (a.hi <= -746.0) return Wide(0.0);
    if (a.hi >= 710.0) return Wide(std::numeric_limits<double>::infinity());
    if (!(a.hi - a.hi == 0.0)) return Wide(std::exp(a.hi));  // nan
    if (a.hi == 0.0 && a.lo == 0.0) return Wide(1.0);

    const double k = std::floor(a.hi / wide_detail::ln2_hi + 0.5);
    // r = a − k·ln2 in double-double: |r| <= ln2/2 + a rounding.
    Wide r = a - Wide(wide_detail::ln2_hi, wide_detail::ln2_lo) * k;
    r = r * 0x1p-5;  // exact: |r| <= ln2/64 ~ 0.0109

    Wide s = r + 1.0;
    Wide t = r;
    for (int n = 2; n <= 16; ++n) {
      t = (t * r) / static_cast<double>(n);  // n is exact in double for n <= 2^53
      s = s + t;
    }
    for (int i = 0; i < 5; ++i) s = s * s;  // undo the 2^-5 reduction

    const int ki = static_cast<int>(k);
    return Wide(std::ldexp(s.hi, ki), std::ldexp(s.lo, ki));  // exact unless it overflows
  }

  // log by two Newton steps on y' = y + (a·exp(−y) − 1) from the double logarithm. The step maps
  // an absolute error e to e²/2, so one step reaches the representation's floor for |log a| ~ 1
  // and the second covers the whole range. NOTE: the error is ABSOLUTE, so log(1 + d) for a tiny
  // d is accurate to ~1e-31 absolute and no better relatively. Nothing on this engine's pricing
  // path calls log on a Scalar (grep: only exp and sqrt appear), so no log1p is provided rather
  // than one being provided untested.
  friend Wide log(const Wide& a) noexcept {
    if (a.hi <= 0.0) return Wide(std::log(a.hi));  // -inf / nan, as libm gives them
    if (!(a.hi - a.hi == 0.0)) return Wide(a.hi);  // inf
    if (a.hi == 1.0 && a.lo == 0.0) return Wide(0.0);
    Wide y(std::log(a.hi));
    y = y + (a * exp(-y) - 1.0);
    y = y + (a * exp(-y) - 1.0);
    return y;
  }
};

static_assert(!std::is_convertible_v<Wide, double>, "no implicit Wide -> double");
static_assert(std::is_trivially_copyable_v<Wide>);

// Both arms constant under a Wide predicate. This one CANNOT be a hidden friend: its arguments are
// (WideBool, double, double), so `Wide` is not an associated class and ADL would never find it —
// the same reason scalar/rec.hpp:244 and scalar/dual.hpp:251 declare their versions at namespace
// scope. `maths/curve/scheme.hpp:547` (`select(dr < 0.0, -1.0, 1.0)`) is a real caller.
// Returns Wide, matching Rec rather than Dual, so a generic `const Scalar x = select(c, 1.0, 2.0)`
// binds the same way on every Scalar.
inline Wide select(const WideBool& c, double a, double b) noexcept { return c.v ? Wide(a) : Wide(b); }

// A structural branch. Wide carries no taint (Rec, D14) and no tangent (Dual), so there is nothing
// here to check and this only unwraps the bit. Stated rather than silently doing less than its
// siblings: Rec remains the authoritative recording-discipline check.
inline bool structural_if(const WideBool& c) noexcept { return c.v; }

// ------------------------------------------------------------------------------------------------
// The precision guard (PRINCIPLES.md §4; the owner's "an oracle silently equal to the thing it
// judges is worse than no oracle").
//
// `oracle_mantissa_bits<T>` is the significand width of a candidate oracle type. The static_assert
// below is on the type actually bound as the oracle, so swapping `Wide` for `long double` here
// fails to COMPILE on any target where `long double` is `double` (Apple arm64) instead of
// degrading quietly. Wide is 106 bits on every target, so the assert holds unconditionally today —
// which is the point: there is no platform on which this oracle needs a skip.
// ------------------------------------------------------------------------------------------------
template <class T>
struct oracle_mantissa_bits : std::integral_constant<int, std::numeric_limits<T>::digits> {};
template <>
struct oracle_mantissa_bits<Wide> : std::integral_constant<int, Wide::mantissa_bits> {};

template <class T>
inline constexpr int oracle_mantissa_bits_v = oracle_mantissa_bits<T>::value;

// The oracle type PRINCIPLES.md §4 means when it says "the templated maths instantiated at wider
// precision". Everything that judges a `double` evaluation against truth instantiates THIS.
using Oracle = Wide;

static_assert(oracle_mantissa_bits_v<Oracle> > std::numeric_limits<double>::digits,
              "the oracle must have strictly more significand bits than double, or it is not an "
              "oracle: it would be judging double with double. On Apple arm64 `long double` IS "
              "`double` (53 bits), which is exactly the configuration this assert exists to stop.");

// How much finer the oracle resolves than a double ulp: 2^(oracle bits − 53). Reported by the
// gates so a number always carries the headroom it was measured with.
inline constexpr int oracle_headroom_bits = oracle_mantissa_bits_v<Oracle> - std::numeric_limits<double>::digits;

// `long double` is the oracle's independent witness, not the oracle (see the header comment).
// It is 64 significand bits on x86-64, 113 on aarch64 Linux, and 53 — i.e. useless — on Apple
// arm64. Gates that use it must skip loudly when this is false.
inline constexpr int witness_mantissa_bits = std::numeric_limits<long double>::digits;
inline constexpr bool witness_is_wider_than_double = witness_mantissa_bits > std::numeric_limits<double>::digits;

}  // namespace epykos
