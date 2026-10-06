// EpykosEngine — the reverse of a Program emitted AS a Program
// (include/epykos/adjoint/adjoint_to_program.hpp; PRINCIPLES.md §1b invariant I1, stages C2/C3a).
//
// An E0 TU (src/**/*_e0.cpp, root CMakeLists.txt, D25): compiled with -ffp-contract=off in every
// preset on every compiler. This file does no floating-point arithmetic of its own — it WRITES
// the literals +0.0, 1.0 and 0.5 into the emitted program and copies `plan.affine_coefs` — but it
// is pinned anyway, because a constant this file writes is one the emitted program's kernels
// then fold, and a pass that mints the constants of an E0 program is not the place to discover
// that a compiler was allowed to touch them.
//
// Everything structural is in the header. What follows is the one invariant this file is written
// against, restated once because every line depends on it:
//
//   the emitted program must be BITWISE adjoint::Adjoint::run, not "the same derivative".
//
// So each emitted chain is the literal transcription of an `acc_*` kernel of adjoint_e0.cpp, in
// that file's own order (a domain's steps last to first; an op's operands a, b, c; a value's
// readers in plan.hpp's CSR order), starting from the +0.0 those kernels memset.
#ifndef EPYKOS_FP_CONTRACT_OFF
#error "adjoint_to_program_e0.cpp must be compiled with -ffp-contract=off (see the *_e0.cpp rule in CMakeLists.txt)"
#endif

#include "epykos/adjoint/adjoint_to_program.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "epykos/mutation/mutation.hpp"

namespace epykos::adjoint {

namespace {

using ir::Column;
using ir::Domain;
using ir::Gather;
using ir::Group;
using ir::Program;
using ir::Segment;
using ir::Slot;
using ir::SlotKind;
using ir::Step;
using ir::domain_id;
using ir::value_id;

inline std::size_t ix(std::int32_t i) noexcept { return static_cast<std::size_t>(i); }
inline std::int32_t i32(std::size_t i) noexcept { return static_cast<std::int32_t>(i); }

// ---- operands -------------------------------------------------------------------------------
//
// What an emitted step reads: a literal (uniform over the domain's rows), a column (one double
// per row) or a per-row value id (a gather). "Per-row value id" covers both a real gather of the
// forward program, remapped, and the IDENTITY gather value_base + r that reads one emitted
// domain's rows from another of the same length.
struct Operand {
  enum class Kind { Literal, Column, Values, Direct } kind = Kind::Literal;
  double literal = 0.0;
  std::vector<double> column;
  std::vector<value_id> values;
  Slot direct{};  // Kind::Direct: a slot already in the host group (an earlier Step of it)

  static Operand lit(double v) {
    Operand o;
    o.kind = Kind::Literal;
    o.literal = v;
    return o;
  }
  // An earlier step of the SAME group. The reverse of a scan needs this: the chain from v̄ to the
  // carry's edge slot must live in one group (a cross-domain cycle is not expressible), so its
  // intermediates are steps rather than their own domains read back through identity gathers.
  static Operand direct_slot(Slot s) {
    Operand o;
    o.kind = Kind::Direct;
    o.direct = s;
    return o;
  }
  static Operand col(std::vector<double> v) {
    Operand o;
    o.kind = Kind::Column;
    o.column = std::move(v);
    return o;
  }
  static Operand vals(std::vector<value_id> v) {
    Operand o;
    o.kind = Kind::Values;
    o.values = std::move(v);
    return o;
  }
};

// ---- the accumulation kernels, as shapes ------------------------------------------------------
//
// One entry per `acc_*` kernel of src/adjoint/adjoint_e0.cpp, named after it. `acc_exp` is
// `acc_plus_mul` with the step's own value as the multiplier, and `acc_div`'s numerator branch is
// `acc_plus_div`, so they share an entry.
enum class Acc {
  Plus,         // acc_plus:         t += ȳ
  Minus,        // acc_minus:        t -= ȳ
  PlusMul,      // acc_plus_mul / acc_exp:  t += ȳ·m
  PlusDiv,      // acc_plus_div / acc_div (numerator): t += ȳ/m
  Sqrt,         // acc_sqrt:         t += (0.5·ȳ)/y
  Recip,        // acc_recip:        t -= (ȳ·y)·y
  DivDen,       // acc_div (denominator): t -= (ȳ/m)·y
  DivAliased,   // acc_div_aliased:  t += (ȳ/m)·(1 − y)
  SelectTrue,   // acc_select_true:  t += p ? ȳ : 0.0
  SelectFalse,  // acc_select_false: t += p ? 0.0 : ȳ
};

// One pending contribution into one target's chain. `ybar` is the adjoint being pushed; `m` the
// operand the kernel multiplies or divides by; `y` the step's own forward value; `pred` a
// Select's predicate.
struct Contribution {
  Acc how = Acc::Plus;
  Operand ybar;
  Operand m;
  Operand y;
  Operand pred;
};

// ---- the emitted program ----------------------------------------------------------------------
struct Builder {
  Program q;
  std::map<std::uint64_t, std::int32_t> literal_of;  // bit pattern -> Program::literals index
  bool drop_zero_start = false;                      // mutant a2p.drop_zero_start

  // Literals are deduplicated by BIT PATTERN, so +0.0 and -0.0 stay distinct entries: the
  // difference between them is the whole of trap 1 and trap 4 (header, points 1 and 4).
  std::int32_t literal(double v) {
    std::uint64_t bits = 0;
    std::memcpy(&bits, &v, sizeof bits);
    const auto it = literal_of.find(bits);
    if (it != literal_of.end()) return it->second;
    q.literals.push_back(v);
    const std::int32_t i = i32(q.literals.size()) - 1;
    literal_of.emplace(bits, i);
    return i;
  }
  Slot lit_slot(double v) { return Slot{SlotKind::Literal, literal(v)}; }

  // Opens a domain of `rows` rows with an empty group and returns its id.
  domain_id open(std::int32_t rows, std::string name) {
    const domain_id id = i32(q.domains.size());
    Domain d;
    d.name = std::move(name);
    d.rows = rows;
    d.value_base = static_cast<value_id>(q.num_values());
    q.domains.push_back(std::move(d));
    Group g;
    g.domain = id;
    q.groups.push_back(std::move(g));
    return id;
  }

  // Materialises an operand as a slot of domain `d` (a literal, a fresh column, a fresh gather).
  Slot slot_of(domain_id d, const Operand& o) {
    switch (o.kind) {
      case Operand::Kind::Direct:
        return o.direct;
      case Operand::Kind::Literal:
        return lit_slot(o.literal);
      case Operand::Kind::Column: {
        Column c;
        c.domain = d;
        c.values = o.column;
        q.columns.push_back(std::move(c));
        return Slot{SlotKind::Column, i32(q.columns.size()) - 1};
      }
      case Operand::Kind::Values:
      default: {
        Gather g;
        g.domain = d;
        g.index = o.values;
        q.gathers.push_back(std::move(g));
        return Slot{SlotKind::Gather, i32(q.gathers.size()) - 1};
      }
    }
  }

  // Appends a step to domain `d`'s group and returns a Slot naming it.
  Slot push(domain_id d, Step s) {
    std::vector<Step>& steps = q.groups[ix(d)].steps;
    steps.push_back(s);
    return Slot{SlotKind::Step, i32(steps.size()) - 1};
  }

  // The value ids of a finished domain's rows, in row order: the index of an identity gather.
  std::vector<value_id> rows_of(domain_id d) const {
    const Domain& dm = q.domains[ix(d)];
    std::vector<value_id> v(ix(dm.rows));
    for (std::int32_t r = 0; r < dm.rows; ++r) v[ix(r)] = dm.value_base + r;
    return v;
  }
  // The rows of a finished domain in the READER's row order: v[i] is the source domain's row
  // rowmap[i]. The reverse of a scan is laid out backwards, so every operand it reads from a
  // forward-order domain is re-indexed through its row map.
  std::vector<value_id> rows_of(domain_id d, const std::vector<std::int32_t>& rowmap) const {
    const value_id base = q.domains[ix(d)].value_base;
    std::vector<value_id> v(rowmap.size());
    for (std::size_t i = 0; i < rowmap.size(); ++i) v[i] = base + rowmap[i];
    return v;
  }
  value_id value_base(domain_id d) const noexcept { return q.domains[ix(d)].value_base; }

  // Folds one accumulator target's contributions into domain `d`'s group, left to right into a
  // running accumulator exactly as adjoint_e0.cpp folds them into a buffer that was memset to
  // +0.0 first, and returns the slot naming the result. The target may be the group's own value
  // (the whole group is this one chain) or an intermediate of a longer group (the reverse of a
  // scan, where the pull, the step adjoints and the carry's edge slot share one group).
  Slot fold_into(domain_id d, const std::vector<Contribution>& cs) {
    if (cs.empty()) {
      // Nothing accumulates here: the runtime's buffer keeps the +0.0 it was memset to.
      return push(d, Step{Op::Const, {}, {}, {}, lit_slot(0.0)});
    }
    Slot acc{};
    bool first = true;
    for (const Contribution& c : cs) {
      // Trap 4: the chain's first term is Add(Lit +0.0, t) / Sub(Lit +0.0, t) and is NOT
      // elidable, because 0.0 + (-0.0) is +0.0 while (-0.0) alone is not, and 0.0 - (+0.0) is
      // +0.0 while -(+0.0) is not. Mutant a2p.drop_zero_start drops exactly that leading term.
      const Slot zero = lit_slot(0.0);
      const Slot start = first ? zero : acc;
      const bool elide = first && drop_zero_start;
      auto step = [&](Op op, Slot a, Slot b) { return push(d, Step{op, a, b, {}, {}}); };
      // acc = acc + term, or the elided form.
      auto add = [&](Slot term) { acc = elide ? term : step(Op::Add, start, term); };
      // acc = acc - term, or the elided form.
      auto sub = [&](Slot term) { acc = elide ? step(Op::Neg, term, {}) : step(Op::Sub, start, term); };
      switch (c.how) {
        case Acc::Plus:
          add(slot_of(d, c.ybar));
          break;
        case Acc::Minus:
          sub(slot_of(d, c.ybar));
          break;
        case Acc::PlusMul:
          add(step(Op::Mul, slot_of(d, c.ybar), slot_of(d, c.m)));
          break;
        case Acc::PlusDiv:
          add(step(Op::Div, slot_of(d, c.ybar), slot_of(d, c.m)));
          break;
        case Acc::Sqrt: {
          const Slot half = step(Op::Mul, lit_slot(0.5), slot_of(d, c.ybar));
          add(step(Op::Div, half, slot_of(d, c.y)));
          break;
        }
        case Acc::Recip: {
          const Slot t = step(Op::Mul, slot_of(d, c.ybar), slot_of(d, c.y));
          sub(step(Op::Mul, t, slot_of(d, c.y)));
          break;
        }
        case Acc::DivDen: {
          const Slot t = step(Op::Div, slot_of(d, c.ybar), slot_of(d, c.m));
          sub(step(Op::Mul, t, slot_of(d, c.y)));
          break;
        }
        case Acc::DivAliased: {
          const Slot t = step(Op::Div, slot_of(d, c.ybar), slot_of(d, c.m));
          const Slot u = step(Op::Sub, lit_slot(1.0), slot_of(d, c.y));
          add(step(Op::Mul, t, u));
          break;
        }
        case Acc::SelectTrue:
          add(push(d, Step{Op::Select, slot_of(d, c.pred), slot_of(d, c.ybar), lit_slot(0.0), {}}));
          break;
        case Acc::SelectFalse:
          add(push(d, Step{Op::Select, slot_of(d, c.pred), lit_slot(0.0), slot_of(d, c.ybar), {}}));
          break;
      }
      first = false;
    }
    return acc;
  }

  // The same chain as its own DOMAIN, over `rows` rows: what every target of a non-recurrent
  // domain's reverse gets, so that later domains can read it through an identity gather.
  domain_id accumulate(std::int32_t rows, const std::vector<Contribution>& cs, std::string name) {
    const domain_id d = open(rows, std::move(name));
    fold_into(d, cs);
    return d;
  }

  // Domain::reads, derived from what was emitted rather than tracked alongside it: ir::validate
  // checks that every listed read is earlier, and exec::Interpreter RELIES on the list being
  // complete (validate does not check completeness), so it is recomputed here from the only two
  // things that can cross a domain boundary — gathers and segment members.
  void finish_reads() {
    for (Domain& d : q.domains) d.reads.clear();
    auto note = [&](domain_id reader, value_id v) {
      std::vector<domain_id>& r = q.domains[ix(reader)].reads;
      const domain_id src = q.domain_of(v);
      if (!r.empty() && r.back() == src) return;  // runs are long: the cheap test first
      for (const domain_id x : r) {
        if (x == src) return;
      }
      r.push_back(src);
    };
    for (const Gather& g : q.gathers) {
      for (const value_id v : g.index) note(g.domain, v);
    }
    for (const Segment& s : q.segments) {
      for (const value_id v : s.members) note(s.domain, v);
    }
    for (Domain& d : q.domains) std::sort(d.reads.begin(), d.reads.end());
  }
};

// A recurrent domain that is not a scan, and a scan whose rows read themselves through anything
// but the carry: the reverse of either would need TWO carried quantities per row, and a domain
// row produces exactly one value, so neither is expressible. Refuse rather than emit something
// that is nearly right. (`build_plan` refuses the first of them too.)
[[noreturn]] void refuse(const std::string& what) {
  throw std::logic_error(
      "adjoint_to_program: " + what +
      ". The reverse of a recurrence carries ONE value per row — the carry gather's edge slot — "
      "and a second carried quantity is not expressible in the IR (C3b, PRINCIPLES.md §1b).");
}

}  // namespace

ir::Program adjoint_to_program(const ir::Program& p, const AdjointPlan& plan) {
  if (p.outputs.empty()) {
    throw std::invalid_argument("adjoint_to_program: the program has no outputs, so there is nothing to seed");
  }
  for (std::size_t d = 0; d < p.domains.size(); ++d) {
    const Domain& dm = p.domains[d];
    if (dm.recurrent && dm.scan < 0) {
      refuse("domain " + std::to_string(d) + " is recurrent but not a scan");
    }
    for (const Step& s : p.groups[d].steps) {
      if (!op_is_supported(s.op)) {
        throw std::invalid_argument(std::string("adjoint_to_program: unsupported op ") + to_string(s.op));
      }
    }
    if (dm.scan < 0) continue;
    // C3b's one structural precondition, checked rather than assumed: a scan row may be read by
    // its own domain ONLY through the carry. Another self-reading gather, or a self-reading
    // segment, would put a second edge slot of this domain into an earlier row's pull — a second
    // carried quantity. Measured absent on both scan fixtures and on compare_ois.
    const std::int32_t carry = p.scans[ix(dm.scan)].carry_gather;
    for (const std::int32_t gi : plan.domains[d].gathers) {
      if (gi == carry) continue;
      for (const value_id v : p.gathers[ix(gi)].index) {
        if (v >= dm.value_base && v < dm.value_base + dm.rows) {
          refuse("scan domain " + std::to_string(d) + " reads itself through gather " + std::to_string(gi) +
                 ", which is not its carry");
        }
      }
    }
    for (const std::int32_t si : plan.domains[d].segments) {
      for (const value_id v : p.segments[ix(si)].members) {
        if (v >= dm.value_base && v < dm.value_base + dm.rows) {
          refuse("scan domain " + std::to_string(d) + " reads itself through segment " + std::to_string(si));
        }
      }
    }
  }

  Builder b;
  // Mutants (D32), queried once each. a2p.drop_zero_start: the leading +0.0 of an accumulation is
  // elided -- the chain's first Add(Lit 0.0, ·) / Sub(Lit 0.0, ·) and, where the pull's every
  // coefficient is 1.0, its Affine konst (emitted as a Sum). a2p.div_not_aliased: an aliased Div
  // emits acc_div's two separate accumulations instead of acc_div_aliased's single one.
  // a2p.scan_forward_order is queried where it fires, below.
  b.drop_zero_start = mutant("a2p.drop_zero_start");
  const bool div_not_aliased = mutant("a2p.div_not_aliased");

  const std::size_t n_dom = p.domains.size();
  const std::vector<Contribution> no_contribution;

  // ---- the forward half: one emitted domain per (forward domain, step) -------------------------
  // Intermediates are materialised, not recomputed per target: recomputing a step once per place
  // its value is needed is correct and explodes (a 37-domain fixture went to ~44k steps).
  std::vector<std::vector<domain_id>> step_domain(n_dom);     // step_domain[d][k]
  std::vector<value_id> fwd(p.num_values(), ir::invalid_value);  // P value id -> Q value id
  std::vector<value_id> q_inputs(p.inputs.size(), ir::invalid_value);

  for (std::size_t d = 0; d < n_dom; ++d) {
    const Domain& dm = p.domains[d];
    const Group& g = p.groups[d];
    const std::size_t last = g.steps.size() - 1;
    step_domain[d].assign(g.steps.size(), -1);
    // C3b. A SCAN's forward half cannot be one emitted domain per step: the step that reads the
    // carry would read the domain holding the group's LAST step, which is emitted after it — a
    // cycle BETWEEN emitted domains, which is not a recurrence inside one and which ir::validate
    // rejects. So the whole group is emitted as ONE domain, and as a genuine `ir::Scan` of Q
    // (same chain layout, the carry gather remapped), which exec::Interpreter and ir::Evaluator
    // already run. The intermediates the reverse needs are then RECOMPUTED after it, one domain
    // per step, each reading the finished scan's rows through an ordinary gather — deterministic,
    // and therefore bitwise: nothing about an elementwise recompute depends on where it runs.
    const bool is_scan = dm.scan >= 0;
    const std::int32_t src_carry = is_scan ? p.scans[ix(dm.scan)].carry_gather : -1;
    // The domain being filled, and whether it is the scan's own group: `remap` reads both, and
    // both change as this loop walks the scan domain and then its recompute domains.
    domain_id nd = -1;
    bool in_scan_group = false;
    std::int32_t scan_carry = -1;  // the carry gather's index in Q, for the Scan descriptor
    // Re-expresses one operand slot of the forward step in the emitted domain.
    auto remap = [&](const Slot& sl) -> Slot {
      switch (sl.kind) {
        case SlotKind::None:
          return Slot{};
        case SlotKind::Literal:
          return b.lit_slot(p.literals[ix(sl.index)]);
        case SlotKind::Column:
          return b.slot_of(nd, Operand::col(p.columns[ix(sl.index)].values));
        case SlotKind::Step: {
          // Inside the scan's own group an earlier step stays an earlier step. Everywhere else
          // it is now its own domain of the same length: the identity gather reads it row for
          // row.
          if (in_scan_group) return sl;
          return b.slot_of(nd, Operand::vals(b.rows_of(step_domain[d][ix(sl.index)])));
        }
        case SlotKind::Gather: {
          // One remapped carry per scan domain, reused by every slot of the group that reads it,
          // so the Scan descriptor names a gather a step of the group actually reads.
          const bool carry = in_scan_group && sl.index == src_carry;
          if (carry && scan_carry >= 0) return Slot{SlotKind::Gather, scan_carry};
          const Gather& og = p.gathers[ix(sl.index)];
          std::vector<value_id> index(ix(dm.rows));
          for (std::int32_t r = 0; r < dm.rows; ++r) index[ix(r)] = fwd[ix(og.index[ix(r)])];
          const Slot out = b.slot_of(nd, Operand::vals(std::move(index)));
          if (carry) scan_carry = out.index;
          return out;
        }
        case SlotKind::Input:
          return Slot{SlotKind::Input, -1};
        case SlotKind::Segment:
        default: {
          const Segment& og = p.segments[ix(sl.index)];
          Segment ng;
          ng.domain = nd;
          ng.offsets = og.offsets;
          ng.coefs = og.coefs;
          ng.members.reserve(og.members.size());
          for (const value_id m : og.members) ng.members.push_back(fwd[ix(m)]);
          b.q.segments.push_back(std::move(ng));
          return Slot{SlotKind::Segment, i32(b.q.segments.size()) - 1};
        }
      }
    };
    auto emit_step = [&](std::size_t k) {
      const Step& s = g.steps[k];
      Step ns;
      ns.op = s.op;
      ns.a = remap(s.a);
      ns.b = remap(s.b);
      ns.c = remap(s.c);
      ns.konst = remap(s.konst);
      b.push(nd, ns);
    };

    if (is_scan) {
      nd = b.open(dm.rows, "f" + std::to_string(d) + "_scan");
      in_scan_group = true;
      // Published BEFORE the operands are remapped, so that the carry gather — whose entries are
      // this domain's own earlier rows — resolves through the same `fwd` map as everything else,
      // which is what makes it the recurrence ir::validate demands of a Scan.
      for (std::int32_t r = 0; r < dm.rows; ++r) fwd[ix(dm.value_base + r)] = b.value_base(nd) + r;
      for (std::size_t k = 0; k < g.steps.size(); ++k) emit_step(k);
      Domain& sd = b.q.domains[ix(nd)];
      sd.recurrent = true;
      sd.scan = i32(b.q.scans.size());
      ir::Scan nsc;
      nsc.domain = nd;
      nsc.carry_gather = scan_carry;
      nsc.chain_offsets = p.scans[ix(dm.scan)].chain_offsets;
      b.q.scans.push_back(std::move(nsc));
      step_domain[d][last] = nd;
      in_scan_group = false;
      for (std::size_t k = 0; k < last; ++k) {
        nd = b.open(dm.rows, "f" + std::to_string(d) + "_" + std::to_string(k) + "r");
        emit_step(k);
        step_domain[d][k] = nd;
      }
      continue;
    }

    for (std::size_t k = 0; k < g.steps.size(); ++k) {
      nd = b.open(dm.rows, "f" + std::to_string(d) + "_" + std::to_string(k));
      emit_step(k);
      step_domain[d][k] = nd;
      if (k != last) continue;
      for (std::int32_t r = 0; r < dm.rows; ++r) fwd[ix(dm.value_base + r)] = b.value_base(nd) + r;
      if (g.steps[k].op != Op::Input) continue;
      const std::vector<std::int32_t>& ordinal = plan.domains[d].ordinal;
      for (std::int32_t r = 0; r < dm.rows; ++r) q_inputs[ix(ordinal[ix(r)])] = b.value_base(nd) + r;
    }
  }

  // ---- the seeds: a second Input domain, ordinals after the forward inputs ----------------------
  const domain_id ob_domain = b.open(i32(p.outputs.size()), "out_bar");
  b.push(ob_domain, Step{Op::Input, Slot{SlotKind::Input, -1}, {}, {}, {}});
  const value_id ob_base = b.value_base(ob_domain);

  // A value READ BY NOTHING has an adjoint of +0.0 and a pull with no terms, and ir::validate
  // rejects a segment row with no members. One shared row holding +0.0 stands in for it: the
  // pull of such a row becomes `konst(+0.0) + 1.0 · 0.0`, which is +0.0 to the bit, the same
  // +0.0 adjoint_e0.cpp's memset leaves in the buffer and never accumulates into.
  //
  // Not a scan matter, and not new: `instrument_sample` and `rfr_book` both declare inputs that
  // nothing reads (12 and 3 rows of their Input domains), and until this existed the emitter
  // refused them for that and not for the scan they also carry — so half the scan fixtures in
  // the tree could not be gated at all. Emitted only when the program has such a value, so that
  // every program that does not keeps the shape it had.
  value_id zero_value = ir::invalid_value;
  for (std::size_t v = 0; v < p.num_values(); ++v) {
    if (plan.domains[ix(p.domain_of(i32(v)))].is_const) continue;  // no pull is emitted for those
    if (plan.output_offsets[v + 1] != plan.output_offsets[v] || plan.gather_offsets[v + 1] != plan.gather_offsets[v] ||
        plan.sum_offsets[v + 1] != plan.sum_offsets[v] || plan.affine_offsets[v + 1] != plan.affine_offsets[v]) {
      continue;
    }
    const domain_id zd = b.open(1, "zero");
    b.push(zd, Step{Op::Const, {}, {}, {}, b.lit_slot(0.0)});
    zero_value = b.value_base(zd);
    break;
  }

  // ---- the reverse half -------------------------------------------------------------------------
  std::vector<value_id> gather_bar(ix(plan.n_gather_slots), ir::invalid_value);    // gbar slot -> Q value
  std::vector<value_id> segment_bar(ix(plan.n_segment_slots), ir::invalid_value);  // segbar slot -> Q value
  std::vector<value_id> value_bar(p.num_values(), ir::invalid_value);              // v̄ of a P value

  // The pull (adjoint_e0.cpp's pull()): v̄ = +0.0, then the four CSR reader lists in plan order
  // -- outputs, gathers, sums, affines -- summed left to right. That is Op::Affine's own fold
  // over one segment, with konst +0.0 (trap 1) and coefficient 1.0 for the first three lists
  // (trap 2: 1.0·x is exact, and the pull already multiplies for affine readers).
  //
  // `host` is the domain the step is pushed into; its emitted row i holds domain `dd`'s forward
  // row `rowmap[i]`, which is the identity everywhere except in the reverse of a scan. `self`,
  // when >= 0, says that `host` IS the recurrent carry domain of a scan: the carry's own edge
  // slots are what host produces, so a carry entry in the CSR resolves to host's PREVIOUS
  // emitted row rather than through `gather_bar`. That one substitution is the whole recurrence.
  // Returns the slot naming the pushed step.
  auto emit_pull = [&](std::size_t dd, domain_id host, const std::vector<std::int32_t>& rowmap, domain_id self,
                       std::int32_t carry_gather) -> Slot {
    const Domain& dm = p.domains[dd];
    const std::int32_t carry_lo = carry_gather >= 0 ? plan.gather_slot_base[ix(carry_gather)] : -1;
    Segment sg;
    sg.domain = host;
    sg.offsets.push_back(0);
    bool all_unit = true;
    for (std::size_t i = 0; i < rowmap.size(); ++i) {
      const std::int32_t r = rowmap[i];
      const std::size_t v = ix(dm.value_base + r);
      for (std::int32_t e = plan.output_offsets[v]; e < plan.output_offsets[v + 1]; ++e) {
        sg.members.push_back(ob_base + plan.output_readers[ix(e)]);
        sg.coefs.push_back(1.0);
      }
      for (std::int32_t e = plan.gather_offsets[v]; e < plan.gather_offsets[v + 1]; ++e) {
        const std::int32_t slot = plan.gather_readers[ix(e)];
        const bool is_carry = self >= 0 && slot >= carry_lo && slot < carry_lo + dm.rows;
        // The carried adjoint: the edge slot (carry, r + 1), which this very domain produced one
        // emitted row ago, because its rows run backwards. i == 0 cannot reach here — forward row
        // `rows - 1` is the last row of the last chain and has no successor to carry from.
        sg.members.push_back(is_carry ? b.value_base(self) + i32(i) - 1 : gather_bar[ix(slot)]);
        sg.coefs.push_back(1.0);
      }
      for (std::int32_t e = plan.sum_offsets[v]; e < plan.sum_offsets[v + 1]; ++e) {
        sg.members.push_back(segment_bar[ix(plan.sum_readers[ix(e)])]);
        sg.coefs.push_back(1.0);
      }
      for (std::int32_t e = plan.affine_offsets[v]; e < plan.affine_offsets[v + 1]; ++e) {
        sg.members.push_back(segment_bar[ix(plan.affine_readers[ix(e)])]);
        const double coef = plan.affine_coefs[ix(e)];
        sg.coefs.push_back(coef);
        all_unit = all_unit && coef == 1.0;
      }
      if (sg.offsets.back() == i32(sg.members.size())) {
        // Read by nothing: the shared +0.0 row stands in, because ir::validate rejects a segment
        // row with no members. `konst(+0.0) + 1.0 · 0.0` is +0.0 to the bit (see `zero_value`).
        if (zero_value == ir::invalid_value) {
          throw std::logic_error("adjoint_to_program: value " + std::to_string(v) + " (domain " +
                                 std::to_string(dd) + ", row " + std::to_string(r) +
                                 ") is read by nothing, but the +0.0 stand-in row was not emitted");
        }
        sg.members.push_back(zero_value);
        sg.coefs.push_back(1.0);
      }
      sg.offsets.push_back(i32(sg.members.size()));
    }
    for (const value_id m : sg.members) {
      if (m == ir::invalid_value || m < 0) {
        throw std::logic_error("adjoint_to_program: a pull member of domain " + std::to_string(dd) +
                               " was not emitted before it was read (reverse order is wrong)");
      }
    }
    const bool as_sum = b.drop_zero_start && all_unit;  // mutant: no leading +0.0
    if (as_sum) sg.coefs.clear();
    b.q.segments.push_back(std::move(sg));
    const Slot seg{SlotKind::Segment, i32(b.q.segments.size()) - 1};
    return b.push(host, as_sum ? Step{Op::Sum, seg, {}, {}, {}} : Step{Op::Affine, seg, {}, {}, b.lit_slot(0.0)});
  };

  // adjoint_e0.cpp's reverse(): the group's steps from the LAST to the first, each op's rule
  // pushing its operands' contributions into their targets — an operand that is an earlier Step
  // into that step's adjoint, one that is a Gather into that (gather, row) edge slot, a Sum /
  // Affine step's own adjoint into the (segment, row) edge slot, a literal or a column nothing.
  //
  // `rowmap[i]` is the forward row the host's emitted row i holds — the identity everywhere
  // except inside the reverse of a scan, whose rows run backwards, so every operand it reads
  // from a forward-order domain is re-indexed through it. `ybar_of(k, chain)` supplies step k's
  // own adjoint: the pull for the last step, and for every earlier one the chain just completed,
  // folded wherever the caller wants it — its own domain on the ordinary path, a step of the
  // host's group inside the reverse of a scan, where the whole chain must live in one group.
  auto collect = [&](std::size_t dd, const std::vector<std::int32_t>& rowmap, auto&& ybar_of,
                     std::map<std::int32_t, std::vector<Contribution>>& gather_acc,
                     std::map<std::int32_t, std::vector<Contribution>>& segment_acc) {
    const Group& g = p.groups[dd];
    std::vector<std::vector<Contribution>> step_acc(g.steps.size());
    // adjoint_e0.cpp's `target()`.
    auto target = [&](const Slot& sl, Contribution c) {
      if (sl.kind == SlotKind::Step) {
        step_acc[ix(sl.index)].push_back(std::move(c));
      } else if (sl.kind == SlotKind::Gather) {
        gather_acc[sl.index].push_back(std::move(c));
      }
    };
    // How a forward operand slot reads in the reverse half: the same value, re-addressed.
    auto operand_of = [&](const Slot& sl) -> Operand {
      switch (sl.kind) {
        case SlotKind::Literal:
          return Operand::lit(p.literals[ix(sl.index)]);
        case SlotKind::Column: {
          const std::vector<double>& src = p.columns[ix(sl.index)].values;
          std::vector<double> col(rowmap.size());
          for (std::size_t i = 0; i < rowmap.size(); ++i) col[i] = src[ix(rowmap[i])];
          return Operand::col(std::move(col));
        }
        case SlotKind::Step:
          return Operand::vals(b.rows_of(step_domain[dd][ix(sl.index)], rowmap));
        case SlotKind::Gather: {
          const Gather& og = p.gathers[ix(sl.index)];
          std::vector<value_id> index(rowmap.size());
          for (std::size_t i = 0; i < rowmap.size(); ++i) index[i] = fwd[ix(og.index[ix(rowmap[i])])];
          return Operand::vals(std::move(index));
        }
        default:
          return Operand::lit(0.0);  // None / Segment / Input: no operand vector is read
      }
    };
    for (std::size_t k = g.steps.size(); k-- > 0;) {
      const Step& s = g.steps[k];
      const Operand ybar = ybar_of(k, step_acc[k]);
      const Operand y = Operand::vals(b.rows_of(step_domain[dd][k], rowmap));
      const Operand a = operand_of(s.a);
      const Operand bb = operand_of(s.b);
      // Trap 5: adjoint_e0.cpp dispatches acc_div_aliased when the two target POINTERS coincide,
      // which happens exactly when the two slots are the same Step or the same Gather (step
      // adjoints and gather edge slots live in different buffers, and distinct gathers own
      // disjoint slot ranges). Decidable here, statically.
      const bool aliased = s.a.kind == s.b.kind && s.a.index == s.b.index &&
                           (s.a.kind == SlotKind::Step || s.a.kind == SlotKind::Gather);
      switch (s.op) {
        case Op::Add:
          target(s.a, {Acc::Plus, ybar, {}, {}, {}});
          target(s.b, {Acc::Plus, ybar, {}, {}, {}});
          break;
        case Op::Sub:
          target(s.a, {Acc::Plus, ybar, {}, {}, {}});
          target(s.b, {Acc::Minus, ybar, {}, {}, {}});
          break;
        case Op::Mul:
          target(s.a, {Acc::PlusMul, ybar, bb, {}, {}});
          target(s.b, {Acc::PlusMul, ybar, a, {}, {}});
          break;
        case Op::Div:
          if (aliased && !div_not_aliased) {
            target(s.a, {Acc::DivAliased, ybar, bb, y, {}});
          } else {
            target(s.a, {Acc::PlusDiv, ybar, bb, {}, {}});
            target(s.b, {Acc::DivDen, ybar, bb, y, {}});
          }
          break;
        case Op::Neg:
          target(s.a, {Acc::Minus, ybar, {}, {}, {}});
          break;
        case Op::Exp:
          target(s.a, {Acc::PlusMul, ybar, y, {}, {}});
          break;
        case Op::Log:
          target(s.a, {Acc::PlusDiv, ybar, a, {}, {}});
          break;
        case Op::Sqrt:
          target(s.a, {Acc::Sqrt, ybar, {}, y, {}});
          break;
        case Op::Recip:
          target(s.a, {Acc::Recip, ybar, {}, y, {}});
          break;
        case Op::Fma:
          target(s.a, {Acc::PlusMul, ybar, bb, {}, {}});
          target(s.b, {Acc::PlusMul, ybar, a, {}, {}});
          target(s.c, {Acc::Plus, ybar, {}, {}, {}});
          break;
        case Op::Select:
          target(s.b, {Acc::SelectTrue, ybar, {}, {}, a});
          target(s.c, {Acc::SelectFalse, ybar, {}, {}, a});
          break;
        case Op::Sum:
          if (ir::is_fixed_sum(s)) {
            // C3b. A scan group's Sum over operand slots (ir::is_fixed_sum): every member of the
            // fold receives the step's adjoint whole, a then b then c, which is exactly
            // adjoint_e0.cpp's `if (ta) acc_plus; if (tb) acc_plus; if (tc) acc_plus`. `target`
            // already drops the slots that receive nothing, c included when it is None.
            target(s.a, {Acc::Plus, ybar, {}, {}, {}});
            target(s.b, {Acc::Plus, ybar, {}, {}, {}});
            target(s.c, {Acc::Plus, ybar, {}, {}, {}});
            break;
          }
          [[fallthrough]];
        case Op::Affine:
          // C3a. The step's adjoint goes whole into the (segment, row) edge slot; plan.cpp
          // guarantees one reading step per segment, so this chain has exactly one term.
          segment_acc[s.a.index].push_back({Acc::Plus, ybar, {}, {}, {}});
          break;
        default:
          break;  // Const and the comparisons contribute nothing (adjoint_e0.cpp's empty cases)
      }
    }
  };

  for (std::size_t dd = n_dom; dd-- > 0;) {
    const Domain& dm = p.domains[dd];
    const Group& g = p.groups[dd];
    const AdjointPlan::DomainPlan& dp = plan.domains[dd];
    if (dp.is_const) continue;  // a Const row's adjoint is read by nothing: adjoint_e0.cpp's `continue`
    const std::size_t last = g.steps.size() - 1;
    const std::int32_t carry = dm.scan >= 0 ? p.scans[ix(dm.scan)].carry_gather : -1;

    // Forward row order everywhere except inside the reverse scan's own recurrent domain.
    std::vector<std::int32_t> ident(ix(dm.rows));
    for (std::int32_t r = 0; r < dm.rows; ++r) ident[ix(r)] = r;

    // ---- C3b: the reverse of a scan ---------------------------------------------------------
    //
    // adjoint_e0.cpp runs a scan's rows from the LAST to the first, one at a time, so that row
    // r's pull sees the edge slot (carry, r + 1) its successor's reverse has just written. Here
    // that is one RECURRENT domain whose emitted row i holds forward row rows - 1 - i and whose
    // value is the carry's edge slot for that row: then (carry, r + 1) is literally the previous
    // emitted row, the self-read is backwards, and nothing had to be reordered — in particular
    // the pull's segment still carries the carry's edge slot as an ordinary member at its TRUE
    // CSR position, which on compare_ois is position 0 of 2 and on affine_scan the last of 2.
    // Reordering the pull so the carry came last would change Adjoint::run's bits and is a §5.2a
    // case-1 question reserved to the owner (D96 §3); this encoding does not raise it.
    //
    // The chain from v̄ to the carry's edge slot lives in that ONE group, because a cycle between
    // domains is not expressible. Everything else the row's reverse produces — the other gather
    // edge slots, the segment edge slots, the step adjoints — is recomputed below in ordinary
    // non-recurrent domains that read this one, which is deterministic and therefore bitwise.
    if (dm.scan >= 0) {
      std::vector<std::int32_t> rev(ix(dm.rows));
      for (std::int32_t i = 0; i < dm.rows; ++i) rev[ix(i)] = dm.rows - 1 - i;
      const domain_id cbar = b.open(dm.rows, "cbar" + std::to_string(dd));
      b.q.domains[ix(cbar)].recurrent = true;
      const Slot vbar = emit_pull(dd, cbar, rev, cbar, carry);
      std::map<std::int32_t, std::vector<Contribution>> cg_acc, cs_acc;
      collect(
          dd, rev,
          [&](std::size_t k, const std::vector<Contribution>& cs) -> Operand {
            return Operand::direct_slot(k == last ? vbar : b.fold_into(cbar, cs));
          },
          cg_acc, cs_acc);
      // The carry's edge slot is this domain's VALUE, so its chain is the group's LAST step:
      // that is what makes the previous emitted row, read by the pull above, the carried adjoint.
      const auto cit = cg_acc.find(carry);
      b.fold_into(cbar, cit == cg_acc.end() ? no_contribution : cit->second);
      // Published to the rest of Q AFTER the domain is built, because the recurrence above reads
      // its own previous row STRUCTURALLY (`value_base + i - 1`) and not through this map.
      //
      // Mutant a2p.scan_forward_order publishes the FORWARD row order instead: the recurrence
      // itself stays valid and self-consistent, and every reader outside it -- the vbar pull
      // below, and the pull of whatever earlier domain holds the chains' initial values -- takes
      // row r's carried adjoint from the row the FORWARD scan would have had. That is the
      // emitter's form of adjoint.scan_forward_order, and it is the only form of it a Program
      // can express: laying this domain out forwards instead would make the self-read a FORWARD
      // read, which ir::validate rejects outright, so there would be nothing to compare.
      const bool forward_order = mutant("a2p.scan_forward_order");
      for (std::int32_t r = 0; r < dm.rows; ++r) {
        gather_bar[ix(plan.gather_slot_base[ix(carry)] + r)] =
            b.value_base(cbar) + (forward_order ? r : dm.rows - 1 - r);
      }
    }

    const domain_id pull = b.open(dm.rows, "vbar" + std::to_string(dd));
    emit_pull(dd, pull, ident, -1, carry);
    for (std::int32_t r = 0; r < dm.rows; ++r) value_bar[ix(dm.value_base + r)] = b.value_base(pull) + r;
    // An Input group's pull IS the state adjoint (adjoint_e0.cpp's Input rule assigns it).
    if (dp.is_input) continue;

    std::map<std::int32_t, std::vector<Contribution>> gather_acc;   // gather index -> chain
    std::map<std::int32_t, std::vector<Contribution>> segment_acc;  // segment index -> chain
    // Steps from the last to the first, as adjoint_e0.cpp's reverse() runs them. A step's own
    // adjoint is complete once every later step has contributed, which is exactly when its
    // domain is emitted here. On a scan domain this is the SECOND walk: the carry's edge slot
    // came out of the recurrent domain above, and every other quantity the row's reverse
    // produces is recomputed here, reading that domain's rows like any other value.
    collect(
        dd, ident,
        [&](std::size_t k, const std::vector<Contribution>& cs) -> Operand {
          if (k == last) return Operand::vals(b.rows_of(pull));
          return Operand::vals(b.rows_of(
              b.accumulate(dm.rows, cs, "sbar" + std::to_string(dd) + "_" + std::to_string(k))));
        },
        gather_acc, segment_acc);

    // C3a. The segment edge slots of this domain, read by the pulls of earlier domains through
    // plan.cpp's transpose: a Sum member reads its row's slot with coefficient 1.0, an Affine
    // member with ITS OWN coefficient (the trap the mutant adjoint.affine_not_transposed pins).
    for (const std::int32_t si : dp.segments) {
      const auto it = segment_acc.find(si);
      const domain_id sd = b.accumulate(dm.rows, it == segment_acc.end() ? no_contribution : it->second,
                                        "segbar" + std::to_string(si));
      for (std::int32_t r = 0; r < dm.rows; ++r) {
        segment_bar[ix(plan.segment_slot_base[ix(si)] + r)] = b.value_base(sd) + r;
      }
    }
    // The gather edge slots of this domain, likewise — except a scan's carry, which the
    // recurrent domain above has already produced and published, and which must NOT be
    // re-emitted here: a second copy would be a different value id and the recurrence's own
    // readers would stop seeing the carried adjoint.
    for (const std::int32_t gi : dp.gathers) {
      if (gi == carry) continue;
      const auto it = gather_acc.find(gi);
      const domain_id gd = b.accumulate(dm.rows, it == gather_acc.end() ? no_contribution : it->second,
                                        "gbar" + std::to_string(gi));
      for (std::int32_t r = 0; r < dm.rows; ++r) {
        gather_bar[ix(plan.gather_slot_base[ix(gi)] + r)] = b.value_base(gd) + r;
      }
    }
  }

  // ---- inputs and outputs -----------------------------------------------------------------------
  b.q.inputs = std::move(q_inputs);
  for (std::int32_t o = 0; o < i32(p.outputs.size()); ++o) b.q.inputs.push_back(ob_base + o);
  b.q.input_values.assign(b.q.inputs.size(), 0.0);
  for (std::size_t k = 0; k < p.input_values.size(); ++k) b.q.input_values[k] = p.input_values[k];
  for (const value_id v : p.outputs) b.q.outputs.push_back(fwd[ix(v)]);
  for (const value_id v : p.inputs) {
    if (value_bar[ix(v)] == ir::invalid_value) {
      throw std::logic_error("adjoint_to_program: input value " + std::to_string(v) +
                             " has no emitted adjoint (its domain was skipped)");
    }
    b.q.outputs.push_back(value_bar[ix(v)]);
  }
  b.finish_reads();
  return std::move(b.q);
}

ir::Program adjoint_to_program(const ir::Program& program) {
  return adjoint_to_program(program, build_plan(program));
}

}  // namespace epykos::adjoint
