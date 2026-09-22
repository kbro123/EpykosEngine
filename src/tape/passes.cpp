#include "epykos/tape/passes.hpp"

#include <algorithm>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace epykos {

namespace {

std::uint64_t bits_of(double v) noexcept {
  std::uint64_t u;
  std::memcpy(&u, &v, sizeof u);
  return u;
}

std::size_t idx(node_id id) noexcept { return static_cast<std::size_t>(id); }

// Copies nodes of `old` into a fresh tape with remapped operands. Const nodes are deduplicated
// by the tape itself; Input nodes keep their ordinals because they are copied in order.
class Rebuilder {
 public:
  explicit Rebuilder(const Tape& old) : old_(old), remap_(old.size(), invalid_node) {}

  node_id mapped(node_id o) const {
    const node_id n = remap_[idx(o)];
    if (n == invalid_node) throw RecordError("pass: operand " + std::to_string(o) + " was dropped");
    return n;
  }
  void set(node_id o, node_id n) { remap_[idx(o)] = n; }
  bool is_mapped(node_id o) const noexcept { return remap_[idx(o)] != invalid_node; }

  // Emits a copy of old node `o` with mapped operands and records the mapping.
  node_id copy(node_id o) {
    const Node& n = old_[o];
    node_id id = invalid_node;
    switch (n.op) {
      case Op::Const:
        id = out_.constant(n.konst);
        break;
      case Op::Input:
        id = out_.input(n.konst);
        break;
      case Op::Sum:
      case Op::Affine: {
        args_.clear();
        for (node_id a : old_.args(n)) args_.push_back(mapped(a));
        id = out_.variadic(n.op, args_, old_.coefs(n), n.konst);
        break;
      }
      default: {
        const int arity = op_arity(n.op);
        if (arity == 1) {
          id = out_.unary(n.op, mapped(n.a));
        } else if (arity == 2) {
          id = out_.binary(n.op, mapped(n.a), mapped(n.b));
        } else if (arity == 3) {
          id = out_.ternary(n.op, mapped(n.a), mapped(n.b), mapped(n.c));
        } else {
          throw RecordError(std::string("pass: unsupported op ") + to_string(n.op));
        }
        break;
      }
    }
    set(o, id);
    return id;
  }

  Tape& out() noexcept { return out_; }
  std::vector<node_id>& remap() noexcept { return remap_; }

  // Registers the old outputs on the new tape (same ordinals) and swaps the result into `target`.
  PassResult finish(Tape& target, std::size_t changed) {
    for (node_id o : old_.outputs()) out_.output(mapped(o));
    PassResult r;
    r.nodes_before = old_.size();
    r.nodes_after = out_.size();
    r.changed = changed;
    r.remap = std::move(remap_);
    target.swap(out_);
    return r;
  }

 private:
  const Tape& old_;
  Tape out_;
  std::vector<node_id> remap_;
  std::vector<node_id> args_;
};

// Use counts plus, for single-use nodes, the unique user (invalid_node when that use is the
// output list).
struct Uses {
  std::vector<std::int32_t> count;
  std::vector<node_id> user;
};

Uses compute_uses(const Tape& t) {
  Uses u;
  u.count.assign(t.size(), 0);
  u.user.assign(t.size(), invalid_node);
  auto ref = [&](node_id p, node_id by) {
    ++u.count[idx(p)];
    u.user[idx(p)] = by;
  };
  const auto& nodes = t.nodes();
  for (std::size_t i = 0; i < nodes.size(); ++i) {
    const Node& n = nodes[i];
    const node_id id = static_cast<node_id>(i);
    if (op_is_variadic(n.op)) {
      for (node_id a : t.args(n)) ref(a, id);
    } else {
      const int arity = op_arity(n.op);
      if (arity >= 1) ref(n.a, id);
      if (arity >= 2) ref(n.b, id);
      if (arity >= 3) ref(n.c, id);
    }
  }
  for (node_id o : t.outputs()) ref(o, invalid_node);
  return u;
}

// ---- cse -----------------------------------------------------------------------------------

struct CseKey {
  Op op = Op::Const;
  node_id a = invalid_node, b = invalid_node, c = invalid_node;
  std::uint64_t kbits = 0;
  std::vector<node_id> args;
  std::vector<std::uint64_t> coefs;
  bool operator==(const CseKey&) const = default;
};

struct CseKeyHash {
  static void mix(std::size_t& h, std::uint64_t v) noexcept {
    h ^= static_cast<std::size_t>(v * 0x9E3779B97F4A7C15ull) + 0x9E3779B9u + (h << 6) + (h >> 2);
  }
  std::size_t operator()(const CseKey& k) const noexcept {
    std::size_t h = static_cast<std::size_t>(k.op);
    mix(h, static_cast<std::uint64_t>(static_cast<std::uint32_t>(k.a)));
    mix(h, static_cast<std::uint64_t>(static_cast<std::uint32_t>(k.b)));
    mix(h, static_cast<std::uint64_t>(static_cast<std::uint32_t>(k.c)));
    mix(h, k.kbits);
    for (node_id x : k.args) mix(h, static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)));
    for (std::uint64_t x : k.coefs) mix(h, x);
    return h;
  }
};

}  // namespace

std::vector<std::int32_t> use_counts(const Tape& tape) { return compute_uses(tape).count; }

PassResult cse(Tape& tape) {
  const Tape old = tape;  // the rebuild reads the old table while the new one is filled
  Rebuilder rb(old);
  std::unordered_map<CseKey, node_id, CseKeyHash> seen;
  seen.reserve(old.size());
  std::size_t merged = 0;
  const auto& nodes = old.nodes();
  for (std::size_t i = 0; i < nodes.size(); ++i) {
    const node_id o = static_cast<node_id>(i);
    const Node& n = nodes[i];
    if (n.op == Op::Input || n.op == Op::Const) {
      // Inputs are never merged; constants are deduplicated by bit pattern on copy.
      const std::size_t before = rb.out().size();
      rb.copy(o);
      if (rb.out().size() == before) ++merged;
      continue;
    }
    CseKey key;
    key.op = n.op;
    if (op_is_variadic(n.op)) {
      for (node_id a : old.args(n)) key.args.push_back(rb.mapped(a));
      for (double c : old.coefs(n)) key.coefs.push_back(bits_of(c));
      key.kbits = bits_of(n.konst);
    } else {
      const int arity = op_arity(n.op);
      if (arity >= 1) key.a = rb.mapped(n.a);
      if (arity >= 2) key.b = rb.mapped(n.b);
      if (arity >= 3) key.c = rb.mapped(n.c);
      if (op_is_commutative(n.op) && key.b < key.a) std::swap(key.a, key.b);
    }
    auto it = seen.find(key);
    if (it != seen.end()) {
      rb.set(o, it->second);
      ++merged;
      continue;
    }
    const node_id id = rb.copy(o);
    seen.emplace(std::move(key), id);
  }
  return rb.finish(tape, merged);
}

// ---- dce -----------------------------------------------------------------------------------

PassResult dce(Tape& tape) {
  const Tape old = tape;
  std::vector<char> live(old.size(), 0);
  for (node_id o : old.outputs()) live[idx(o)] = 1;
  const auto& nodes = old.nodes();
  for (std::size_t i = nodes.size(); i-- > 0;) {
    const Node& n = nodes[i];
    if (n.op == Op::Input) live[i] = 1;  // ordinals stay stable
    if (!live[i]) continue;
    if (op_is_variadic(n.op)) {
      for (node_id a : old.args(n)) live[idx(a)] = 1;
    } else {
      const int arity = op_arity(n.op);
      if (arity >= 1) live[idx(n.a)] = 1;
      if (arity >= 2) live[idx(n.b)] = 1;
      if (arity >= 3) live[idx(n.c)] = 1;
    }
  }
  Rebuilder rb(old);
  std::size_t removed = 0;
  for (std::size_t i = 0; i < nodes.size(); ++i) {
    if (live[i]) {
      rb.copy(static_cast<node_id>(i));
    } else {
      ++removed;
    }
  }
  return rb.finish(tape, removed);
}

// ---- fold_sum ------------------------------------------------------------------------------

PassResult fold_sum(Tape& tape, FoldSumOptions options) {
  const int min_terms = options.min_terms < 2 ? 2 : options.min_terms;
  const Tape old = tape;
  const Uses uses = compute_uses(old);
  const auto& nodes = old.nodes();
  const std::size_t n_nodes = nodes.size();

  // Chain nodes are Add and Sum. The left slot of a chain node is `a` for Add and args[0] for
  // Sum. An Add or Sum is an inner chain node when its single use is the left slot of another
  // chain node: Sum(Sum(a, b), c) and (a + b) + c are both ((a + b) + c).
  auto is_chain_op = [&](node_id x) {
    const Op op = nodes[idx(x)].op;
    return op == Op::Add || op == Op::Sum;
  };
  auto left_of = [&](node_id x) {
    const Node& n = nodes[idx(x)];
    return n.op == Op::Sum ? old.args(n)[0] : n.a;
  };
  auto is_inner = [&](node_id x) {
    if (!is_chain_op(x) || uses.count[idx(x)] != 1) return false;
    const node_id u = uses.user[idx(x)];
    return u != invalid_node && is_chain_op(u) && left_of(u) == x;
  };

  std::vector<char> absorbed(n_nodes, 0);
  std::vector<std::vector<node_id>> chain(n_nodes);  // head -> addends in fold order
  for (std::size_t i = 0; i < n_nodes; ++i) {
    const node_id o = static_cast<node_id>(i);
    if (!is_chain_op(o) || is_inner(o)) continue;
    // o is a chain head: walk the left spine, collecting right-hand addends bottom-up later.
    std::vector<node_id> spine;
    for (node_id cur = o;;) {
      spine.push_back(cur);
      const node_id left = left_of(cur);
      if (!is_inner(left)) break;
      cur = left;
    }
    if (nodes[i].op == Op::Sum && spine.size() == 1) continue;  // already a flat Sum
    std::vector<node_id> terms;
    for (std::size_t s = spine.size(); s-- > 0;) {
      const Node& n = nodes[idx(spine[s])];
      if (n.op == Op::Sum) {
        const auto a = old.args(n);
        for (std::size_t k = (s + 1 == spine.size() ? 0 : 1); k < a.size(); ++k) terms.push_back(a[k]);
      } else {
        if (s + 1 == spine.size()) terms.push_back(n.a);
        terms.push_back(n.b);
      }
    }
    if (static_cast<int>(terms.size()) < min_terms) continue;
    chain[i] = std::move(terms);
    for (std::size_t s = 1; s < spine.size(); ++s) absorbed[idx(spine[s])] = 1;
  }

  Rebuilder rb(old);
  std::vector<node_id> args;
  std::size_t folded = 0;
  for (std::size_t i = 0; i < n_nodes; ++i) {
    const node_id o = static_cast<node_id>(i);
    if (absorbed[i]) {
      ++folded;
      continue;
    }
    if (!chain[i].empty()) {
      args.clear();
      for (node_id t : chain[i]) args.push_back(rb.mapped(t));
      rb.set(o, rb.out().variadic(Op::Sum, args));
      ++folded;
      continue;
    }
    rb.copy(o);
  }
  return rb.finish(tape, folded);
}

// ---- affine_collapse -----------------------------------------------------------------------

namespace {

struct AffineTerm {
  double coef;
  node_id atom;  // old id
};

struct AffineSpec {
  double konst = -0.0;  // -0.0 is the exact additive identity when there is no leading constant
  std::vector<AffineTerm> terms;
  std::vector<node_id> absorbed;  // spine nodes below the head and Mul/Neg term nodes
};

struct Addend {
  double sign;   // +1 or -1
  node_id node;  // old id
  std::size_t spine;  // index into the spine list (bottom = 0)
};

}  // namespace

PassResult affine_collapse(Tape& tape) {
  const Tape old = tape;
  const Uses uses = compute_uses(old);
  const auto& nodes = old.nodes();
  const std::size_t n_nodes = nodes.size();

  auto is_chain_op = [&](node_id x) {
    const Op op = nodes[idx(x)].op;
    return op == Op::Add || op == Op::Sub || op == Op::Sum;
  };
  // x is the left operand of the chain node `parent` and has no other use.
  auto is_inner = [&](node_id x, node_id parent) {
    if (!is_chain_op(x) || !is_chain_op(parent) || uses.count[idx(x)] != 1) return false;
    const Node& p = nodes[idx(parent)];
    return p.op == Op::Sum ? old.args(p)[0] == x : p.a == x;
  };

  std::vector<char> collapsed(n_nodes, 0);  // old node emitted as an Affine by this pass
  std::vector<char> absorbed(n_nodes, 0);
  std::vector<AffineSpec> spec(n_nodes);
  std::vector<char> has_spec(n_nodes, 0);

  // An atom the Affine node may read: an Input, an Affine (from this or an earlier run), or a
  // value that does not depend on inputs at all.
  auto atom_ok = [&](node_id m) {
    const Node& n = nodes[idx(m)];
    return n.op == Op::Input || n.op == Op::Affine || collapsed[idx(m)] || !n.tainted;
  };

  // Turns an addend into a term; returns false if it cannot be represented exactly. A scaled
  // (Mul by a Const) or negated (Neg) atom is taken as (coef, atom) whatever its use count: the
  // Affine computes the same product with the same rounding, so this is exact. The Mul/Neg node
  // itself is absorbed (dropped) only when this chain is its single use; a shared one is kept
  // for its other users (two interpolations at different times share a weight·knot product on
  // the M1 book) and disappears in the following dce once every user has collapsed.
  auto make_term = [&](const Addend& ad, AffineTerm& out, node_id& absorbed_node) {
    const Node& n = nodes[idx(ad.node)];
    absorbed_node = invalid_node;
    const bool single_use = uses.count[idx(ad.node)] == 1;
    if (!absorbed[idx(ad.node)] && n.op == Op::Mul) {
      const Node& l = nodes[idx(n.a)];
      const Node& r = nodes[idx(n.b)];
      if (l.op == Op::Const && r.op != Op::Const && atom_ok(n.b)) {
        out = {ad.sign * l.konst, n.b};
        if (single_use) absorbed_node = ad.node;
        return true;
      }
      if (r.op == Op::Const && l.op != Op::Const && atom_ok(n.a)) {
        out = {ad.sign * r.konst, n.a};
        if (single_use) absorbed_node = ad.node;
        return true;
      }
    }
    if (!absorbed[idx(ad.node)] && n.op == Op::Neg && atom_ok(n.a)) {
      out = {-ad.sign, n.a};
      if (single_use) absorbed_node = ad.node;
      return true;
    }
    if (atom_ok(ad.node)) {
      out = {ad.sign, ad.node};
      return true;
    }
    return false;
  };

  std::vector<node_id> spine;
  std::vector<Addend> addends;
  for (std::size_t i = 0; i < n_nodes; ++i) {
    const node_id head = static_cast<node_id>(i);
    if (!is_chain_op(head) || absorbed[i]) continue;
    // Is `head` itself the inner node of a later chain node? Then that node handles it.
    if (uses.count[i] == 1) {
      const node_id u = uses.user[i];
      if (u != invalid_node && is_inner(head, u)) continue;
    }

    // Flatten the left spine: spine[0] is the bottom node, spine.back() is the head.
    spine.clear();
    for (node_id cur = head;;) {
      spine.push_back(cur);
      const Node& n = nodes[idx(cur)];
      const node_id left = n.op == Op::Sum ? old.args(n)[0] : n.a;
      if (!is_inner(left, cur)) break;
      cur = left;
    }
    std::reverse(spine.begin(), spine.end());
    addends.clear();
    for (std::size_t s = 0; s < spine.size(); ++s) {
      const Node& n = nodes[idx(spine[s])];
      if (n.op == Op::Sum) {
        const auto a = old.args(n);
        for (std::size_t k = (s == 0 ? 0 : 1); k < a.size(); ++k) addends.push_back({1.0, a[k], s});
      } else {
        if (s == 0) addends.push_back({1.0, n.a, s});
        addends.push_back({n.op == Op::Sub ? -1.0 : 1.0, n.b, s});
      }
    }

    // The longest prefix of addends that can be represented exactly.
    AffineSpec sp;
    std::vector<node_id> absorbed_terms;
    std::size_t p = 0;
    bool any_tainted = false;
    for (; p < addends.size(); ++p) {
      const Addend& ad = addends[p];
      if (p == 0 && nodes[idx(ad.node)].op == Op::Const) {
        sp.konst = nodes[idx(ad.node)].konst;
        continue;
      }
      AffineTerm term;
      node_id abs_node;
      if (!make_term(ad, term, abs_node)) break;
      sp.terms.push_back(term);
      if (abs_node != invalid_node) absorbed_terms.push_back(abs_node);
      any_tainted = any_tainted || nodes[idx(term.atom)].tainted;
    }
    if (p < 2 || !any_tainted) continue;
    // Addends are listed spine by spine from the bottom, so spine s is fully inside the prefix
    // iff its last addend index is < p. Take the largest such s: everything above it stays.
    std::vector<std::size_t> last_of_spine(spine.size(), 0);
    for (std::size_t k = 0; k < addends.size(); ++k) last_of_spine[addends[k].spine] = k;
    if (last_of_spine[0] >= p) continue;  // even the bottom node is not fully representable
    std::size_t top = 0;
    while (top + 1 < spine.size() && last_of_spine[top + 1] < p) ++top;
    // Trim the prefix to the addends of spine[0..top].
    const std::size_t n_addends_kept = last_of_spine[top] + 1;
    if (n_addends_kept < 2) continue;
    const bool leading_const = nodes[idx(addends[0].node)].op == Op::Const;
    const std::size_t n_terms_kept = n_addends_kept - (leading_const ? 1 : 0);
    sp.terms.resize(n_terms_kept);
    bool kept_tainted = false;
    for (const AffineTerm& t : sp.terms) kept_tainted = kept_tainted || nodes[idx(t.atom)].tainted;
    if (!kept_tainted) continue;
    // Absorbed Mul/Neg nodes belong to kept addends only.
    absorbed_terms.clear();
    for (std::size_t k = 0; k < n_addends_kept; ++k) {
      const Addend& ad = addends[k];
      if (k == 0 && leading_const) continue;
      AffineTerm term;
      node_id abs_node;
      make_term(ad, term, abs_node);
      if (abs_node != invalid_node) absorbed_terms.push_back(abs_node);
    }

    const node_id target = spine[top];
    for (std::size_t s = 0; s < top; ++s) {
      absorbed[idx(spine[s])] = 1;
      sp.absorbed.push_back(spine[s]);
    }
    for (node_id m : absorbed_terms) {
      absorbed[idx(m)] = 1;
      sp.absorbed.push_back(m);
    }
    collapsed[idx(target)] = 1;
    has_spec[idx(target)] = 1;
    spec[idx(target)] = std::move(sp);
  }

  Rebuilder rb(old);
  std::vector<node_id> args;
  std::vector<double> coefs;
  std::size_t changed = 0;
  for (std::size_t i = 0; i < n_nodes; ++i) {
    const node_id o = static_cast<node_id>(i);
    if (absorbed[i]) {
      ++changed;
      continue;
    }
    if (has_spec[i]) {
      const AffineSpec& sp = spec[i];
      args.clear();
      coefs.clear();
      for (const AffineTerm& t : sp.terms) {
        args.push_back(rb.mapped(t.atom));
        coefs.push_back(t.coef);
      }
      rb.set(o, rb.out().variadic(Op::Affine, args, coefs, sp.konst));
      ++changed;
      continue;
    }
    rb.copy(o);
  }
  return rb.finish(tape, changed);
}

// ---- composition ---------------------------------------------------------------------------

namespace {

void compose(PassResult& total, const PassResult& step) {
  for (node_id& m : total.remap) {
    if (m != invalid_node) m = step.remap[idx(m)];
  }
  total.nodes_after = step.nodes_after;
  total.changed += step.changed;
}

}  // namespace

PassResult standard_passes(Tape& tape, FoldSumOptions fold) {
  PassResult total = cse(tape);
  compose(total, dce(tape));
  compose(total, fold_sum(tape, fold));
  compose(total, affine_collapse(tape));
  compose(total, dce(tape));
  return total;
}

}  // namespace epykos
