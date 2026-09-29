// EpykosEngine — `Dual<N>`, the forward-mode scalar (M2/Q2; docs/WORKLOADS.md §M2 "Forward mode").
//
// Dual<N> = {double v; std::array<double, N> d}: a value and N tangents (directional derivatives,
// one per seeded direction). The templated maths instantiated on Dual<N> computes, in one pass,
// its value and its Jacobian-times-direction for N directions at once; N = 1 is a single
// directional derivative, N = n_inputs is the full Jacobian.
//
// Contract (the same operator surface as Rec, so price_book<Dual<N>> compiles unchanged):
//   * + − * / between Duals, unary minus, mixed with double on either side (the double is a
//     constant: zero tangent), compound assignments, exp / log / sqrt / recip / fma;
//   * the VALUE channel is computed exactly as `double` would, operation for operation, so under
//     -ffp-contract=off the value of price_book<Dual<N>> is bitwise price_book<double>
//     (tests/scalar/dual_m1_e0_test.cpp); the tangent channel applies the standard rules;
//   * every tangent slot is computed independently by the same sequence of double operations, so
//     under -ffp-contract=off Dual<N> slot i is bitwise the Dual<1> pass seeded with direction i;
//   * comparisons return DualBool, which does not convert to bool: `if (a < b)` on a Dual does not
//     compile, exactly as on Rec — the maths writes select(c, a, b) (both arms computed, the chosen
//     arm's tangent returned) or structural_if(c). The choice of a restricted type over plain bool
//     keeps the recording discipline (CLAUDE.md) identical on every Scalar the maths is instantiated
//     on: a template developed and tested on Dual first cannot slip a value branch past the compiler
//     and fail only when recorded. It costs nothing (a bool in a struct);
//   * DualBool also carries `active`: at least one operand had a nonzero tangent, so the predicate
//     demonstrably depends on the differentiated inputs. structural_if throws DualError on an active
//     predicate. This is a strictly weaker check than Rec's taint (a zero tangent proves nothing:
//     the direction may be orthogonal), so it never throws where Rec would not; Rec remains the
//     authoritative discipline check;
//   * max / min / abs via select, with the value semantics of scalar/select.hpp and scalar/rec.hpp
//     (abs(−0.0) is −0.0);
//   * no implicit conversion Dual → double (value() and d[] are explicit reads).
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

template <int N = 1>
class Dual {
  static_assert(N >= 1, "Dual<N>: at least one tangent slot");

 public:
  using tangent_type = std::array<double, N>;
  static constexpr int n_tangents = N;

 private:
  static constexpr std::size_t n_ = static_cast<std::size_t>(N);

 public:

  double v = 0.0;
  tangent_type d{};  // d[i] = derivative of v along direction i

  constexpr Dual() noexcept = default;
  constexpr Dual(double value) noexcept : v(value) {}  // NOLINT: implicit by design (a constant)
  constexpr Dual(double value, const tangent_type& dv) noexcept : v(value), d(dv) {}

  // A variable: value `value`, tangent `di` in slot i (a unit vector by default), zero elsewhere.
  static constexpr Dual variable(double value, int i, double di = 1.0) noexcept {
    Dual x(value);
    x.d[static_cast<std::size_t>(i)] = di;
    return x;
  }

  constexpr double value() const noexcept { return v; }
  constexpr double tangent(int i) const noexcept { return d[static_cast<std::size_t>(i)]; }
  // Any tangent slot nonzero (a NaN counts as nonzero).
  constexpr bool has_tangent() const noexcept {
    for (std::size_t i = 0; i < n_; ++i) {
      if (!(d[i] == 0.0)) return true;
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

  friend Dual exp(const Dual& a) noexcept {
    Dual r(std::exp(a.v));
    for (std::size_t i = 0; i < n_; ++i) r.d[i] = r.v * a.d[i];
    return r;
  }
  friend Dual log(const Dual& a) noexcept {
    Dual r(std::log(a.v));
    for (std::size_t i = 0; i < n_; ++i) r.d[i] = a.d[i] / a.v;
    return r;
  }
  friend Dual sqrt(const Dual& a) noexcept {
    Dual r(std::sqrt(a.v));
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
    Dual r(std::fma(a.v, b.v, c.v));
    for (std::size_t i = 0; i < n_; ++i) r.d[i] = a.d[i] * b.v + a.v * b.d[i] + c.d[i];
    return r;
  }

  // ---- comparisons -> DualBool --------------------------------------------------------------

  friend DualBool operator<(const Dual& a, const Dual& b) noexcept {
    return DualBool(a.v < b.v, a.has_tangent() || b.has_tangent());
  }
  friend DualBool operator<=(const Dual& a, const Dual& b) noexcept {
    return DualBool(a.v <= b.v, a.has_tangent() || b.has_tangent());
  }
  friend DualBool operator>(const Dual& a, const Dual& b) noexcept {
    return DualBool(a.v > b.v, a.has_tangent() || b.has_tangent());
  }
  friend DualBool operator>=(const Dual& a, const Dual& b) noexcept {
    return DualBool(a.v >= b.v, a.has_tangent() || b.has_tangent());
  }
  friend DualBool operator==(const Dual& a, const Dual& b) noexcept {
    return DualBool(a.v == b.v, a.has_tangent() || b.has_tangent());
  }
  friend DualBool operator<(const Dual& a, double b) noexcept { return DualBool(a.v < b, a.has_tangent()); }
  friend DualBool operator<=(const Dual& a, double b) noexcept { return DualBool(a.v <= b, a.has_tangent()); }
  friend DualBool operator>(const Dual& a, double b) noexcept { return DualBool(a.v > b, a.has_tangent()); }
  friend DualBool operator>=(const Dual& a, double b) noexcept { return DualBool(a.v >= b, a.has_tangent()); }
  friend DualBool operator==(const Dual& a, double b) noexcept { return DualBool(a.v == b, a.has_tangent()); }
  friend DualBool operator<(double a, const Dual& b) noexcept { return DualBool(a < b.v, b.has_tangent()); }
  friend DualBool operator<=(double a, const Dual& b) noexcept { return DualBool(a <= b.v, b.has_tangent()); }
  friend DualBool operator>(double a, const Dual& b) noexcept { return DualBool(a > b.v, b.has_tangent()); }
  friend DualBool operator>=(double a, const Dual& b) noexcept { return DualBool(a >= b.v, b.has_tangent()); }
  friend DualBool operator==(double a, const Dual& b) noexcept { return DualBool(a == b.v, b.has_tangent()); }

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

// A double arm pair under a Dual predicate: a constant either way (returns double, which converts
// to Dual<N> implicitly where a Scalar is expected).
inline double select(const DualBool& c, double a, double b) noexcept { return c.v ? a : b; }

// A structural branch: returns the bit, but throws DualError if the predicate demonstrably
// depends on the differentiated inputs (an operand carried a nonzero tangent). Rec's taint check
// is the authoritative one; this catches what a Dual pass can see.
inline bool structural_if(const DualBool& c) {
  if (c.active) throw DualError("structural_if on a predicate that depends on an input (nonzero tangent)");
  return c.v;
}

}  // namespace epykos
