// EpykosEngine — building the adjoint plan (include/epykos/adjoint/plan.hpp). Structure only:
// no floating-point arithmetic happens here (coefficients are copied), so this TU needs no pin.
#include "epykos/adjoint/plan.hpp"

#include <algorithm>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "epykos/mutation/mutation.hpp"

namespace epykos::adjoint {

namespace {

using std::size_t;
using ir::Domain;
using ir::Gather;
using ir::Group;
using ir::Program;
using ir::Segment;
using ir::Slot;
using ir::SlotKind;
using ir::Step;

inline size_t idx(std::int32_t i) { return static_cast<size_t>(i); }

// Counting sort into a CSR: `count` bumps per key, then `place` fills. Keys are value ids.
struct Csr {
  std::vector<std::int32_t> offsets;
  std::vector<std::int32_t> cursor;
  explicit Csr(size_t n) : offsets(n + 1, 0) {}
  void count(std::int32_t v) { ++offsets[idx(v) + 1]; }
  void finish() {
    for (size_t i = 1; i < offsets.size(); ++i) offsets[i] += offsets[i - 1];
    cursor.assign(offsets.begin(), offsets.end() - 1);
  }
  size_t place(std::int32_t v) { return idx(cursor[idx(v)]++); }
  size_t total() const { return idx(offsets.back()); }
};

}  // namespace

OpRule adjoint_rule(Op op) noexcept {
  switch (op) {
    case Op::Const: return {false, false, false, "leaf"};
    case Op::Input: return {false, false, false, "state_bar[ordinal] = ybar"};
    case Op::Add: return {true, true, false, "abar += ybar; bbar += ybar"};
    case Op::Sub: return {true, true, false, "abar += ybar; bbar -= ybar"};
    case Op::Mul: return {true, true, false, "abar += ybar*b; bbar += ybar*a"};
    case Op::Div: return {true, true, false, "t = ybar/b; abar += t; bbar -= t*y"};
    case Op::Neg: return {true, false, false, "abar -= ybar"};
    case Op::Exp: return {true, false, false, "abar += ybar*y"};
    case Op::Log: return {true, false, false, "abar += ybar/a"};
    case Op::Sqrt: return {true, false, false, "abar += (0.5*ybar)/y"};
    case Op::Recip: return {true, false, false, "abar -= (ybar*y)*y"};
    case Op::Fma: return {true, true, true, "abar += ybar*b; bbar += ybar*a; cbar += ybar"};
    case Op::Select: return {false, true, true, "bbar += a ? ybar : 0; cbar += a ? 0 : ybar"};
    case Op::CmpLt:
    case Op::CmpLe:
    case Op::CmpGt:
    case Op::CmpGe:
    case Op::CmpEq: return {false, false, false, "piecewise constant: nothing"};
    case Op::Sum: return {true, false, false, "segbar[segment, row] += ybar (members pull it)"};
    case Op::Affine: return {true, false, false, "segbar[segment, row] += ybar (members pull coef*it)"};
    default: return {false, false, false, "unsupported"};
  }
}

std::size_t AdjointPlan::table_bytes() const noexcept {
  size_t bytes = 0;
  bytes += (gather_slot_base.size() + segment_slot_base.size()) * sizeof(std::int32_t) + segment_is_affine.size();
  bytes += (output_offsets.size() + output_readers.size() + gather_offsets.size() + gather_readers.size() +
            sum_offsets.size() + sum_readers.size() + affine_offsets.size() + affine_readers.size()) *
           sizeof(std::int32_t);
  bytes += affine_coefs.size() * sizeof(double);
  for (const DomainPlan& d : domains) {
    bytes += (d.gathers.size() + d.segments.size() + d.ordinal.size()) * sizeof(std::int32_t);
  }
  return bytes;
}

AdjointPlan build_plan(const Program& p) {
  ir::validate(p);  // std::runtime_error on a structural violation
  AdjointPlan plan;
  const size_t n_dom = p.domains.size();
  const size_t n_values = p.num_values();
  plan.num_values = n_values;
  plan.domains.resize(n_dom);

  for (size_t d = 0; d < n_dom; ++d) {
    const Domain& dom = p.domains[d];
    if (dom.recurrent) {
      throw std::invalid_argument("adjoint: domain " + std::to_string(d) + " (" + dom.name +
                                  ") is recurrent (a scan; unsupported until M5)");
    }
    const Group& g = p.groups[d];
    for (const Step& s : g.steps) {
      if (!op_is_supported(s.op)) {
        throw std::invalid_argument(std::string("adjoint: unsupported op ") + to_string(s.op));
      }
    }
    plan.max_steps = std::max(plan.max_steps, static_cast<int>(g.steps.size()));
    AdjointPlan::DomainPlan& dp = plan.domains[d];
    dp.is_input = g.steps.size() == 1 && g.steps[0].op == Op::Input;
    dp.is_const = g.steps.size() == 1 && g.steps[0].op == Op::Const;
  }

  // Edge slots: one per (gather, row) and per (segment, row).
  plan.gather_slot_base.resize(p.gathers.size());
  std::int32_t slot = 0;
  for (size_t g = 0; g < p.gathers.size(); ++g) {
    plan.gather_slot_base[g] = slot;
    slot += p.domains[idx(p.gathers[g].domain)].rows;
    plan.domains[idx(p.gathers[g].domain)].gathers.push_back(static_cast<std::int32_t>(g));
  }
  plan.n_gather_slots = slot;
  plan.segment_slot_base.resize(p.segments.size());
  slot = 0;
  for (size_t s = 0; s < p.segments.size(); ++s) {
    plan.segment_slot_base[s] = slot;
    slot += p.domains[idx(p.segments[s].domain)].rows;
    plan.domains[idx(p.segments[s].domain)].segments.push_back(static_cast<std::int32_t>(s));
  }
  plan.n_segment_slots = slot;

  // Which step reads each segment (exactly one: its op decides Sum vs Affine pulls).
  plan.segment_is_affine.assign(p.segments.size(), 0);
  {
    std::vector<int> reads(p.segments.size(), 0);
    for (size_t d = 0; d < n_dom; ++d) {
      for (const Step& s : p.groups[d].steps) {
        if (s.a.kind == SlotKind::Segment) {
          ++reads[idx(s.a.index)];
          plan.segment_is_affine[idx(s.a.index)] = s.op == Op::Affine ? 1 : 0;
        }
      }
    }
    for (size_t s = 0; s < reads.size(); ++s) {
      if (reads[s] > 1) {
        throw std::invalid_argument("adjoint: segment " + std::to_string(s) + " is read by " + std::to_string(reads[s]) +
                                    " steps (its edge slot would be ambiguous)");
      }
    }
  }

  // Input ordinals per row of the Input domain(s).
  {
    std::vector<std::int32_t> ordinal_of(n_values, -1);
    for (size_t k = 0; k < p.inputs.size(); ++k) ordinal_of[idx(p.inputs[k])] = static_cast<std::int32_t>(k);
    for (size_t d = 0; d < n_dom; ++d) {
      const Group& g = p.groups[d];
      bool has_input = false;
      for (const Step& s : g.steps) has_input |= s.op == Op::Input;
      if (!has_input) continue;
      const Domain& dom = p.domains[d];
      AdjointPlan::DomainPlan& dp = plan.domains[d];
      dp.ordinal.resize(idx(dom.rows));
      for (std::int32_t r = 0; r < dom.rows; ++r) {
        const std::int32_t o = ordinal_of[idx(dom.value_base + r)];
        if (o < 0) {
          throw std::invalid_argument("adjoint: Input row " + std::to_string(r) + " of domain " + std::to_string(d) +
                                      " has no input ordinal");
        }
        dp.ordinal[idx(r)] = o;
      }
    }
  }

  // Mutants (D32; M2/Q4b), each queried once per build_plan. adjoint.wrong_transpose: gather 0's
  // reader slots come from the forward index array - value v is pulled from the slot of row
  // index[r] (mod rows) instead of the row r that read it. adjoint.drop_broadcast: the last
  // member of every Sum row gets no reader entry, so the broadcast of the row's adjoint skips it.
  // adjoint.affine_not_transposed: an Affine reader's coefficient is read from the forward table
  // at the reader's transposed position (W's entries in W^T's order: the table was not transposed
  // with the readers).
  const bool wrong_transpose = mutant("adjoint.wrong_transpose");
  const std::int32_t drop_broadcast = mutant("adjoint.drop_broadcast") ? 1 : 0;
  const bool affine_not_transposed = mutant("adjoint.affine_not_transposed");
  // One past the last member of row r of segment s that receives the row's adjoint.
  auto member_end = [&](const Segment& sg, std::int32_t r, bool affine) {
    return sg.offsets[idx(r) + 1] - (affine ? 0 : drop_broadcast);
  };

  // "Who reads me" CSRs.
  Csr out_csr(n_values), gat_csr(n_values), sum_csr(n_values), aff_csr(n_values);
  for (size_t o = 0; o < p.outputs.size(); ++o) out_csr.count(p.outputs[o]);
  for (const Gather& g : p.gathers) {
    for (ir::value_id v : g.index) gat_csr.count(v);
  }
  for (size_t s = 0; s < p.segments.size(); ++s) {
    const Segment& sg = p.segments[s];
    const bool affine = plan.segment_is_affine[s] != 0;
    Csr& c = affine ? aff_csr : sum_csr;
    const std::int32_t rows = p.domains[idx(sg.domain)].rows;
    for (std::int32_t r = 0; r < rows; ++r) {
      for (std::int32_t m = sg.offsets[idx(r)]; m < member_end(sg, r, affine); ++m) c.count(sg.members[idx(m)]);
    }
  }
  out_csr.finish();
  gat_csr.finish();
  sum_csr.finish();
  aff_csr.finish();
  plan.output_readers.resize(out_csr.total());
  plan.gather_readers.resize(gat_csr.total());
  plan.sum_readers.resize(sum_csr.total());
  plan.affine_readers.resize(aff_csr.total());
  plan.affine_coefs.resize(aff_csr.total());
  for (size_t o = 0; o < p.outputs.size(); ++o) {
    plan.output_readers[out_csr.place(p.outputs[o])] = static_cast<std::int32_t>(o);
  }
  for (size_t g = 0; g < p.gathers.size(); ++g) {
    const Gather& gt = p.gathers[g];
    const std::int32_t rows = p.domains[idx(gt.domain)].rows;
    for (size_t r = 0; r < gt.index.size(); ++r) {
      const std::int32_t row = wrong_transpose && g == 0 ? gt.index[r] % rows : static_cast<std::int32_t>(r);
      plan.gather_readers[gat_csr.place(gt.index[r])] = plan.gather_slot_base[g] + row;
    }
  }
  for (size_t s = 0; s < p.segments.size(); ++s) {
    const Segment& sg = p.segments[s];
    const bool affine = plan.segment_is_affine[s] != 0;
    const std::int32_t rows = p.domains[idx(sg.domain)].rows;
    for (std::int32_t r = 0; r < rows; ++r) {
      const std::int32_t edge = plan.segment_slot_base[s] + r;
      for (std::int32_t m = sg.offsets[idx(r)]; m < member_end(sg, r, affine); ++m) {
        const ir::value_id v = sg.members[idx(m)];
        if (affine) {
          const size_t at = aff_csr.place(v);
          plan.affine_readers[at] = edge;
          plan.affine_coefs[at] = sg.coefs[affine_not_transposed ? at % sg.coefs.size() : idx(m)];
        } else {
          plan.sum_readers[sum_csr.place(v)] = edge;
        }
      }
    }
  }
  plan.output_offsets = std::move(out_csr.offsets);
  plan.gather_offsets = std::move(gat_csr.offsets);
  plan.sum_offsets = std::move(sum_csr.offsets);
  plan.affine_offsets = std::move(aff_csr.offsets);

  for (size_t d = 0; d < n_dom; ++d) {
    const Domain& dom = p.domains[d];
    const size_t lo = idx(dom.value_base);
    const size_t hi = lo + idx(dom.rows);
    std::int32_t n = 0;
    n += plan.output_offsets[hi] - plan.output_offsets[lo];
    n += plan.gather_offsets[hi] - plan.gather_offsets[lo];
    n += plan.sum_offsets[hi] - plan.sum_offsets[lo];
    n += plan.affine_offsets[hi] - plan.affine_offsets[lo];
    plan.domains[d].readers = n;
  }
  return plan;
}

std::string describe(const AdjointPlan& plan, const Program& p) {
  std::ostringstream os;
  os << "adjoint plan: " << p.domains.size() << " domains, " << plan.num_values << " values, " << plan.n_gather_slots
     << " gather edge slots, " << plan.n_segment_slots << " segment edge slots, readers: " << plan.output_readers.size()
     << " output + " << plan.gather_readers.size() << " gather + " << plan.sum_readers.size() << " sum + "
     << plan.affine_readers.size() << " affine; row values materialised, intermediate steps recomputed per tile\n";
  for (size_t d = p.domains.size(); d-- > 0;) {
    const Domain& dom = p.domains[d];
    const Group& g = p.groups[d];
    const AdjointPlan::DomainPlan& dp = plan.domains[d];
    os << "  reverse domain " << d << " " << dom.name << ": " << dom.rows << " rows, " << g.steps.size() << " steps, "
       << dp.readers << " reader entries over its rows";
    if (dp.is_input) os << ", Input (writes state_bar)";
    if (dp.is_const) os << ", Const (nothing to reverse)";
    os << '\n';
    for (size_t k = g.steps.size(); k-- > 0;) {
      const Step& s = g.steps[k];
      const OpRule rule = adjoint_rule(s.op);
      os << "    step " << k << " " << to_string(s.op) << ": " << rule.formula;
      auto target = [&](const char* name, const Slot& sl, bool receives) {
        if (sl.kind == SlotKind::None) return;
        os << "; " << name << " -> ";
        switch (sl.kind) {
          case SlotKind::Step: os << (receives ? "step " : "(step, no adjoint) ") << sl.index; break;
          case SlotKind::Literal: os << "literal (none)"; break;
          case SlotKind::Column: os << "column (none)"; break;
          case SlotKind::Gather:
            os << (receives ? "gather edge slots " : "gather (no adjoint) ") << sl.index << " [base "
               << plan.gather_slot_base[idx(sl.index)] << "]";
            break;
          case SlotKind::Segment:
            os << "segment edge slots " << sl.index << " [base " << plan.segment_slot_base[idx(sl.index)] << ", "
               << (plan.segment_is_affine[idx(sl.index)] ? "affine: coef * segbar" : "sum: segbar") << "]";
            break;
          case SlotKind::Input: os << "input ordinal"; break;
          default: break;
        }
      };
      target("a", s.a, rule.a);
      target("b", s.b, rule.b);
      target("c", s.c, rule.c);
      os << '\n';
    }
  }
  os << "  tables " << plan.table_bytes() << " bytes\n";
  return os.str();
}

}  // namespace epykos::adjoint
