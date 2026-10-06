// EpykosEngine — `Dual<N, S>`, the forward-mode scalar (M2/Q2; docs/WORKLOADS.md §M2 "Forward
// mode"), NESTING (PRINCIPLES.md §1b, invariant I2).
//
// Dual<N, S> = {S v; std::array<S, N> d}: a value and N tangents (directional derivatives, one per
// seeded direction) over the scalar S. The templated maths instantiated on Dual<N, S> computes, in
// one pass, its value and its Jacobian-times-direction for N directions at once; N = 1 is a single
// directional derivative, N = n_inputs is the full Jacobian.
//
// `Dual<N>` is `Dual<N, double>` — the first-order scalar, unchanged in every respect. The
// parameter order is `<N, S>` rather than `<S, N>` precisely so that it is: every existing call
// site passes one argument and compiles untouched. (PRINCIPLES.md §1b writes the gate
// `Dual<Dual<1>, N>`; the substance is the nesting, not the order of the parameters.)
//
// **Second order.** S may itself be a Dual, so `Dual<N, Dual<M>>` carries N outer directions of M
// inner directions. Seed a variable x_k as  value = Dual<M>(x_k, w_k),  tangent[i] = Dual<M>(u_ki)
// — the inner direction rides in the VALUE channel and the outer direction in the tangent channel.
// Then for f recorded on Scalar = Dual<N, Dual<M>>:
//   r.v.v        = f(x)
//   r.v.d[j]     = Σ_k ∂f/∂x_k · w_kj
//   r.d[i].v     = Σ_k ∂f/∂x_k · u_ki
//   r.d[i].d[j]  = Σ_{k,l} ∂²f/∂x_k∂x_l · u_ki · w_lj
// Nothing in this header knows about second derivatives: they are the first-order rules applied
// twice, which is the whole point — no hand-written second derivative exists anywhere (CLAUDE.md).
//
// Contract (the same operator surface as Rec, so price_book<Dual<N>> compiles unchanged):
//   * + − * / between Duals, unary minus, mixed with double on either side (the double is a
//     constant: zero tangent, at EVERY depth), compound assignments, exp / log / sqrt / recip / fma;
//   * the VALUE channel is computed exactly as S would, operation for operation, so under
//     -ffp-contract=off the value of price_book<Dual<N>> is bitwise price_book<double>
//     (tests/scalar/dual_m1_e0_test.cpp); the tangent channel applies the standard rules;
//   * every tangent slot is computed independently by the same sequence of S operations, so
//     under -ffp-contract=off Dual<N> slot i is bitwise the Dual<1> pass seeded with direction i;
//   * comparisons return DualBool, which does not convert to bool: `if (a < b)` on a Dual does not
//     compile, exactly as on Rec — the maths writes select(c, a, b) (both arms computed, the chosen
//     arm's tangent returned) or structural_if(c). The choice of a restricted type over plain bool
//     keeps the recording discipline (CLAUDE.md) identical on every Scalar the maths is instantiated
//     on: a template developed and tested on Dual first cannot slip a value branch past the compiler
//     and fail only when recorded. It costs nothing (a bool in a struct). DualBool is ONE type at
//     every depth, so a nested comparison feeds the same select();
//   * DualBool also carries `active`: at least one operand demonstrably depends on the
//     differentiated inputs — a nonzero tangent at this level or, when nested, at any level
//     inside it. structural_if throws DualError on an active predicate. This is a strictly weaker
//     check than Rec's taint (a zero tangent proves nothing: the direction may be orthogonal), so
//     it never throws where Rec would not; Rec remains the authoritative discipline check;
//   * max / min / abs via select, with the value semantics of scalar/select.hpp and scalar/rec.hpp
//     (abs(−0.0) is −0.0);
//   * no implicit conversion Dual → double, and none Dual<N, Dual<M>> → Dual<M> either (value()
//     and d[] are explicit reads). A double converts TO a Dual at any depth: it is a constant.
//
// No tape, no allocation, no dependency on the recorder. Not an Eigen scalar (NumTraits is M4 work).
#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <type_traits>

namespace epykos {

// Thrown by structural_if(DualBool) when the predicate demonstrably depends on an input.
class DualError : public std::logic_error {
 public:
  using std::logic_error::logic_error;
};

// A comparison of Duals. Not convertible to bool: use select or structural_if.
struct DualBool {
  bool v = false;       // the bit
  bool active = false;  // an operand carried a nonzero tangent: the predicate depends on the inputs

  constexpr DualBool() noexcept = default;
  constexpr DualBool(bool value, bool depends) noexcept : v(value), active(depends) {}

  // The bit with no check. Debugging and tests only.
  constexpr bool unchecked_value() const noexcept { return v; }
};

static_assert(!std::is_convertible_v<DualBool, bool>, "no implicit DualBool -> bool");
static_assert(!std::is_constructible_v<bool, DualBool>, "no explicit DualBool -> bool either");

namespace detail {
// The value-channel bit of a comparison of the underlying scalar: a plain bool when that scalar is
// double, the DualBool's own bit when it is itself a Dual. This is the one place the comparison
// machinery has to know that the value channel may not be a double.
constexpr bool dual_bit(bool b) noexcept { return b; }
constexpr bool dual_bit(const DualBool& b) noexcept { return b.unchecked_value(); }
}  // namespace detail

template <int N = 1, class S = double>
class Dual {
  static_assert(N >= 1, "Dual<N, S>: at least one tangent slot");

 public:
  using scalar_type = S;
  using tangent_type = std::array<S, N>;
  static constexpr int n_tangents = N;
  // S is itself a Dual: this instantiation reaches second order (or higher).
  static constexpr bool nested = !std::is_same_v<S, double>;

 private:
  static constexpr std::size_t n_ = static_cast<std::size_t>(N);

  // Is one slot (of type S) NOT identically zero? A NaN counts as nonzero, so the test is
  // `!(x == 0.0)` and not `x != 0.0`. At depth 1 this is exactly the `d[i] == 0.0` it replaces.
  static constexpr bool slot_nonzero(const S& s) noexcept {
    if constexpr (nested) {
      return !S::all_zero(s);
    } else {
      return !(s == 0.0);
    }
  }

 public:
  // Identically zero in the value and in every tangent, at every depth. The level above uses it
  // to decide whether one of ITS slots is zero.
  static constexpr bool all_zero(const Dual& x) noexcept {
    if (slot_nonzero(x.v)) return false;
    for (std::size_t i = 0; i < n_; ++i) {
      if (slot_nonzero(x.d[i])) return false;
    }
    return true;
  }

  S v{};
  tangent_type d{};  // d[i] = derivative of v along direction i

  constexpr Dual() noexcept = default;
  constexpr Dual(const S& value) noexcept : v(value) {}  // NOLINT: implicit by design (a constant)
  constexpr Dual(const S& value, const tangent_type& dv) noexcept : v(value), d(dv) {}
  // A plain double is a constant at EVERY depth. Without these, `Scalar x = 2.0` on a nested Dual
  // would need two user-defined conversions (double -> Dual<M> -> Dual<N, Dual<M>>) and would not
  // compile — and the maths writes exactly that (scheme.hpp's monotone limiter, coupon.hpp's
  // realised factor). Absent at depth 1, where `Dual(const S&)` already IS `Dual(double)`.
  constexpr Dual(double value) noexcept
    requires(nested)
      : v(S(value)) {}
  constexpr Dual(double value, const tangent_type& dv) noexcept
    requires(nested)
      : v(S(value)), d(dv) {}

  // A variable: value `value`, tangent `di` in slot i (a unit vector by default), zero elsewhere.
  static constexpr Dual variable(const S& value, int i, const S& di = S(1.0)) noexcept {
    Dual x(value);
    x.d[static_cast<std::size_t>(i)] = di;
    return x;
  }

  constexpr S value() const noexcept { return v; }
  constexpr S tangent(int i) const noexcept { return d[static_cast<std::size_t>(i)]; }
  // Any tangent slot nonzero (a NaN counts as nonzero).
  constexpr bool has_tangent() const noexcept {
    for (std::size_t i = 0; i < n_; ++i) {
      if (slot_nonzero(d[i])) return true;
    }
    return false;
  }
  // Demonstrable dependence on a differentiated input: a tangent at this level or, when nested,
  // one carried inside the value channel (where the inner seed rides). Identical to has_tangent()
  // at depth 1; this is what a comparison's `active` flag reports.
  constexpr bool depends() const noexcept {
    if (has_tangent()) return true;
    if constexpr (nested) {
      if (v.depends()) return true;
    }
    return false;
  }

  // ---- arithmetic (hidden friends: found by ADL, implicit Dual(double) applies) ----------------

  friend Dual operator+(const Dual& a, const Dual& b) noexcept {
    Dual r(a.v + b.v);
    for (std::size_t i = 0; i < n_; ++i) r.d[i] = a.d[i] + b.d[i];
    return r;
  }
  friend Dual operator-(const Dual& a, const Dual& b) noexcept {
    Dual r(a.v - b.v);
    for (std::size_t i = 0; i < n_; ++i) r.d[i] = a.d[i] - b.d[i];
    return r;
  }
  friend Dual operator*(const Dual& a, const Dual& b) noexcept {
    Dual r(a.v * b.v);
    for (std::size_t i = 0; i < n_; ++i) r.d[i] = a.d[i] * b.v + a.v * b.d[i];
    return r;
  }
  // d(a/b) = (a' − (a/b)·b') / b, using the rounded quotient.
  friend Dual operator/(const Dual& a, const Dual& b) noexcept {
    Dual r(a.v / b.v);
    for (std::size_t i = 0; i < n_; ++i) r.d[i] = (a.d[i] - r.v * b.d[i]) / b.v;
    return r;
  }

  friend Dual operator+(const Dual& a, double b) noexcept { return Dual(a.v + b, a.d); }
  friend Dual operator-(const Dual& a, double b) noexcept { return Dual(a.v - b, a.d); }
  friend Dual operator*(const Dual& a, double b) noexcept {
    Dual r(a.v * b);
    for (std::size_t i = 0; i < n_; ++i) r.d[i] = a.d[i] * b;
    return r;
  }
  friend Dual operator/(const Dual& a, double b) noexcept {
    Dual r(a.v / b);
    for (std::size_t i = 0; i < n_; ++i) r.d[i] = a.d[i] / b;
    return r;
  }
  friend Dual operator+(double a, const Dual& b) noexcept { return Dual(a + b.v, b.d); }
  friend Dual operator-(double a, const Dual& b) noexcept {
    Dual r(a - b.v);
    for (std::size_t i = 0; i < n_; ++i) r.d[i] = -b.d[i];
    return r;
  }
  friend Dual operator*(double a, const Dual& b) noexcept {
    Dual r(a * b.v);
    for (std::size_t i = 0; i < n_; ++i) r.d[i] = a * b.d[i];
    return r;
  }
  // d(a/b) = −(a/b)·b' / b, using the rounded quotient.
  friend Dual operator/(double a, const Dual& b) noexcept {
    Dual r(a / b.v);
    for (std::size_t i = 0; i < n_; ++i) r.d[i] = -(r.v * b.d[i]) / b.v;
    return r;
  }

  friend Dual operator-(const Dual& a) noexcept {
    Dual r(-a.v);
    for (std::size_t i = 0; i < n_; ++i) r.d[i] = -a.d[i];
    return r;
  }
  friend Dual operator+(const Dual& a) noexcept { return a; }

  Dual& operator+=(const Dual& o) noexcept { return *this = *this + o; }
  Dual& operator-=(const Dual& o) noexcept { return *this = *this - o; }
  Dual& operator*=(const Dual& o) noexcept { return *this = *this * o; }
  Dual& operator/=(const Dual& o) noexcept { return *this = *this / o; }
  Dual& operator+=(double o) noexcept { return *this = *this + o; }
  Dual& operator-=(double o) noexcept { return *this = *this - o; }
  Dual& operator*=(double o) noexcept { return *this = *this * o; }
  Dual& operator/=(double o) noexcept { return *this = *this / o; }

  // ---- transcendental -----------------------------------------------------------------------
  //
  // The value channel makes the SAME call S itself would: `using std::exp;` plus an unqualified
  // call is std::exp(double) at depth 1 — a using-declaration does not suppress ADL
  // ([basic.lookup.argdep]) — and the inner Dual's hidden friend when nested. The recursion
  // terminates at double, which has no associated namespace.

  friend Dual exp(const Dual& a) noexcept {
    using std::exp;
    Dual r(exp(a.v));
    for (std::size_t i = 0; i < n_; ++i) r.d[i] = r.v * a.d[i];
    return r;
  }
  friend Dual log(const Dual& a) noexcept {
    using std::log;
    Dual r(log(a.v));
    for (std::size_t i = 0; i < n_; ++i) r.d[i] = a.d[i] / a.v;
    return r;
  }
  friend Dual sqrt(const Dual& a) noexcept {
    using std::sqrt;
    Dual r(sqrt(a.v));
    for (std::size_t i = 0; i < n_; ++i) r.d[i] = a.d[i] / (2.0 * r.v);
    return r;
  }
  friend Dual recip(const Dual& a) noexcept {
    Dual r(1.0 / a.v);
    for (std::size_t i = 0; i < n_; ++i) r.d[i] = -(r.v * r.v) * a.d[i];
    return r;
  }
  // fma(a, b, c) = a·b + c with a single rounding in the value channel; the tangent is the
  // product rule plus c'.
  friend Dual fma(const Dual& a, const Dual& b, const Dual& c) noexcept {
    using std::fma;
    Dual r(fma(a.v, b.v, c.v));
    for (std::size_t i = 0; i < n_; ++i) r.d[i] = a.d[i] * b.v + a.v * b.d[i] + c.d[i];
    return r;
  }

  // ---- comparisons -> DualBool --------------------------------------------------------------
  //
  // The bit is the value channel's own comparison, all the way down (detail::dual_bit); `active`
  // is demonstrable dependence at any depth (depends()).

  friend DualBool operator<(const Dual& a, const Dual& b) noexcept {
    return DualBool(detail::dual_bit(a.v < b.v), a.depends() || b.depends());
  }
  friend DualBool operator<=(const Dual& a, const Dual& b) noexcept {
    return DualBool(detail::dual_bit(a.v <= b.v), a.depends() || b.depends());
  }
  friend DualBool operator>(const Dual& a, const Dual& b) noexcept {
    return DualBool(detail::dual_bit(a.v > b.v), a.depends() || b.depends());
  }
  friend DualBool operator>=(const Dual& a, const Dual& b) noexcept {
    return DualBool(detail::dual_bit(a.v >= b.v), a.depends() || b.depends());
  }
  friend DualBool operator==(const Dual& a, const Dual& b) noexcept {
    return DualBool(detail::dual_bit(a.v == b.v), a.depends() || b.depends());
  }
  friend DualBool operator<(const Dual& a, double b) noexcept { return DualBool(detail::dual_bit(a.v < b), a.depends()); }
  friend DualBool operator<=(const Dual& a, double b) noexcept { return DualBool(detail::dual_bit(a.v <= b), a.depends()); }
  friend DualBool operator>(const Dual& a, double b) noexcept { return DualBool(detail::dual_bit(a.v > b), a.depends()); }
  friend DualBool operator>=(const Dual& a, double b) noexcept { return DualBool(detail::dual_bit(a.v >= b), a.depends()); }
  friend DualBool operator==(const Dual& a, double b) noexcept { return DualBool(detail::dual_bit(a.v == b), a.depends()); }
  friend DualBool operator<(double a, const Dual& b) noexcept { return DualBool(detail::dual_bit(a < b.v), b.depends()); }
  friend DualBool operator<=(double a, const Dual& b) noexcept { return DualBool(detail::dual_bit(a <= b.v), b.depends()); }
  friend DualBool operator>(double a, const Dual& b) noexcept { return DualBool(detail::dual_bit(a > b.v), b.depends()); }
  friend DualBool operator>=(double a, const Dual& b) noexcept { return DualBool(detail::dual_bit(a >= b.v), b.depends()); }
  friend DualBool operator==(double a, const Dual& b) noexcept { return DualBool(detail::dual_bit(a == b.v), b.depends()); }

  // ---- branches -----------------------------------------------------------------------------

  // c ? a : b, both arms already computed (D5); the chosen arm's value and tangent.
  friend Dual select(const DualBool& c, const Dual& a, const Dual& b) noexcept { return c.v ? a : b; }
  friend Dual select(const DualBool& c, const Dual& a, double b) noexcept { return c.v ? a : Dual(b); }
  friend Dual select(const DualBool& c, double a, const Dual& b) noexcept { return c.v ? Dual(a) : b; }

  // max/min/abs via select. Value semantics match scalar/select.hpp and scalar/rec.hpp:
  // max(a,b) = a < b ? b : a; min(a,b) = b < a ? b : a; abs(a) = a < 0 ? -a : a.
  friend Dual max(const Dual& a, const Dual& b) noexcept { return select(a < b, b, a); }
  friend Dual min(const Dual& a, const Dual& b) noexcept { return select(b < a, b, a); }
  friend Dual abs(const Dual& a) noexcept { return select(a < 0.0, -a, a); }
  friend Dual max(const Dual& a, double b) noexcept { return max(a, Dual(b)); }
  friend Dual max(double a, const Dual& b) noexcept { return max(Dual(a), b); }
  friend Dual min(const Dual& a, double b) noexcept { return min(a, Dual(b)); }
  friend Dual min(double a, const Dual& b) noexcept { return min(Dual(a), b); }
};

static_assert(!std::is_convertible_v<Dual<1>, double>, "no implicit Dual -> double");
static_assert(std::is_trivially_copyable_v<Dual<1>>);
// I2's nesting, asserted where it is defined rather than only where it is used.
static_assert(std::is_same_v<Dual<3>, Dual<3, double>>, "Dual<N> is Dual<N, double>");
static_assert(!Dual<1>::nested && Dual<1, Dual<1>>::nested);
static_assert(!std::is_convertible_v<Dual<1, Dual<1>>, double>, "no implicit Dual -> double at depth 2");
static_assert(!std::is_convertible_v<Dual<1, Dual<1>>, Dual<1>>, "no implicit outer Dual -> inner Dual");
static_assert(std::is_convertible_v<double, Dual<1, Dual<1>>>, "a double is a constant at every depth");
static_assert(std::is_convertible_v<Dual<1>, Dual<1, Dual<1>>>, "an inner value is a constant of the outer level");
static_assert(std::is_trivially_copyable_v<Dual<1, Dual<1>>>);
static_assert(sizeof(Dual<1, Dual<1>>) == 4 * sizeof(double), "{{v, d[1]}, {v, d[1]}}");

// A double arm pair under a Dual predicate: a constant either way (returns double, which converts
// to Dual<N, S> implicitly where a Scalar is expected — at any depth).
inline double select(const DualBool& c, double a, double b) noexcept { return c.v ? a : b; }

// A structural branch: returns the bit, but throws DualError if the predicate demonstrably
// depends on the differentiated inputs (an operand carried a nonzero tangent). Rec's taint check
// is the authoritative one; this catches what a Dual pass can see.
inline bool structural_if(const DualBool& c) {
  if (c.active) throw DualError("structural_if on a predicate that depends on an input (nonzero tangent)");
  return c.v;
}

}  // namespace epykos
