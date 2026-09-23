#include "epykos/ir/expand.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace epykos::ir {

namespace {

using std::size_t;
using std::uint64_t;

size_t idx(std::int32_t i) noexcept { return static_cast<size_t>(i); }

uint64_t bits_of(double v) noexcept {
  uint64_t u;
  std::memcpy(&u, &v, sizeof u);
  return u;
}

uint64_t mix(uint64_t h, uint64_t v) noexcept {
  h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
  h ^= h >> 30;
  h *= 0xBF58476D1CE4E5B9ull;
  h ^= h >> 27;
  h *= 0x94D049BB133111EBull;
  h ^= h >> 31;
  return h;
}

[[noreturn]] void fail(const std::string& what) { throw std::runtime_error("ir::expand: " + what); }

}  // namespace

// ---- expand -----------------------------------------------------------------------------

Tape expand(const Program& p) {
  validate(p);
  Tape t;
  std::vector<node_id> node_of(p.num_values(), invalid_node);
  std::vector<std::int32_t> ordinal_of(p.num_values(), -1);
  for (size_t k = 0; k < p.inputs.size(); ++k) ordinal_of[idx(p.inputs[k])] = static_cast<std::int32_t>(k);

  std::vector<node_id> scratch;
  std::vector<node_id> members;
  std::vector<double> coefs;
  for (size_t d = 0; d < p.domains.size(); ++d) {
    const Domain& dom = p.domains[d];
    const Group& g = p.groups[d];
    scratch.assign(g.steps.size(), invalid_node);
    for (row_id r = 0; r < dom.rows; ++r) {
      const value_id self = dom.value_base + r;
      auto value_of_slot = [&](const Slot& s) -> double {
        switch (s.kind) {
          case SlotKind::Literal: return p.literals[idx(s.index)];
          case SlotKind::Column: return p.columns[idx(s.index)].values[idx(r)];
          default: fail("a constant slot was expected");
        }
      };
      auto operand = [&](const Slot& s) -> node_id {
        switch (s.kind) {
          case SlotKind::Step: return scratch[idx(s.index)];
          case SlotKind::Literal:
          case SlotKind::Column: return t.constant(value_of_slot(s));
          case SlotKind::Gather: {
            const node_id n = node_of[idx(p.gathers[idx(s.index)].index[idx(r)])];
            if (n == invalid_node) fail("gather reads a value that has not been emitted");
            return n;
          }
          default: fail("bad operand slot");
        }
      };
      for (size_t k = 0; k < g.steps.size(); ++k) {
        const Step& s = g.steps[k];
        node_id id = invalid_node;
        switch (s.op) {
          case Op::Const:
            id = t.constant(value_of_slot(s.konst));
            break;
          case Op::Input: {
            const std::int32_t ordinal = ordinal_of[idx(self)];
            if (ordinal < 0 || static_cast<size_t>(ordinal) != t.num_inputs()) {
              fail("Input rows must be emitted in ordinal order");
            }
            id = t.input(p.input_values[idx(ordinal)]);
            break;
          }
          case Op::Sum:
          case Op::Affine: {
            const Segment& seg = p.segments[idx(s.a.index)];
            members.clear();
            coefs.clear();
            for (std::int32_t m = seg.offsets[idx(r)]; m < seg.offsets[idx(r) + 1]; ++m) {
              const node_id n = node_of[idx(seg.members[idx(m)])];
              if (n == invalid_node) fail("segment reads a value that has not been emitted");
              members.push_back(n);
              if (s.op == Op::Affine) coefs.push_back(seg.coefs[idx(m)]);
            }
            id = (s.op == Op::Sum) ? t.variadic(Op::Sum, members)
                                   : t.variadic(Op::Affine, members, coefs, value_of_slot(s.konst));
            break;
          }
          default: {
            const int arity = op_arity(s.op);
            if (arity == 1) {
              id = t.unary(s.op, operand(s.a));
            } else if (arity == 2) {
              const node_id a = operand(s.a);
              const node_id b = operand(s.b);
              id = t.binary(s.op, a, b);
            } else if (arity == 3) {
              const node_id a = operand(s.a);
              const node_id b = operand(s.b);
              const node_id c = operand(s.c);
              id = t.ternary(s.op, a, b, c);
            } else {
              fail(std::string("unsupported op ") + to_string(s.op));
            }
            break;
          }
        }
        scratch[k] = id;
      }
      node_of[idx(self)] = scratch.back();
    }
  }
  for (value_id v : p.outputs) {
    if (node_of[idx(v)] == invalid_node) fail("output value was not emitted");
    t.output(node_of[idx(v)]);
  }
  t.validate();
  return t;
}

// ---- canonical form ----------------------------------------------------------------------

CanonicalTape canonical_form(const Tape& t) {
  const size_t n = t.size();
  const std::vector<Node>& nodes = t.nodes();
  // Merkle hashes.
  std::vector<uint64_t> m(n, 0);
  for (size_t i = 0; i < n; ++i) {
    const Node& nd = nodes[i];
    uint64_t h = mix(0x9E3779B97F4A7C15ull, static_cast<uint64_t>(nd.op) + 0x100);
    switch (nd.op) {
      case Op::Const:
        h = mix(h, bits_of(nd.konst));
        break;
      case Op::Input:
        h = mix(h, static_cast<uint64_t>(nd.a));
        h = mix(h, bits_of(nd.konst));
        break;
      case Op::Sum:
        for (node_id a : t.args(nd)) h = mix(h, m[idx(a)]);
        break;
      case Op::Affine: {
        h = mix(h, bits_of(nd.konst));
        const auto args = t.args(nd);
        const auto coefs = t.coefs(nd);
        for (size_t k = 0; k < args.size(); ++k) {
          h = mix(h, m[idx(args[k])]);
          h = mix(h, bits_of(coefs[k]));
        }
        break;
      }
      default: {
        const int arity = op_arity(nd.op);
        uint64_t ha = arity >= 1 ? m[idx(nd.a)] : 0;
        uint64_t hb = arity >= 2 ? m[idx(nd.b)] : 0;
        if (op_is_commutative(nd.op) && hb < ha) std::swap(ha, hb);
        if (arity >= 1) h = mix(h, ha);
        if (arity >= 2) h = mix(h, hb);
        if (arity >= 3) h = mix(h, m[idx(nd.c)]);
        break;
      }
    }
    m[i] = h;
  }

  // Canonical order: inputs by ordinal, then a post-order walk from the outputs.
  std::vector<std::int32_t> canon(n, -1);
  std::vector<node_id> order;  // canonical id -> node
  order.reserve(n);
  for (node_id in : t.inputs()) {
    canon[idx(in)] = static_cast<std::int32_t>(order.size());
    order.push_back(in);
  }
  struct Frame {
    node_id id;
    std::int32_t k;   // next child position
    std::int32_t nk;  // child count
    bool swap;        // commutative: visit b before a
  };
  std::vector<Frame> stack;
  auto child_count = [&](const Node& nd) -> std::int32_t {
    if (op_is_variadic(nd.op)) return nd.nargs;
    const int arity = op_arity(nd.op);
    return arity < 0 ? 0 : arity;
  };
  auto child = [&](const Frame& f, std::int32_t k) -> node_id {
    const Node& nd = nodes[idx(f.id)];
    if (op_is_variadic(nd.op)) return t.args(nd)[idx(k)];
    std::int32_t pos = k;
    if (f.swap && k < 2) pos = 1 - k;
    return pos == 0 ? nd.a : pos == 1 ? nd.b : nd.c;
  };
  auto open = [&](node_id id) {
    const Node& nd = nodes[idx(id)];
    Frame f{id, 0, child_count(nd), false};
    if (op_is_commutative(nd.op) && m[idx(nd.b)] < m[idx(nd.a)]) f.swap = true;
    stack.push_back(f);
  };
  std::vector<std::uint8_t> opened(n, 0);
  for (node_id in : t.inputs()) opened[idx(in)] = 1;
  for (node_id out : t.outputs()) {
    if (opened[idx(out)]) continue;
    opened[idx(out)] = 1;
    open(out);
    while (!stack.empty()) {
      Frame& f = stack.back();
      if (f.k < f.nk) {
        const node_id c = child(f, f.k++);
        if (!opened[idx(c)]) {
          opened[idx(c)] = 1;
          open(c);  // invalidates f
        }
        continue;
      }
      canon[idx(f.id)] = static_cast<std::int32_t>(order.size());
      order.push_back(f.id);
      stack.pop_back();
    }
  }

  CanonicalTape ct;
  ct.nodes.resize(order.size());
  for (size_t c = 0; c < order.size(); ++c) {
    const Node& nd = nodes[idx(order[c])];
    CanonicalNode& cn = ct.nodes[c];
    cn.op = nd.op;
    cn.konst_bits = (nd.op == Op::Const || nd.op == Op::Input || nd.op == Op::Affine) ? bits_of(nd.konst) : 0;
    cn.input_ordinal = nd.op == Op::Input ? nd.a : -1;
    if (op_is_variadic(nd.op)) {
      for (node_id a : t.args(nd)) cn.operands.push_back(canon[idx(a)]);
      if (nd.op == Op::Affine) {
        for (double k : t.coefs(nd)) cn.coefs.push_back(bits_of(k));
      }
    } else {
      const int arity = op_arity(nd.op);
      if (arity >= 1) cn.operands.push_back(canon[idx(nd.a)]);
      if (arity >= 2) cn.operands.push_back(canon[idx(nd.b)]);
      if (arity >= 3) cn.operands.push_back(canon[idx(nd.c)]);
      if (op_is_commutative(nd.op) && cn.operands[1] < cn.operands[0]) std::swap(cn.operands[0], cn.operands[1]);
    }
  }
  for (node_id in : t.inputs()) ct.inputs.push_back(canon[idx(in)]);
  for (node_id out : t.outputs()) ct.outputs.push_back(canon[idx(out)]);
  ct.unreachable = n - order.size();
  return ct;
}

// ---- comparison --------------------------------------------------------------------------

namespace {

std::string describe(const CanonicalNode& n) {
  std::ostringstream os;
  os << to_string(n.op);
  if (n.op == Op::Const || n.op == Op::Input || n.op == Op::Affine) {
    char buf[24];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(n.konst_bits));
    double v;
    std::memcpy(&v, &n.konst_bits, sizeof v);
    os << " konst=" << buf << " (" << v << ')';
  }
  if (n.input_ordinal >= 0) os << " @" << n.input_ordinal;
  os << " [";
  for (size_t k = 0; k < n.operands.size(); ++k) os << (k ? " " : "") << '#' << n.operands[k];
  os << ']';
  if (!n.coefs.empty()) {
    os << " coefs=[";
    for (size_t k = 0; k < n.coefs.size(); ++k) {
      double v;
      std::memcpy(&v, &n.coefs[k], sizeof v);
      os << (k ? " " : "") << v;
    }
    os << ']';
  }
  return os.str();
}

bool same(const CanonicalNode& a, const CanonicalNode& b) {
  return a.op == b.op && a.konst_bits == b.konst_bits && a.input_ordinal == b.input_ordinal &&
         a.operands == b.operands && a.coefs == b.coefs;
}

}  // namespace

bool roundtrip_identical(const Tape& original, const Tape& expanded, std::string* diff) {
  const CanonicalTape a = canonical_form(original);
  const CanonicalTape b = canonical_form(expanded);
  std::ostringstream os;
  size_t differences = 0;
  constexpr size_t cap = 40;
  auto note = [&](const std::string& line) {
    if (differences < cap) os << line << '\n';
    ++differences;
  };
  if (a.nodes.size() != b.nodes.size()) {
    note("reachable node count: original " + std::to_string(a.nodes.size()) + ", expanded " + std::to_string(b.nodes.size()));
  }
  if (a.unreachable != b.unreachable) {
    note("unreachable node count: original " + std::to_string(a.unreachable) + ", expanded " + std::to_string(b.unreachable));
  }
  const size_t n = std::min(a.nodes.size(), b.nodes.size());
  for (size_t c = 0; c < n; ++c) {
    if (!same(a.nodes[c], b.nodes[c])) {
      note("canonical #" + std::to_string(c) + ": original " + describe(a.nodes[c]) + " | expanded " + describe(b.nodes[c]));
    }
  }
  if (a.inputs != b.inputs) note("input lists differ");
  if (a.outputs != b.outputs) {
    note("output lists differ");
    const size_t no = std::min(a.outputs.size(), b.outputs.size());
    for (size_t k = 0; k < no; ++k) {
      if (a.outputs[k] != b.outputs[k]) {
        note("  output " + std::to_string(k) + ": original #" + std::to_string(a.outputs[k]) + ", expanded #" + std::to_string(b.outputs[k]));
      }
    }
  }
  if (differences > cap) os << "... " << (differences - cap) << " more\n";
  if (diff != nullptr) *diff = os.str();
  return differences == 0;
}

}  // namespace epykos::ir
