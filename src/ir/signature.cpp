#include "epykos/ir/signature.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "epykos/mutation/mutation.hpp"

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

// A 64-bit mixer (splitmix64 finaliser over a combined word).
uint64_t mix(uint64_t h, uint64_t v) noexcept {
  h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
  h ^= h >> 30;
  h *= 0xBF58476D1CE4E5B9ull;
  h ^= h >> 27;
  h *= 0x94D049BB133111EBull;
  h ^= h >> 31;
  return h;
}

constexpr uint64_t k_seed = 0x243F6A8885A308D3ull;

// ---- shapes -----------------------------------------------------------------------------

enum class Kind : std::uint8_t { None, Step, ConstSlot, RefSlot, Segment, Input };

struct ProtoOperand {
  Kind kind = Kind::None;
  std::int32_t index = -1;
  bool operator==(const ProtoOperand&) const = default;
};

struct ProtoStep {
  Op op = Op::Const;
  ProtoOperand a, b, c;
  ProtoOperand konst;  // Affine c_0 / Const-row value
  bool operator==(const ProtoStep&) const = default;
};

// One boundary's tree, extracted in canonical order.
struct Extract {
  std::vector<ProtoStep> proto;
  std::vector<std::uint8_t> swapped;  // per step: commutative operands were reordered
  std::vector<double> consts;         // constant slots in slot order
  std::vector<node_id> refs;          // reference slots in slot order
  void clear() {
    proto.clear();
    swapped.clear();
    consts.clear();
    refs.clear();
  }
};

// A signature class with its per-row data (node ids for references; converted to value ids
// once domains are laid out).
struct Class {
  uint64_t hash = 0;
  std::vector<ProtoStep> proto;
  std::vector<std::uint8_t> emit_swapped;  // first instance's recorded operand order
  std::int32_t n_const = 0;
  std::int32_t n_ref = 0;
  std::vector<node_id> rows;       // boundary node per row, tape order
  std::vector<double> const_vals;  // rows × n_const
  std::vector<node_id> ref_nodes;  // rows × n_ref
  // Variadic classes (Sum / Affine): members per row in fold order.
  std::vector<std::int32_t> seg_offsets{0};
  std::vector<node_id> seg_members;
  std::vector<double> seg_coefs;
  // Affine c_0 per row / Const-row value per row.
  std::vector<double> konst_vals;
};

struct Frame {
  node_id id = invalid_node;
  int arity = 0;
  int k = 0;
  std::array<node_id, 3> ops{invalid_node, invalid_node, invalid_node};
  std::array<ProtoOperand, 3> res{};
  int parent = -1;
  int parent_slot = -1;
  bool swapped = false;
};

class Inference {
 public:
  explicit Inference(const Tape& tape) : t_(tape), nodes_(tape.nodes()), n_(tape.size()) {}

  Program run(InferStats* stats);

 private:
  void compute_uses();
  void initial_boundaries();
  void compute_deep();
  void promote_shared();
  uint64_t token(node_id c) const noexcept {
    if (is_const_[idx(c)]) return merge_classes_ ? tok_ref_ : tok_const_;  // mutant: a constant slot hashes as a reference
    if (boundary_[idx(c)]) return tok_ref_;
    return h_[idx(c)];
  }
  void hash_all();
  void extract_tree(node_id root, Extract& out) const;
  std::int32_t class_for(const Extract& ex, uint64_t hash);
  std::int32_t const_row(node_id c);
  void partition();
  void levels_and_domains();
  Program assemble();

  const Tape& t_;
  const std::vector<Node>& nodes_;
  size_t n_;

  std::vector<std::int32_t> use_count_;
  std::vector<std::uint8_t> member_use_;  // variadic operand or registered output
  std::vector<std::uint8_t> is_const_;
  std::vector<std::uint8_t> boundary_;
  std::vector<uint64_t> deep_;  // expression hash through shared nodes (boundary-independent)
  std::vector<uint64_t> h_;     // local tree hash (stops at boundaries)
  uint64_t tok_const_ = mix(k_seed, 1);
  uint64_t tok_ref_ = mix(k_seed, 2);
  // Mutant signature.merge_classes: the const-slot pattern is not part of the signature. A
  // constant slot hashes like a reference (token) and two trees with the same hash and the same
  // slot counts are one class (class_for), so sub($, @) and sub(@, $) share a class and the
  // second shape is emitted with the first's operand order.
  const bool merge_classes_ = mutant("signature.merge_classes");

  std::vector<Class> classes_;
  std::unordered_map<uint64_t, std::vector<std::int32_t>> class_by_hash_;
  std::vector<std::int32_t> class_of_;  // per node; -1 for inner / const leaves
  std::vector<std::int32_t> row_of_;    // row within class
  std::int32_t const_class_ = -1;
  std::unordered_map<node_id, std::int32_t> const_rows_;  // Const node -> row in const class

  // Domains: (class, level) in creation order.
  struct DomainKey {
    std::int32_t cls;
    std::int32_t level;
    bool operator<(const DomainKey& o) const noexcept {
      return cls != o.cls ? cls < o.cls : level < o.level;
    }
  };
  std::vector<std::int32_t> level_of_;      // per node (rows only)
  std::vector<std::uint8_t> class_recurrent_;
  std::vector<std::int32_t> scc_of_;        // per class
  std::vector<std::uint8_t> scc_nontrivial_;
  std::map<DomainKey, std::int32_t> domain_index_;
  std::vector<DomainKey> domain_key_;
  std::vector<std::vector<node_id>> domain_rows_;  // node per row, tape order
  std::vector<node_id> domain_first_;              // first row node (creation order)
  std::vector<std::int32_t> domain_of_node_;
  std::vector<std::int32_t> row_in_domain_;
  std::vector<std::int32_t> domain_order_;         // evaluation order -> domain index
  std::vector<value_id> value_of_node_;

  InferStats stats_;
};

void Inference::compute_uses() {
  use_count_.assign(n_, 0);
  member_use_.assign(n_, 0);
  for (size_t i = 0; i < n_; ++i) {
    const Node& nd = nodes_[i];
    if (op_is_variadic(nd.op)) {
      for (node_id a : t_.args(nd)) {
        ++use_count_[idx(a)];
        member_use_[idx(a)] = 1;
      }
    } else {
      const int arity = op_arity(nd.op);
      if (arity >= 1) ++use_count_[idx(nd.a)];
      if (arity >= 2) ++use_count_[idx(nd.b)];
      if (arity >= 3) ++use_count_[idx(nd.c)];
    }
  }
  for (node_id o : t_.outputs()) {
    ++use_count_[idx(o)];
    member_use_[idx(o)] = 1;
  }
}

void Inference::initial_boundaries() {
  is_const_.assign(n_, 0);
  boundary_.assign(n_, 0);
  for (size_t i = 0; i < n_; ++i) {
    const Node& nd = nodes_[i];
    if (nd.op == Op::Const) {
      is_const_[i] = 1;
      continue;
    }
    const bool structural = nd.op == Op::Input || op_is_variadic(nd.op);
    boundary_[i] = (structural || use_count_[i] != 1 || member_use_[i]) ? 1 : 0;
  }
}

void Inference::hash_all() {
  h_.assign(n_, 0);
  for (size_t i = 0; i < n_; ++i) {
    const Node& nd = nodes_[i];
    uint64_t h = mix(k_seed, static_cast<uint64_t>(nd.op) + 0x100);
    if (nd.op == Op::Const || nd.op == Op::Input || op_is_variadic(nd.op)) {
      h_[i] = h;
      continue;
    }
    const int arity = op_arity(nd.op);
    uint64_t ta = arity >= 1 ? token(nd.a) : 0;
    uint64_t tb = arity >= 2 ? token(nd.b) : 0;
    const uint64_t tc = arity >= 3 ? token(nd.c) : 0;
    if (op_is_commutative(nd.op) && tb < ta) std::swap(ta, tb);
    if (arity >= 1) h = mix(h, ta);
    if (arity >= 2) h = mix(h, tb);
    if (arity >= 3) h = mix(h, tc);
    h_[i] = h;
  }
}

// The deep hash of a node: its expression modulo constants, seen through every fixed-arity
// node whether shared or not, stopping at Inputs (all alike), Sums and Affines (their operands
// are segment members) and Const leaves. Two nodes with the same deep hash are the same
// computation on different data; it does not depend on which nodes are boundaries.
void Inference::compute_deep() {
  deep_.assign(n_, 0);
  for (size_t i = 0; i < n_; ++i) {
    const Node& nd = nodes_[i];
    uint64_t h = mix(k_seed, static_cast<uint64_t>(nd.op) + 0x200);
    if (nd.op == Op::Const) {
      deep_[i] = tok_const_;
      continue;
    }
    if (nd.op == Op::Input || op_is_variadic(nd.op)) {
      deep_[i] = h;
      continue;
    }
    const int arity = op_arity(nd.op);
    uint64_t ta = arity >= 1 ? deep_[idx(nd.a)] : 0;
    uint64_t tb = arity >= 2 ? deep_[idx(nd.b)] : 0;
    const uint64_t tc = arity >= 3 ? deep_[idx(nd.c)] : 0;
    if (op_is_commutative(nd.op) && tb < ta) std::swap(ta, tb);
    if (arity >= 1) h = mix(h, ta);
    if (arity >= 2) h = mix(h, tb);
    if (arity >= 3) h = mix(h, tc);
    deep_[i] = h;
  }
}

// The sharing rule, applied per computation rather than per node: when any instance of a deep
// class is a boundary (shared, a member, an output), every instance is materialised, so that
// the consumers of shared and unshared instances alike see a reference. One pass suffices: the
// deep hashes do not depend on the boundary set.
void Inference::promote_shared() {
  stats_.boundary_rounds = 1;
  stats_.class_promoted = 0;
  std::unordered_set<uint64_t> shared;
  for (size_t i = 0; i < n_; ++i) {
    if (boundary_[i]) shared.insert(deep_[i]);
  }
  for (size_t i = 0; i < n_; ++i) {
    if (boundary_[i] || is_const_[i]) continue;
    if (shared.count(deep_[i]) != 0) {
      boundary_[i] = 1;
      ++stats_.class_promoted;
    }
  }
}

void Inference::extract_tree(node_id root, Extract& out) const {
  out.clear();
  std::vector<Frame> stack;
  auto open = [&](node_id id, int parent, int parent_slot) {
    const Node& nd = nodes_[idx(id)];
    Frame f;
    f.id = id;
    f.arity = op_arity(nd.op);
    f.parent = parent;
    f.parent_slot = parent_slot;
    f.ops = {nd.a, nd.b, nd.c};
    if (op_is_commutative(nd.op) && token(nd.b) < token(nd.a)) {
      std::swap(f.ops[0], f.ops[1]);
      f.swapped = true;
    }
    stack.push_back(f);
  };
  open(root, -1, -1);
  while (!stack.empty()) {
    const size_t top = stack.size() - 1;
    Frame& f = stack[top];
    if (f.k < f.arity) {
      const int slot = f.k++;
      const node_id c = f.ops[static_cast<size_t>(slot)];
      if (is_const_[idx(c)]) {
        f.res[static_cast<size_t>(slot)] = {Kind::ConstSlot, static_cast<std::int32_t>(out.consts.size())};
        out.consts.push_back(nodes_[idx(c)].konst);
      } else if (boundary_[idx(c)]) {
        f.res[static_cast<size_t>(slot)] = {Kind::RefSlot, static_cast<std::int32_t>(out.refs.size())};
        out.refs.push_back(c);
      } else {
        open(c, static_cast<int>(top), slot);  // invalidates f
      }
      continue;
    }
    ProtoStep s;
    s.op = nodes_[idx(f.id)].op;
    s.a = f.res[0];
    s.b = f.res[1];
    s.c = f.res[2];
    const std::int32_t step_index = static_cast<std::int32_t>(out.proto.size());
    out.proto.push_back(s);
    out.swapped.push_back(f.swapped ? 1 : 0);
    const int parent = f.parent;
    const int pslot = f.parent_slot;
    stack.pop_back();
    if (parent >= 0) stack[static_cast<size_t>(parent)].res[static_cast<size_t>(pslot)] = {Kind::Step, step_index};
  }
}

std::int32_t Inference::class_for(const Extract& ex, uint64_t hash) {
  std::vector<std::int32_t>& cands = class_by_hash_[hash];
  for (std::int32_t c : cands) {
    if (classes_[idx(c)].proto == ex.proto) return c;
    if (merge_classes_ && classes_[idx(c)].n_const == static_cast<std::int32_t>(ex.consts.size()) && classes_[idx(c)].n_ref == static_cast<std::int32_t>(ex.refs.size())) return c;
  }
  Class cl;
  cl.hash = hash;
  cl.proto = ex.proto;
  cl.emit_swapped = ex.swapped;
  cl.n_const = static_cast<std::int32_t>(ex.consts.size());
  cl.n_ref = static_cast<std::int32_t>(ex.refs.size());
  classes_.push_back(std::move(cl));
  const std::int32_t c = static_cast<std::int32_t>(classes_.size()) - 1;
  cands.push_back(c);
  return c;
}

// A Const node used as a Sum / Affine member or registered as an output is a row of the const
// class (one Const step reading its value slot).
std::int32_t Inference::const_row(node_id c) {
  auto it = const_rows_.find(c);
  if (it != const_rows_.end()) return it->second;
  if (const_class_ < 0) {
    Extract ex;
    ProtoStep s;
    s.op = Op::Const;
    s.konst = {Kind::ConstSlot, 0};
    ex.proto.push_back(s);
    ex.swapped.push_back(0);
    const_class_ = class_for(ex, mix(k_seed, static_cast<uint64_t>(Op::Const) + 0x100));
    classes_[idx(const_class_)].n_const = 0;
  }
  Class& cl = classes_[idx(const_class_)];
  const std::int32_t row = static_cast<std::int32_t>(cl.rows.size());
  cl.rows.push_back(c);
  cl.konst_vals.push_back(nodes_[idx(c)].konst);
  class_of_[idx(c)] = const_class_;
  row_of_[idx(c)] = row;
  const_rows_.emplace(c, row);
  return row;
}

void Inference::partition() {
  class_of_.assign(n_, -1);
  row_of_.assign(n_, -1);
  Extract ex;
  stats_.inner = 0;
  stats_.boundaries = 0;
  for (size_t i = 0; i < n_; ++i) {
    if (is_const_[i]) continue;
    if (!boundary_[i]) {
      ++stats_.inner;
      continue;
    }
    const node_id root = static_cast<node_id>(i);
    const Node& nd = nodes_[i];
    std::int32_t c;
    if (nd.op == Op::Input) {
      ex.clear();
      ProtoStep s;
      s.op = Op::Input;
      s.a = {Kind::Input, -1};
      ex.proto.push_back(s);
      ex.swapped.push_back(0);
      c = class_for(ex, h_[i]);
    } else if (op_is_variadic(nd.op)) {
      ex.clear();
      ProtoStep s;
      s.op = nd.op;
      s.a = {Kind::Segment, 0};
      if (nd.op == Op::Affine) s.konst = {Kind::ConstSlot, 0};
      ex.proto.push_back(s);
      ex.swapped.push_back(0);
      c = class_for(ex, h_[i]);
      // Members: boundaries (rows already assigned: they are earlier nodes) or Const rows. The
      // const rows are created first: const_row may add a class, which moves classes_.
      for (node_id a : t_.args(nd)) {
        if (is_const_[idx(a)]) const_row(a);
      }
      Class& cl = classes_[idx(c)];
      cl.n_const = 0;
      for (node_id a : t_.args(nd)) cl.seg_members.push_back(a);
      cl.seg_offsets.push_back(static_cast<std::int32_t>(cl.seg_members.size()));
      if (nd.op == Op::Affine) {
        const auto coefs = t_.coefs(nd);
        cl.seg_coefs.insert(cl.seg_coefs.end(), coefs.begin(), coefs.end());
        cl.konst_vals.push_back(nd.konst);
      }
    } else {
      extract_tree(root, ex);
      c = class_for(ex, h_[i]);
      Class& cl = classes_[idx(c)];
      cl.const_vals.insert(cl.const_vals.end(), ex.consts.begin(), ex.consts.end());
      cl.ref_nodes.insert(cl.ref_nodes.end(), ex.refs.begin(), ex.refs.end());
    }
    Class& cl = classes_[idx(c)];
    class_of_[i] = c;
    row_of_[i] = static_cast<std::int32_t>(cl.rows.size());
    cl.rows.push_back(root);
    ++stats_.boundaries;
  }
  // Outputs that are Const nodes are rows of the const class too.
  for (node_id o : t_.outputs()) {
    if (is_const_[idx(o)]) const_row(o);
  }
  stats_.boundaries = 0;
  for (const Class& cl : classes_) stats_.boundaries += cl.rows.size();
  stats_.const_leaves = 0;
  for (size_t i = 0; i < n_; ++i) stats_.const_leaves += (is_const_[i] && class_of_[i] < 0);
  stats_.classes = classes_.size();
}

// Class dependency graph -> SCCs (iterative Tarjan) -> per-row level inside a non-trivial SCC
// -> domains (class, level) in order of first appearance.
void Inference::levels_and_domains() {
  const size_t n_classes = classes_.size();
  std::vector<std::vector<std::int32_t>> edges(n_classes);
  class_recurrent_.assign(n_classes, 0);
  {
    std::vector<std::unordered_set<std::int32_t>> seen(n_classes);
    for (size_t c = 0; c < n_classes; ++c) {
      const Class& cl = classes_[c];
      auto add = [&](node_id target) {
        const std::int32_t tc = class_of_[idx(target)];
        if (tc == static_cast<std::int32_t>(c)) class_recurrent_[c] = 1;
        if (seen[c].insert(tc).second) edges[c].push_back(tc);
      };
      for (node_id r : cl.ref_nodes) add(r);
      for (node_id m : cl.seg_members) add(m);
    }
  }
  // Tarjan, iterative.
  scc_of_.assign(n_classes, -1);
  std::vector<std::int32_t> index(n_classes, -1), low(n_classes, 0);
  std::vector<std::uint8_t> on_stack(n_classes, 0);
  std::vector<std::int32_t> st;
  std::vector<std::int32_t> scc_size;
  std::int32_t next_index = 0;
  struct Call {
    std::int32_t v;
    size_t edge;
  };
  for (size_t root = 0; root < n_classes; ++root) {
    if (index[root] >= 0) continue;
    std::vector<Call> calls;
    calls.push_back({static_cast<std::int32_t>(root), 0});
    index[root] = low[root] = next_index++;
    st.push_back(static_cast<std::int32_t>(root));
    on_stack[root] = 1;
    while (!calls.empty()) {
      Call& cur = calls.back();
      const std::int32_t v = cur.v;
      if (cur.edge < edges[idx(v)].size()) {
        const std::int32_t w = edges[idx(v)][cur.edge++];
        if (index[idx(w)] < 0) {
          index[idx(w)] = low[idx(w)] = next_index++;
          st.push_back(w);
          on_stack[idx(w)] = 1;
          calls.push_back({w, 0});
        } else if (on_stack[idx(w)]) {
          low[idx(v)] = std::min(low[idx(v)], index[idx(w)]);
        }
        continue;
      }
      if (low[idx(v)] == index[idx(v)]) {
        const std::int32_t id = static_cast<std::int32_t>(scc_size.size());
        std::int32_t size = 0;
        for (;;) {
          const std::int32_t w = st.back();
          st.pop_back();
          on_stack[idx(w)] = 0;
          scc_of_[idx(w)] = id;
          ++size;
          if (w == v) break;
        }
        scc_size.push_back(size);
      }
      calls.pop_back();
      if (!calls.empty()) {
        const std::int32_t parent = calls.back().v;
        low[idx(parent)] = std::min(low[idx(parent)], low[idx(v)]);
      }
    }
  }
  scc_nontrivial_.assign(scc_size.size(), 0);
  for (size_t c = 0; c < n_classes; ++c) {
    if (scc_size[idx(scc_of_[c])] > 1 || class_recurrent_[c]) scc_nontrivial_[idx(scc_of_[c])] = 1;
  }

  // Levels: rows in tape order (every read is an earlier node, so one pass is exact); only
  // reads inside the same non-trivial SCC count.
  level_of_.assign(n_, 0);
  for (size_t i = 0; i < n_; ++i) {
    const std::int32_t c = class_of_[i];
    if (c < 0 || !scc_nontrivial_[idx(scc_of_[idx(c)])]) continue;
    const Class& cl = classes_[idx(c)];
    const size_t r = idx(row_of_[i]);
    std::int32_t lvl = 0;
    auto consider = [&](node_id target) {
      if (scc_of_[idx(class_of_[idx(target)])] != scc_of_[idx(c)]) return;
      lvl = std::max(lvl, level_of_[idx(target)] + 1);
    };
    for (std::int32_t k = 0; k < cl.n_ref; ++k) consider(cl.ref_nodes[r * idx(cl.n_ref) + idx(k)]);
    if (!cl.seg_offsets.empty() && r + 1 < cl.seg_offsets.size()) {
      for (std::int32_t m = cl.seg_offsets[r]; m < cl.seg_offsets[r + 1]; ++m) consider(cl.seg_members[idx(m)]);
    }
    level_of_[i] = lvl;
  }

  // Domains in order of first appearance (tape order of their first row).
  domain_of_node_.assign(n_, -1);
  row_in_domain_.assign(n_, -1);
  for (size_t i = 0; i < n_; ++i) {
    const std::int32_t c = class_of_[i];
    if (c < 0) continue;
    const DomainKey key{c, level_of_[i]};
    auto it = domain_index_.find(key);
    if (it == domain_index_.end()) {
      const std::int32_t d = static_cast<std::int32_t>(domain_key_.size());
      it = domain_index_.emplace(key, d).first;
      domain_key_.push_back(key);
      domain_rows_.emplace_back();
      domain_first_.push_back(static_cast<node_id>(i));
    }
    const std::int32_t d = it->second;
    domain_of_node_[i] = d;
    row_in_domain_[i] = static_cast<std::int32_t>(domain_rows_[idx(d)].size());
    domain_rows_[idx(d)].push_back(static_cast<node_id>(i));
  }
  stats_.domains = domain_key_.size();

  // Evaluation order: topological over domain reads, ties by first appearance.
  const size_t n_dom = domain_key_.size();
  std::vector<std::vector<std::int32_t>> readers(n_dom);  // d -> domains that read d
  std::vector<std::int32_t> pending(n_dom, 0);
  {
    std::vector<std::unordered_set<std::int32_t>> seen(n_dom);
    for (size_t d = 0; d < n_dom; ++d) {
      const Class& cl = classes_[idx(domain_key_[d].cls)];
      auto add = [&](node_id target) {
        const std::int32_t td = domain_of_node_[idx(target)];
        if (td == static_cast<std::int32_t>(d)) return;  // a recurrent class reads earlier rows of itself
        if (seen[d].insert(td).second) {
          readers[idx(td)].push_back(static_cast<std::int32_t>(d));
          ++pending[d];
        }
      };
      for (node_id r : domain_rows_[d]) {
        const size_t row = idx(row_of_[idx(r)]);
        for (std::int32_t k = 0; k < cl.n_ref; ++k) add(cl.ref_nodes[row * idx(cl.n_ref) + idx(k)]);
        if (row + 1 < cl.seg_offsets.size()) {
          for (std::int32_t m = cl.seg_offsets[row]; m < cl.seg_offsets[row + 1]; ++m) add(cl.seg_members[idx(m)]);
        }
      }
    }
  }
  domain_order_.clear();
  std::vector<std::int32_t> ready;
  for (size_t d = 0; d < n_dom; ++d) {
    if (pending[d] == 0) ready.push_back(static_cast<std::int32_t>(d));
  }
  auto by_first = [&](std::int32_t a, std::int32_t b) { return domain_first_[idx(a)] > domain_first_[idx(b)]; };
  std::make_heap(ready.begin(), ready.end(), by_first);
  while (!ready.empty()) {
    std::pop_heap(ready.begin(), ready.end(), by_first);
    const std::int32_t d = ready.back();
    ready.pop_back();
    domain_order_.push_back(d);
    for (std::int32_t r : readers[idx(d)]) {
      if (--pending[idx(r)] == 0) {
        ready.push_back(r);
        std::push_heap(ready.begin(), ready.end(), by_first);
      }
    }
  }
  if (domain_order_.size() != n_dom) {
    std::string msg = "ir::infer: domain dependency graph is not acyclic after level splitting; pending:";
    for (size_t d = 0; d < n_dom; ++d) {
      if (pending[d] == 0) continue;
      const DomainKey key = domain_key_[d];
      msg += " [d" + std::to_string(d) + " class " + std::to_string(key.cls) + " op " +
             to_string(classes_[idx(key.cls)].proto.back().op) + " level " + std::to_string(key.level) + " scc " +
             std::to_string(scc_of_[idx(key.cls)]) + " rows " + std::to_string(domain_rows_[d].size()) + " first #" +
             std::to_string(domain_first_[d]) + " pending " + std::to_string(pending[d]) + " reads";
      const Class& cl = classes_[idx(key.cls)];
      for (node_id r : domain_rows_[d]) {
        const size_t row = idx(row_of_[idx(r)]);
        for (std::int32_t k = 0; k < cl.n_ref; ++k) {
          const node_id tg = cl.ref_nodes[row * idx(cl.n_ref) + idx(k)];
          msg += " #" + std::to_string(tg) + "(d" + std::to_string(domain_of_node_[idx(tg)]) + ")";
        }
        if (row + 1 < cl.seg_offsets.size()) {
          for (std::int32_t m = cl.seg_offsets[row]; m < cl.seg_offsets[row + 1]; ++m) {
            const node_id tg = cl.seg_members[idx(m)];
            msg += " #" + std::to_string(tg) + "(d" + std::to_string(domain_of_node_[idx(tg)]) + ")";
          }
        }
      }
      msg += "]";
    }
    throw std::logic_error(msg);
  }
  // Value ids.
  value_of_node_.assign(n_, invalid_value);
  value_id base = 0;
  for (std::int32_t d : domain_order_) {
    for (node_id r : domain_rows_[idx(d)]) value_of_node_[idx(r)] = base + row_in_domain_[idx(r)];
    base += static_cast<value_id>(domain_rows_[idx(d)].size());
  }
}

Program Inference::assemble() {
  Program p;
  std::unordered_map<uint64_t, std::int32_t> literal_index;
  auto literal = [&](double v) {
    const uint64_t b = bits_of(v);
    auto it = literal_index.find(b);
    if (it != literal_index.end()) return it->second;
    const std::int32_t i = static_cast<std::int32_t>(p.literals.size());
    p.literals.push_back(v);
    literal_index.emplace(b, i);
    return i;
  };
  // A constant slot over the rows of one domain: literal when one bit pattern, else a column.
  auto const_slot = [&](std::int32_t d, const std::vector<double>& vals) {
    bool uniform = true;
    for (size_t r = 1; r < vals.size(); ++r) {
      if (bits_of(vals[r]) != bits_of(vals[0])) {
        uniform = false;
        break;
      }
    }
    if (uniform && !vals.empty()) return Slot{SlotKind::Literal, literal(vals[0])};
    Column col;
    col.domain = d;
    col.values = vals;
    p.columns.push_back(std::move(col));
    return Slot{SlotKind::Column, static_cast<std::int32_t>(p.columns.size()) - 1};
  };

  value_id base = 0;
  for (size_t pos = 0; pos < domain_order_.size(); ++pos) {
    const std::int32_t d_src = domain_order_[pos];
    const std::int32_t d = static_cast<std::int32_t>(pos);
    const DomainKey key = domain_key_[idx(d_src)];
    const Class& cl = classes_[idx(key.cls)];
    const std::vector<node_id>& rows = domain_rows_[idx(d_src)];
    const size_t n_rows = rows.size();

    Domain dom;
    dom.rows = static_cast<std::int32_t>(n_rows);
    dom.value_base = base;
    dom.level = key.level;
    dom.scan_class = class_recurrent_[idx(key.cls)] != 0;  // the class-level fact (D23)
    base += dom.rows;

    // Per-slot data over this domain's rows.
    std::vector<Slot> const_slots(idx(cl.n_const));
    std::vector<double> vals(n_rows);
    for (std::int32_t k = 0; k < cl.n_const; ++k) {
      for (size_t r = 0; r < n_rows; ++r) {
        vals[r] = cl.const_vals[idx(row_of_[idx(rows[r])]) * idx(cl.n_const) + idx(k)];
      }
      const_slots[idx(k)] = const_slot(d, vals);
    }
    std::vector<Slot> ref_slots(idx(cl.n_ref));
    std::unordered_set<std::int32_t> reads;
    for (std::int32_t k = 0; k < cl.n_ref; ++k) {
      Gather g;
      g.domain = d;
      g.index.resize(n_rows);
      for (size_t r = 0; r < n_rows; ++r) {
        const node_id target = cl.ref_nodes[idx(row_of_[idx(rows[r])]) * idx(cl.n_ref) + idx(k)];
        g.index[r] = value_of_node_[idx(target)];
        reads.insert(domain_of_node_[idx(target)]);
      }
      p.gathers.push_back(std::move(g));
      ref_slots[idx(k)] = Slot{SlotKind::Gather, static_cast<std::int32_t>(p.gathers.size()) - 1};
    }
    Slot segment_slot;
    Slot konst_slot;
    const Op op = cl.proto.back().op;
    if (op == Op::Sum || op == Op::Affine) {
      Segment seg;
      seg.domain = d;
      seg.offsets.push_back(0);
      for (size_t r = 0; r < n_rows; ++r) {
        const size_t row = idx(row_of_[idx(rows[r])]);
        for (std::int32_t m = cl.seg_offsets[row]; m < cl.seg_offsets[row + 1]; ++m) {
          const node_id target = cl.seg_members[idx(m)];
          seg.members.push_back(value_of_node_[idx(target)]);
          reads.insert(domain_of_node_[idx(target)]);
          if (op == Op::Affine) seg.coefs.push_back(cl.seg_coefs[idx(m)]);
        }
        seg.offsets.push_back(static_cast<std::int32_t>(seg.members.size()));
      }
      p.segments.push_back(std::move(seg));
      segment_slot = Slot{SlotKind::Segment, static_cast<std::int32_t>(p.segments.size()) - 1};
      if (op == Op::Affine) {
        for (size_t r = 0; r < n_rows; ++r) vals[r] = cl.konst_vals[idx(row_of_[idx(rows[r])])];
        konst_slot = const_slot(d, vals);
      }
    } else if (op == Op::Const) {
      for (size_t r = 0; r < n_rows; ++r) vals[r] = cl.konst_vals[idx(row_of_[idx(rows[r])])];
      konst_slot = const_slot(d, vals);
    }

    Group g;
    g.domain = d;
    auto to_slot = [&](const ProtoOperand& po) {
      switch (po.kind) {
        case Kind::Step: return Slot{SlotKind::Step, po.index};
        case Kind::ConstSlot: return const_slots[idx(po.index)];
        case Kind::RefSlot: return ref_slots[idx(po.index)];
        case Kind::Segment: return segment_slot;
        case Kind::Input: return Slot{SlotKind::Input, -1};
        default: return Slot{};
      }
    };
    for (size_t k = 0; k < cl.proto.size(); ++k) {
      const ProtoStep& ps = cl.proto[k];
      Step s;
      s.op = ps.op;
      s.a = to_slot(ps.a);
      s.b = to_slot(ps.b);
      s.c = to_slot(ps.c);
      if (ps.konst.kind == Kind::ConstSlot) s.konst = konst_slot;
      if (cl.emit_swapped[k]) std::swap(s.a, s.b);
      g.steps.push_back(s);
    }
    dom.reads.assign(reads.begin(), reads.end());
    // Per domain: do this domain's rows read this domain? Never after level splitting (a row of
    // a self-reading class reads rows of a lower level, i.e. another domain), but computed, not
    // copied from the class (D23).
    dom.recurrent = reads.count(d_src) != 0;
    // reads are source-domain ids in creation order; convert to evaluation positions.
    p.domains.push_back(std::move(dom));

    p.groups.push_back(std::move(g));
  }
  // Domain ids in `reads` were creation ids; map to positions.
  std::vector<std::int32_t> position(domain_order_.size(), -1);
  for (size_t pos = 0; pos < domain_order_.size(); ++pos) position[idx(domain_order_[pos])] = static_cast<std::int32_t>(pos);
  for (Domain& dom : p.domains) {
    for (std::int32_t& r : dom.reads) r = position[idx(r)];
    std::sort(dom.reads.begin(), dom.reads.end());
  }
  for (size_t d = 0; d < p.domains.size(); ++d) {
    p.domains[d].name = shape_string(p, static_cast<domain_id>(d));
    if (scc_nontrivial_[idx(scc_of_[idx(domain_key_[idx(domain_order_[d])].cls)])]) {
      p.domains[d].name += "@L" + std::to_string(p.domains[d].level);
    }
  }
  // Inputs and outputs.
  for (node_id in : t_.inputs()) {
    p.inputs.push_back(value_of_node_[idx(in)]);
    p.input_values.push_back(nodes_[idx(in)].konst);
  }
  for (node_id o : t_.outputs()) p.outputs.push_back(value_of_node_[idx(o)]);
  return p;
}

Program Inference::run(InferStats* stats) {
  t_.validate();
  for (const Node& nd : nodes_) {
    if (!op_is_supported(nd.op)) {
      throw std::invalid_argument(std::string("ir::infer: unsupported op ") + to_string(nd.op));
    }
  }
  stats_ = InferStats{};
  stats_.nodes = n_;
  compute_uses();
  initial_boundaries();
  compute_deep();
  promote_shared();
  hash_all();
  partition();
  levels_and_domains();
  Program p = assemble();
  validate(p);
  if (stats != nullptr) *stats = stats_;
  return p;
}

}  // namespace

Program infer(const Tape& tape, InferStats* stats) {
  Inference inf(tape);
  return inf.run(stats);
}

}  // namespace epykos::ir
