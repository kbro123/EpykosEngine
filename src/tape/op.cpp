#include "epykos/tape/op.hpp"

namespace epykos {

const char* to_string(Op op) noexcept {
  switch (op) {
    case Op::Const: return "const";
    case Op::Input: return "input";
    case Op::Add: return "add";
    case Op::Sub: return "sub";
    case Op::Mul: return "mul";
    case Op::Div: return "div";
    case Op::Neg: return "neg";
    case Op::Exp: return "exp";
    case Op::Log: return "log";
    case Op::Sqrt: return "sqrt";
    case Op::Recip: return "recip";
    case Op::Fma: return "fma";
    case Op::Select: return "select";
    case Op::CmpLt: return "cmp_lt";
    case Op::CmpLe: return "cmp_le";
    case Op::CmpGt: return "cmp_gt";
    case Op::CmpGe: return "cmp_ge";
    case Op::CmpEq: return "cmp_eq";
    case Op::Sum: return "sum";
    case Op::Affine: return "affine";
    case Op::Gather: return "gather";
    case Op::SegmentSum: return "segment_sum";
    case Op::Scan: return "scan";
    case Op::Linmap: return "linmap";
    case Op::Quot: return "quot";
    case Op::Quadform: return "quadform";
    case Op::SmoothStep: return "smooth_step";
    case Op::Frozen: return "frozen";
    case Op::Implicit: return "implicit";
    case Op::Pin: return "pin";
    case Op::Count_: break;
  }
  return "?";
}

}  // namespace epykos
