// tools/revcollapse — DOES THE ALGEBRA ABOVE THE PIN COLLAPSE THE EMITTED REVERSE, AND IS THE
// COLLAPSED REVERSE STILL THE SAME ANSWER? (PRINCIPLES.md §1b invariant I1; D98-D100 built
// `adjoint(P)`, this measures whether it buys anything.) See CMakeLists.txt for what it prints.
//
// The route, all of it already in the tree:
//
//   T      = the recording                    (fixtures, `passes = false`)
//   Tpin   = compile(T)                        THE PIN (PRINCIPLES.md §1)
//   P      = ir::infer(Tpin)
//   Q      = adjoint::Adjoint(P).to_program()  I1's adjoint(P)
//   T0     = ir::expand(Q)                     Q back as a scalar tape
//   T1     = standard_passes(T0)               cse, dce, fold_sum, affine_collapse, dce
//   T2     = compile(T0)                       + simplify, i.e. the algebra
//
// WHICH SIDE OF THE PIN IS T0 -> T2 ON? It is not stated in the contract and this tool does not
// decide it; it measures what the answer would cost either way, and the distinction is why T1 and
// T2 are reported separately:
//
//   * T1 is the four passes tape/passes.hpp calls VALUE-PRESERVING. The replay after them is
//     bit-identical to the replay before, so T0 -> T1 is a legal below-the-pin rewrite of a
//     below-the-pin artifact today, needing no decision.
//   * T2 adds `simplify`, which is the algebra phase and is licensed ABOVE the pin only. Q is
//     produced BELOW the pin (§1's diagram puts `adjoint` after it), so on the contract as
//     written §5.2's below-the-pin column -- "may change roundings: no" -- forbids it. Reading Q
//     as a fresh recording entering its own pin is the other available reading and would make
//     §1b's I1 gate a tolerance comparison instead of a bitwise one, which §5.2a case 1 says is
//     never done to a failing case-1 gate. That is an owner decision.
//
// MEASURED, the question is moot on every program in the tree: T2 is bitwise equal to
// `Adjoint::run` as well, so the stricter standard is met and no licence is needed. That is not a
// theorem -- `simplify`'s `add(0,x)->x` is not an IEEE identity when x is `-0.0`, and the
// telescope is not an identity in floating point at all -- so this tool prints the bit-mismatch
// count rather than asserting it, and prints how many of any mismatches are the sign of a zero.
//
// WHY THE ARITHMETIC COUNTS AND NOT ONLY THE NODE COUNTS. A tape node is not an operation. The
// emitted reverse's accumulation chains are `Add(Lit +0.0, t)` and its pulls are
// `Affine(konst +0.0, coefs, members)`; `Adjoint::run` does the same additions (`pull()` zeroes
// `dst` and then `+=` per contribution) but no multiply for a unit coefficient on an output,
// gather or Sum reader, and -- `acc_div` -- only ONE quotient where the emitter, which must put
// the two operand chains in two domains, writes two. So the node ratio overstates the arithmetic
// ratio and this tool reports both.
//
// AND WHETHER THE DUPLICATION IS REAL. `cse` finds duplicate structure in the EXPANDED tape, which
// would mean nothing if `ir::expand` had introduced it. So the same question is asked of Q itself
// (`q_duplication` below), over the segments the plan carries.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "epykos/adjoint/adjoint.hpp"
#include "epykos/compile.hpp"
#include "epykos/exec/interpreter.hpp"
#include "epykos/fixtures/affine_scan.hpp"
#include "epykos/fixtures/compare_ois.hpp"
#include "epykos/fixtures/instrument_sample.hpp"
#include "epykos/fixtures/m1_book.hpp"
#include "epykos/fixtures/nearmiss_shapes.hpp"
#include "epykos/fixtures/record_m1.hpp"
#include "epykos/fixtures/rfr_book.hpp"
#include "epykos/fixtures/rfr_price.hpp"
#include "epykos/fixtures/spike_telescoped.hpp"
#include "epykos/fixtures/stage_a.hpp"
#include "epykos/ir/evaluate.hpp"
#include "epykos/ir/expand.hpp"
#include "epykos/ir/program.hpp"
#include "epykos/ir/signature.hpp"
#include "epykos/tape/passes.hpp"
#include "epykos/tape/replay.hpp"
#include "epykos/tape/tape.hpp"

namespace {

using namespace epykos;

bool same_bits(double a, double b) { return std::memcmp(&a, &b, sizeof(double)) == 0; }

// ---- the graph of a scalar tape ---------------------------------------------------------------
void operands(const Tape& t, node_id n, std::vector<node_id>* out) {
  out->clear();
  const Node& nd = t.node(n);
  switch (nd.op) {
    case Op::Const:
    case Op::Input:
      return;
    case Op::Sum:
    case Op::Affine: {
      const node_id* a = t.arg_table().data() + nd.first_arg;
      for (std::int32_t k = 0; k < nd.nargs; ++k) out->push_back(a[k]);
      return;
    }
    default:
      break;
  }
  const int ar = op_arity(nd.op);
  if (ar >= 1) out->push_back(nd.a);
  if (ar >= 2) out->push_back(nd.b);
  if (ar >= 3) out->push_back(nd.c);
}

std::vector<char> reach(const Tape& t, std::size_t lo, std::size_t hi) {
  std::vector<char> m(t.size(), 0);
  std::vector<node_id> work, ops;
  const std::vector<node_id>& outs = t.outputs();
  for (std::size_t o = lo; o < hi && o < outs.size(); ++o) {
    if (!m[outs[o]]) {
      m[outs[o]] = 1;
      work.push_back(outs[o]);
    }
  }
  while (!work.empty()) {
    const node_id n = work.back();
    work.pop_back();
    operands(t, n, &ops);
    for (node_id k : ops) {
      if (k >= 0 && !m[k]) {
        m[k] = 1;
        work.push_back(k);
      }
    }
  }
  return m;
}

// Q's outputs are (P's outputs, then P's state adjoints), so the first `n_fwd` outputs cut the
// tape into the forward half -- which IS the pinned forward tape, asserted below -- the
// reverse-only half, what both need, and what nothing needs.
enum Half { kFwd = 0, kRev = 1, kShared = 2, kDead = 3 };
struct Cone {
  std::size_t fwd = 0, rev = 0, shared = 0, dead = 0;
};
std::vector<char> halves(const Tape& t, std::size_t n_fwd, Cone* c) {
  const std::vector<char> f = reach(t, 0, n_fwd);
  const std::vector<char> r = reach(t, n_fwd, t.num_outputs());
  std::vector<char> h(t.size(), kDead);
  for (std::size_t i = 0; i < t.size(); ++i) {
    if (f[i] && r[i]) {
      h[i] = kShared;
      ++c->shared;
    } else if (f[i]) {
      h[i] = kFwd;
      ++c->fwd;
    } else if (r[i]) {
      h[i] = kRev;
      ++c->rev;
    } else {
      ++c->dead;
    }
  }
  return h;
}

// ---- arithmetic, over one half of the tape -----------------------------------------------------
struct Ops {
  long long nodes = 0, adds = 0, muls = 0, divs = 0, trans = 0, other = 0;
  long long zero_adds = 0;  // an Add against a literal zero: the emitter's accumulator start
  long long total() const { return adds + muls + divs + trans + other; }
};
Ops arithmetic(const Tape& t, const std::vector<char>& h, char want) {
  Ops o;
  for (std::size_t i = 0; i < t.size(); ++i) {
    if (h[i] != want) continue;
    const Node& n = t.node(i);
    if (n.op == Op::Const || n.op == Op::Input) continue;
    ++o.nodes;
    const auto zero = [&](node_id x) {
      const Node& c = t.node(x);
      return c.op == Op::Const && c.konst == 0.0;
    };
    switch (n.op) {
      case Op::Add:
        ++o.adds;
        if (zero(n.a) || zero(n.b)) ++o.zero_adds;
        break;
      case Op::Sub:
      case Op::Neg: ++o.adds; break;
      case Op::Mul: ++o.muls; break;
      case Op::Div:
      case Op::Recip: ++o.divs; break;
      case Op::Exp:
      case Op::Log:
      case Op::Sqrt: ++o.trans; break;
      case Op::Sum: o.adds += n.nargs - 1; break;  // the first member is a copy
      case Op::Affine: {
        o.adds += n.nargs;  // `acc = konst` then one `+=` per member, as the pull does
        const double* c = t.coef_table().data() + n.first_arg;
        for (std::int32_t k = 0; k < n.nargs; ++k) {
          if (c[k] != 1.0) ++o.muls;  // a unit coefficient is not a multiply in `pull()`
        }
        break;
      }
      default: ++o.other; break;
    }
  }
  return o;
}

// ---- `simplify`'s rule sites, read off src/tape/passes.cpp's `simplify_binary` ----------------
std::map<std::string, long long> rule_sites(const Tape& t, const std::vector<char>& h, bool rev_only) {
  std::map<std::string, long long> m;
  const auto konst_is = [&](node_id x, double v) {
    const Node& n = t.node(x);
    return n.op == Op::Const && n.konst == v;
  };
  for (std::size_t i = 0; i < t.size(); ++i) {
    if (rev_only && h[i] != kRev) continue;
    const Node& n = t.node(i);
    const char* rule = nullptr;
    switch (n.op) {
      case Op::Add:
        if (konst_is(n.a, 0.0) || konst_is(n.b, 0.0)) rule = "add(0,x)->x";
        else if (t.node(n.b).op == Op::Sub && t.node(n.b).b == n.a) rule = "add(a,sub(x,a))->x";
        else if (t.node(n.a).op == Op::Sub && t.node(n.a).b == n.b) rule = "add(sub(x,a),a)->x";
        break;
      case Op::Sub:
        if (konst_is(n.b, 0.0)) rule = "sub(x,0)->x";
        else if (n.a == n.b) rule = "sub(x,x)->0";
        else if (t.node(n.a).op == Op::Add && (t.node(n.a).b == n.b || t.node(n.a).a == n.b))
          rule = "sub(add(x,a),a)->x";
        break;
      case Op::Mul:
        if (konst_is(n.a, 1.0) || konst_is(n.b, 1.0)) rule = "mul(1,x)->x";
        else if (t.node(n.a).op == Op::Div && t.node(n.a).b == n.b) rule = "mul(div(x,c),c)->x";
        else if (t.node(n.b).op == Op::Div && t.node(n.b).b == n.a) rule = "mul(c,div(x,c))->x";
        else if (t.node(n.a).op == Op::Div && t.node(n.b).op == Op::Div &&
                 (t.node(n.a).b == t.node(n.b).a || t.node(n.b).b == t.node(n.a).a))
          rule = "TELESCOPE";
        break;
      case Op::Div:
        if (konst_is(n.b, 1.0)) rule = "div(x,1)->x";
        else if (n.a == n.b) rule = "div(x,x)->1";
        else if (t.node(n.a).op == Op::Mul && (t.node(n.a).b == n.b || t.node(n.a).a == n.b))
          rule = "div(mul(x,c),c)->x";
        break;
      default: break;
    }
    if (rule != nullptr) ++m[rule];
  }
  return m;
}

// ---- what `cse` would merge, by op (first order: its hash-cons also cascades) ------------------
std::map<std::string, long long> cse_sites(const Tape& t, const std::vector<char>& h, bool rev_only) {
  std::map<std::string, long long> m;
  std::unordered_map<std::string, char> seen;
  std::vector<node_id> ops;
  for (std::size_t i = 0; i < t.size(); ++i) {
    const Node& n = t.node(i);
    if (n.op == Op::Input) continue;
    operands(t, static_cast<node_id>(i), &ops);
    if (n.op == Op::Add || n.op == Op::Mul) std::sort(ops.begin(), ops.end());
    std::string key(to_string(n.op));
    std::uint64_t kb = 0;
    std::memcpy(&kb, &n.konst, sizeof kb);
    key += ":" + std::to_string(kb);
    for (node_id o : ops) key += "," + std::to_string(o);
    if (n.op == Op::Affine) {
      const double* c = t.coef_table().data() + n.first_arg;
      for (std::int32_t k = 0; k < n.nargs; ++k) {
        std::uint64_t cb = 0;
        std::memcpy(&cb, &c[k], sizeof cb);
        key += "/" + std::to_string(cb);
      }
    }
    if (seen.emplace(key, 1).second) continue;
    if (rev_only && h[i] != kRev) continue;
    ++m[to_string(n.op)];
  }
  return m;
}

// ---- seeds -------------------------------------------------------------------------------------
// The all-ones seed makes every chain live; the zero seed and the mixed signed-zero seed are the
// two that can expose the sign of a zero, which is the only thing `add(0,x)->x` can move.
std::vector<std::vector<double>> seeds_for(std::size_t n) {
  std::vector<std::vector<double>> s;
  s.emplace_back(n, 1.0);
  s.emplace_back(n, 0.0);
  {
    std::vector<double> e(n, 0.0);
    e[n - 1] = 1.0;
    s.push_back(e);
  }
  {
    std::vector<double> m(n, 0.0);
    for (std::size_t o = 0; o < n; ++o) m[o] = static_cast<double>(static_cast<int>(o % 3) - 1);
    s.push_back(m);
  }
  {
    std::vector<double> m(n, 0.0);
    for (std::size_t o = 0; o < n; ++o) m[o] = (o % 2 == 0) ? -0.0 : 1.0;
    s.push_back(m);
  }
  return s;
}

struct Mismatch {
  long long compared = 0, bits = 0, signed_zero = 0;
  double max_rel = 0.0;
};
void compare_into(Mismatch* m, const std::vector<double>& got, const std::vector<double>& ref) {
  for (std::size_t i = 0; i < ref.size() && i < got.size(); ++i) {
    ++m->compared;
    if (same_bits(got[i], ref[i])) continue;
    ++m->bits;
    if (got[i] == 0.0 && ref[i] == 0.0) ++m->signed_zero;
    if (std::fabs(ref[i]) > 0.0) {
      const double rel = std::fabs(got[i] - ref[i]) / std::fabs(ref[i]);
      if (rel > m->max_rel) m->max_rel = rel;
    }
  }
}

// ---- the same question asked of Q ITSELF, not of its expansion ---------------------------------
//
// `cse` on the expanded tape removes duplicate structure; this says whether that duplication is
// already in the plan `adjoint::Adjoint::run` walks, or is an artefact of `ir::expand`. Q's pulls
// and folds are `Affine` / `Sum` steps over SEGMENTS, one row per target, so two rows with the
// identical (member, coefficient) list are two folds `run()` evaluates separately over identical
// inputs. A row with one unit-coefficient member is `dst = 0.0; dst += src` -- `pull()` in
// src/adjoint/adjoint_e0.cpp, which zeroes and then accumulates -- where `dst = src` would do.
struct QDup {
  long long rows = 0, empty = 0, identity = 0, duplicate = 0;
};
QDup q_duplication(const ir::Program& q) {
  QDup d;
  std::unordered_map<std::string, char> seen;
  for (const ir::Segment& sg : q.segments) {
    for (std::size_t r = 0; r + 1 < sg.offsets.size(); ++r) {
      const std::int32_t lo = sg.offsets[r], hi = sg.offsets[r + 1];
      ++d.rows;
      if (lo == hi) {
        ++d.empty;
        continue;
      }
      if (hi - lo == 1 && (sg.coefs.empty() || sg.coefs[static_cast<std::size_t>(lo)] == 1.0)) ++d.identity;
      std::string key;
      for (std::int32_t m = lo; m < hi; ++m) {
        key += std::to_string(sg.members[static_cast<std::size_t>(m)]);
        if (!sg.coefs.empty()) {
          std::uint64_t cb = 0;
          const double c = sg.coefs[static_cast<std::size_t>(m)];
          std::memcpy(&cb, &c, sizeof cb);
          key += ":" + std::to_string(cb);
        }
        key += ",";
      }
      if (!seen.emplace(key, 1).second) ++d.duplicate;
    }
  }
  return d;
}

// ---- one fixture -------------------------------------------------------------------------------
void measure(const std::string& name, Tape raw, const std::vector<int>& batches) {
  const std::size_t fwd_raw = raw.size();
  compile(raw);
  raw.validate();
  const std::size_t fwd_pin = raw.size();
  const ir::Program p = ir::infer(raw);

  adjoint::Options ao;
  ao.max_batch = *std::max_element(batches.begin(), batches.end());
  const adjoint::Adjoint ad(p, ao);
  const ir::Program q = ad.to_program();

  const std::size_t n_in = p.inputs.size(), n_out = p.outputs.size();
  Tape t0 = ir::expand(q);
  t0.validate();
  Tape t1 = t0, t2 = t0;
  standard_passes(t1);
  t1.validate();
  compile(t2);
  t2.validate();
  const ir::Program q2 = ir::infer(t2);

  Cone c0, c1, c2;
  const std::vector<char> h0 = halves(t0, n_out, &c0);
  const std::vector<char> h1 = halves(t1, n_out, &c1);
  const std::vector<char> h2 = halves(t2, n_out, &c2);

  std::printf("\n================================ %s ================================\n", name.c_str());
  std::printf("  FORWARD  tape  %9zu recorded -> %9zu pinned      (%.3gx)\n", fwd_raw, fwd_pin,
              fwd_pin ? double(fwd_raw) / double(fwd_pin) : 0.0);
  std::printf("  P              %9zu values / %zu domains / %zu scans / %zu in / %zu out\n",
              p.num_values(), p.domains.size(), ir::scan_domains(p).size(), n_in, n_out);
  std::printf("  Q = adjoint(P) %9zu values / %zu domains      (%.3gx P values, %.3gx P domains)\n",
              q.num_values(), q.domains.size(), double(q.num_values()) / double(p.num_values()),
              double(q.domains.size()) / double(p.domains.size()));
  std::printf("  REVERSE  tape  %9zu raw -> %9zu std_passes (%.3gx) -> %9zu compile (%.3gx)\n",
              t0.size(), t1.size(), double(t0.size()) / double(t1.size()), t2.size(),
              double(t0.size()) / double(t2.size()));
  std::printf("  infer(T2)      %9zu values / %zu domains / %zu scans   (Q had %zu domains: "
              "%.3g rows per domain against %.3g)\n",
              q2.num_values(), q2.domains.size(), ir::scan_domains(q2).size(), q.domains.size(),
              double(q2.num_values()) / double(q2.domains.size()),
              double(q.num_values()) / double(q.domains.size()));

  std::printf("  HALVES of the reverse tape\n");
  std::printf("    raw      fwd %8zu + shared %8zu = %8zu (the pinned forward tape is %zu%s)"
              " | REV-ONLY %9zu | dead %6zu\n",
              c0.fwd, c0.shared, c0.fwd + c0.shared, fwd_pin,
              (c0.fwd + c0.shared) == fwd_pin ? ", equal" : ", DIFFERENT", c0.rev, c0.dead);
  std::printf("    std                                                "
              "                              | REV-ONLY %9zu | dead %6zu\n", c1.rev, c1.dead);
  std::printf("    compile                                            "
              "                              | REV-ONLY %9zu | dead %6zu\n", c2.rev, c2.dead);
  std::printf("    REV-ONLY nodes: %zu -> %zu std (%.3gx) -> %zu compile (%.3gx)\n", c0.rev, c1.rev,
              c1.rev ? double(c0.rev) / double(c1.rev) : 0.0, c2.rev,
              c2.rev ? double(c0.rev) / double(c2.rev) : 0.0);

  std::printf("  ARITHMETIC of the reverse-only half (not nodes; see this file's header)\n");
  const Ops a0 = arithmetic(t0, h0, kRev);
  const Ops a1 = arithmetic(t1, h1, kRev);
  const Ops a2 = arithmetic(t2, h2, kRev);
  const Ops fwd_ops = [&] {
    Ops o = arithmetic(t0, h0, kFwd);
    const Ops s = arithmetic(t0, h0, kShared);
    o.adds += s.adds; o.muls += s.muls; o.divs += s.divs; o.trans += s.trans; o.other += s.other;
    return o;
  }();
  // `0 + t` counts EXPLICIT `Add` nodes against a literal zero. After `fold_sum` /
  // `affine_collapse` those same additions live inside a `Sum` / `Affine` fold, where this
  // detector cannot see them -- which is why the std row reads 0 there while its `add` count is
  // unchanged. The OPS column is the one to read.
  const auto row = [&](const char* tag, const Ops& a) {
    std::printf("    %-9s add %9lld (explicit `0 + t` %8lld) mul %9lld div %8lld trans %7lld "
                "other %6lld | OPS %10lld  (%.3gx)\n",
                tag, a.adds, a.zero_adds, a.muls, a.divs, a.trans, a.other, a.total(),
                a.total() ? double(a0.total()) / double(a.total()) : 0.0);
  };
  row("raw", a0);
  row("std", a1);
  row("compile", a2);
  std::printf("    the forward half for scale: OPS %lld, so the reverse is %.2fx the forward raw and"
              " %.2fx collapsed\n",
              fwd_ops.total(), fwd_ops.total() ? double(a0.total()) / double(fwd_ops.total()) : 0.0,
              fwd_ops.total() ? double(a2.total()) / double(fwd_ops.total()) : 0.0);

  std::printf("  PER-PASS, in `compile`'s own order, to a fixpoint (src/compile.cpp)\n");
  {
    Tape t = t0;
    for (int round = 0; round < 8; ++round) {
      const std::size_t before = t.size();
      const auto one = [&](const char* nm, const std::function<void()>& fn) {
        const std::size_t b = t.size();
        fn();
        if (b != t.size()) std::printf("    %-16s %9zu -> %9zu   removed %8zu\n", nm, b, t.size(), b - t.size());
      };
      one("simplify", [&] { simplify(t); });
      one("cse", [&] { cse(t); });
      one("dce", [&] { dce(t); });
      one("fold_sum", [&] { fold_sum(t); });
      one("affine_collapse", [&] { affine_collapse(t); });
      one("dce", [&] { dce(t); });
      if (round > 0 && t.size() >= before) break;
    }
  }
  std::printf("  LEAVE-ONE-OUT, the same loop with one pass disabled (compile = %zu nodes)\n", t2.size());
  {
    const char* names[] = {"simplify", "cse", "dce", "fold_sum", "affine_collapse"};
    for (const char* skip : names) {
      Tape t = t0;
      for (int round = 0; round < 8; ++round) {
        const std::size_t before = t.size();
        if (std::strcmp(skip, "simplify") != 0) simplify(t);
        if (std::strcmp(skip, "cse") != 0) cse(t);
        if (std::strcmp(skip, "dce") != 0) dce(t);
        if (std::strcmp(skip, "fold_sum") != 0) fold_sum(t);
        if (std::strcmp(skip, "affine_collapse") != 0) affine_collapse(t);
        if (std::strcmp(skip, "dce") != 0) dce(t);
        if (round > 0 && t.size() >= before) break;
      }
      std::printf("    without %-16s %9zu   (+%lld)\n", skip, t.size(),
                  static_cast<long long>(t.size()) - static_cast<long long>(t2.size()));
    }
  }
  std::printf("  `simplify` RULE SITES in the raw reverse tape (reverse-only half in brackets)\n");
  {
    const std::map<std::string, long long> all = rule_sites(t0, h0, false);
    const std::map<std::string, long long> rev = rule_sites(t0, h0, true);
    if (all.empty()) std::printf("    none\n");
    for (const auto& kv : all) {
      const auto it = rev.find(kv.first);
      std::printf("    %-24s %9lld   [%lld]\n", kv.first.c_str(), kv.second,
                  it == rev.end() ? 0LL : it->second);
    }
  }
  std::printf("  `cse`-MERGEABLE nodes, first order (reverse-only half in brackets)\n");
  {
    const std::map<std::string, long long> all = cse_sites(t0, h0, false);
    const std::map<std::string, long long> rev = cse_sites(t0, h0, true);
    if (all.empty()) std::printf("    none\n");
    for (const auto& kv : all) {
      const auto it = rev.find(kv.first);
      std::printf("    %-24s %9lld   [%lld]\n", kv.first.c_str(), kv.second,
                  it == rev.end() ? 0LL : it->second);
    }
  }
  {
    long long affines = 0, single = 0, unit_single = 0;
    for (std::size_t i = 0; i < t0.size(); ++i) {
      const Node& n = t0.node(i);
      if (n.op != Op::Affine || h0[i] != kRev) continue;  // the pulls of the reverse half only
      ++affines;
      if (n.nargs != 1) continue;
      ++single;
      if (n.konst == 0.0 && t0.coef_table()[static_cast<std::size_t>(n.first_arg)] == 1.0) ++unit_single;
    }
    std::printf("    of %lld Affine (pull) nodes in the reverse-only half, %lld have ONE member and"
                " %lld are `+0.0 + 1.0*x`, an identity (%.1f%%)\n",
                affines, single, unit_single, affines ? 100.0 * double(unit_single) / double(affines) : 0.0);
  }
  {
    const QDup d = q_duplication(q);
    std::printf("  THE SAME DUPLICATION, measured in Q ITSELF -- the plan `Adjoint::run` walks, not"
                " its expansion\n");
    std::printf("    %lld segment rows (pull and fold inputs): %lld empty, %lld an identity copy "
                "(%.1f%%), %lld an exact duplicate of an earlier row (%.1f%%)\n",
                d.rows, d.empty, d.identity, d.rows ? 100.0 * double(d.identity) / double(d.rows) : 0.0,
                d.duplicate, d.rows ? 100.0 * double(d.duplicate) / double(d.rows) : 0.0);
  }

  // ---- the answer -------------------------------------------------------------------------------
  std::printf("  BITWISE against `adjoint::Adjoint::run`, %zu seeds x %zu batch sizes\n",
              seeds_for(n_out).size(), batches.size());
  Mismatch m0, m1, m2, me;
  std::vector<std::unique_ptr<exec::Interpreter>> i0, i1;
  for (int B : batches) {
    exec::Options eo;
    eo.max_batch = B;
    eo.exp = exec::ExpMode::std_exp;  // E0: the only mode a bitwise comparison may use
    i0.push_back(std::make_unique<exec::Interpreter>(q, eo));
    i1.push_back(std::make_unique<exec::Interpreter>(q2, eo));
  }
  Replayer r0(t0), r1(t1), r2(t2);
  const std::vector<std::vector<double>> seeds = seeds_for(n_out);
  for (const std::vector<double>& sd : seeds) {
    for (std::size_t bi = 0; bi < batches.size(); ++bi) {
      const std::size_t B = static_cast<std::size_t>(batches[bi]);
      std::vector<double> state(n_in * B, 0.0), out_bar(n_out * B, 0.0);
      for (std::size_t b = 0; b < B; ++b) {
        for (std::size_t k = 0; k < n_in; ++k) state[k * B + b] = p.input_values[k];
        for (std::size_t k = 0; k < n_out; ++k) out_bar[k * B + b] = sd[k];
      }
      std::vector<double> out(n_out * B, 0.0), sbar(n_in * B, 0.0);
      ad.run(state.data(), batches[bi], out_bar.data(), out.data(), sbar.data());
      std::vector<double> ref;
      ref.insert(ref.end(), out.begin(), out.end());
      ref.insert(ref.end(), sbar.begin(), sbar.end());
      std::vector<double> in;
      in.insert(in.end(), state.begin(), state.end());
      in.insert(in.end(), out_bar.begin(), out_bar.end());
      // Q itself, through exec::Interpreter -- I1's own gate, re-run here as the control.
      {
        std::vector<double> got(q.outputs.size() * B, 0.0);
        i0[bi]->run(in.data(), batches[bi], got.data());
        compare_into(&me, got, ref);
      }
      // the collapsed reverse, as a Program, through exec::Interpreter
      {
        std::vector<double> got(q2.outputs.size() * B, 0.0);
        i1[bi]->run(in.data(), batches[bi], got.data());
        compare_into(&m2, got, ref);
      }
      if (B != 1) continue;  // the scalar replays are B = 1
      std::vector<double> v0(t0.num_outputs(), 0.0), v1(t1.num_outputs(), 0.0), v2(t2.num_outputs(), 0.0);
      r0.run(in.data(), v0.data());
      r1.run(in.data(), v1.data());
      r2.run(in.data(), v2.data());
      compare_into(&m0, v0, ref);
      compare_into(&m1, v1, ref);
      compare_into(&m2, v2, ref);
    }
  }
  const auto verdict = [](const char* tag, const Mismatch& m) {
    std::printf("    %-34s %9lld compared, %lld differ (%lld only in the sign of a zero), "
                "worst relative %.3e%s\n",
                tag, m.compared, m.bits, m.signed_zero, m.max_rel, m.bits == 0 ? "   BITWISE" : "");
  };
  verdict("Q, exec::Interpreter [control]", me);
  verdict("expand(Q), Replayer", m0);
  verdict("+ standard_passes, Replayer", m1);
  verdict("+ compile, Replayer and Interpreter", m2);
  std::fflush(stdout);
}

struct Entry {
  const char* name;
  std::function<Tape()> build;
  std::vector<int> batches;
};

const std::vector<Entry>& fixture_table() {
  static const std::vector<Entry> kAll = {
      {"nearmiss", [] { return fixtures::record_nearmiss(); }, {1, 4}},
      {"m1_book", [] { return fixtures::record_m1_raw(fixtures::make_m1_book()); }, {1, 4}},
      {"affine_scan(final)",
       [] { return fixtures::record_affine_scan(fixtures::make_affine_scan(), false, false); }, {1, 4}},
      {"affine_scan(path)",
       [] { return fixtures::record_affine_scan(fixtures::make_affine_scan(), true, false); }, {1, 4}},
      {"instrument_sample",
       [] { return fixtures::record_sample(fixtures::make_instrument_sample(), false); }, {1, 4}},
      {"rfr_book", [] { return fixtures::record_rfr(fixtures::make_rfr_book(), false); }, {1, 4}},
      // compare_ois in BOTH coupon forms (D81 §7, fixtures/spike_telescoped.hpp). The two recordings
      // differ by two orders of magnitude and compile to the SAME pinned tape, so they have the same
      // Q and the same reverse -- which is the point, and is why only the first column moves.
      {"compare_ois(naive, 16)",
       [] {
         fixtures::CompareOisOptions o;
         o.trades = 16;
         return fixtures::spike::record_compare_ois_spike(fixtures::make_compare_ois(o),
                                                          fixtures::spike::Form::naive, false)
             .tape;
       },
       {1, 4}},
      {"compare_ois(telescoped, 16)",
       [] {
         fixtures::CompareOisOptions o;
         o.trades = 16;
         return fixtures::spike::record_compare_ois_spike(fixtures::make_compare_ois(o),
                                                          fixtures::spike::Form::telescoped, false)
             .tape;
       },
       {1, 4}},
      {"compare_ois(naive, 64)",
       [] {
         fixtures::CompareOisOptions o;
         o.trades = 64;
         return fixtures::spike::record_compare_ois_spike(fixtures::make_compare_ois(o),
                                                          fixtures::spike::Form::naive, false)
             .tape;
       },
       {1, 4}},
      {"compare_ois(telescoped, 256)",
       [] {
         fixtures::CompareOisOptions o;
         o.trades = 256;
         return fixtures::spike::record_compare_ois_spike(fixtures::make_compare_ois(o),
                                                          fixtures::spike::Form::telescoped, false)
             .tape;
       },
       {1, 4}},
      // The desk problem. Recording it peaks the process at ~3.8 GB before the adjoint is touched
      // (D100 §4), so it is not in the default set: ask for it by name.
      {"stage_a", [] { return fixtures::record_stage_a(fixtures::make_stage_a(), false).tape; }, {1}},
  };
  return kAll;
}

}  // namespace

int main(int argc, char** argv) {
  const std::string which = argc > 1 ? argv[1] : "";
  if (which == "--list") {
    for (const Entry& e : fixture_table()) std::printf("%s\n", e.name);
    return 0;
  }
  bool any = false;
  for (const Entry& e : fixture_table()) {
    const std::string n = e.name;
    const bool is_stage_a = n == "stage_a";
    const bool selected = which.empty() ? !is_stage_a
                                        : (which == "all" || n == which || n.rfind(which, 0) == 0);
    if (!selected) continue;
    any = true;
    measure(n, e.build(), e.batches);
  }
  if (!any) {
    std::fprintf(stderr, "revcollapse: no fixture matches \"%s\" (try --list, or \"all\")\n", which.c_str());
    return 2;
  }
  return 0;
}
