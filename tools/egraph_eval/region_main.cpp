// tools/egraph_eval/ — how big is a pure-field region, really? (docs/DECISIONS.md D75)
//
//   egraph_eval_regions [--trades N] [--tenors a,b,c] [--no-passes] [--dump-largest <path>]
//                       [--dump-top K] [--json <path>]
//
// The owner's question for the "write or wrap an e-graph" evaluation was: take the
// `compare_ois` fixture, extract a MAXIMAL SUBGRAPH OF PURE FIELD OPERATIONS
// (Add Sub Mul Div Neg Recip Fma Sum Affine), bounded by the structural ops a term-level
// e-graph library will never see (gathers, segments, Linmap, Select, Cmp), and report how
// large such regions actually are — "because if they are tiny the whole approach is weaker
// than it sounds".
//
// This tool answers it at BOTH levels the engine has, because they give very different
// answers and the difference is the finding:
//
//   * TAPE level (`epykos::Tape`): the recorded scalar DAG. There are no Gather / Segment /
//     Linmap nodes here at all (`op_is_supported` stops at Affine), so a region is bounded
//     only by Const / Input leaves, by Exp / Log / Sqrt, and by Select / Cmp.
//   * IR level (`ir::Program`, what D73's term e-graph is actually designed over): steps
//     inside a group, connected through `SlotKind::Step`. Every other slot kind — Gather,
//     Segment, Column, Literal, Input — is a region boundary by construction.
//
// A region is a maximal connected component under the relation "consumer and operand are
// both field ops", i.e. exactly the unit an e-graph could be handed in one piece. Reported
// per region: size, the number of distinct boundary values it reads (its leaves) and the
// number of its members something outside it reads (its exits, which a rewrite must keep).
//
// Like everything under tools/, this is a measurement tool: not engine API, not a gate, not
// a ctest or Google Benchmark target.
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <numeric>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "epykos/fixtures/compare_ois.hpp"
#include "epykos/ir/program.hpp"
#include "epykos/ir/signature.hpp"
#include "epykos/tape/op.hpp"
#include "epykos/tape/tape.hpp"

using namespace epykos;

namespace {

// PRINCIPLES.md tier 1 is algebra over a field. These are the ops an e-graph library would
// be given; everything else is a boundary it never sees.
bool is_field_op(Op op) noexcept {
  switch (op) {
    case Op::Add:
    case Op::Sub:
    case Op::Mul:
    case Op::Div:
    case Op::Neg:
    case Op::Recip:
    case Op::Fma:
    case Op::Sum:
    case Op::Affine:
      return true;
    default:
      return false;
  }
}

// ---- union-find ---------------------------------------------------------------------------
struct DisjointSet {
  std::vector<std::int32_t> parent;
  explicit DisjointSet(std::size_t n) : parent(n) { std::iota(parent.begin(), parent.end(), 0); }
  std::int32_t find(std::int32_t x) {
    while (parent[static_cast<std::size_t>(x)] != x) {
      parent[static_cast<std::size_t>(x)] = parent[static_cast<std::size_t>(parent[static_cast<std::size_t>(x)])];
      x = parent[static_cast<std::size_t>(x)];
    }
    return x;
  }
  void unite(std::int32_t a, std::int32_t b) {
    a = find(a);
    b = find(b);
    if (a != b) parent[static_cast<std::size_t>(b)] = a;
  }
};

struct RegionStats {
  std::size_t count = 0;         // number of regions
  std::size_t members = 0;       // total field nodes inside regions
  std::size_t largest = 0;
  std::size_t largest_leaves = 0;
  std::size_t largest_exits = 0;
  std::int32_t largest_root = -1;
  double mean = 0.0;
  std::size_t median = 0;
  std::size_t p90 = 0;
  std::size_t singletons = 0;    // regions of exactly one node
  std::size_t at_least_8 = 0;
  std::size_t at_least_64 = 0;
  std::vector<std::size_t> sizes;  // descending
};

RegionStats summarise(std::vector<std::size_t> sizes) {
  RegionStats st;
  st.count = sizes.size();
  st.members = std::accumulate(sizes.begin(), sizes.end(), std::size_t{0});
  std::sort(sizes.begin(), sizes.end(), std::greater<>());
  if (!sizes.empty()) {
    st.largest = sizes.front();
    st.mean = static_cast<double>(st.members) / static_cast<double>(sizes.size());
    st.median = sizes[sizes.size() / 2];
    st.p90 = sizes[static_cast<std::size_t>(0.1 * static_cast<double>(sizes.size()))];
    st.singletons = static_cast<std::size_t>(std::count(sizes.begin(), sizes.end(), std::size_t{1}));
    st.at_least_8 = static_cast<std::size_t>(std::count_if(sizes.begin(), sizes.end(),
                                                           [](std::size_t s) { return s >= 8; }));
    st.at_least_64 = static_cast<std::size_t>(std::count_if(sizes.begin(), sizes.end(),
                                                            [](std::size_t s) { return s >= 64; }));
  }
  st.sizes = std::move(sizes);
  return st;
}

// ---- tape-level regions -------------------------------------------------------------------
struct TapeRegions {
  DisjointSet ds;
  std::vector<std::int32_t> root_of;   // node -> region root, -1 when not a field node
  std::map<std::int32_t, std::vector<node_id>> members;
  RegionStats stats;
};

// Every operand of `n`, fixed-arity and variadic alike.
void for_each_operand(const Tape& tape, node_id id, const std::vector<node_id>*& vararg_out,
                      node_id ops[3], int& n_fixed) {
  const Node& n = tape[id];
  n_fixed = 0;
  vararg_out = nullptr;
  if (op_is_variadic(n.op)) return;
  const int arity = op_arity(n.op);
  if (arity >= 1) ops[n_fixed++] = n.a;
  if (arity >= 2) ops[n_fixed++] = n.b;
  if (arity >= 3) ops[n_fixed++] = n.c;
}

TapeRegions tape_regions(const Tape& tape) {
  const std::size_t n = tape.size();
  TapeRegions r{DisjointSet(n), std::vector<std::int32_t>(n, -1), {}, {}};

  for (std::size_t i = 0; i < n; ++i) {
    const node_id id = static_cast<node_id>(i);
    const Node& node = tape[id];
    if (!is_field_op(node.op)) continue;
    if (op_is_variadic(node.op)) {
      for (const node_id a : tape.args(node)) {
        if (is_field_op(tape[a].op)) r.ds.unite(id, a);
      }
    } else {
      node_id ops[3];
      int n_fixed = 0;
      const std::vector<node_id>* unused = nullptr;
      for_each_operand(tape, id, unused, ops, n_fixed);
      for (int k = 0; k < n_fixed; ++k) {
        if (is_field_op(tape[ops[k]].op)) r.ds.unite(id, ops[k]);
      }
    }
  }

  std::vector<std::size_t> sizes;
  for (std::size_t i = 0; i < n; ++i) {
    const node_id id = static_cast<node_id>(i);
    if (!is_field_op(tape[id].op)) continue;
    const std::int32_t root = r.ds.find(id);
    r.root_of[i] = root;
    r.members[root].push_back(id);
  }
  sizes.reserve(r.members.size());
  for (const auto& [root, ms] : r.members) sizes.push_back(ms.size());
  r.stats = summarise(std::move(sizes));

  // Leaves and exits of the largest region.
  std::int32_t best = -1;
  std::size_t best_n = 0;
  for (const auto& [root, ms] : r.members) {
    if (ms.size() > best_n) {
      best_n = ms.size();
      best = root;
    }
  }
  r.stats.largest_root = best;
  if (best >= 0) {
    const std::vector<node_id>& ms = r.members[best];
    const std::unordered_set<node_id> inside(ms.begin(), ms.end());
    std::unordered_set<node_id> leaves;
    for (const node_id id : ms) {
      const Node& node = tape[id];
      if (op_is_variadic(node.op)) {
        for (const node_id a : tape.args(node)) {
          if (!inside.count(a)) leaves.insert(a);
        }
      } else {
        node_id ops[3];
        int n_fixed = 0;
        const std::vector<node_id>* unused = nullptr;
        for_each_operand(tape, id, unused, ops, n_fixed);
        for (int k = 0; k < n_fixed; ++k) {
          if (!inside.count(ops[k])) leaves.insert(ops[k]);
        }
      }
    }
    r.stats.largest_leaves = leaves.size();

    std::unordered_set<node_id> exits;
    for (std::size_t i = 0; i < n; ++i) {
      const node_id id = static_cast<node_id>(i);
      if (inside.count(id)) continue;
      const Node& node = tape[id];
      if (op_is_variadic(node.op)) {
        for (const node_id a : tape.args(node)) {
          if (inside.count(a)) exits.insert(a);
        }
      } else {
        node_id ops[3];
        int n_fixed = 0;
        const std::vector<node_id>* unused = nullptr;
        for_each_operand(tape, id, unused, ops, n_fixed);
        for (int k = 0; k < n_fixed; ++k) {
          if (inside.count(ops[k])) exits.insert(ops[k]);
        }
      }
    }
    for (const node_id o : tape.outputs()) {
      if (inside.count(o)) exits.insert(o);
    }
    r.stats.largest_exits = exits.size();
  }
  return r;
}

// ---- IR-level regions ---------------------------------------------------------------------
// A term, per D73, is the sub-DAG rooted at one Step of one Group. Steps reference only
// EARLIER STEPS OF THE SAME GROUP (`SlotKind::Step`); every cross-domain edge is a Gather or
// a Segment, which is a boundary. So an IR region can never leave its group.
struct IrRegionStats {
  RegionStats stats;
  std::size_t groups = 0;
  std::size_t field_steps = 0;
  std::size_t total_steps = 0;
  std::size_t largest_group_steps = 0;
  std::map<std::string, std::size_t> op_histogram;
};

IrRegionStats ir_regions(const ir::Program& p) {
  IrRegionStats out;
  out.groups = p.groups.size();
  std::vector<std::size_t> sizes;
  for (const ir::Group& g : p.groups) {
    out.total_steps += g.steps.size();
    out.largest_group_steps = std::max(out.largest_group_steps, g.steps.size());
    const std::size_t ns = g.steps.size();
    DisjointSet ds(ns);
    for (std::size_t s = 0; s < ns; ++s) {
      const ir::Step& step = g.steps[s];
      out.op_histogram[to_string(step.op)]++;
      if (!is_field_op(step.op)) continue;
      for (const ir::Slot* slot : {&step.a, &step.b, &step.c, &step.konst}) {
        if (slot->kind != ir::SlotKind::Step) continue;
        const std::size_t t = static_cast<std::size_t>(slot->index);
        if (t < ns && is_field_op(g.steps[t].op)) ds.unite(static_cast<std::int32_t>(s), static_cast<std::int32_t>(t));
      }
    }
    std::map<std::int32_t, std::size_t> counts;
    for (std::size_t s = 0; s < ns; ++s) {
      if (!is_field_op(g.steps[s].op)) continue;
      counts[ds.find(static_cast<std::int32_t>(s))]++;
      out.field_steps++;
    }
    for (const auto& [root, c] : counts) sizes.push_back(c);
  }
  out.stats = summarise(std::move(sizes));
  return out;
}

// ---- JSON dump of one tape region -----------------------------------------------------------
// The exchange form the scratch harness reads. Deliberately explicit about everything the
// translation has to carry: op, the three fixed operands, the variadic side arrays, the input
// ordinal, the taint bit and the constant value.
std::string dump_region_json(const Tape& tape, const std::vector<node_id>& members,
                             const std::string& label) {
  const std::unordered_set<node_id> inside(members.begin(), members.end());
  // Close over the immediate boundary operands so the region is self-contained.
  std::vector<node_id> order;
  std::unordered_set<node_id> needed(members.begin(), members.end());
  for (const node_id id : members) {
    const Node& n = tape[id];
    if (op_is_variadic(n.op)) {
      for (const node_id a : tape.args(n)) needed.insert(a);
    } else {
      const int arity = op_arity(n.op);
      if (arity >= 1) needed.insert(n.a);
      if (arity >= 2) needed.insert(n.b);
      if (arity >= 3) needed.insert(n.c);
    }
  }
  order.assign(needed.begin(), needed.end());
  std::sort(order.begin(), order.end());
  std::unordered_map<node_id, std::size_t> local;
  for (std::size_t i = 0; i < order.size(); ++i) local[order[i]] = i;

  // Exits: members read from outside, plus registered outputs.
  std::unordered_set<node_id> exits;
  for (std::size_t i = 0; i < tape.size(); ++i) {
    const node_id id = static_cast<node_id>(i);
    if (inside.count(id)) continue;
    const Node& n = tape[id];
    if (op_is_variadic(n.op)) {
      for (const node_id a : tape.args(n)) {
        if (inside.count(a)) exits.insert(a);
      }
    } else {
      const int arity = op_arity(n.op);
      if (arity >= 1 && inside.count(n.a)) exits.insert(n.a);
      if (arity >= 2 && inside.count(n.b)) exits.insert(n.b);
      if (arity >= 3 && inside.count(n.c)) exits.insert(n.c);
    }
  }
  for (const node_id o : tape.outputs()) {
    if (inside.count(o)) exits.insert(o);
  }

  std::ostringstream o;
  o << std::setprecision(17);
  o << "{\n  \"label\": \"" << label << "\",\n";
  o << "  \"members\": " << members.size() << ",\n";
  o << "  \"nodes\": [\n";
  for (std::size_t i = 0; i < order.size(); ++i) {
    const node_id id = order[i];
    const Node& n = tape[id];
    const bool member = inside.count(id) != 0;
    o << "    {\"i\": " << i << ", \"tape_id\": " << id << ", \"op\": \"" << to_string(n.op)
      << "\", \"member\": " << (member ? "true" : "false") << ", \"tainted\": "
      << (n.tainted ? "true" : "false");
    if (n.op == Op::Const) o << ", \"konst\": " << n.konst;
    if (n.op == Op::Input) o << ", \"ordinal\": " << n.a << ", \"konst\": " << n.konst;
    if (member) {
      if (op_is_variadic(n.op)) {
        o << ", \"args\": [";
        const std::span<const node_id> as = tape.args(n);
        for (std::size_t k = 0; k < as.size(); ++k) o << (k ? ", " : "") << local[as[k]];
        o << "]";
        if (n.op == Op::Affine) {
          o << ", \"coefs\": [";
          const std::span<const double> cs = tape.coefs(n);
          for (std::size_t k = 0; k < cs.size(); ++k) o << (k ? ", " : "") << cs[k];
          o << "], \"konst\": " << n.konst;
        }
      } else {
        const int arity = op_arity(n.op);
        o << ", \"args\": [";
        if (arity >= 1) o << local[n.a];
        if (arity >= 2) o << ", " << local[n.b];
        if (arity >= 3) o << ", " << local[n.c];
        o << "]";
      }
    }
    o << "}" << (i + 1 < order.size() ? "," : "") << "\n";
  }
  o << "  ],\n  \"exits\": [";
  {
    std::vector<node_id> ev(exits.begin(), exits.end());
    std::sort(ev.begin(), ev.end());
    for (std::size_t k = 0; k < ev.size(); ++k) o << (k ? ", " : "") << local[ev[k]];
  }
  o << "]\n}\n";
  return o.str();
}

// The longest chain of ONE op inside the region: the length of the longest path in the sub-DAG
// restricted to nodes of that op. This is the number that decides whether associativity is
// usable, because AC-saturating a chain of n leaves closes over the non-empty subsets of those
// leaves: 2^n - 1 e-classes. It is a property of the identity, not of any implementation.
std::size_t longest_same_op_chain(const Tape& tape, Op op, const std::vector<node_id>& members) {
  const std::unordered_set<node_id> inside(members.begin(), members.end());
  std::unordered_map<node_id, std::size_t> depth;
  std::size_t best = 0;
  for (const node_id id : members) {  // members are ascending, so operands come first
    if (tape[id].op != op) continue;
    std::size_t d = 1;
    const Node& n = tape[id];
    auto consider = [&](node_id a) {
      if (!inside.count(a) || tape[a].op != op) return;
      const auto it = depth.find(a);
      if (it != depth.end()) d = std::max(d, it->second + 1);
    };
    if (op_is_variadic(n.op)) {
      for (const node_id a : tape.args(n)) consider(a);
    } else {
      const int ar = op_arity(n.op);
      if (ar >= 1) consider(n.a);
      if (ar >= 2) consider(n.b);
      if (ar >= 3) consider(n.c);
    }
    depth[id] = d;
    best = std::max(best, d);
  }
  return best;
}

void print_stats(const char* title, const RegionStats& s, std::size_t universe) {
  std::cout << "\n" << title << "\n";
  std::cout << "  regions                 " << s.count << "\n";
  std::cout << "  nodes inside regions    " << s.members;
  if (universe) {
    std::cout << "  (" << std::fixed << std::setprecision(1)
              << 100.0 * static_cast<double>(s.members) / static_cast<double>(universe)
              << "% of " << universe << ")";
  }
  std::cout << std::defaultfloat << "\n";
  std::cout << "  largest region          " << s.largest << "\n";
  std::cout << "  mean / median / p90     " << std::fixed << std::setprecision(2) << s.mean
            << std::defaultfloat << " / " << s.median << " / " << s.p90 << "\n";
  std::cout << "  singletons              " << s.singletons;
  if (s.count) {
    std::cout << " (" << std::fixed << std::setprecision(1)
              << 100.0 * static_cast<double>(s.singletons) / static_cast<double>(s.count) << "%)"
              << std::defaultfloat;
  }
  std::cout << "\n";
  std::cout << "  regions >= 8 / >= 64    " << s.at_least_8 << " / " << s.at_least_64 << "\n";
  std::cout << "  top 10 sizes            ";
  for (std::size_t i = 0; i < std::min<std::size_t>(10, s.sizes.size()); ++i) {
    std::cout << (i ? ", " : "") << s.sizes[i];
  }
  std::cout << "\n";
}

[[noreturn]] void usage(const char* why) {
  std::cerr << "egraph_eval_regions: " << why << "\n"
            << "usage: egraph_eval_regions [--trades N] [--tenors 1Y,2Y,...] [--no-passes]\n"
            << "                           [--dump-largest <path>] [--dump-top K] [--json <path>]\n";
  std::exit(2);
}

std::vector<std::string> split_commas(const std::string& s) {
  std::vector<std::string> out;
  std::string cur;
  std::istringstream in(s);
  while (std::getline(in, cur, ',')) {
    if (!cur.empty()) out.push_back(cur);
  }
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  int trades = 64;
  bool passes = true;
  std::string dump_largest;
  std::string json_path;
  int dump_top = 1;
  std::vector<std::string> tenors;

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&](const char* what) -> std::string {
      if (i + 1 >= argc) usage(what);
      return argv[++i];
    };
    if (a == "--trades") {
      trades = std::atoi(next("--trades needs a value").c_str());
    } else if (a == "--tenors") {
      tenors = split_commas(next("--tenors needs a value"));
    } else if (a == "--no-passes") {
      passes = false;
    } else if (a == "--dump-largest") {
      dump_largest = next("--dump-largest needs a path");
    } else if (a == "--dump-top") {
      dump_top = std::atoi(next("--dump-top needs a value").c_str());
    } else if (a == "--json") {
      json_path = next("--json needs a path");
    } else if (a == "--help" || a == "-h") {
      usage("help");
    } else {
      usage(("unknown argument " + a).c_str());
    }
  }

  fixtures::CompareOisOptions options;
  options.trades = trades;
  if (!tenors.empty()) options.tenors = tenors;

  std::cout << "egraph_eval_regions: compare_ois, trades=" << trades
            << ", passes=" << (passes ? "on" : "off") << "\n";

  const auto t_build0 = std::chrono::steady_clock::now();
  const fixtures::CompareOis s = fixtures::make_compare_ois(options);
  const auto t_build1 = std::chrono::steady_clock::now();
  fixtures::CompareOisTape t = fixtures::record_compare_ois(s, passes);
  const auto t_rec1 = std::chrono::steady_clock::now();

  std::cout << "  quotes " << s.n_quotes() << ", trades " << s.n_trades()
            << ", tape nodes " << t.tape.size() << " (raw " << t.nodes_raw << ", after passes "
            << t.nodes_after_passes << ")\n";
  std::cout << "  build " << std::chrono::duration<double>(t_build1 - t_build0).count() << " s, record "
            << std::chrono::duration<double>(t_rec1 - t_build1).count() << " s\n";

  // Op histogram of the tape.
  {
    const std::vector<std::size_t> h = t.tape.op_histogram();
    std::cout << "  tape op histogram:";
    for (int i = 0; i < op_count; ++i) {
      if (h[static_cast<std::size_t>(i)]) {
        std::cout << " " << to_string(static_cast<Op>(i)) << "=" << h[static_cast<std::size_t>(i)];
      }
    }
    std::cout << "\n";
  }

  const auto t_tr0 = std::chrono::steady_clock::now();
  TapeRegions tr = tape_regions(t.tape);
  const auto t_tr1 = std::chrono::steady_clock::now();
  print_stats("TAPE-level pure-field regions (boundaries: Const, Input, Exp, Log, Sqrt, Select, Cmp*)",
              tr.stats, t.tape.size());
  std::cout << "  largest region leaves   " << tr.stats.largest_leaves << "\n";
  std::cout << "  largest region exits    " << tr.stats.largest_exits << "\n";
  if (tr.stats.largest_root >= 0) {
    const std::vector<node_id>& ms = tr.members[tr.stats.largest_root];
    std::cout << "  longest same-op chain in the largest region (decides whether AC is usable:\n"
              << "  AC-closing a chain of n leaves yields 2^n - 1 e-classes):\n";
    for (const Op op : {Op::Mul, Op::Div, Op::Sub, Op::Sum, Op::Affine}) {
      const std::size_t c = longest_same_op_chain(t.tape, op, ms);
      if (c) {
        std::cout << "    " << to_string(op) << " chain " << c << "  -> AC closure 2^" << (c + 1)
                  << " - 1 e-classes\n";
      }
    }
  }
  std::cout << "  region scan             " << std::chrono::duration<double>(t_tr1 - t_tr0).count()
            << " s\n";

  const auto t_ir0 = std::chrono::steady_clock::now();
  ir::InferStats istats;
  const ir::Program p = ir::infer(t.tape, &istats);
  const auto t_ir1 = std::chrono::steady_clock::now();
  const IrRegionStats irs = ir_regions(p);
  std::cout << "\nIR: " << p.domains.size() << " domains, " << irs.total_steps << " steps ("
            << irs.field_steps << " field), largest group " << irs.largest_group_steps
            << " steps; infer " << std::chrono::duration<double>(t_ir1 - t_ir0).count() << " s\n";
  std::cout << "  IR step op histogram:";
  for (const auto& [name, n] : irs.op_histogram) std::cout << " " << name << "=" << n;
  std::cout << "\n";
  print_stats("IR-level pure-field regions (boundaries: Gather, Segment, Column, Literal, Input, Select, Cmp*)",
              irs.stats, irs.total_steps);

  if (!dump_largest.empty()) {
    std::vector<std::pair<std::size_t, std::int32_t>> by_size;
    for (const auto& [root, ms] : tr.members) by_size.emplace_back(ms.size(), root);
    std::sort(by_size.rbegin(), by_size.rend());
    const int k = std::min<int>(dump_top, static_cast<int>(by_size.size()));
    for (int i = 0; i < k; ++i) {
      const std::int32_t root = by_size[static_cast<std::size_t>(i)].second;
      std::string path = dump_largest;
      if (k > 1) {
        const std::size_t dot = path.rfind('.');
        const std::string stem = dot == std::string::npos ? path : path.substr(0, dot);
        const std::string ext = dot == std::string::npos ? std::string() : path.substr(dot);
        path = stem + "_" + std::to_string(i) + ext;
      }
      std::ofstream f(path);
      if (!f) {
        std::cerr << "cannot write " << path << "\n";
        return 1;
      }
      f << dump_region_json(t.tape, tr.members[root],
                            "compare_ois trades=" + std::to_string(trades) + " region#" + std::to_string(i));
      std::cout << "wrote " << path << " (" << tr.members[root].size() << " member nodes)\n";
    }
  }

  if (!json_path.empty()) {
    std::ofstream f(json_path);
    if (!f) {
      std::cerr << "cannot write " << json_path << "\n";
      return 1;
    }
    f << "{\n  \"fixture\": \"compare_ois\",\n  \"trades\": " << trades << ",\n"
      << "  \"quotes\": " << s.n_quotes() << ",\n"
      << "  \"tape_nodes\": " << t.tape.size() << ",\n"
      << "  \"tape\": {\"regions\": " << tr.stats.count << ", \"members\": " << tr.stats.members
      << ", \"largest\": " << tr.stats.largest << ", \"median\": " << tr.stats.median
      << ", \"singletons\": " << tr.stats.singletons
      << ", \"largest_leaves\": " << tr.stats.largest_leaves
      << ", \"largest_exits\": " << tr.stats.largest_exits << "},\n"
      << "  \"ir\": {\"domains\": " << p.domains.size() << ", \"steps\": " << irs.total_steps
      << ", \"field_steps\": " << irs.field_steps << ", \"regions\": " << irs.stats.count
      << ", \"members\": " << irs.stats.members << ", \"largest\": " << irs.stats.largest
      << ", \"median\": " << irs.stats.median << ", \"singletons\": " << irs.stats.singletons
      << ", \"largest_group_steps\": " << irs.largest_group_steps << "}\n}\n";
    std::cout << "wrote " << json_path << "\n";
  }

  return 0;
}
