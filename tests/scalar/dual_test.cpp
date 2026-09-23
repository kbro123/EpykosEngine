// M2/Q2: the forward-mode scalar. Every operator's value is the double result and its tangent the
// hand derivative, mixed forms on both sides; several tangent slots are independent single
// directions; DualBool does not convert to bool; select chooses the arm (value and tangent);
// max/min/abs go through select; structural_if returns the bit and throws when the predicate
// carries a nonzero tangent.
//
// Tangents that are a sum of two products are compared at 4 ulps (EXPECT_DOUBLE_EQ): this TU may
// contract them. Exact bit equality of the tangent arithmetic is asserted in the E0 TU
// (dual_m1_e0_test.cpp), where it is a theorem rather than a compiler decision.
#include <gtest/gtest.h>

#include <cmath>
#include <type_traits>
#include <utility>

#include "epykos/scalar/dual.hpp"
#include "epykos/scalar/select.hpp"

using epykos::Dual;
using epykos::DualBool;
using epykos::DualError;
using D1 = Dual<1>;
using D3 = Dual<3>;

// ---- compile-time discipline ------------------------------------------------------------------

static_assert(!std::is_convertible_v<D1, double>, "no implicit Dual -> double");
static_assert(!std::is_convertible_v<Dual<12>, double>, "no implicit Dual -> double");
static_assert(!std::is_convertible_v<DualBool, bool>, "no implicit DualBool -> bool");
static_assert(!std::is_constructible_v<bool, DualBool>, "no explicit DualBool -> bool");
static_assert(std::is_convertible_v<double, D1>, "double -> Dual is allowed (Scalar x = 0.0)");
static_assert(std::is_trivially_copyable_v<D1> && std::is_trivially_copyable_v<Dual<12>>);
static_assert(std::is_trivially_copyable_v<DualBool>);
static_assert(sizeof(D1) == 2 * sizeof(double), "{v, d[1]}");
static_assert(sizeof(Dual<12>) == 13 * sizeof(double), "{v, d[12]}");
static_assert(D1::n_tangents == 1 && Dual<12>::n_tangents == 12);

// A predicate that is only well-formed when `if (DualBool)` would compile.
template <class T, class = void>
struct usable_as_condition : std::false_type {};
template <class T>
struct usable_as_condition<T, std::void_t<decltype(std::declval<T>() ? 1 : 0)>> : std::true_type {};
static_assert(!usable_as_condition<DualBool>::value, "DualBool must not be usable as a condition");
static_assert(usable_as_condition<bool>::value, "sanity check of the detector");

// Comparisons yield DualBool, on either side of a double.
static_assert(std::is_same_v<decltype(std::declval<D1>() < std::declval<D1>()), DualBool>);
static_assert(std::is_same_v<decltype(std::declval<D1>() <= 1.0), DualBool>);
static_assert(std::is_same_v<decltype(1.0 > std::declval<D1>()), DualBool>);
static_assert(std::is_same_v<decltype(std::declval<D1>() >= std::declval<D1>()), DualBool>);
static_assert(std::is_same_v<decltype(std::declval<D1>() == std::declval<D1>()), DualBool>);
static_assert(std::is_same_v<decltype(1.0 == std::declval<D1>()), DualBool>);

// Arithmetic yields Dual, on either side of a double; the double vocabulary still returns double.
static_assert(std::is_same_v<decltype(2.0 * std::declval<D1>()), D1>);
static_assert(std::is_same_v<decltype(std::declval<D1>() / 2.0), D1>);
static_assert(std::is_same_v<decltype(exp(std::declval<D3>())), D3>);
static_assert(std::is_same_v<decltype(select(std::declval<DualBool>(), 1.0, 0.0)), double>);
static_assert(std::is_same_v<decltype(select(std::declval<DualBool>(), std::declval<D1>(), 0.0)), D1>);
static_assert(std::is_same_v<decltype(epykos::select(true, 1.0, 0.0)), double>);

namespace {

D1 var(double v, double dv) { return D1(v, {dv}); }

}  // namespace

// ---- leaves -----------------------------------------------------------------------------------

TEST(Dual, Leaves) {
  const D1 zero;
  EXPECT_EQ(zero.v, 0.0);
  EXPECT_EQ(zero.d[0], 0.0);
  EXPECT_FALSE(zero.has_tangent());

  const D1 k = 2.5;  // a constant: zero tangent
  EXPECT_EQ(k.value(), 2.5);
  EXPECT_EQ(k.tangent(0), 0.0);
  EXPECT_FALSE(k.has_tangent());

  const D1 x = var(1.5, 3.0);
  EXPECT_EQ(x.v, 1.5);
  EXPECT_EQ(x.d[0], 3.0);
  EXPECT_TRUE(x.has_tangent());

  const D3 y = D3::variable(0.5, 1);
  EXPECT_EQ(y.v, 0.5);
  EXPECT_EQ(y.d, (std::array<double, 3>{0.0, 1.0, 0.0}));
  const D3 w = D3::variable(0.5, 2, -4.0);
  EXPECT_EQ(w.d, (std::array<double, 3>{0.0, 0.0, -4.0}));
  EXPECT_TRUE(w.has_tangent());
  const D3 nan_t(1.0, {0.0, std::nan(""), 0.0});
  EXPECT_TRUE(nan_t.has_tangent()) << "a NaN tangent counts as nonzero";
}

// ---- arithmetic rules vs hand derivatives -------------------------------------------------------

TEST(Dual, AddSub) {
  const D1 a = var(3.0, 0.5);
  const D1 b = var(0.25, -2.0);
  D1 r = a + b;
  EXPECT_EQ(r.v, 3.25);
  EXPECT_EQ(r.d[0], -1.5);
  r = a - b;
  EXPECT_EQ(r.v, 2.75);
  EXPECT_EQ(r.d[0], 2.5);
  r = a + 2.0;
  EXPECT_EQ(r.v, 5.0);
  EXPECT_EQ(r.d[0], 0.5);
  r = 2.0 + a;
  EXPECT_EQ(r.v, 5.0);
  EXPECT_EQ(r.d[0], 0.5);
  r = a - 2.0;
  EXPECT_EQ(r.v, 1.0);
  EXPECT_EQ(r.d[0], 0.5);
  r = 2.0 - a;
  EXPECT_EQ(r.v, -1.0);
  EXPECT_EQ(r.d[0], -0.5);
  r = +a;
  EXPECT_EQ(r.v, 3.0);
  EXPECT_EQ(r.d[0], 0.5);
  r = -a;
  EXPECT_EQ(r.v, -3.0);
  EXPECT_EQ(r.d[0], -0.5);
}

TEST(Dual, MulDiv) {
  const D1 a = var(3.0, 0.5);
  const D1 b = var(0.25, -2.0);
  D1 r = a * b;
  EXPECT_EQ(r.v, 0.75);
  EXPECT_DOUBLE_EQ(r.d[0], 0.5 * 0.25 + 3.0 * (-2.0));  // a'b + ab' = -5.875
  r = a / b;
  EXPECT_EQ(r.v, 12.0);
  EXPECT_DOUBLE_EQ(r.d[0], (0.5 * 0.25 - 3.0 * (-2.0)) / (0.25 * 0.25));  // (a'b - ab')/b² = 98
  r = a * 2.0;
  EXPECT_EQ(r.v, 6.0);
  EXPECT_EQ(r.d[0], 1.0);
  r = 2.0 * a;
  EXPECT_EQ(r.v, 6.0);
  EXPECT_EQ(r.d[0], 1.0);
  r = a / 2.0;
  EXPECT_EQ(r.v, 1.5);
  EXPECT_EQ(r.d[0], 0.25);
  r = 2.0 / a;
  EXPECT_EQ(r.v, 2.0 / 3.0);
  EXPECT_DOUBLE_EQ(r.d[0], -2.0 * 0.5 / 9.0);  // -a b'/b²
  // A constant on either side contributes no tangent term: the result is exactly k·x'.
  const D1 c = var(-7.0, 1.0 / 3.0);
  EXPECT_EQ((c * 0.1).d[0], (1.0 / 3.0) * 0.1);
  EXPECT_EQ((0.1 * c).d[0], 0.1 * (1.0 / 3.0));
}

TEST(Dual, ExpLogSqrtRecipFma) {
  const D1 x = var(0.5, 2.0);
  D1 r = exp(x);
  EXPECT_EQ(r.v, std::exp(0.5));
  EXPECT_EQ(r.d[0], std::exp(0.5) * 2.0);
  const D1 y = var(4.0, 3.0);
  r = log(y);
  EXPECT_EQ(r.v, std::log(4.0));
  EXPECT_EQ(r.d[0], 0.75);
  const D1 s = var(9.0, 4.0);
  r = sqrt(s);
  EXPECT_EQ(r.v, 3.0);
  EXPECT_EQ(r.d[0], 4.0 / 6.0);  // x'/(2√x)
  r = recip(y);
  EXPECT_EQ(r.v, 0.25);
  EXPECT_EQ(r.d[0], -0.1875);  // -x'/x² = -3/16
  const D1 a = var(3.0, 0.5);
  const D1 b = var(0.25, -2.0);
  const D1 c = var(1.0, 7.0);
  r = fma(a, b, c);
  EXPECT_EQ(r.v, std::fma(3.0, 0.25, 1.0));
  EXPECT_DOUBLE_EQ(r.d[0], 0.5 * 0.25 + 3.0 * (-2.0) + 7.0);  // a'b + ab' + c' = 1.125
  r = fma(a, 2.0, c);  // mixed: the double is a constant
  EXPECT_EQ(r.v, 7.0);
  EXPECT_DOUBLE_EQ(r.d[0], 0.5 * 2.0 + 7.0);
  r = fma(2.0, a, 1.0);
  EXPECT_EQ(r.v, 7.0);
  EXPECT_DOUBLE_EQ(r.d[0], 1.0);
  // The double vocabulary is untouched.
  EXPECT_EQ(epykos::exp(0.5), std::exp(0.5));
  EXPECT_EQ(epykos::recip(4.0), 0.25);
}

TEST(Dual, CompoundAssignment) {
  D1 x = var(3.0, 0.5);
  const D1 b = var(0.25, -2.0);
  x += b;
  EXPECT_EQ(x.v, 3.25);
  EXPECT_EQ(x.d[0], -1.5);
  x -= b;
  EXPECT_EQ(x.v, 3.0);
  EXPECT_EQ(x.d[0], 0.5);
  x *= 2.0;
  EXPECT_EQ(x.v, 6.0);
  EXPECT_EQ(x.d[0], 1.0);
  x /= 4.0;
  EXPECT_EQ(x.v, 1.5);
  EXPECT_EQ(x.d[0], 0.25);
  x += 1.0;
  EXPECT_EQ(x.v, 2.5);
  EXPECT_EQ(x.d[0], 0.25);
  x -= 0.5;
  EXPECT_EQ(x.v, 2.0);
  x *= b;
  EXPECT_EQ(x.v, 0.5);
  EXPECT_DOUBLE_EQ(x.d[0], 0.25 * 0.25 + 2.0 * (-2.0));
  x /= b;
  EXPECT_EQ(x.v, 2.0);
  EXPECT_DOUBLE_EQ(x.d[0], 0.25);
}

// A chain of every operator against the analytic derivative.
TEST(Dual, ChainRuleVsAnalytic) {
  // f(x) = exp(-x t) / (1 + x) + sqrt(x) log(x) - 2 / x, t = 3.5
  const double t = 3.5;
  auto f = [t](auto x) {
    using std::exp;
    using std::log;
    using std::sqrt;
    return exp(-x * t) / (1.0 + x) + sqrt(x) * log(x) - 2.0 / x;
  };
  for (double x0 : {0.3, 1.0, 2.75}) {
    const D1 r = f(var(x0, 1.0));
    EXPECT_EQ(r.v, f(x0));
    const double e = std::exp(-x0 * t);
    const double analytic = (-t * e * (1.0 + x0) - e) / ((1.0 + x0) * (1.0 + x0)) +
                            std::log(x0) / (2.0 * std::sqrt(x0)) + 1.0 / std::sqrt(x0) + 2.0 / (x0 * x0);
    EXPECT_NEAR(r.d[0], analytic, 1e-13 * std::fabs(analytic)) << x0;
    // Scaling the direction scales the tangent.
    const D1 r3 = f(var(x0, 3.0));
    EXPECT_NEAR(r3.d[0], 3.0 * analytic, 1e-13 * 3.0 * std::fabs(analytic)) << x0;
  }
}

// N tangent slots are N independent single directions through the same value channel.
TEST(Dual, SlotsAreIndependentDirections) {
  const D3 x(1.5, {1.0, 0.0, 0.3});
  const D3 y(2.0, {0.0, 1.0, -1.0});
  auto f = [](auto a, auto b) {
    using std::exp;
    using std::log;
    return (a * b + exp(a) - log(b) / a) / (b - 0.5 * a) + fma(a, b, 2.0) + sqrt(a + b);
  };
  const D3 r = f(x, y);
  for (int i = 0; i < 3; ++i) {
    const auto s = static_cast<std::size_t>(i);
    const D1 r1 = f(D1(x.v, {x.d[s]}), D1(y.v, {y.d[s]}));
    EXPECT_EQ(r1.v, r.v);
    EXPECT_DOUBLE_EQ(r1.d[0], r.d[s]) << "slot " << i;
  }
  // Slot 2 is the combination 0.3·(slot 0) − 1·(slot 1) to rounding.
  EXPECT_NEAR(r.d[2], 0.3 * r.d[0] - r.d[1], 1e-13 * (std::fabs(0.3 * r.d[0]) + std::fabs(r.d[1])));
}

// ---- comparisons, select, structural_if -------------------------------------------------------

TEST(Dual, ComparisonsCarryTheBitAndWhetherAnOperandHasATangent) {
  const D1 a = var(1.0, 0.0);
  const D1 b = var(2.0, 0.0);
  const D1 c = var(2.0, 1.0);
  EXPECT_TRUE((a < b).unchecked_value());
  EXPECT_FALSE((a < b).active);
  EXPECT_TRUE((a < c).active);
  EXPECT_TRUE((c > a).active);
  EXPECT_FALSE((a <= b).active);
  EXPECT_TRUE((a <= b).v);
  EXPECT_FALSE((a > b).v);
  EXPECT_FALSE((a >= b).v);
  EXPECT_TRUE((b >= a).v);
  EXPECT_TRUE((b == c).v);
  EXPECT_TRUE((b == c).active);
  EXPECT_FALSE((a == b).v);
  // Mixed with double on either side.
  EXPECT_TRUE((a < 1.5).v);
  EXPECT_FALSE((a < 1.5).active);
  EXPECT_TRUE((c < 2.5).active);
  EXPECT_TRUE((1.5 < b).v);
  EXPECT_FALSE((1.5 < b).active);
  EXPECT_TRUE((2.5 > c).active);
  EXPECT_TRUE((2.0 == c).v);
  EXPECT_TRUE((c <= 2.0).v);
  EXPECT_TRUE((2.0 >= c).v);
  EXPECT_TRUE((c >= 2.0).v);
  EXPECT_TRUE((2.0 <= c).v);
  EXPECT_TRUE((c == 2.0).v);
  const DualBool none;
  EXPECT_FALSE(none.v);
  EXPECT_FALSE(none.active);
}

TEST(Dual, SelectChoosesTheArmValueAndTangent) {
  const D1 a = var(3.0, 0.5);
  const D1 b = var(0.25, -2.0);
  D1 r = select(a < b, a, b);
  EXPECT_EQ(r.v, 0.25);
  EXPECT_EQ(r.d[0], -2.0);
  r = select(b < a, a, b);
  EXPECT_EQ(r.v, 3.0);
  EXPECT_EQ(r.d[0], 0.5);
  r = select(a < b, a, 9.0);
  EXPECT_EQ(r.v, 9.0);
  EXPECT_EQ(r.d[0], 0.0);
  r = select(b < a, a, 9.0);
  EXPECT_EQ(r.v, 3.0);
  EXPECT_EQ(r.d[0], 0.5);
  r = select(a < b, 9.0, b);
  EXPECT_EQ(r.v, 0.25);
  EXPECT_EQ(r.d[0], -2.0);
  r = select(b < a, 9.0, b);
  EXPECT_EQ(r.v, 9.0);
  EXPECT_EQ(r.d[0], 0.0);
  const double k = select(a < b, 1.0, 0.0);
  EXPECT_EQ(k, 0.0);
  // The same on three slots: the whole tangent array of the chosen arm.
  const D3 x(1.0, {1.0, 2.0, 3.0});
  const D3 y(2.0, {-1.0, -2.0, -3.0});
  const D3 s = select(x < y, x, y);
  EXPECT_EQ(s.v, 1.0);
  EXPECT_EQ(s.d, x.d);
}

TEST(Dual, MaxMinAbsViaSelect) {
  const D1 a = var(3.0, 0.5);
  const D1 b = var(0.25, -2.0);
  D1 r = max(a, b);
  EXPECT_EQ(r.v, 3.0);
  EXPECT_EQ(r.d[0], 0.5);
  r = min(a, b);
  EXPECT_EQ(r.v, 0.25);
  EXPECT_EQ(r.d[0], -2.0);
  r = max(a, 4.0);
  EXPECT_EQ(r.v, 4.0);
  EXPECT_EQ(r.d[0], 0.0);
  r = max(4.0, a);
  EXPECT_EQ(r.v, 4.0);
  r = min(a, 4.0);
  EXPECT_EQ(r.v, 3.0);
  EXPECT_EQ(r.d[0], 0.5);
  r = min(-4.0, a);
  EXPECT_EQ(r.v, -4.0);
  EXPECT_EQ(r.d[0], 0.0);
  // Ties follow the select.hpp / rec.hpp semantics: max(a,b) = a < b ? b : a keeps a.
  const D1 a2 = var(3.0, 7.0);
  EXPECT_EQ(max(a, a2).d[0], 0.5);
  EXPECT_EQ(min(a, a2).d[0], 0.5);
  const D1 n = var(-2.0, 3.0);
  r = abs(n);
  EXPECT_EQ(r.v, 2.0);
  EXPECT_EQ(r.d[0], -3.0);
  r = abs(a);
  EXPECT_EQ(r.v, 3.0);
  EXPECT_EQ(r.d[0], 0.5);
  const D1 mz = var(-0.0, 1.0);
  r = abs(mz);
  EXPECT_TRUE(std::signbit(r.v)) << "abs(-0.0) is -0.0, as in select.hpp";
  EXPECT_EQ(r.d[0], 1.0);
  // The double vocabulary is untouched.
  EXPECT_EQ(epykos::max(1.0, 2.0), 2.0);
  EXPECT_EQ(epykos::abs(-1.0), 1.0);
}

TEST(Dual, StructuralIfReturnsTheBitAndThrowsOnAnActivePredicate) {
  const D1 k1 = 1.0;
  const D1 k2 = 2.0;
  EXPECT_TRUE(epykos::structural_if(k1 < k2));
  EXPECT_FALSE(epykos::structural_if(k2 < k1));
  EXPECT_TRUE(epykos::structural_if(k1 < 1.5));
  const D1 x = var(1.0, 1.0);
  EXPECT_THROW(epykos::structural_if(x < k2), DualError);
  EXPECT_THROW(epykos::structural_if(2.0 > x), DualError);
  EXPECT_THROW(epykos::structural_if(x == 1.0), DualError);
  // A zero tangent is not evidence either way: the bit is returned (Rec's taint is authoritative).
  const D1 y = var(1.0, 0.0);
  EXPECT_TRUE(epykos::structural_if(y < k2));
  // The bool vocabulary still works.
  EXPECT_TRUE(epykos::structural_if(true));
}
