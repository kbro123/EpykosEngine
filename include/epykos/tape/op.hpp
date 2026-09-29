// EpykosEngine — the opcode enum shared by every pass (D14).
//
// One enum for the recorder, CSE/DCE, fold-sum, affine collapse, the signature pass, the expander,
// the interpreters, the adjoint and the catalogue. Values are appended, never renumbered, so that
// serialised tapes and IR stay readable. Ops after `Affine` are reserved for later milestones: the
// recorder and the M1 passes do not produce them and the replay interpreter rejects them.
#pragma once

#include <cstdint>

namespace epykos {

enum class Op : std::uint8_t {
  // Leaves.
  Const = 0,  // konst = the value (deduplicated by bit pattern)
  Input = 1,  // a = input ordinal; konst = value at record time

  // Elementwise arithmetic.
  Add = 2,    // a + b
  Sub = 3,    // a - b
  Mul = 4,    // a * b
  Div = 5,    // a / b
  Neg = 6,    // -a
  Exp = 7,    // exp(a)
  Log = 8,    // log(a)
  Sqrt = 9,   // sqrt(a)
  Recip = 10, // 1 / a
  Fma = 11,   // fma(a, b, c) = a*b + c with one rounding

  // Value branch: c ? a : b where c is a comparison node. Both arms are recorded (D5).
  Select = 12,  // a = predicate, b = if-true arm, c = if-false arm

  // Comparisons: boolean-valued nodes (1.0 / 0.0 under replay). Never converted to bool.
  CmpLt = 13,  // a <  b
  CmpLe = 14,  // a <= b
  CmpGt = 15,  // a >  b
  CmpGe = 16,  // a >= b
  CmpEq = 17,  // a == b

  // Variadic (operands in the tape's side array, [first_arg, first_arg + nargs)).
  Sum = 18,     // ((x_0 + x_1) + x_2) + ... left fold in operand order; nargs >= 1
  Affine = 19,  // ((konst + c_0*x_0) + c_1*x_1) + ... left fold; coefficients c_i parallel to args

  // Reserved for later milestones (no recorder support in M1).
  Gather = 20,
  SegmentSum = 21,
  Scan = 22,
  Linmap = 23,
  Quot = 24,
  Quadform = 25,
  SmoothStep = 26,
  Frozen = 27,
  Implicit = 28,
  Pin = 29,

  Count_ = 30  // one past the last op
};

inline constexpr int op_count = static_cast<int>(Op::Count_);

// Stable lower-case names: "const", "input", "add", ..., "affine", "gather", ... "pin".
const char* to_string(Op op) noexcept;

// Number of fixed operands (a, b, c) an op reads; -1 for the variadic ops (Sum, Affine); 0 for
// leaves. Reserved ops report -1 (their layout is decided when they are introduced).
constexpr int op_arity(Op op) noexcept {
  switch (op) {
    case Op::Const:
    case Op::Input:
      return 0;
    case Op::Neg:
    case Op::Exp:
    case Op::Log:
    case Op::Sqrt:
    case Op::Recip:
      return 1;
    case Op::Add:
    case Op::Sub:
    case Op::Mul:
    case Op::Div:
    case Op::CmpLt:
    case Op::CmpLe:
    case Op::CmpGt:
    case Op::CmpGe:
    case Op::CmpEq:
      return 2;
    case Op::Fma:
    case Op::Select:
      return 3;
    default:
      return -1;
  }
}

constexpr bool op_is_variadic(Op op) noexcept { return op == Op::Sum || op == Op::Affine; }
constexpr bool op_is_leaf(Op op) noexcept { return op == Op::Const || op == Op::Input; }
constexpr bool op_is_comparison(Op op) noexcept {
  return op == Op::CmpLt || op == Op::CmpLe || op == Op::CmpGt || op == Op::CmpGe || op == Op::CmpEq;
}
// Operand order does not change the value (bit-identically): a+b == b+a, a*b == b*a, a==b.
constexpr bool op_is_commutative(Op op) noexcept {
  return op == Op::Add || op == Op::Mul || op == Op::CmpEq;
}
// Supported by the M1 recorder, passes and replay interpreter.
constexpr bool op_is_supported(Op op) noexcept {
  return static_cast<int>(op) <= static_cast<int>(Op::Affine);
}

}  // namespace epykos
