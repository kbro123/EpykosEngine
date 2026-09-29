// EpykosEngine — straight-line scalar replay of a tape.
//
// Replayer evaluates the node table in order with one double per node. It is header-only on
// purpose: a TU compiled with -ffp-contract=off (any *_e0_test.cpp, or everything under the
// reference preset) then replays without fused multiply-adds, which is what the E0 gates need.
// Every arithmetic op is evaluated exactly as the Rec operator that recorded it (same
// expression, same rounding), so a replay at the record-point inputs reproduces Rec::v bitwise
// under the same contraction setting.
//
// Sum:    ((x_0 + x_1) + x_2) + ...            (left fold, recorded order)
// Affine: ((c_0 + k_0*x_0) + k_1*x_1) + ...     (left fold; each product rounded, then added)
//
// Zero allocations after construction: the workspace is sized once for the tape.
#pragma once

#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <vector>

#include "epykos/tape/tape.hpp"

namespace epykos {

class Replayer {
 public:
  // Sizes the workspace for `tape`. The tape must outlive the Replayer and must not be rewritten
  // by a pass afterwards (construct a new Replayer after running passes).
  explicit Replayer(const Tape& tape) : tape_(&tape), vals_(tape.size(), 0.0) {
    if (tape.size() == 0) return;
    for (const Node& n : tape.nodes()) {
      if (!op_is_supported(n.op)) {
        throw std::invalid_argument(std::string("Replayer: unsupported op ") + to_string(n.op));
      }
    }
  }

  // inputs[k] for input ordinal k (tape.num_inputs() values); outputs[k] for output ordinal k
  // (tape.num_outputs() values). No allocation.
  void run(const double* inputs, double* outputs) noexcept {
    const Tape& t = *tape_;
    const std::vector<Node>& nodes = t.nodes();
    const node_id* args = t.arg_table().data();
    const double* coefs = t.coef_table().data();
    double* v = vals_.data();
    const std::size_t n_nodes = nodes.size();
    for (std::size_t i = 0; i < n_nodes; ++i) {
      const Node& n = nodes[i];
      double r;
      switch (n.op) {
        case Op::Const: r = n.konst; break;
        case Op::Input: r = inputs[n.a]; break;
        case Op::Add: r = v[n.a] + v[n.b]; break;
        case Op::Sub: r = v[n.a] - v[n.b]; break;
        case Op::Mul: r = v[n.a] * v[n.b]; break;
        case Op::Div: r = v[n.a] / v[n.b]; break;
        case Op::Neg: r = -v[n.a]; break;
        case Op::Exp: r = std::exp(v[n.a]); break;
        case Op::Log: r = std::log(v[n.a]); break;
        case Op::Sqrt: r = std::sqrt(v[n.a]); break;
        case Op::Recip: r = 1.0 / v[n.a]; break;
        case Op::Fma: r = std::fma(v[n.a], v[n.b], v[n.c]); break;
        case Op::Select: r = (v[n.a] != 0.0) ? v[n.b] : v[n.c]; break;
        case Op::CmpLt: r = (v[n.a] < v[n.b]) ? 1.0 : 0.0; break;
        case Op::CmpLe: r = (v[n.a] <= v[n.b]) ? 1.0 : 0.0; break;
        case Op::CmpGt: r = (v[n.a] > v[n.b]) ? 1.0 : 0.0; break;
        case Op::CmpGe: r = (v[n.a] >= v[n.b]) ? 1.0 : 0.0; break;
        case Op::CmpEq: r = (v[n.a] == v[n.b]) ? 1.0 : 0.0; break;
        case Op::Sum: {
          const node_id* a = args + n.first_arg;
          double acc = v[a[0]];
          for (std::int32_t k = 1; k < n.nargs; ++k) acc = acc + v[a[k]];
          r = acc;
          break;
        }
        case Op::Affine: {
          const node_id* a = args + n.first_arg;
          const double* c = coefs + n.first_arg;
          double acc = n.konst;
          for (std::int32_t k = 0; k < n.nargs; ++k) {
            const double p = c[k] * v[a[k]];  // separate statement: no contraction with the add
            acc = acc + p;
          }
          r = acc;
          break;
        }
        default: r = std::nan(""); break;  // reserved ops are rejected by the constructor
      }
      v[i] = r;
    }
    const std::vector<node_id>& outs = t.outputs();
    for (std::size_t k = 0; k < outs.size(); ++k) outputs[k] = v[outs[k]];
  }

  // Values of every node after the last run (debugging, tests).
  const double* values() const noexcept { return vals_.data(); }
  std::size_t size() const noexcept { return vals_.size(); }
  const Tape& tape() const noexcept { return *tape_; }

 private:
  const Tape* tape_;
  std::vector<double> vals_;
};

// Convenience: one-shot replay (allocates the workspace).
inline void replay(const Tape& tape, const double* inputs, double* outputs) {
  Replayer(tape).run(inputs, outputs);
}
inline std::vector<double> replay(const Tape& tape, const std::vector<double>& inputs) {
  std::vector<double> out(tape.num_outputs(), 0.0);
  replay(tape, inputs.data(), out.data());
  return out;
}

}  // namespace epykos
