// PRINCIPLES.md §1b invariant I2, the scalar half: `Dual` nests inside itself, so second order is
// reachable with no hand-written second derivative anywhere.
//
// `Dual<N, S>` carries N tangents over the scalar S; S = double is first order (`Dual<N>`, every
// existing call site) and S = Dual<M> is second. The seeding convention of scalar/dual.hpp is that
// the INNER direction rides in the value channel and the OUTER direction in the tangent channel:
//
//     x_k.v    = Dual<M>(x_k, w_k)        the inner seed
//     x_k.d[i] = Dual<M>(u_ki)            the outer seed, itself a constant of the inner level
//     f(x).d[i].d[j] = Σ_{k,l} ∂²f/∂x_k∂x_l · u_ki · w_lj
//
// This TU checks the ALGEBRA of that: every rule of the header against its hand second derivative
// in closed form, symmetry of a mixed partial, and the discipline (no implicit conversion, no
// `if` on a comparison, structural_if's active flag) at depth 2. The pricing maths is instantiated
// nested in tests/maths/second_order_test.cpp, which is also where the agreement tolerance is
// measured; the first-order channel's bit-identity is dual_m1_e0_test.cpp's, unchanged.
//
// Second derivatives of a sum of products are compared at 4 ulps (EXPECT_DOUBLE_EQ) in this TU:
// it may contract. Nothing here asserts bits.
#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <type_traits>
#include <utility>

#include "epykos/scalar/dual.hpp"
#include "epykos/scalar/select.hpp"

using epykos::Dual;
using epykos::DualBool;
using epykos::DualError;

using D1 = Dual<1>;        // first order, one direction
using D2 = Dual<1, D1>;    // second order, one outer direction over one inner
using D22 = Dual<2, D1>;   // second order, two outer directions over one inner
using D3 = Dual<1, D2>;    // third order — the nesting is closed, not special-cased at depth 2

// ---- the type, at depth 2 ---------------------------------------------------------------------

static_assert(std::is_same_v<Dual<3>, Dual<3, double>>, "Dual<N> is Dual<N, double>: 150 call sites");
static_assert(std::is_same_v<D2::scalar_type, D1> && std::is_same_v<D1::scalar_type, double>);
static_assert(!D1::nested && D2::nested && D22::nested);
static_assert(std::is_same_v<D2::tangent_type, std::array<D1, 1>>);
static_assert(std::is_trivially_copyable_v<D2> && std::is_trivially_copyable_v<D22>);
static_assert(sizeof(D2) == 4 * sizeof(double), "{{v, d[1]}, {v, d[1]}}");
static_assert(sizeof(D22) == 6 * sizeof(double), "{v, d[2]} of {v, d[1]}");
static_assert(D2::n_tangents == 1 && D22::n_tangents == 2);

// The discipline survives the nesting: no silent collapse to a double, and no silent collapse of
// the outer level to the inner one either (that would drop a derivative without a diagnostic).
static_assert(!std::is_convertible_v<D2, double>, "no implicit Dual -> double at depth 2");
static_assert(!std::is_convertible_v<D2, D1>, "no implicit outer Dual -> inner Dual");
static_assert(!std::is_convertible_v<D22, D1>);
// A double is a constant at every depth: `Scalar x = 2.0` must compile, or the maths does not.
static_assert(std::is_convertible_v<double, D2>, "a double is a constant at depth 2");
static_assert(std::is_convertible_v<D1, D2>, "an inner value is a constant of the outer level");

// Comparisons still yield DualBool — ONE type at every depth, so one select() serves both.
static_assert(std::is_same_v<decltype(std::declval<D2>() < std::declval<D2>()), DualBool>);
static_assert(std::is_same_v<decltype(std::declval<D2>() <= 1.0), DualBool>);
static_assert(std::is_same_v<decltype(1.0 > std::declval<D2>()), DualBool>);
template <class T, class = void>
struct usable_as_condition : std::false_type {};
template <class T>
struct usable_as_condition<T, std::void_t<decltype(std::declval<T>() ? 1 : 0)>> : std::true_type {};
static_assert(!usable_as_condition<DualBool>::value);

// Arithmetic at depth 2 keeps its type on either side of a double, and the double/double select
// still returns a double (which converts in, as the monotone limiter of curve/scheme.hpp needs).
static_assert(std::is_same_v<decltype(2.0 * std::declval<D2>()), D2>);
static_assert(std::is_same_v<decltype(std::declval<D2>() / 2.0), D2>);
static_assert(std::is_same_v<decltype(exp(std::declval<D2>())), D2>);
static_assert(std::is_same_v<decltype(select(std::declval<DualBool>(), 1.0, 0.0)), double>);
static_assert(std::is_same_v<decltype(select(std::declval<DualBool>(), std::declval<D2>(), 0.0)), D2>);
// An inner Dual mixed with an outer one is unambiguous: it converts in as a constant of the outer
// level, keeping its own inner tangent.
static_assert(std::is_same_v<decltype(std::declval<D2>() * std::declval<D1>()), D2>);

namespace {

// x seeded in BOTH channels: f(x).d[0].d[0] is ∂²f/∂x².
D2 var2(double x) { return D2(D1(x, {1.0}), {D1(1.0)}); }
// x seeded in the OUTER channel only (direction u).
D2 var_outer(double x) { return D2(D1(x), {D1(1.0)}); }
// x seeded in the INNER channel only (direction w).
D2 var_inner(double x) { return D2(D1(x, {1.0}), {D1(0.0)}); }
// A constant at both levels.
D2 konst(double x) { return D2(x); }

double value(const D2& r) { return r.v.v; }
double first(const D2& r) { return r.d[0].v; }    // Σ ∂f/∂x_k u_k
double first_inner(const D2& r) { return r.v.d[0]; }  // Σ ∂f/∂x_k w_k
double second(const D2& r) { return r.d[0].d[0]; }

}  // namespace

// ---- leaves ------------------------------------------------------------------------------------

TEST(DualNested, Leaves) {
  const D2 zero;
  EXPECT_EQ(value(zero), 0.0);
  EXPECT_EQ(first(zero), 0.0);
  EXPECT_EQ(second(zero), 0.0);
  EXPECT_FALSE(zero.has_tangent());
  EXPECT_FALSE(zero.depends());

  const D2 k = 2.5;  // a double is a constant at depth 2 (one user-defined conversion)
  EXPECT_EQ(value(k), 2.5);
  EXPECT_EQ(first(k), 0.0);
  EXPECT_EQ(second(k), 0.0);
  EXPECT_FALSE(k.depends());

  const D2 x = var2(2.5);
  EXPECT_EQ(value(x), 2.5);
  EXPECT_EQ(first(x), 1.0);
  EXPECT_EQ(first_inner(x), 1.0);
  EXPECT_EQ(second(x), 0.0);  // x is linear in itself
  EXPECT_TRUE(x.has_tangent());
  EXPECT_TRUE(x.depends());

  // An inner-only seed carries no OUTER tangent, but it still demonstrably depends on an input:
  // that is the difference between has_tangent() and depends(), and it exists only when nested.
  const D2 w = var_inner(2.5);
  EXPECT_FALSE(w.has_tangent());
  EXPECT_TRUE(w.depends());

  // variable() seeds a slot at either depth.
  const D22 v = D22::variable(D1(3.0, {1.0}), 1);
  EXPECT_EQ(v.v.v, 3.0);
  EXPECT_EQ(v.d[0].v, 0.0);
  EXPECT_EQ(v.d[1].v, 1.0);
}

// ---- the arithmetic rules, against the hand second derivative ----------------------------------

TEST(DualNested, Arithmetic) {
  const double x0 = 1.75, y0 = 0.625;

  // x·x: ∂²/∂x² = 2
  {
    const D2 x = var2(x0);
    const D2 r = x * x;
    EXPECT_DOUBLE_EQ(value(r), x0 * x0);
    EXPECT_DOUBLE_EQ(first(r), 2.0 * x0);
    EXPECT_DOUBLE_EQ(second(r), 2.0);
  }
  // x + x, x − x, unary minus: all linear, second derivative zero.
  {
    const D2 x = var2(x0);
    EXPECT_DOUBLE_EQ(second(x + x), 0.0);
    EXPECT_DOUBLE_EQ(second(x - x), 0.0);
    EXPECT_DOUBLE_EQ(second(-x), 0.0);
    EXPECT_DOUBLE_EQ(second(x + 3.0), 0.0);
    EXPECT_DOUBLE_EQ(second(3.0 - x), 0.0);
    EXPECT_DOUBLE_EQ(second(4.0 * x), 0.0);
    EXPECT_DOUBLE_EQ(first(4.0 * x), 4.0);
  }
  // x/y with both seeded the same way: ∂²/∂x² of x/y0 is 0; ∂²/∂y² of x0/y is 2x0/y³.
  {
    const D2 y = var2(y0);
    const D2 r = konst(x0) / y;
    EXPECT_DOUBLE_EQ(value(r), x0 / y0);
    EXPECT_DOUBLE_EQ(first(r), -x0 / (y0 * y0));
    EXPECT_DOUBLE_EQ(second(r), 2.0 * x0 / (y0 * y0 * y0));
  }
  // 1/y written as the double/Dual overload, same rule.
  {
    const D2 y = var2(y0);
    const D2 r = 1.0 / y;
    EXPECT_DOUBLE_EQ(second(r), 2.0 / (y0 * y0 * y0));
  }
  // The MIXED partial, and its symmetry: ∂²(x/y)/∂x∂y = −1/y².
  {
    const D2 r1 = var_outer(x0) / var_inner(y0);  // u = e_x, w = e_y
    const D2 r2 = var_inner(x0) / var_outer(y0);  // u = e_y, w = e_x
    EXPECT_DOUBLE_EQ(second(r1), -1.0 / (y0 * y0));
    EXPECT_DOUBLE_EQ(second(r2), -1.0 / (y0 * y0));
    EXPECT_DOUBLE_EQ(second(r1), second(r2));  // Clairaut, computed two different ways
  }
  // ∂²(x·y)/∂x∂y = 1, and ∂²/∂x² = 0.
  {
    EXPECT_DOUBLE_EQ(second(var_outer(x0) * var_inner(y0)), 1.0);
    EXPECT_DOUBLE_EQ(second(var2(x0) * konst(y0)), 0.0);
  }
  // Compound assignment goes through the same operators.
  {
    D2 t = var2(x0);
    t *= t;
    EXPECT_DOUBLE_EQ(second(t), 2.0);
    D2 u = var2(x0);
    u /= konst(y0);
    EXPECT_DOUBLE_EQ(second(u), 0.0);
  }
}

TEST(DualNested, Transcendental) {
  const double x0 = 1.75;

  {  // exp'' = exp
    const D2 r = exp(var2(x0));
    EXPECT_DOUBLE_EQ(value(r), std::exp(x0));
    EXPECT_DOUBLE_EQ(first(r), std::exp(x0));
    EXPECT_DOUBLE_EQ(second(r), std::exp(x0));
  }
  {  // log'' = −1/x²
    const D2 r = log(var2(x0));
    EXPECT_DOUBLE_EQ(value(r), std::log(x0));
    EXPECT_DOUBLE_EQ(first(r), 1.0 / x0);
    EXPECT_DOUBLE_EQ(second(r), -1.0 / (x0 * x0));
  }
  {  // sqrt'' = −1/(4 x^{3/2})
    const D2 r = sqrt(var2(x0));
    EXPECT_DOUBLE_EQ(value(r), std::sqrt(x0));
    EXPECT_DOUBLE_EQ(first(r), 0.5 / std::sqrt(x0));
    EXPECT_DOUBLE_EQ(second(r), -0.25 / (x0 * std::sqrt(x0)));
  }
  {  // recip'' = 2/x³
    const D2 r = recip(var2(x0));
    EXPECT_DOUBLE_EQ(value(r), 1.0 / x0);
    EXPECT_DOUBLE_EQ(first(r), -1.0 / (x0 * x0));
    EXPECT_DOUBLE_EQ(second(r), 2.0 / (x0 * x0 * x0));
  }
  {  // exp(−z·t), the discount factor: ∂²DF/∂z² = t²·DF. The one that matters.
    const double t = 7.0, z0 = 0.042;
    const D2 df = exp(-var2(z0) * t);
    EXPECT_DOUBLE_EQ(value(df), std::exp(-z0 * t));
    EXPECT_DOUBLE_EQ(first(df), -t * std::exp(-z0 * t));
    EXPECT_DOUBLE_EQ(second(df), t * t * std::exp(-z0 * t));
  }
  {  // fma(a, b, c) = a·b + c: ∂²/∂a∂b = 1, ∂²/∂a² = 0
    EXPECT_DOUBLE_EQ(second(fma(var_outer(1.5), var_inner(2.5), konst(0.5))), 1.0);
    EXPECT_DOUBLE_EQ(second(fma(var2(1.5), konst(2.5), konst(0.5))), 0.0);
    const D2 r = fma(var2(1.5), var2(2.5), konst(0.5));  // a = b = x: (x·x + c)'' = 2
    EXPECT_DOUBLE_EQ(second(r), 2.0);
  }
}

// ---- the second-order Jacobian in one pass: Dual<N, Dual<1>> ------------------------------------

TEST(DualNested, WideOuterGivesAHessianRow) {
  // f(x, y) = exp(x)·y²  ->  f_xx = e^x y², f_xy = 2 e^x y, f_yy = 2 e^x
  const double x0 = 0.3, y0 = 1.7;
  // One pass with TWO outer directions (e_x, e_y) and the inner direction e_x gives the Hessian's
  // x row: r.d[i].d[0] = ∂²f/∂x_i∂x.
  const D22 x(D1(x0, {1.0}), {D1(1.0), D1(0.0)});
  const D22 y(D1(y0, {0.0}), {D1(0.0), D1(1.0)});
  const D22 r = exp(x) * y * y;
  EXPECT_DOUBLE_EQ(r.v.v, std::exp(x0) * y0 * y0);
  EXPECT_DOUBLE_EQ(r.d[0].v, std::exp(x0) * y0 * y0);        // f_x
  EXPECT_DOUBLE_EQ(r.d[1].v, 2.0 * std::exp(x0) * y0);       // f_y
  EXPECT_DOUBLE_EQ(r.d[0].d[0], std::exp(x0) * y0 * y0);     // f_xx
  EXPECT_DOUBLE_EQ(r.d[1].d[0], 2.0 * std::exp(x0) * y0);    // f_yx
}

// ---- the nesting is closed: depth 3 is depth 2's construction again ----------------------------

// I2 says second order is REACHABLE, and the honest test of that is whether depth 2 is a special
// case or the general one. It is the general one: `Dual<1, Dual<1, Dual<1>>>` needs no new code,
// and the innermost-seed-in-the-value convention extends by induction — a constant 1 in each
// tangent channel above the seeded value, and r.d[0].d[0].d[0] is the third derivative.
TEST(DualNested, ThirdOrderNeedsNothingNew) {
  static_assert(std::is_same_v<D3::scalar_type, D2> && D3::nested);
  static_assert(sizeof(D3) == 8 * sizeof(double));
  static_assert(std::is_trivially_copyable_v<D3>);
  static_assert(std::is_convertible_v<double, D3> && !std::is_convertible_v<D3, D2>);

  const double x0 = 1.75;
  const D3 x(D2(D1(x0, {1.0}), {D1(1.0)}), {D2(D1(1.0), {D1(0.0)})});
  auto third = [](const D3& r) { return r.d[0].d[0].d[0]; };
  auto second = [](const D3& r) { return r.d[0].d[0].v; };
  auto first3 = [](const D3& r) { return r.d[0].v.v; };

  {  // every derivative of exp is exp
    const D3 r = exp(x);
    EXPECT_DOUBLE_EQ(r.v.v.v, std::exp(x0));
    EXPECT_DOUBLE_EQ(first3(r), std::exp(x0));
    EXPECT_DOUBLE_EQ(second(r), std::exp(x0));
    EXPECT_DOUBLE_EQ(third(r), std::exp(x0));
  }
  {  // (1/x)''' = −6/x⁴
    const D3 r = recip(x);
    EXPECT_DOUBLE_EQ(second(r), 2.0 / (x0 * x0 * x0));
    EXPECT_DOUBLE_EQ(third(r), -6.0 / (x0 * x0 * x0 * x0));
  }
  {  // (log x)''' = 2/x³
    EXPECT_DOUBLE_EQ(third(log(x)), 2.0 / (x0 * x0 * x0));
  }
  {  // (x³)''' = 6 exactly
    const D3 r = x * x * x;
    EXPECT_DOUBLE_EQ(second(r), 6.0 * x0);
    EXPECT_EQ(third(r), 6.0);
  }
}

// ---- branches and the recording discipline at depth 2 -------------------------------------------

TEST(DualNested, SelectAndLimiter) {
  const D2 a = var2(2.0), b = var2(3.0);
  // select carries the chosen arm whole, at both levels.
  EXPECT_DOUBLE_EQ(second(select(a < b, a * a, b * b)), 2.0);
  EXPECT_DOUBLE_EQ(value(select(a < b, a * a, b * b)), 4.0);
  EXPECT_DOUBLE_EQ(value(select(a > b, a, 9.0)), 9.0);
  EXPECT_DOUBLE_EQ(value(select(a > b, 9.0, a)), 2.0);

  // max / min / abs via select, same value semantics as scalar/select.hpp.
  EXPECT_DOUBLE_EQ(value(max(a, b)), 3.0);
  EXPECT_DOUBLE_EQ(value(min(a, b)), 2.0);
  EXPECT_DOUBLE_EQ(value(abs(-a)), 2.0);
  EXPECT_DOUBLE_EQ(first(abs(-a)), 1.0);
  EXPECT_DOUBLE_EQ(value(max(a, 10.0)), 10.0);
  EXPECT_DOUBLE_EQ(value(min(10.0, a)), 2.0);

  // The shape maths/curve/scheme.hpp's monotone limiter writes: a double/double select assigned
  // straight into a Scalar. Without `Dual(double)` at depth 2 this needs two user-defined
  // conversions and does not compile.
  const D2 sigma = select(a < 0.0, -1.0, 1.0);
  EXPECT_DOUBLE_EQ(value(sigma), 1.0);
  EXPECT_FALSE(sigma.depends());
  // And the shape maths/instrument/coupon.hpp writes: `Scalar acc = <a double field>`.
  const D2 acc = 1.0;
  EXPECT_DOUBLE_EQ(value(acc), 1.0);
}

TEST(DualNested, StructuralIfSeesBothDepths) {
  const D2 k = 3.0;
  EXPECT_TRUE(epykos::structural_if(k > 1.0));     // a constant: no tangent at either depth
  EXPECT_FALSE(epykos::structural_if(k < 1.0));

  EXPECT_THROW((void)epykos::structural_if(var2(3.0) > 1.0), DualError);
  EXPECT_THROW((void)epykos::structural_if(var_outer(3.0) > 1.0), DualError);
  // The one a depth-1 `has_tangent` would miss: seeded in the INNER channel only. The predicate
  // demonstrably depends on an input, so it must still throw.
  EXPECT_THROW((void)epykos::structural_if(var_inner(3.0) > 1.0), DualError);
}

TEST(DualNested, ComparisonsTakeTheValueChannelBitAllTheWayDown) {
  const D2 a = var2(2.0), b = var2(3.0);
  EXPECT_TRUE((a < b).unchecked_value());
  EXPECT_TRUE((a <= b).unchecked_value());
  EXPECT_FALSE((a > b).unchecked_value());
  EXPECT_FALSE((a >= b).unchecked_value());
  EXPECT_FALSE((a == b).unchecked_value());
  EXPECT_TRUE((a == a).unchecked_value());
  EXPECT_TRUE((a < 3.0).unchecked_value());
  EXPECT_TRUE((1.0 < a).unchecked_value());
  EXPECT_TRUE((a < b).active);
  EXPECT_FALSE((konst(2.0) < konst(3.0)).active);

  // A NaN tangent counts as a tangent, at either depth.
  const double nan = std::nan("");
  EXPECT_TRUE(D2(D1(1.0), {D1(nan)}).depends());
  EXPECT_TRUE(D2(D1(1.0, {nan}), {D1(0.0)}).depends());
}
