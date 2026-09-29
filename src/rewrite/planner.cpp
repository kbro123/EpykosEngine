#include "epykos/rewrite/planner.hpp"

#include <algorithm>

namespace epykos::rewrite::planner {

bool is_whole_segment(const ir::Group& g) noexcept {
  if (g.steps.size() != 1) return false;
  const ir::Step& s = g.steps.front();
  return (s.op == Op::Sum || s.op == Op::Affine) && s.a.kind == ir::SlotKind::Segment;
}

bool row_fusion_pays(int lane_tile) noexcept { return lane_tile == 1 || lane_tile >= 16; }

// -------------------------------------------------------------------------------------------
// planner.reduction_fusion / planner.emit_outputs
// -------------------------------------------------------------------------------------------

FusionAnalysis analyze_fusion(const ir::Program& program) {
  const std::size_t nd = program.domains.size();
  const std::size_t nv = program.num_values();
  FusionAnalysis a;
  a.marked.assign(nv, 0);
  a.whole_member.assign(nv, 0);
  a.is_output.assign(nv, 0);
  a.member_refs.assign(nd, 0);
  for (ir::value_id v : program.outputs) a.is_output[static_cast<std::size_t>(v)] = 1;
  for (const ir::Gather& g : program.gathers) {
    for (ir::value_id v : g.index) a.marked[static_cast<std::size_t>(v)] = 1;
  }
  for (const ir::Segment& seg : program.segments) {
    const bool whole = is_whole_segment(program.groups[static_cast<std::size_t>(seg.domain)]);
    for (ir::value_id v : seg.members) {
      if (whole) {
        ++a.member_refs[static_cast<std::size_t>(program.domain_of(v))];
        a.whole_member[static_cast<std::size_t>(v)] = 1;
      } else {
        a.marked[static_cast<std::size_t>(v)] = 1;
      }
    }
  }
  return a;
}

bool decide_reduction_fusion(const ir::Program& program, const FusionAnalysis& analysis, ir::domain_id d,
                             const ReductionFusionParams& params, int lane_tile, std::vector<std::int32_t>* keep_rows) {
  const ir::Domain& dom = program.domains[static_cast<std::size_t>(d)];
  keep_rows->clear();
  if (dom.recurrent || dom.rows < 1 || analysis.member_refs[static_cast<std::size_t>(d)] == 0) return false;
  if (is_whole_segment(program.groups[static_cast<std::size_t>(d)])) return false;
  if (static_cast<double>(analysis.member_refs[static_cast<std::size_t>(d)]) >
      params.max_refs_per_row * static_cast<double>(dom.rows)) {
    return false;
  }
  // An output row that CANNOT be emitted (below emit_min_bytes, or not a whole-domain member) is
  // kept regardless: it is the only remaining path to `out`, since a fused row is otherwise never
  // written to the value buffer and planner.emit_outputs has no other way to reach it.
  const bool emit_ok =
      static_cast<std::size_t>(dom.rows) * static_cast<std::size_t>(lane_tile) * sizeof(double) >= params.emit_min_bytes;
  for (std::int32_t r = 0; r < dom.rows; ++r) {
    const std::size_t v = static_cast<std::size_t>(dom.value_base + r);
    if (analysis.marked[v] || (analysis.is_output[v] && !(emit_ok && analysis.whole_member[v]))) keep_rows->push_back(r);
  }
  if (static_cast<double>(keep_rows->size()) > params.max_kept_fraction * static_cast<double>(dom.rows)) return false;
  return true;
}

ir::PlanAnnotations reduction_fusion_plan(const ir::Program& program, int lane_tile, const ReductionFusionParams& params) {
  const FusionAnalysis analysis = analyze_fusion(program);
  ir::PlanAnnotations plan;
  plan.domain.assign(program.domains.size(), ir::DomainPlan{});
  std::vector<std::int32_t> keep;
  for (std::size_t d = 0; d < program.domains.size(); ++d) {
    if (decide_reduction_fusion(program, analysis, static_cast<ir::domain_id>(d), params, lane_tile, &keep)) {
      plan.domain[d].choice = ir::Materialise::FuseIntoReduction;
      plan.domain[d].keep_rows = keep;
    }
  }
  return plan;
}

ir::PlanAnnotations emit_outputs_plan(const ir::Program& program, const ir::PlanAnnotations& plan, int lane_tile,
                                     const EmitOutputsParams& params) {
  const FusionAnalysis analysis = analyze_fusion(program);
  ir::PlanAnnotations out;
  out.emitted.assign(program.num_values(), 0);
  for (std::size_t d = 0; d < program.domains.size(); ++d) {
    if (d >= plan.domain.size() || plan.domain[d].choice != ir::Materialise::FuseIntoReduction) continue;
    const ir::Domain& dom = program.domains[d];
    const bool emit_ok =
        static_cast<std::size_t>(dom.rows) * static_cast<std::size_t>(lane_tile) * sizeof(double) >= params.emit_min_bytes;
    if (!emit_ok) continue;
    for (std::int32_t r = 0; r < dom.rows; ++r) {
      const std::size_t v = static_cast<std::size_t>(dom.value_base + r);
      if (analysis.is_output[v] && analysis.whole_member[v] && !analysis.marked[v]) out.emitted[v] = 1;
    }
  }
  return out;
}

// -------------------------------------------------------------------------------------------
// planner.inline_producers
// -------------------------------------------------------------------------------------------

InlineAnalysis analyze_inline(const ir::Program& program) {
  const std::size_t nd = program.domains.size();
  const std::size_t ng = program.gathers.size();
  InlineAnalysis a;
  a.blocked.assign(nd, 0);
  for (ir::value_id v : program.outputs) a.blocked[static_cast<std::size_t>(program.domain_of(v))] = 1;
  for (const ir::Segment& seg : program.segments) {
    for (ir::value_id v : seg.members) a.blocked[static_cast<std::size_t>(program.domain_of(v))] = 1;
  }
  a.gather_user.assign(ng, -1);
  for (std::size_t d = 0; d < nd; ++d) {
    for (const ir::Step& st : program.groups[d].steps) {
      for (const ir::Slot* sl : {&st.a, &st.b, &st.c, &st.konst}) {
        if (sl->kind != ir::SlotKind::Gather) continue;
        std::int32_t& u = a.gather_user[static_cast<std::size_t>(sl->index)];
        if (u == -1 || u == static_cast<std::int32_t>(d)) {
          u = static_cast<std::int32_t>(d);
        } else {
          u = -2;
        }
      }
    }
  }
  a.readers.assign(nd, {});
  a.refs.assign(nd, 0);
  for (std::size_t k = 0; k < ng; ++k) {
    std::vector<char> seen(nd, 0);
    for (ir::value_id v : program.gathers[k].index) {
      const std::size_t t = static_cast<std::size_t>(program.domain_of(v));
      ++a.refs[t];
      if (!seen[t]) {
        seen[t] = 1;
        a.readers[t].push_back(k);
      }
    }
  }
  return a;
}

ir::PlanAnnotations inline_producers_plan(const ir::Program& program, const ir::PlanAnnotations& plan, int lane_tile,
                                         const InlineProducersParams& params) {
  if (!row_fusion_pays(lane_tile)) return {};  // decide_inline's own early return: nothing decided
  ir::PlanAnnotations out;
  const std::size_t nd = program.domains.size();
  out.domain.assign(nd, ir::DomainPlan{});
  const InlineAnalysis analysis = analyze_inline(program);
  auto is_fused = [&](std::size_t d) {
    return d < plan.domain.size() && plan.domain[d].choice == ir::Materialise::FuseIntoReduction;
  };
  std::vector<ir::domain_id> inlined_into(nd, -1);
  std::vector<std::size_t> n_refs_to_consumer(nd, 0);  // consumer -> how many InlineRef entries it has so far (order-independent from the interpreter's own rebuild, kept only to mirror n_inline_temps if a caller wants it)
  for (std::size_t d = 0; d < nd; ++d) {
    const ir::Domain& dom = program.domains[d];
    if (dom.recurrent || dom.rows < 1 || analysis.blocked[d] || is_fused(d) || analysis.readers[d].empty()) continue;
    if (n_refs_to_consumer[d] > 0) continue;  // d already inlines producers itself: stays a contiguous-row group
    const ir::Group& grp = program.groups[d];
    if (is_whole_segment(grp)) {
      const ir::Segment& seg = program.segments[static_cast<std::size_t>(grp.steps.front().a.index)];
      bool uniform = true;
      bool materialised = true;
      const std::int32_t len0 = seg.offsets.size() > 1 ? seg.offsets[1] - seg.offsets[0] : 0;
      for (std::size_t r = 0; r + 1 < seg.offsets.size(); ++r) uniform &= (seg.offsets[r + 1] - seg.offsets[r]) == len0;
      for (ir::value_id v : seg.members) {
        const std::size_t t = static_cast<std::size_t>(program.domain_of(v));
        materialised &= !is_fused(t) && inlined_into[t] < 0;
      }
      if (!uniform || !materialised) continue;
    }
    const std::int32_t c = analysis.gather_user[analysis.readers[d][0]];
    if (c < 0 || c == static_cast<std::int32_t>(d)) continue;
    bool ok = true;
    for (std::size_t k : analysis.readers[d]) ok &= analysis.gather_user[k] == c;
    if (!ok) continue;
    const std::size_t cd = static_cast<std::size_t>(c);
    if (is_whole_segment(program.groups[cd]) || is_fused(cd) || program.domains[cd].recurrent || inlined_into[cd] >= 0) {
      continue;
    }
    if (static_cast<double>(analysis.refs[d]) > params.max_refs_per_row * static_cast<double>(dom.rows)) continue;
    for (std::size_t k : analysis.readers[d]) {
      for (ir::value_id v : program.gathers[k].index) {
        const std::size_t t = static_cast<std::size_t>(program.domain_of(v));
        if (t != d && inlined_into[t] >= 0) ok = false;
      }
    }
    if (!ok) continue;
    inlined_into[d] = c;
    n_refs_to_consumer[cd] += analysis.readers[d].size();
  }
  for (std::size_t d = 0; d < nd; ++d) {
    if (inlined_into[d] >= 0) {
      out.domain[d].choice = ir::Materialise::InlineIntoConsumer;
      out.domain[d].inline_consumer = inlined_into[d];
    }
  }
  return out;
}

// -------------------------------------------------------------------------------------------
// planner.fused_pairs / planner.chain_tails
// -------------------------------------------------------------------------------------------

std::vector<int> step_uses(const ir::Group& grp) {
  std::vector<int> uses(grp.steps.size(), 0);
  for (const ir::Step& st : grp.steps) {
    for (const ir::Slot* sl : {&st.a, &st.b, &st.c, &st.konst}) {
      if (sl->kind == ir::SlotKind::Step) ++uses[static_cast<std::size_t>(sl->index)];
    }
  }
  return uses;
}

OperandKind operand_kind_of(const ir::Slot& s) noexcept {
  switch (s.kind) {
    case ir::SlotKind::Literal:
    case ir::SlotKind::Column: return OperandKind::Scalar;
    case ir::SlotKind::Gather: return OperandKind::Gathered;
    default: return OperandKind::None;
  }
}

namespace {
bool is_arith(Op op) noexcept { return op == Op::Add || op == Op::Sub || op == Op::Mul || op == Op::Div; }
bool is_commutative(Op op) noexcept { return op == Op::Add || op == Op::Mul; }
}  // namespace

std::optional<PairShape> match_pair(const ir::Group& grp, const std::vector<int>& uses, std::size_t k) {
  if (k + 1 >= grp.steps.size()) return std::nullopt;
  const ir::Step& s0 = grp.steps[k];
  const ir::Step& s1 = grp.steps[k + 1];
  if (uses[k] != 1) return std::nullopt;

  PairShape shape;
  if (s0.op == Op::Neg) {
    if (operand_kind_of(s0.a) != OperandKind::Gathered) return std::nullopt;
    shape.a = s0.a;
    shape.ka = OperandKind::Gathered;
    shape.kb = OperandKind::None;
  } else if (is_arith(s0.op)) {
    shape.ka = operand_kind_of(s0.a);
    shape.kb = operand_kind_of(s0.b);
    if (shape.ka == OperandKind::None || shape.kb == OperandKind::None ||
        (shape.ka == OperandKind::Scalar && shape.kb == OperandKind::Scalar)) {
      return std::nullopt;
    }
    shape.a = s0.a;
    shape.b = s0.b;
    if (is_commutative(s0.op) && shape.ka == OperandKind::Gathered && shape.kb == OperandKind::Scalar) {
      std::swap(shape.a, shape.b);
      std::swap(shape.ka, shape.kb);
    }
  } else {
    return std::nullopt;
  }

  const ir::Slot prev{ir::SlotKind::Step, static_cast<std::int32_t>(k)};
  if (s1.op == Op::Neg) {
    if (!(s1.a == prev)) return std::nullopt;
  } else if (is_arith(s1.op)) {
    const bool left = s1.a == prev;
    const bool right = s1.b == prev;
    if (left == right) return std::nullopt;  // neither, or both (t op t)
    const ir::Slot& other = left ? s1.b : s1.a;
    shape.kc = operand_kind_of(other);
    if (shape.kc == OperandKind::None) return std::nullopt;
    shape.c = other;
    shape.prev_right = right && !is_commutative(s1.op);
  } else {
    return std::nullopt;
  }
  shape.op1 = s0.op;
  shape.op2 = s1.op;
  return shape;
}

bool match_tail(const ir::Step& st, std::size_t prev, int n_tail_so_far, int max_tail) {
  if (n_tail_so_far >= max_tail) return false;
  if (st.op != Op::Exp && st.op != Op::Log) return false;
  const ir::Slot p{ir::SlotKind::Step, static_cast<std::int32_t>(prev)};
  return st.a == p;
}

ir::PlanAnnotations fused_pairs_plan(const ir::Program& program) {
  ir::PlanAnnotations out;
  out.group.assign(program.domains.size(), ir::GroupPlan{});
  for (std::size_t d = 0; d < program.domains.size(); ++d) {
    const ir::Group& grp = program.groups[d];
    if (grp.steps.empty() || is_whole_segment(grp)) continue;
    const std::vector<int> uses = step_uses(grp);
    for (std::size_t k = 0; k < grp.steps.size();) {
      if (ir::is_fixed_sum(grp.steps[k])) {
        ++k;
        continue;
      }
      if (match_pair(grp, uses, k).has_value()) {
        ir::StepPairing pairing;
        pairing.first = static_cast<std::int32_t>(k);
        pairing.second = static_cast<std::int32_t>(k + 1);
        out.group[d].pairings.push_back(pairing);
        k += 2;
      } else {
        ++k;
      }
    }
  }
  return out;
}

ir::PlanAnnotations chain_tails_plan(const ir::Program& program, const ir::PlanAnnotations& plan, int lane_tile,
                                    int max_tail) {
  ir::PlanAnnotations out;
  out.group.assign(program.domains.size(), ir::GroupPlan{});
  if (!row_fusion_pays(lane_tile)) return out;
  for (std::size_t d = 0; d < program.domains.size() && d < plan.group.size(); ++d) {
    if (plan.group[d].pairings.empty()) continue;
    const ir::Group& grp = program.groups[d];
    const std::vector<int> uses = step_uses(grp);
    ir::GroupPlan gp = plan.group[d];
    for (ir::StepPairing& pairing : gp.pairings) {
      if (pairing.second < 0) continue;
      std::size_t last = static_cast<std::size_t>(pairing.second);
      while (last + 1 < grp.steps.size() && uses[last] == 1 &&
             match_tail(grp.steps[last + 1], last, static_cast<int>(pairing.tail.size()), max_tail)) {
        pairing.tail.push_back(static_cast<std::int32_t>(last + 1));
        ++last;
      }
    }
    out.group[d] = std::move(gp);
  }
  return out;
}

}  // namespace epykos::rewrite::planner
