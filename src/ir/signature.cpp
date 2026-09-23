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

// ---- scan detection parameters (D41) ------------------------------------------------------

// A chain needs at least this many steps: two identical steps in a row are straight-line code
// (x·a·b with a, b references would otherwise be a two-step scan).
constexpr int scan_min_steps = 3;
// The carry is looked for at most this deep in a step's tree (acc·g: 1; a·x + b: 2).
constexpr int scan_max_depth = 4;
// A tree bigger than this is not a scan step (the raw recording's leg add trees): the tree of
// a chain's second step contains the first step expanded, so two steps plus the initial value's
// tree must fit (an interpolated compounding step on the raw recording is ~32 entries).
constexpr int scan_max_tree = 256;

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
  std::int32_t carry_ref = -1;        // scan step: the ref slot that is the carry
  void clear() {
    proto.clear();
    swapped.clear();
    consts.clear();
    refs.clear();
    carry_ref = -1;
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
  std::int32_t carry_slot = -1;    // scan class: the ref slot that reads the carry
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

// A chain the scan detection found: members in step order, the initial value, the step hash
// and the carry's position in the step (the operand path from the root, in recorded order).
struct Chain {
  uint64_t hash = 0;
  std::uint32_t path = 0;
  node_id init = invalid_node;
  std::vector<node_id> members;
};

// Thrown when a scan class cannot be laid out as one domain (its step reads a class that reads
// the scan, or a row reads a later row after the chain-major reordering): infer() bans every
// node of its chains from any chain and runs the inference again, and the class is split by
// level as before (D23).
struct ScanRetry {
  std::vector<node_id> nodes;
  std::string reason;
};

class Inference {
 public:
  Inference(const Tape& tape, const std::unordered_set<node_id>& banned)
      : t_(tape), nodes_(tape.nodes()), n_(tape.size()), banned_(banned) {}

  Program run(InferStats* stats);

 private:
  void compute_uses();
  void initial_boundaries();
  void compute_deep();
  void promote_shared();
  void detect_chains();
  void hash_all();
  void extract_tree(node_id root, Extract& out) const;
  std::int32_t class_for(const Extract& ex, uint64_t hash);
  std::int32_t const_row(node_id c);
  void partition();
  void levels_and_domains();
  Program assemble();

  // Operands of node i as the extraction sees them: a fixed-arity op's a, b, c or a fixed-arity
  // Sum's members (a scan step's Sum, D41).
  int arity_of(node_id i) const noexcept {
    return fixed_sum_[idx(i)] ? nodes_[idx(i)].nargs : op_arity(nodes_[idx(i)].op);
  }
  node_id operand_of(node_id i, int j) const noexcept {
    const Node& nd = nodes_[idx(i)];
    if (fixed_sum_[idx(i)]) return t_.args(nd)[idx(j)];
    return j == 0 ? nd.a : j == 1 ? nd.b : nd.c;
  }
  uint64_t token(node_id c) const noexcept {
    if (is_const_[idx(c)]) return merge_classes_ ? tok_ref_ : tok_const_;  // mutant: a constant slot hashes as a reference
    if (boundary_[idx(c)]) return tok_ref_;
    return h_[idx(c)];
  }
  // The token of operand j of node i: a Const that is a chain's initial value is a reference
  // (a row of the const domain), so the chain's first step hashes like every other step.
  uint64_t token_at(node_id i, int j, node_id c) const noexcept {
    if (is_const_[idx(c)] && const_init_slot_[idx(i)] == j) return tok_ref_;
    return token(c);
  }
  bool const_init_at(node_id i, int j, node_id c) const noexcept {
    return is_const_[idx(c)] && const_init_slot_[idx(i)] == j;
  }

  const Tape& t_;
  const std::vector<Node>& nodes_;
  size_t n_;
  const std::unordered_set<node_id>& banned_;

  std::vector<std::int32_t> use_count_;
  std::vector<std::uint8_t> member_use_;  // variadic operand or registered output
  std::vector<std::uint8_t> is_const_;
  std::vector<std::uint8_t> boundary_;
  std::vector<std::uint8_t> promoted_;    // made a boundary by the sharing rule
  std::vector<uint64_t> deep_;  // expression hash through shared nodes (boundary-independent)
  std::vector<uint64_t> h_;     // local tree hash (stops at boundaries)
  uint64_t tok_const_ = mix(k_seed, 1);
  uint64_t tok_ref_ = mix(k_seed, 2);
  uint64_t tok_carry_ = mix(k_seed, 3);
  uint64_t tok_scan_ = mix(k_seed, 4);
  // Mutant signature.merge_classes: the const-slot pattern is not part of the signature. A
  // constant slot hashes like a reference (token) and two trees with the same hash and the same
  // slot counts are one class (class_for), so sub($, @) and sub(@, $) share a class and the
  // second shape is emitted with the first's operand order.
  const bool merge_classes_ = mutant("signature.merge_classes");

  // Scan detection (D41): per node its chain and position; per node whether it is an inner step
  // of a scan group although a Sum or a Sum member (scan_inner_), whether it is a Sum evaluated
  // as a fixed-arity step (fixed_sum_), and the operand slot holding a Const initial value.
  std::vector<Chain> chains_;
  std::vector<std::int32_t> chain_of_;
  std::vector<std::int32_t> pos_in_chain_;
  std::vector<std::uint8_t> scan_inner_;
  std::vector<std::uint8_t> fixed_sum_;
  std::vector<std::int8_t> const_init_slot_;

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
  std::vector<std::uint8_t> class_scan_;    // per class: a scan class (carry_slot >= 0)
  std::vector<std::int32_t> scc_of_;        // per class
  std::vector<std::uint8_t> scc_nontrivial_;
  std::map<DomainKey, std::int32_t> domain_index_;
  std::vector<DomainKey> domain_key_;
  std::vector<std::vector<node_id>> domain_rows_;  // node per row, tape order (chain-major for a scan)
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
  promoted_.assign(n_, 0);
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
    const node_id id = static_cast<node_id>(i);
    if (fixed_sum_[i]) {
      // A scan step's Sum over its members, in fold order (no reordering: the fold is a left
      // fold and only the two-member case commutes).
      for (int j = 0; j < arity_of(id); ++j) h = mix(h, token_at(id, j, operand_of(id, j)));
    } else if (nd.op == Op::Const || nd.op == Op::Input || op_is_variadic(nd.op)) {
      h_[i] = h;
      continue;
    } else {
      const int arity = op_arity(nd.op);
      uint64_t ta = arity >= 1 ? token_at(id, 0, nd.a) : 0;
      uint64_t tb = arity >= 2 ? token_at(id, 1, nd.b) : 0;
      const uint64_t tc = arity >= 3 ? token_at(id, 2, nd.c) : 0;
      if (op_is_commutative(nd.op) && tb < ta) std::swap(ta, tb);
      if (arity >= 1) h = mix(h, ta);
      if (arity >= 2) h = mix(h, tb);
      if (arity >= 3) h = mix(h, tc);
    }
    // A scan step is never in a class with a non-scan node of the same shape (D41).
    if (chain_of_[i] >= 0) h = mix(h, tok_scan_);
    h_[i] = h;
  }
}

// The deep hash of a node: its expression modulo constants, seen through every fixed-arity
// node whether shared or not, stopping at Inputs (all alike), Sums and Affines (their operands
// are segment members), Const leaves and, once chains are known, the steps of a chain (a scan
// row is a stop like a Sum: its depth in the chain must not tell two instances of the same
// computation apart). Two nodes with the same deep hash are the same computation on different
// data; it does not depend on which nodes are boundaries.
void Inference::compute_deep() {
  deep_.assign(n_, 0);
  for (size_t i = 0; i < n_; ++i) {
    const Node& nd = nodes_[i];
    uint64_t h = mix(k_seed, static_cast<uint64_t>(nd.op) + 0x200);
    if (nd.op == Op::Const) {
      deep_[i] = tok_const_;
      continue;
    }
    if (nd.op == Op::Input || op_is_variadic(nd.op) || chain_of_[i] >= 0) {
      deep_[i] = chain_of_[i] >= 0 ? mix(h, tok_scan_) : h;
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
// deep hashes do not depend on the boundary set. An inner step of a scan group (scan_inner_) is
// never promoted: the scan step's tree wins (D41).
void Inference::promote_shared() {
  ++stats_.boundary_rounds;
  std::unordered_set<uint64_t> shared;
  for (size_t i = 0; i < n_; ++i) {
    if (boundary_[i]) shared.insert(deep_[i]);
  }
  for (size_t i = 0; i < n_; ++i) {
    if (boundary_[i] || is_const_[i] || scan_inner_[i]) continue;
    if (shared.count(deep_[i]) != 0) {
      boundary_[i] = 1;
      promoted_[i] = 1;
      ++stats_.class_promoted;
    }
  }
}

// ---- scan detection (DESIGN.md §5.6, D41) ---------------------------------------------------
//
// A chain is a run of nodes n_1, ..., n_K where n_{k+1} = f(n_k, ...) with the same step f for
// every k: the natural product loop acc = acc·(1 + r·τ), the path x_{k+1} = a_k·x_k + b_k. The
// step of n is its expression tree with one node — the carry — cut out and replaced by a carry
// token; two consecutive nodes with the same step hash, the second cutting the first, form a
// chain, and the first's own cut is the chain's initial value (an Input, a shared value, an
// inner value that becomes a boundary, or a Const, which becomes a row of the const domain).
//
// The tree of n is walked as the extraction will walk it — through single-use fixed-arity
// nodes, stopping at boundaries and constants — with one extension: a Sum of two or three
// members used once, and a member used once, are walked as well *when they lie on the path to
// the carry* (fold_sum turned the step's `+` into a Sum node; it becomes a fixed-arity Sum step
// of the scan group) and are references when they do not. The step hash therefore depends on the
// path, and is computed per candidate carry. A cut directly under an Add or a Sum whose target
// is an inner (single-use) value is not a chain link: an inner running sum is a reduction and
// fold_sum's business, never a scan (the raw M1 leg sums). Chains shorter than scan_min_steps
// are dropped. Nodes are visited in tape order; a chain's nodes are never walked into by later
// nodes (they will be boundaries), so a step's tree stays small.
namespace scan {

enum class TKind : std::uint8_t {
  Const = 0,     // a Const leaf
  Ref = 1,       // a boundary (or a chain node): a reference
  Inner = 2,     // expanded under the normal rules
  OnPath = 3,    // expanded only on the path to the carry (a small Sum, a single-use member)
};

struct TEntry {
  node_id id = invalid_node;
  std::int32_t parent = -1;
  std::int8_t slot = -1;      // operand slot in the parent
  std::int8_t depth = 0;
  TKind kind = TKind::Ref;
  bool expanded = false;
  std::int32_t child0 = 0, nchild = 0;
  uint64_t hn = 0;            // hash under the normal rules (OnPath entries are references)
};

}  // namespace scan

void Inference::detect_chains() {
  using scan::TEntry;
  using scan::TKind;
  chains_.clear();
  chain_of_.assign(n_, -1);
  pos_in_chain_.assign(n_, -1);
  scan_inner_.assign(n_, 0);
  fixed_sum_.assign(n_, 0);
  const_init_slot_.assign(n_, -1);

  auto small_sum = [&](node_id c) {
    const Node& nd = nodes_[idx(c)];
    return nd.op == Op::Sum && nd.nargs >= 2 && nd.nargs <= 3;
  };
  auto n_operands = [&](node_id c) -> int {
    const Node& nd = nodes_[idx(c)];
    if (nd.op == Op::Sum) return nd.nargs;
    return op_arity(nd.op);
  };
  auto child_of = [&](node_id c, int j) -> node_id {
    const Node& nd = nodes_[idx(c)];
    if (nd.op == Op::Sum) return t_.args(nd)[idx(j)];
    return j == 0 ? nd.a : j == 1 ? nd.b : nd.c;
  };
  // How child c is seen from an expanded parent.
  auto classify = [&](node_id c) -> TKind {
    if (is_const_[idx(c)]) return TKind::Const;
    if (chain_of_[idx(c)] >= 0) return TKind::Ref;
    const Node& nd = nodes_[idx(c)];
    if (nd.op == Op::Input || nd.op == Op::Affine) return TKind::Ref;
    if (nd.op == Op::Sum) return (small_sum(c) && use_count_[idx(c)] == 1) ? TKind::OnPath : TKind::Ref;
    if (use_count_[idx(c)] != 1 || promoted_[idx(c)]) return TKind::Ref;
    return member_use_[idx(c)] ? TKind::OnPath : TKind::Inner;
  };

  // The tree of `root` (entries in preorder, children contiguous). Returns false when it
  // exceeds scan_max_tree. Every Inner and OnPath entry is expanded (the OnPath ones so that
  // the carry can be found beneath them; off the path they hash as references).
  std::vector<TEntry> tree;
  auto build_tree = [&](node_id root) -> bool {
    tree.clear();
    TEntry r;
    r.id = root;
    r.kind = TKind::Inner;
    tree.push_back(r);
    for (size_t e = 0; e < tree.size(); ++e) {
      const TKind k = tree[e].kind;
      if (k != TKind::Inner && k != TKind::OnPath) continue;
      const node_id id = tree[e].id;
      const int m = n_operands(id);
      tree[e].expanded = true;
      tree[e].child0 = static_cast<std::int32_t>(tree.size());
      tree[e].nchild = m;
      if (tree.size() + static_cast<size_t>(m) > static_cast<size_t>(scan_max_tree)) return false;
      for (int j = 0; j < m; ++j) {
        TEntry c;
        c.id = child_of(id, j);
        c.parent = static_cast<std::int32_t>(e);
        c.slot = static_cast<std::int8_t>(j);
        c.depth = static_cast<std::int8_t>(tree[e].depth + 1);
        c.kind = classify(c.id);
        tree.push_back(c);
      }
    }
    return true;
  };
  // The hash of entry e's subtree with the entries flagged `on_path` expanded (and the target
  // entries replaced by the carry token), every other OnPath entry a reference.
  std::vector<std::uint8_t> on_path, is_target;
  auto hash_entry = [&](auto&& self, std::int32_t e) -> uint64_t {
    const TEntry& t = tree[idx(e)];
    if (is_target[idx(e)]) return tok_carry_;
    if (t.kind == TKind::Const) return tok_const_;
    if (t.kind == TKind::Ref) return tok_ref_;
    if (t.kind == TKind::OnPath && !on_path[idx(e)]) return tok_ref_;
    if (!on_path[idx(e)]) return t.hn;  // unchanged below this entry
    const Node& nd = nodes_[idx(t.id)];
    uint64_t h = mix(k_seed, static_cast<uint64_t>(nd.op) + 0x300);
    if (nd.op == Op::Sum) {
      for (std::int32_t j = 0; j < t.nchild; ++j) h = mix(h, self(self, t.child0 + j));
      return h;
    }
    uint64_t ta = t.nchild >= 1 ? self(self, t.child0) : 0;
    uint64_t tb = t.nchild >= 2 ? self(self, t.child0 + 1) : 0;
    const uint64_t tc = t.nchild >= 3 ? self(self, t.child0 + 2) : 0;
    if (op_is_commutative(nd.op) && tb < ta) std::swap(ta, tb);
    if (t.nchild >= 1) h = mix(h, ta);
    if (t.nchild >= 2) h = mix(h, tb);
    if (t.nchild >= 3) h = mix(h, tc);
    return h;
  };
  // Marks the ancestors of every entry holding `target` (and the entries themselves) and returns
  // the step hash of the root with that cut.
  auto cut_hash = [&](node_id target) -> uint64_t {
    on_path.assign(tree.size(), 0);
    is_target.assign(tree.size(), 0);
    for (size_t e = 1; e < tree.size(); ++e) {
      if (tree[e].id != target) continue;
      // Only entries whose ancestors are all expanded are reachable (an entry under an
      // unexpanded OnPath entry is only reachable if that entry is on the path: it is).
      is_target[e] = 1;
      for (std::int32_t a = tree[e].parent; a >= 0; a = tree[idx(a)].parent) on_path[idx(a)] = 1;
    }
    return hash_entry(hash_entry, 0);
  };

  // Per node: its cuts (target, step hash, the carry's operand path), kept for the chain-start
  // test of later nodes. A link needs the same hash AND the same path: under a commutative op
  // the cut at either operand hashes alike (a·x: the coefficient or the carry), and a chain
  // must carry through the same position of the step at every step, which is also what makes
  // the extraction of the steps identical.
  struct Cut {
    node_id target;
    uint64_t hash;
    std::uint32_t path;
  };
  auto path_of = [&](size_t e) {
    std::uint32_t key = 0;
    for (std::int32_t a = static_cast<std::int32_t>(e); a > 0; a = tree[idx(a)].parent) {
      key = key * 4u + static_cast<std::uint32_t>(tree[idx(a)].slot) + 1u;
    }
    return key;
  };
  std::vector<std::int32_t> cut_begin(n_ + 1, 0);
  std::vector<Cut> cuts;
  std::vector<node_id> seen_targets;
  std::unordered_set<node_id> starts;  // nodes that are the first member of a chain (dissolved ones too)

  for (size_t i = 0; i < n_; ++i) {
    cut_begin[i] = static_cast<std::int32_t>(cuts.size());
    const Node& nd = nodes_[i];
    const node_id root = static_cast<node_id>(i);
    if (is_const_[i] || nd.op == Op::Input || nd.op == Op::Affine) continue;
    if (nd.op == Op::Sum && !small_sum(root)) continue;
    if (banned_.count(root) != 0) continue;  // a retried class's step: never a chain node again
    if (!build_tree(root)) continue;
    // Normal-rule hashes bottom-up (entries are in preorder, so children come after parents:
    // walk backwards).
    on_path.assign(tree.size(), 0);
    is_target.assign(tree.size(), 0);
    for (size_t e = tree.size(); e-- > 0;) {
      TEntry& t = tree[e];
      if (t.kind == TKind::Const) { t.hn = tok_const_; continue; }
      if (t.kind == TKind::Ref || (t.kind == TKind::OnPath)) { t.hn = tok_ref_; continue; }
      // Inner (and the root): expanded; children's hn are final.
      on_path[e] = 1;
      t.hn = hash_entry(hash_entry, static_cast<std::int32_t>(e));
      on_path[e] = 0;
    }
    // Candidate carries: entries at depth 1..scan_max_depth, each node id once, excluding the
    // inner running-sum shape.
    seen_targets.clear();
    for (size_t e = 1; e < tree.size(); ++e) {
      const TEntry& t = tree[e];
      if (t.depth > scan_max_depth) continue;
      const node_id target = t.id;
      if (std::find(seen_targets.begin(), seen_targets.end(), target) != seen_targets.end()) continue;
      seen_targets.push_back(target);
      const Op parent_op = nodes_[idx(tree[idx(t.parent)].id)].op;
      const bool inner_target = !is_const_[idx(target)] && !boundary_[idx(target)] && chain_of_[idx(target)] < 0;
      if ((parent_op == Op::Add || parent_op == Op::Sum) && inner_target) continue;
      cuts.push_back({target, cut_hash(target), path_of(e)});
    }
    // Extend a chain whose tail this node cuts with the chain's step, else start one with a
    // node whose own cut has this step.
    for (std::int32_t c = cut_begin[i]; c < static_cast<std::int32_t>(cuts.size()); ++c) {
      const node_id m = cuts[idx(c)].target;
      const uint64_t h = cuts[idx(c)].hash;
      const std::uint32_t path = cuts[idx(c)].path;
      const std::int32_t ch = chain_of_[idx(m)];
      if (ch >= 0) {
        Chain& chain = chains_[idx(ch)];
        if (chain.hash != h || chain.path != path || chain.members.back() != m) continue;
        chain.members.push_back(root);
        chain_of_[i] = ch;
        pos_in_chain_[i] = static_cast<std::int32_t>(chain.members.size()) - 1;
        break;
      }
      if (is_const_[idx(m)] || starts.count(m) != 0) continue;
      bool started = false;
      for (std::int32_t d = cut_begin[idx(m)]; d < cut_begin[idx(m) + 1]; ++d) {
        if (cuts[idx(d)].hash != h || cuts[idx(d)].path != path) continue;
        Chain chain;
        chain.hash = h;
        chain.path = path;
        chain.init = cuts[idx(d)].target;
        chain.members = {m, root};
        chains_.push_back(std::move(chain));
        chain_of_[idx(m)] = static_cast<std::int32_t>(chains_.size()) - 1;
        pos_in_chain_[idx(m)] = 0;
        chain_of_[i] = chain_of_[idx(m)];
        pos_in_chain_[i] = 1;
        starts.insert(m);
        started = true;
        break;
      }
      if (started) break;
    }
  }
  cut_begin[n_] = static_cast<std::int32_t>(cuts.size());

  // Drop the short chains; the rest become scan classes: every member a boundary, the initial
  // value a boundary (or a const row), the nodes on the carry's path inner steps of the group.
  std::vector<Chain> kept;
  for (Chain& chain : chains_) {
    if (static_cast<int>(chain.members.size()) < scan_min_steps) {
      for (node_id m : chain.members) {
        chain_of_[idx(m)] = -1;
        pos_in_chain_[idx(m)] = -1;
      }
      continue;
    }
    kept.push_back(std::move(chain));
  }
  chains_ = std::move(kept);
  for (size_t ch = 0; ch < chains_.size(); ++ch) {
    Chain& chain = chains_[ch];
    for (size_t k = 0; k < chain.members.size(); ++k) {
      const node_id m = chain.members[k];
      chain_of_[idx(m)] = static_cast<std::int32_t>(ch);
      pos_in_chain_[idx(m)] = static_cast<std::int32_t>(k);
      boundary_[idx(m)] = 1;
    }
    if (!is_const_[idx(chain.init)]) boundary_[idx(chain.init)] = 1;
  }
  stats_.chains = chains_.size();
  stats_.chain_nodes = 0;
  for (const Chain& chain : chains_) {
    stats_.chain_nodes += chain.members.size();
    for (size_t k = 0; k < chain.members.size(); ++k) {
      const node_id m = chain.members[k];
      const node_id carry = k == 0 ? chain.init : chain.members[k - 1];
      if (!build_tree(m)) throw std::logic_error("ir::infer: a scan step's tree grew after detection");
      if (nodes_[idx(m)].op == Op::Sum) fixed_sum_[idx(m)] = 1;
      on_path.assign(tree.size(), 0);
      is_target.assign(tree.size(), 0);
      for (size_t e = 1; e < tree.size(); ++e) {
        if (tree[e].id != carry) continue;
        is_target[e] = 1;
        const TEntry& parent = tree[idx(tree[e].parent)];
        if (is_const_[idx(carry)]) const_init_slot_[idx(parent.id)] = tree[e].slot;
        for (std::int32_t a = tree[e].parent; a > 0; a = tree[idx(a)].parent) {
          on_path[idx(a)] = 1;
          const TEntry& t = tree[idx(a)];
          if (t.kind != TKind::OnPath) continue;
          scan_inner_[idx(t.id)] = 1;
          boundary_[idx(t.id)] = 0;
          if (nodes_[idx(t.id)].op == Op::Sum) fixed_sum_[idx(t.id)] = 1;
        }
      }
      // Every inner node of the step's final tree (expanded under the normal rules, or a small
      // Sum / single-use member on the carry's path, beneath expanded entries only, above the
      // carry) is part of the step: the sharing rule's second pass must not promote it, or the
      // step would lose its carry (the output sqrt of a sqrt chain shares the deep hash of the
      // sqrt inside every step).
      std::vector<std::uint8_t> in_step(tree.size(), 0);
      in_step[0] = 1;
      for (size_t e = 1; e < tree.size(); ++e) {
        const TEntry& t = tree[e];
        if (!in_step[idx(t.parent)] || is_target[e]) continue;
        const bool expanded = t.kind == TKind::Inner || (t.kind == TKind::OnPath && on_path[e]);
        if (!expanded) continue;
        in_step[e] = 1;
        scan_inner_[idx(t.id)] = 1;
      }
    }
  }
}

void Inference::extract_tree(node_id root, Extract& out) const {
  out.clear();
  const node_id carry = chain_of_[idx(root)] >= 0
                            ? (pos_in_chain_[idx(root)] == 0 ? chains_[idx(chain_of_[idx(root)])].init
                                                              : chains_[idx(chain_of_[idx(root)])].members[idx(pos_in_chain_[idx(root)]) - 1])
                            : invalid_node;
  std::vector<Frame> stack;
  auto open = [&](node_id id, int parent, int parent_slot) {
    const Node& nd = nodes_[idx(id)];
    Frame f;
    f.id = id;
    f.arity = arity_of(id);
    f.parent = parent;
    f.parent_slot = parent_slot;
    for (int j = 0; j < f.arity && j < 3; ++j) f.ops[idx(j)] = operand_of(id, j);
    if (op_is_commutative(nd.op) && token_at(id, 1, nd.b) < token_at(id, 0, nd.a)) {
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
      // The recorded slot of c (a swapped commutative pair reads its tokens the other way round).
      const int rec_slot = f.swapped && slot < 2 ? 1 - slot : slot;
      if (is_const_[idx(c)] && !const_init_at(f.id, rec_slot, c)) {
        f.res[static_cast<size_t>(slot)] = {Kind::ConstSlot, static_cast<std::int32_t>(out.consts.size())};
        out.consts.push_back(nodes_[idx(c)].konst);
      } else if (is_const_[idx(c)] || boundary_[idx(c)]) {
        if (c == carry && out.carry_ref < 0) out.carry_ref = static_cast<std::int32_t>(out.refs.size());
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
    if (classes_[idx(c)].carry_slot != ex.carry_ref) continue;
    if (classes_[idx(c)].proto == ex.proto) return c;
    if (merge_classes_ && classes_[idx(c)].n_const == static_cast<std::int32_t>(ex.consts.size()) && classes_[idx(c)].n_ref == static_cast<std::int32_t>(ex.refs.size())) return c;
  }
  Class cl;
  cl.hash = hash;
  cl.proto = ex.proto;
  cl.emit_swapped = ex.swapped;
  cl.n_const = static_cast<std::int32_t>(ex.consts.size());
  cl.n_ref = static_cast<std::int32_t>(ex.refs.size());
  cl.carry_slot = ex.carry_ref;
  classes_.push_back(std::move(cl));
  const std::int32_t c = static_cast<std::int32_t>(classes_.size()) - 1;
  cands.push_back(c);
  return c;
}

// A Const node used as a Sum / Affine member, registered as an output or the initial value of
// a chain is a row of the const class (one Const step reading its value slot).
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
    } else if (op_is_variadic(nd.op) && !fixed_sum_[i]) {
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
      // A chain's first step reading a Const initial value: that Const is a row of the const
      // class (created before the extraction takes references into classes_).
      if (chain_of_[i] >= 0 && pos_in_chain_[i] == 0) {
        const node_id init = chains_[idx(chain_of_[i])].init;
        if (is_const_[idx(init)]) const_row(init);
      }
      extract_tree(root, ex);
      if (chain_of_[i] >= 0 && ex.carry_ref < 0) {
        throw std::logic_error("ir::infer: a scan step's extraction did not find its carry");
      }
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
  stats_.scan_classes = 0;
  for (const Class& cl : classes_) stats_.scan_classes += cl.carry_slot >= 0 ? 1 : 0;
}

// Class dependency graph -> SCCs (iterative Tarjan) -> per-row level inside a non-trivial SCC
// -> domains (class, level) in order of first appearance. A scan class's carry edges (a step
// reading the previous step of its chain) are not dependency edges: the class is one recurrent
// domain; a scan class that still sits in a cycle with other classes cannot be, and is retried
// without its chains (ScanRetry).
void Inference::levels_and_domains() {
  const size_t n_classes = classes_.size();
  std::vector<std::vector<std::int32_t>> edges(n_classes);
  class_recurrent_.assign(n_classes, 0);
  class_scan_.assign(n_classes, 0);
  for (size_t c = 0; c < n_classes; ++c) class_scan_[c] = classes_[c].carry_slot >= 0 ? 1 : 0;
  {
    std::vector<std::unordered_set<std::int32_t>> seen(n_classes);
    for (size_t c = 0; c < n_classes; ++c) {
      const Class& cl = classes_[c];
      auto add = [&](node_id target) {
        const std::int32_t tc = class_of_[idx(target)];
        if (tc == static_cast<std::int32_t>(c)) class_recurrent_[c] = 1;
        if (seen[c].insert(tc).second) edges[c].push_back(tc);
      };
      if (cl.n_ref > 0) {
        for (size_t r = 0; r < cl.rows.size(); ++r) {
          for (std::int32_t k = 0; k < cl.n_ref; ++k) {
            const node_id target = cl.ref_nodes[r * idx(cl.n_ref) + idx(k)];
            if (k == cl.carry_slot && class_of_[idx(target)] == static_cast<std::int32_t>(c)) continue;
            add(target);
          }
        }
      }
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
    if (scc_size[idx(scc_of_[c])] > 1 || (class_recurrent_[c] && !class_scan_[c])) scc_nontrivial_[idx(scc_of_[c])] = 1;
  }

  // Levels: rows in tape order (every read is an earlier node, so one pass is exact); only
  // reads inside the same non-trivial SCC count. A scan class is normally inside such an SCC
  // (every Sum of a program is one class: the compounding steps read the 1 + r·τ sums, the leg
  // sums read the coupons that read the steps), so it takes part in the split as a unit: its
  // carry reads are not edges, and every step of a chain gets the chain's level — one more than
  // the deepest read of any of its steps — so that the whole chain lands in one level-domain,
  // evaluated after everything its steps read. Levels only rise, so the fixed point is reached
  // in a few passes; a chain that reads itself through other classes (a step reading a value
  // that depends on an earlier step of the same chain) never settles and its class is retried
  // without chains (ScanRetry).
  level_of_.assign(n_, 0);
  auto propagate = [&]() {
    bool changed = false;
    for (size_t i = 0; i < n_; ++i) {
      const std::int32_t c = class_of_[i];
      if (c < 0 || !scc_nontrivial_[idx(scc_of_[idx(c)])]) continue;
      const Class& cl = classes_[idx(c)];
      const size_t r = idx(row_of_[i]);
      std::int32_t lvl = level_of_[i];
      auto consider = [&](node_id target) {
        if (scc_of_[idx(class_of_[idx(target)])] != scc_of_[idx(c)]) return;
        lvl = std::max(lvl, level_of_[idx(target)] + 1);
      };
      for (std::int32_t k = 0; k < cl.n_ref; ++k) {
        const node_id target = cl.ref_nodes[r * idx(cl.n_ref) + idx(k)];
        if (k == cl.carry_slot && class_of_[idx(target)] == c) continue;
        consider(target);
      }
      if (!cl.seg_offsets.empty() && r + 1 < cl.seg_offsets.size()) {
        for (std::int32_t m = cl.seg_offsets[r]; m < cl.seg_offsets[r + 1]; ++m) consider(cl.seg_members[idx(m)]);
      }
      if (lvl != level_of_[i]) {
        level_of_[i] = lvl;
        changed = true;
      }
    }
    return changed;
  };
  auto unify_chains = [&]() {
    bool changed = false;
    for (const Chain& chain : chains_) {
      const std::int32_t c = class_of_[idx(chain.members.front())];
      if (c < 0 || !class_scan_[idx(c)] || !scc_nontrivial_[idx(scc_of_[idx(c)])]) continue;
      std::int32_t top = 0;
      for (node_id m : chain.members) top = std::max(top, level_of_[idx(m)]);
      for (node_id m : chain.members) {
        if (level_of_[idx(m)] != top) {
          level_of_[idx(m)] = top;
          changed = true;
        }
      }
    }
    return changed;
  };
  {
    propagate();
    size_t rounds = 0;
    const size_t max_rounds = 8 + n_classes;
    while (unify_chains()) {
      if (++rounds > max_rounds || !propagate()) {
        if (rounds > max_rounds) {
          ScanRetry retry;
          for (size_t c = 0; c < n_classes; ++c) {
            if (!class_scan_[c] || !scc_nontrivial_[idx(scc_of_[c])]) continue;
            for (node_id r : classes_[c].rows) retry.nodes.push_back(r);
            retry.reason += " scan class " + std::to_string(c) + " (" + to_string(classes_[c].proto.back().op) + ", " +
                            std::to_string(classes_[c].rows.size()) + " rows) reads itself through other classes;";
          }
          throw retry;
        }
        break;
      }
    }
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
    domain_rows_[idx(d)].push_back(static_cast<node_id>(i));
  }
  // A scan domain's rows are chain-major: chains in order of their first step, steps in order
  // (a structure-only permutation; rows of the other domains stay in tape order).
  for (size_t d = 0; d < domain_key_.size(); ++d) {
    if (!class_scan_[idx(domain_key_[d].cls)]) continue;
    std::vector<node_id>& rows = domain_rows_[d];
    std::vector<node_id> first_of(chains_.size(), invalid_node);
    for (size_t ch = 0; ch < chains_.size(); ++ch) first_of[ch] = chains_[ch].members.front();
    std::stable_sort(rows.begin(), rows.end(), [&](node_id a, node_id b) {
      const node_id fa = first_of[idx(chain_of_[idx(a)])], fb = first_of[idx(chain_of_[idx(b)])];
      if (fa != fb) return fa < fb;
      return pos_in_chain_[idx(a)] < pos_in_chain_[idx(b)];
    });
  }
  for (size_t d = 0; d < domain_key_.size(); ++d) {
    const std::vector<node_id>& rows = domain_rows_[d];
    for (size_t r = 0; r < rows.size(); ++r) {
      domain_of_node_[idx(rows[r])] = static_cast<std::int32_t>(d);
      row_in_domain_[idx(rows[r])] = static_cast<std::int32_t>(r);
    }
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
    const bool is_scan = class_scan_[idx(key.cls)] != 0;

    Domain dom;
    dom.rows = static_cast<std::int32_t>(n_rows);
    dom.value_base = base;
    dom.level = key.level;
    dom.scan_class = (class_recurrent_[idx(key.cls)] != 0) || is_scan;  // the class-level fact (D23)
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
    const bool variadic = (op == Op::Sum || op == Op::Affine) && cl.proto.back().a.kind == Kind::Segment;
    if (variadic) {
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
    // Per domain: do this domain's rows read this domain? A scan does (its carry); never
    // otherwise after level splitting, but computed, not copied from the class (D23).
    dom.recurrent = reads.count(d_src) != 0;

    if (is_scan) {
      // The recurrence: chains from the chain-major row order; the carry gather must read the
      // previous row of the chain (a chain's first row an earlier domain) and every other read
      // of this domain an earlier row, else the layout is not a scan and the chains are retried.
      Scan sc;
      sc.domain = d;
      sc.carry_gather = ref_slots[idx(cl.carry_slot)].index;
      sc.chain_offsets.push_back(0);
      bool ok = true;
      for (size_t r = 0; r < n_rows; ++r) {
        const bool first = pos_in_chain_[idx(rows[r])] == 0;
        if (first && r > 0) sc.chain_offsets.push_back(static_cast<std::int32_t>(r));
        for (std::int32_t k = 0; k < cl.n_ref; ++k) {
          const value_id v = p.gathers[idx(ref_slots[idx(k)].index)].index[r];
          if (k == cl.carry_slot) {
            ok &= first ? v < dom.value_base : v == dom.value_base + static_cast<value_id>(r) - 1;
          } else {
            ok &= v < dom.value_base + static_cast<value_id>(r);
          }
        }
      }
      sc.chain_offsets.push_back(static_cast<std::int32_t>(n_rows));
      if (!ok) {
        ScanRetry retry;
        retry.nodes.assign(rows.begin(), rows.end());
        retry.reason = " scan class " + std::to_string(key.cls) + " (" + std::to_string(n_rows) +
                       " rows) reads a row that is not earlier in the chain-major order;";
        throw retry;
      }
      dom.scan = static_cast<std::int32_t>(p.scans.size());
      p.scans.push_back(std::move(sc));
    }
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
    if (p.domains[d].scan >= 0) {
      p.domains[d].name += "@scan";
    } else if (scc_nontrivial_[idx(scc_of_[idx(domain_key_[idx(domain_order_[d])].cls)])]) {
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
  chain_of_.assign(n_, -1);
  scan_inner_.assign(n_, 0);
  fixed_sum_.assign(n_, 0);
  const_init_slot_.assign(n_, -1);
  compute_uses();
  initial_boundaries();
  compute_deep();
  promote_shared();
  detect_chains();
  if (!chains_.empty()) {
    // The chains' steps are boundaries now: the deep hashes stop at them, and the sharing rule
    // is applied once more so that every instance of a computation reading a scan row is
    // materialised alike.
    compute_deep();
    promote_shared();
  }
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
  std::unordered_set<node_id> banned;
  std::string retries;
  for (int round = 0;; ++round) {
    Inference inf(tape, banned);
    try {
      Program p = inf.run(stats);
      if (stats != nullptr) {
        stats->scan_rounds = static_cast<size_t>(round) + 1;
        stats->scan_retries = retries;
      }
      return p;
    } catch (const ScanRetry& retry) {
      if (round >= 16) throw std::logic_error("ir::infer: scan detection did not settle in 16 rounds; last reason:" + retry.reason);
      retries += "round " + std::to_string(round) + ":" + retry.reason;
      for (node_id n : retry.nodes) banned.insert(n);
    }
  }
}

}  // namespace epykos::ir
