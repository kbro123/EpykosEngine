#include "epykos/tape/select_export.hpp"

#include <cmath>
#include <cstddef>

namespace epykos {

SelectExports export_selects(Tape& tape) {
  tape.validate();
  SelectExports ex;
  ex.first_output = static_cast<int>(tape.num_outputs());
  const std::size_t n_nodes = tape.size();  // new nodes are appended past this point
  for (std::size_t i = 0; i < n_nodes; ++i) {
    const Node& n = tape[static_cast<node_id>(i)];
    if (n.op != Op::Select) continue;
    SelectExport e;
    e.select = static_cast<node_id>(i);
    e.predicate = n.a;
    const Node& p = tape[n.a];
    e.predicate_op = p.op;
    node_id margin = n.a;  // a non-comparison predicate: the mask itself
    switch (p.op) {
      case Op::CmpLt:
      case Op::CmpLe:
        margin = tape.binary(Op::Sub, p.b, p.a);
        break;
      case Op::CmpGt:
      case Op::CmpGe:
      case Op::CmpEq:
        margin = tape.binary(Op::Sub, p.a, p.b);
        break;
      default:
        break;
    }
    const node_id gap = tape.binary(Op::Sub, n.b, n.c);
    e.mask = tape.output(n.a);
    e.margin = tape.output(margin);
    e.gap = tape.output(gap);
    ex.selects.push_back(e);
  }
  return ex;
}

Flip classify_flip(double mask_a, double margin_a, double gap_a, double mask_b, double margin_b, double gap_b,
                   double threshold) noexcept {
  Flip f;
  f.margin_a = margin_a;
  f.margin_b = margin_b;
  f.gap_a = gap_a;
  f.gap_b = gap_b;
  f.flipped = (mask_a != 0.0) != (mask_b != 0.0);
  const double g = std::fmax(std::fabs(gap_a), std::fabs(gap_b));
  f.significant = f.flipped && g > threshold;
  return f;
}

}  // namespace epykos
