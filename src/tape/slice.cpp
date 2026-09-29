#include "epykos/tape/slice.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace epykos {

Slice slice(const Tape& tape, std::span<const node_id> roots) {
  const std::vector<Node>& nodes = tape.nodes();
  const std::size_t n = nodes.size();
  std::vector<std::uint8_t> keep(n, 0);
  for (node_id r : roots) {
    if (r < 0 || static_cast<std::size_t>(r) >= n) {
      throw RecordError("slice: root node id " + std::to_string(r) + " is not on this tape");
    }
    keep[static_cast<std::size_t>(r)] = 1;
  }
  // Backward closure: a node's operands are earlier nodes, so one pass from the end suffices.
  for (std::size_t i = n; i-- > 0;) {
    if (!keep[i]) continue;
    const Node& nd = nodes[i];
    if (op_is_variadic(nd.op)) {
      for (node_id a : tape.args(nd)) keep[static_cast<std::size_t>(a)] = 1;
    } else {
      const int arity = op_arity(nd.op);
      if (arity >= 1) keep[static_cast<std::size_t>(nd.a)] = 1;
      if (arity >= 2) keep[static_cast<std::size_t>(nd.b)] = 1;
      if (arity >= 3) keep[static_cast<std::size_t>(nd.c)] = 1;
    }
  }
  Slice out;
  out.node_map.assign(n, invalid_node);
  std::vector<node_id> args;
  for (std::size_t i = 0; i < n; ++i) {
    if (!keep[i]) continue;
    const Node& nd = nodes[i];
    node_id id = invalid_node;
    switch (nd.op) {
      case Op::Const:
        id = out.tape.constant(nd.konst);
        break;
      case Op::Input:
        id = out.tape.input(nd.konst);
        out.input_ordinals.push_back(static_cast<int>(nd.a));
        break;
      case Op::Sum:
      case Op::Affine: {
        args.clear();
        for (node_id a : tape.args(nd)) args.push_back(out.node_map[static_cast<std::size_t>(a)]);
        id = out.tape.variadic(nd.op, args, tape.coefs(nd), nd.konst);
        break;
      }
      default: {
        const int arity = op_arity(nd.op);
        const node_id a = arity >= 1 ? out.node_map[static_cast<std::size_t>(nd.a)] : invalid_node;
        const node_id b = arity >= 2 ? out.node_map[static_cast<std::size_t>(nd.b)] : invalid_node;
        const node_id c = arity >= 3 ? out.node_map[static_cast<std::size_t>(nd.c)] : invalid_node;
        if (arity == 1) {
          id = out.tape.unary(nd.op, a);
        } else if (arity == 2) {
          id = out.tape.binary(nd.op, a, b);
        } else if (arity == 3) {
          id = out.tape.ternary(nd.op, a, b, c);
        } else {
          throw RecordError(std::string("slice: unsupported op ") + to_string(nd.op));
        }
        break;
      }
    }
    out.node_map[i] = id;
  }
  for (node_id r : roots) out.tape.output(out.node_map[static_cast<std::size_t>(r)]);
  out.tape.validate();
  return out;
}

}  // namespace epykos
