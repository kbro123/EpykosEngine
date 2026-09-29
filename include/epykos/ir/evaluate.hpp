// EpykosEngine — a scalar reference evaluator for the domain IR: the evaluation contract of
// program.hpp written out row by row, one double per value. This is the specification P4's tiled
// interpreter must match (E0 against tape/replay.hpp under the same contraction setting); it is
// not a fast path. Header-only so the including TU's flags govern the arithmetic (E0 gates are
// *_e0_test.cpp TUs, compiled with -ffp-contract=off).
//
// Iteration order: domains in Program order; rows in order; steps in order. A gather reads the
// value space, which holds every earlier domain's rows (and, for a recurrent domain — a scan —
// earlier rows of the same domain). Sum: acc = m_0; acc = acc + m_k (a fixed-arity Sum folds its
// operand slots a, b, c the same way). Affine: acc = c_0; p = k·m; acc = acc + p.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "epykos/ir/program.hpp"

namespace epykos::ir {

class Evaluator {
 public:
  explicit Evaluator(const Program& p) : p_(&p), values_(p.num_values(), 0.0) {
    std::size_t max_steps = 0;
    for (const Group& g : p.groups) max_steps = std::max(max_steps, g.steps.size());
    scratch_.assign(max_steps, 0.0);
    ordinal_.assign(p.num_values(), -1);
    for (std::size_t k = 0; k < p.inputs.size(); ++k) {
      ordinal_[static_cast<std::size_t>(p.inputs[k])] = static_cast<std::int32_t>(k);
    }
    for (const Group& g : p.groups) {
      for (const Step& s : g.steps) {
        if (!op_is_supported(s.op)) {
          throw std::invalid_argument(std::string("ir::Evaluator: unsupported op ") + to_string(s.op));
        }
      }
    }
  }

  // inputs[k] per input ordinal; outputs[k] per output ordinal. No allocation.
  void run(const double* inputs, double* outputs) noexcept {
    const Program& p = *p_;
    double* v = values_.data();
    double* s = scratch_.data();
    for (std::size_t d = 0; d < p.domains.size(); ++d) {
      const Domain& dom = p.domains[d];
      const Group& g = p.groups[d];
      const std::vector<Step>& steps = g.steps;
      const value_id base = dom.value_base;
      for (row_id r = 0; r < dom.rows; ++r) {
        for (std::size_t k = 0; k < steps.size(); ++k) {
          const Step& st = steps[k];
          double res;
          switch (st.op) {
            case Op::Const: res = fetch(st.konst, r, s, inputs, base + r); break;
            case Op::Input: res = inputs[ordinal_[static_cast<std::size_t>(base + r)]]; break;
            case Op::Add: res = fetch(st.a, r, s, inputs, base + r) + fetch(st.b, r, s, inputs, base + r); break;
            case Op::Sub: res = fetch(st.a, r, s, inputs, base + r) - fetch(st.b, r, s, inputs, base + r); break;
            case Op::Mul: res = fetch(st.a, r, s, inputs, base + r) * fetch(st.b, r, s, inputs, base + r); break;
            case Op::Div: res = fetch(st.a, r, s, inputs, base + r) / fetch(st.b, r, s, inputs, base + r); break;
            case Op::Neg: res = -fetch(st.a, r, s, inputs, base + r); break;
            case Op::Exp: res = std::exp(fetch(st.a, r, s, inputs, base + r)); break;
            case Op::Log: res = std::log(fetch(st.a, r, s, inputs, base + r)); break;
            case Op::Sqrt: res = std::sqrt(fetch(st.a, r, s, inputs, base + r)); break;
            case Op::Recip: res = 1.0 / fetch(st.a, r, s, inputs, base + r); break;
            case Op::Fma:
              res = std::fma(fetch(st.a, r, s, inputs, base + r), fetch(st.b, r, s, inputs, base + r),
                             fetch(st.c, r, s, inputs, base + r));
              break;
            case Op::Select:
              res = (fetch(st.a, r, s, inputs, base + r) != 0.0) ? fetch(st.b, r, s, inputs, base + r)
                                                                  : fetch(st.c, r, s, inputs, base + r);
              break;
            case Op::CmpLt: res = (fetch(st.a, r, s, inputs, base + r) < fetch(st.b, r, s, inputs, base + r)) ? 1.0 : 0.0; break;
            case Op::CmpLe: res = (fetch(st.a, r, s, inputs, base + r) <= fetch(st.b, r, s, inputs, base + r)) ? 1.0 : 0.0; break;
            case Op::CmpGt: res = (fetch(st.a, r, s, inputs, base + r) > fetch(st.b, r, s, inputs, base + r)) ? 1.0 : 0.0; break;
            case Op::CmpGe: res = (fetch(st.a, r, s, inputs, base + r) >= fetch(st.b, r, s, inputs, base + r)) ? 1.0 : 0.0; break;
            case Op::CmpEq: res = (fetch(st.a, r, s, inputs, base + r) == fetch(st.b, r, s, inputs, base + r)) ? 1.0 : 0.0; break;
            case Op::Sum: {
              if (is_fixed_sum(st)) {
                double acc = fetch(st.a, r, s, inputs, base + r);
                acc = acc + fetch(st.b, r, s, inputs, base + r);
                if (st.c.kind != SlotKind::None) acc = acc + fetch(st.c, r, s, inputs, base + r);
                res = acc;
                break;
              }
              const Segment& seg = p.segments[static_cast<std::size_t>(st.a.index)];
              const std::int32_t lo = seg.offsets[static_cast<std::size_t>(r)];
              const std::int32_t hi = seg.offsets[static_cast<std::size_t>(r) + 1];
              double acc = v[seg.members[static_cast<std::size_t>(lo)]];
              for (std::int32_t m = lo + 1; m < hi; ++m) acc = acc + v[seg.members[static_cast<std::size_t>(m)]];
              res = acc;
              break;
            }
            case Op::Affine: {
              const Segment& seg = p.segments[static_cast<std::size_t>(st.a.index)];
              const std::int32_t lo = seg.offsets[static_cast<std::size_t>(r)];
              const std::int32_t hi = seg.offsets[static_cast<std::size_t>(r) + 1];
              double acc = fetch(st.konst, r, s, inputs, base + r);
              for (std::int32_t m = lo; m < hi; ++m) {
                const double prod = seg.coefs[static_cast<std::size_t>(m)] * v[seg.members[static_cast<std::size_t>(m)]];
                acc = acc + prod;
              }
              res = acc;
              break;
            }
            default: res = std::nan(""); break;
          }
          s[k] = res;
        }
        v[static_cast<std::size_t>(base + r)] = s[steps.size() - 1];
      }
    }
    for (std::size_t k = 0; k < p.outputs.size(); ++k) outputs[k] = v[static_cast<std::size_t>(p.outputs[k])];
  }

  const double* values() const noexcept { return values_.data(); }
  const Program& program() const noexcept { return *p_; }

 private:
  double fetch(const Slot& slot, row_id r, const double* scratch, const double* inputs, value_id self) const noexcept {
    const Program& p = *p_;
    switch (slot.kind) {
      case SlotKind::Step: return scratch[slot.index];
      case SlotKind::Literal: return p.literals[static_cast<std::size_t>(slot.index)];
      case SlotKind::Column: return p.columns[static_cast<std::size_t>(slot.index)].values[static_cast<std::size_t>(r)];
      case SlotKind::Gather: return values_[static_cast<std::size_t>(p.gathers[static_cast<std::size_t>(slot.index)].index[static_cast<std::size_t>(r)])];
      case SlotKind::Input: return inputs[ordinal_[static_cast<std::size_t>(self)]];
      default: return std::nan("");
    }
  }

  const Program* p_;
  std::vector<double> values_;
  std::vector<double> scratch_;
  std::vector<std::int32_t> ordinal_;
};

inline std::vector<double> evaluate(const Program& p, const std::vector<double>& inputs) {
  std::vector<double> out(p.outputs.size(), 0.0);
  Evaluator(p).run(inputs.data(), out.data());
  return out;
}

}  // namespace epykos::ir
