// EpykosEngine — `Rec`, the recording scalar, and `RecBool` (D14).
//
// Rec = {double v; node_id id}. Arithmetic on Rec computes the value as `double` would and, when a
// Tape::Scope is active on this thread, appends the op to that tape. Constants are leaves: a
// `double` mixed into an expression (or a Rec constructed from a double) becomes a Const node
// the first time it is used in a recorded op. There is no implicit conversion Rec -> double and
// no conversion RecBool -> bool, so `if (a < b)` on Rec does not compile: write
// `select(a < b, x, y)` (both arms recorded) or `structural_if(a < b)` (throws at record time if
// the predicate depends on an input).
//
// Outside a recording scope, Rec values that were never recorded (id == invalid_node) behave as
// plain doubles; using a value that carries a tape node outside its scope throws RecordError.
//
// Node ids held by Rec values are valid until a pass (tape/passes.hpp) rewrites the tape.
#pragma once

#include <cmath>
#include <type_traits>

#include "epykos/tape/tape.hpp"

namespace epykos {

struct Rec;
struct RecBool;

namespace detail {

// The tape to record on, or nullptr. When nullptr, `recorded` (any operand carries a node id)
// is an error: a tape value escaped its scope.
inline Tape* recording_tape(bool recorded) {
  Tape* t = Tape::current();
  if (t == nullptr && recorded) {
    throw RecordError("Rec: a recorded value was used outside of its Tape::Scope");
  }
  return t;
}

}  // namespace detail

struct Rec {
  double v = 0.0;
  node_id id = invalid_node;

  constexpr Rec() noexcept = default;
  constexpr Rec(double value) noexcept : v(value), id(invalid_node) {}  // NOLINT: implicit by design
  constexpr Rec(double value, node_id node) noexcept : v(value), id(node) {}

  // The value. Throws RecordError in record mode if the node depends on an input: the maths
  // must not look at active values (use select / structural_if).
  double value() const {
    if (Tape* t = Tape::current(); t != nullptr && id != invalid_node && t->tainted(id)) {
      throw RecordError("Rec::value() on a tainted value in record mode");
    }
    return v;
  }
  // The value with no check. Debugging and tests only.
  constexpr double unchecked_value() const noexcept { return v; }
  constexpr node_id node() const noexcept { return id; }
  constexpr bool recorded() const noexcept { return id != invalid_node; }

  // The node on `t`, materialising a detached constant as a Const leaf.
  node_id node_on(Tape& t) const { return id != invalid_node ? id : t.constant(v); }

  Rec& operator+=(const Rec& o);
  Rec& operator-=(const Rec& o);
  Rec& operator*=(const Rec& o);
  Rec& operator/=(const Rec& o);
  Rec& operator+=(double o);
  Rec& operator-=(double o);
  Rec& operator*=(double o);
  Rec& operator/=(double o);
};

static_assert(!std::is_convertible_v<Rec, double>, "no implicit Rec -> double");

// A recorded comparison. Not convertible to bool: use select or structural_if.
struct RecBool {
  bool v = false;
  node_id id = invalid_node;

  constexpr RecBool() noexcept = default;
  constexpr RecBool(bool value, node_id node) noexcept : v(value), id(node) {}

  constexpr node_id node() const noexcept { return id; }
  constexpr bool recorded() const noexcept { return id != invalid_node; }
  // The bit with no check. Debugging and tests only.
  constexpr bool unchecked_value() const noexcept { return v; }

  node_id node_on(Tape& t) const { return id != invalid_node ? id : t.constant(v ? 1.0 : 0.0); }
};

static_assert(!std::is_convertible_v<RecBool, bool>, "no implicit RecBool -> bool");
static_assert(!std::is_constructible_v<bool, RecBool>, "no explicit RecBool -> bool either");

// ---- inputs and outputs -----------------------------------------------------------------

// A new Input node on `t` (ordinal = t.num_inputs() before the call) holding `value`.
inline Rec make_input(Tape& t, double value) { return Rec(value, t.input(value)); }
// Same on the current tape; throws RecordError when no scope is active.
inline Rec make_input(double value) {
  Tape* t = Tape::current();
  if (t == nullptr) throw RecordError("make_input: no Tape::Scope is active");
  return make_input(*t, value);
}
// Registers x as the next output of `t` (a detached constant is materialised); returns ordinal.
inline int register_output(Tape& t, const Rec& x) { return t.output(x.node_on(t)); }
inline int register_output(const Rec& x) {
  Tape* t = Tape::current();
  if (t == nullptr) throw RecordError("register_output: no Tape::Scope is active");
  return register_output(*t, x);
}

// ---- arithmetic --------------------------------------------------------------------------

namespace detail {

inline Rec rec_unary(Op op, const Rec& a, double v) {
  Tape* t = recording_tape(a.recorded());
  if (t == nullptr) return Rec(v);
  return Rec(v, t->unary(op, a.node_on(*t)));
}

inline Rec rec_binary(Op op, const Rec& a, const Rec& b, double v) {
  Tape* t = recording_tape(a.recorded() || b.recorded());
  if (t == nullptr) return Rec(v);
  const node_id na = a.node_on(*t);
  const node_id nb = b.node_on(*t);
  return Rec(v, t->binary(op, na, nb));
}

inline RecBool rec_compare(Op op, const Rec& a, const Rec& b, bool v) {
  Tape* t = recording_tape(a.recorded() || b.recorded());
  if (t == nullptr) return RecBool(v, invalid_node);
  const node_id na = a.node_on(*t);
  const node_id nb = b.node_on(*t);
  return RecBool(v, t->binary(op, na, nb));
}

}  // namespace detail

inline Rec operator+(const Rec& a, const Rec& b) { return detail::rec_binary(Op::Add, a, b, a.v + b.v); }
inline Rec operator-(const Rec& a, const Rec& b) { return detail::rec_binary(Op::Sub, a, b, a.v - b.v); }
inline Rec operator*(const Rec& a, const Rec& b) { return detail::rec_binary(Op::Mul, a, b, a.v * b.v); }
inline Rec operator/(const Rec& a, const Rec& b) { return detail::rec_binary(Op::Div, a, b, a.v / b.v); }

inline Rec operator+(const Rec& a, double b) { return a + Rec(b); }
inline Rec operator-(const Rec& a, double b) { return a - Rec(b); }
inline Rec operator*(const Rec& a, double b) { return a * Rec(b); }
inline Rec operator/(const Rec& a, double b) { return a / Rec(b); }
inline Rec operator+(double a, const Rec& b) { return Rec(a) + b; }
inline Rec operator-(double a, const Rec& b) { return Rec(a) - b; }
inline Rec operator*(double a, const Rec& b) { return Rec(a) * b; }
inline Rec operator/(double a, const Rec& b) { return Rec(a) / b; }

inline Rec operator-(const Rec& a) { return detail::rec_unary(Op::Neg, a, -a.v); }
inline Rec operator+(const Rec& a) { return a; }

inline Rec& Rec::operator+=(const Rec& o) { return *this = *this + o; }
inline Rec& Rec::operator-=(const Rec& o) { return *this = *this - o; }
inline Rec& Rec::operator*=(const Rec& o) { return *this = *this * o; }
inline Rec& Rec::operator/=(const Rec& o) { return *this = *this / o; }
inline Rec& Rec::operator+=(double o) { return *this = *this + o; }
inline Rec& Rec::operator-=(double o) { return *this = *this - o; }
inline Rec& Rec::operator*=(double o) { return *this = *this * o; }
inline Rec& Rec::operator/=(double o) { return *this = *this / o; }

inline Rec exp(const Rec& a) { return detail::rec_unary(Op::Exp, a, std::exp(a.v)); }
inline Rec log(const Rec& a) { return detail::rec_unary(Op::Log, a, std::log(a.v)); }
inline Rec sqrt(const Rec& a) { return detail::rec_unary(Op::Sqrt, a, std::sqrt(a.v)); }
inline Rec recip(const Rec& a) { return detail::rec_unary(Op::Recip, a, 1.0 / a.v); }

// fma(a, b, c) = a*b + c with a single rounding, recorded as one Fma node.
inline Rec fma(const Rec& a, const Rec& b, const Rec& c) {
  Tape* t = detail::recording_tape(a.recorded() || b.recorded() || c.recorded());
  const double v = std::fma(a.v, b.v, c.v);
  if (t == nullptr) return Rec(v);
  const node_id na = a.node_on(*t);
  const node_id nb = b.node_on(*t);
  const node_id nc = c.node_on(*t);
  return Rec(v, t->ternary(Op::Fma, na, nb, nc));
}

// ---- comparisons -> RecBool ---------------------------------------------------------------

inline RecBool operator<(const Rec& a, const Rec& b) { return detail::rec_compare(Op::CmpLt, a, b, a.v < b.v); }
inline RecBool operator<=(const Rec& a, const Rec& b) { return detail::rec_compare(Op::CmpLe, a, b, a.v <= b.v); }
inline RecBool operator>(const Rec& a, const Rec& b) { return detail::rec_compare(Op::CmpGt, a, b, a.v > b.v); }
inline RecBool operator>=(const Rec& a, const Rec& b) { return detail::rec_compare(Op::CmpGe, a, b, a.v >= b.v); }
inline RecBool operator==(const Rec& a, const Rec& b) { return detail::rec_compare(Op::CmpEq, a, b, a.v == b.v); }

inline RecBool operator<(const Rec& a, double b) { return a < Rec(b); }
inline RecBool operator<=(const Rec& a, double b) { return a <= Rec(b); }
inline RecBool operator>(const Rec& a, double b) { return a > Rec(b); }
inline RecBool operator>=(const Rec& a, double b) { return a >= Rec(b); }
inline RecBool operator==(const Rec& a, double b) { return a == Rec(b); }
inline RecBool operator<(double a, const Rec& b) { return Rec(a) < b; }
inline RecBool operator<=(double a, const Rec& b) { return Rec(a) <= b; }
inline RecBool operator>(double a, const Rec& b) { return Rec(a) > b; }
inline RecBool operator>=(double a, const Rec& b) { return Rec(a) >= b; }
inline RecBool operator==(double a, const Rec& b) { return Rec(a) == b; }

// ---- branches ------------------------------------------------------------------------------

// c ? a : b as a Select node. Both arms are already recorded values (D5).
inline Rec select(const RecBool& c, const Rec& a, const Rec& b) {
  Tape* t = detail::recording_tape(c.recorded() || a.recorded() || b.recorded());
  const double v = c.v ? a.v : b.v;
  if (t == nullptr) return Rec(v);
  const node_id nc = c.node_on(*t);
  const node_id na = a.node_on(*t);
  const node_id nb = b.node_on(*t);
  return Rec(v, t->ternary(Op::Select, nc, na, nb));
}
inline Rec select(const RecBool& c, const Rec& a, double b) { return select(c, a, Rec(b)); }
inline Rec select(const RecBool& c, double a, const Rec& b) { return select(c, Rec(a), b); }
inline Rec select(const RecBool& c, double a, double b) { return select(c, Rec(a), Rec(b)); }

// A structural branch: returns the bit, but throws RecordError in record mode if the predicate
// depends on an input (that branch would be folded into the recording).
inline bool structural_if(const RecBool& c) {
  if (Tape* t = Tape::current(); t != nullptr && c.id != invalid_node && t->tainted(c.id)) {
    throw RecordError("structural_if on a predicate that depends on an input");
  }
  return c.v;
}

// max/min/abs via select (both arms recorded). Value semantics match scalar/select.hpp:
// max(a,b) = a < b ? b : a; min(a,b) = b < a ? b : a; abs(a) = a < 0 ? -a : a.
inline Rec max(const Rec& a, const Rec& b) { return select(a < b, b, a); }
inline Rec min(const Rec& a, const Rec& b) { return select(b < a, b, a); }
inline Rec abs(const Rec& a) { return select(a < 0.0, -a, a); }
inline Rec max(const Rec& a, double b) { return max(a, Rec(b)); }
inline Rec max(double a, const Rec& b) { return max(Rec(a), b); }
inline Rec min(const Rec& a, double b) { return min(a, Rec(b)); }
inline Rec min(double a, const Rec& b) { return min(Rec(a), b); }

}  // namespace epykos
