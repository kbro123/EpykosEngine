// EpykosEngine — the cost model implementation (M4/CM; see include/epykos/optimise/cost.hpp for
// what is priced and what is not).
#include "epykos/optimise/cost.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <set>
#include <stdexcept>

#include "epykos/mutation/mutation.hpp"
#include "epykos/rewrite/planner.hpp"
#include "epykos/rewrite/planner_rules.hpp"
#include "epykos/util/json.hpp"

namespace epykos::optimise {

namespace ir = epykos::ir;
namespace json = epykos::json;

int lane_variant_index(int lane_width) noexcept {
  int idx = 0;
  for (int v = 0; v < n_lane_variants; ++v) {
    if (lane_variants[v] <= lane_width) idx = v;
    else break;
  }
  return idx;
}

ByteTier classify_bytes(std::size_t bytes, std::size_t l1_bytes, std::size_t l2_bytes, std::size_t l3_bytes) noexcept {
  if (bytes <= l1_bytes) return ByteTier::L1;
  if (bytes <= l2_bytes) return ByteTier::L2;
  if (bytes <= l3_bytes) return ByteTier::L3;
  return ByteTier::Dram;
}

const DomainPlan& Plan::of(ir::domain_id d) const { return domains[static_cast<std::size_t>(d)]; }

// ---- step pairing (D63) ----------------------------------------------------------------------

std::vector<std::uint8_t> internal_steps(const ir::Group& group, const std::vector<ir::StepPairing>& pairings) {
  std::vector<std::uint8_t> internal(group.steps.size(), 0);
  const auto n = static_cast<std::int32_t>(group.steps.size());
  for (const ir::StepPairing& p : pairings) {
    // The run's covered steps, in execution order: first, second (when present), then the tail.
    // Every one but the LAST stays in registers (src/exec/interpreter.cpp: one StepPlan for the
    // whole run, `sp.out` set from the run's last step only).
    std::vector<std::int32_t> covered;
    if (p.first >= 0) covered.push_back(p.first);
    if (p.second >= 0) covered.push_back(p.second);
    for (std::int32_t t : p.tail) covered.push_back(t);
    for (std::size_t i = 0; i + 1 < covered.size(); ++i) {
      const std::int32_t k = covered[i];
      if (k >= 0 && k < n) internal[static_cast<std::size_t>(k)] = 1;
    }
  }
  return internal;
}

std::size_t kernel_calls(const ir::Program& program, const Plan& plan, ir::domain_id d) {
  const std::size_t di = static_cast<std::size_t>(d);
  const ir::Group& grp = program.groups[di];
  const std::vector<std::uint8_t> internal = internal_steps(grp, plan.of(d).pairings);
  std::size_t calls = grp.steps.size();
  for (std::uint8_t f : internal) {
    if (f != 0) --calls;
  }
  return calls;
}

CostCoefficients CostCoefficients::defaults() {
  CostCoefficients c;
  for (auto& row : c.op_ns) row.fill(1.0);
  for (auto& row : c.op_ns) {
    row[static_cast<std::size_t>(Op::Const)] = 0.0;  // leaves: no arithmetic, just occupy a row
    row[static_cast<std::size_t>(Op::Input)] = 0.0;
  }
  c.byte_ns[static_cast<std::size_t>(ByteTier::L1)] = 0.3;
  c.byte_ns[static_cast<std::size_t>(ByteTier::L2)] = 1.0;
  c.byte_ns[static_cast<std::size_t>(ByteTier::L3)] = 3.0;
  c.byte_ns[static_cast<std::size_t>(ByteTier::Dram)] = 12.0;
  c.gather_ns = 2.0;
  c.dispatch_ns = 20.0;
  c.reduction_epilogue_ns = 1.0;
  c.intermediate_ns = 0.3;  // D63: one L1 store or reload of one double, ~1 cycle at ~3 GHz (an estimate)
  return c;
}

// ---- structural facts -------------------------------------------------------------------------

std::vector<DomainFacts> analyze(const ir::Program& program) {
  const std::size_t nd = program.domains.size();
  std::vector<DomainFacts> facts(nd);
  std::vector<std::set<ir::domain_id>> gather_readers(nd), segment_readers(nd), scan_readers(nd);
  std::vector<std::size_t> gather_refs(nd, 0);
  for (std::size_t d = 0; d < nd; ++d) {
    for (const ir::Step& st : program.groups[d].steps) {
      for (const ir::Slot* sl : {&st.a, &st.b, &st.c}) {
        if (sl->kind == ir::SlotKind::Gather) {
          std::set<ir::domain_id> seen;
          for (ir::value_id v : program.gathers[static_cast<std::size_t>(sl->index)].index) {
            const ir::domain_id r = program.domain_of(v);
            seen.insert(r);
            if (r != static_cast<ir::domain_id>(d)) ++gather_refs[static_cast<std::size_t>(r)];
          }
          for (ir::domain_id r : seen) {
            if (r == static_cast<ir::domain_id>(d)) continue;  // a scan's own carry
            gather_readers[static_cast<std::size_t>(r)].insert(static_cast<ir::domain_id>(d));
            if (program.domains[d].scan >= 0) scan_readers[static_cast<std::size_t>(r)].insert(static_cast<ir::domain_id>(d));
          }
        } else if (sl->kind == ir::SlotKind::Segment) {
          std::set<ir::domain_id> seen;
          for (ir::value_id v : program.segments[static_cast<std::size_t>(sl->index)].members) seen.insert(program.domain_of(v));
          for (ir::domain_id r : seen) segment_readers[static_cast<std::size_t>(r)].insert(static_cast<ir::domain_id>(d));
        }
      }
    }
  }
  for (ir::value_id v : program.outputs) ++facts[static_cast<std::size_t>(program.domain_of(v))].output_rows;
  for (std::size_t d = 0; d < nd; ++d) {
    facts[d].gather_readers.assign(gather_readers[d].begin(), gather_readers[d].end());
    facts[d].segment_readers.assign(segment_readers[d].begin(), segment_readers[d].end());
    facts[d].scan_readers.assign(scan_readers[d].begin(), scan_readers[d].end());
    facts[d].gather_refs = gather_refs[d];
  }
  return facts;
}

// D63 removed four file-local predicates from here (`is_reduction_shape`, `is_scan`,
// `row_fusion_pays`, `has_uniform_row_length`): they were infer_plan's own reconstruction of
// exec/interpreter.cpp's `decide_fusion` / `decide_inline`, and infer_plan now calls
// `rewrite::planner::default_plan` — the real decision — instead of reconstructing it. D48
// point 2 had already had to correct one of them against measurement once; keeping a second
// copy of a rule that lives in rewrite/planner.cpp was the standing cause.

Plan infer_plan(const ir::Program& program, int tile, int lane_tile) {
  const std::vector<DomainFacts> facts = analyze(program);
  const std::size_t nd = program.domains.size();

  Plan plan;
  plan.tile = tile;
  plan.lane_tile = lane_tile;
  plan.domains.resize(nd);
  for (std::size_t d = 0; d < nd; ++d) plan.domains[d].domain = static_cast<ir::domain_id>(d);

  // D63 part (b), completing D48 point 6: this is `rewrite::planner::default_plan` — the
  // interpreter's OWN decision, the same five rules exec::Interpreter::Impl::build_plan runs when
  // a Program carries no plan of its own — translated into optimise::Plan's vocabulary. It is not
  // a reproduction of those rules any more.
  //
  // What that replaced, and why (measured on fingerprint d448afd70180, 2 repetitions, loads 3.5-4.1,
  // tools/costmodel/costmodel_collect --fuse-reductions 0 / --inline-producers 0): infer_plan used
  // to re-derive materialisation from its own structural predicates, and on the M1 book those
  // predicates concluded "every domain Materialized". The real planner folds domains 3 (16,103
  // rows) and 5 (15,703 rows) into the reduction that reads them, and turning that fold off costs
  // the M1 book 1.618x of its whole runtime (61.5 us -> 99.5 us) — the single largest decision the
  // interpreter makes, and the cost model could not see it at all. Worse, `tools/costmodel/
  // fit_main.cpp` builds the FIT's feature matrix from this function, so every fit to date was
  // asking the solver to explain domain 3's measured 0.29 us with a 16,103-row materialised-domain
  // feature vector, and domain 6's 32.91 us without the folded work that is actually in it.
  const rewrite::planner::DefaultPlanOptions options{/*fuse_reductions=*/true, /*fuse_pairs=*/true,
                                                     /*inline_producers=*/true, lane_tile};
  const ir::PlanAnnotations planned = rewrite::planner::default_plan(program, options);

  if (planned.domain.size() == nd) {
    for (std::size_t d = 0; d < nd; ++d) {
      switch (planned.domain[d].choice) {
        case ir::Materialise::Materialize:
          plan.domains[d].treatment = Treatment::Materialized;
          break;
        case ir::Materialise::FuseIntoReduction:
          plan.domains[d].treatment = Treatment::FusedIntoReduction;
          // Unlike the old structural pass, `keep_rows` is the planner's real answer, not 0.
          plan.domains[d].kept_rows = planned.domain[d].keep_rows.size();
          plan.domains[d].consumers = facts[d].segment_readers;
          break;
        case ir::Materialise::InlineIntoConsumer:
          plan.domains[d].treatment = Treatment::Inlined;
          plan.domains[d].consumers = {planned.domain[d].inline_consumer};
          break;
      }
    }
  }
  // Step pairing and chain tails, from the same plan (D63 part (a)). A domain the planner named
  // no StepPairing for runs one kernel per step, exactly as with Options::fuse_pairs off.
  for (std::size_t d = 0; d < nd && d < planned.group.size(); ++d) plan.domains[d].pairings = planned.group[d].pairings;
  return plan;
}

// ---- estimation ---------------------------------------------------------------------------

namespace {

double domain_op_ns(const ir::Program& program, ir::domain_id d, int L, const CostCoefficients& c) {
  double ns = 0.0;
  const int v = lane_variant_index(L);
  const std::size_t di = static_cast<std::size_t>(d);
  const double rows = static_cast<double>(program.domains[di].rows);
  const double lanes = static_cast<double>(L);
  for (const ir::Step& st : program.groups[di].steps) {
    if (op_is_variadic(st.op) && st.a.kind == ir::SlotKind::Segment) {
      const ir::Segment& seg = program.segments[static_cast<std::size_t>(st.a.index)];
      ns += static_cast<double>(seg.members.size()) * lanes * c.reduction_epilogue_ns;
    } else {
      ns += rows * lanes * c.op_ns[static_cast<std::size_t>(v)][static_cast<std::size_t>(st.op)];
    }
  }
  return ns;
}

double domain_gather_ns(const ir::Program& program, ir::domain_id d, int L, const CostCoefficients& c) {
  const std::size_t di = static_cast<std::size_t>(d);
  std::size_t gather_slots = 0;
  for (const ir::Step& st : program.groups[di].steps) {
    for (const ir::Slot* sl : {&st.a, &st.b, &st.c}) {
      if (sl->kind == ir::SlotKind::Gather) ++gather_slots;
    }
  }
  const double rows = static_cast<double>(program.domains[di].rows);
  return static_cast<double>(gather_slots) * rows * static_cast<double>(L) * c.gather_ns;
}

std::size_t domain_dispatch_units(const ir::Program& program, ir::domain_id d, int tile) {
  const std::size_t di = static_cast<std::size_t>(d);
  const ir::Domain& dom = program.domains[di];
  const std::size_t t = tile > 0 ? static_cast<std::size_t>(tile) : 1;
  if (dom.scan >= 0) {
    const ir::Scan& sc = program.scans[static_cast<std::size_t>(dom.scan)];
    std::size_t max_len = 0;
    for (std::size_t ci = 0; ci + 1 < sc.chain_offsets.size(); ++ci) {
      max_len = std::max(max_len, static_cast<std::size_t>(sc.chain_offsets[ci + 1] - sc.chain_offsets[ci]));
    }
    const std::size_t chains = static_cast<std::size_t>(std::max(1, sc.chains()));
    const std::size_t tiles_per_wave = (chains + t - 1) / t;
    return std::max<std::size_t>(1, max_len) * std::max<std::size_t>(1, tiles_per_wave);
  }
  return (static_cast<std::size_t>(dom.rows) + t - 1) / t;
}

// `total_bytes` is the quantity moved (priced at the resulting rate); `tier_bytes` is the
// *working-set* size that decides which cache tier applies. They differ because the interpreter
// streams a domain through tiles rather than touching it all at once (DESIGN.md §4 "Layout": "an
// intermediate is a contiguous vector of tile·L doubles in a preallocated scratch (L1-sized at
// the default tile for small L)") — the relevant footprint for the tier is one tile's worth
// (tile rows x L lanes), not the whole domain, so a wide lane chunk or a large tile can push an
// otherwise-small domain into a worse tier even though its total bytes are unchanged.
double byte_cost(double total_bytes, double tier_bytes, const CostCoefficients& c) {
  const ByteTier tier = classify_bytes(static_cast<std::size_t>(tier_bytes), c.l1_bytes, c.l2_bytes, c.l3_bytes);
  return total_bytes * c.byte_ns[static_cast<std::size_t>(tier)];
}

// D63: how many times, per row and per lane, this domain's group touches the PER-STEP tile
// scratch (src/exec/interpreter.cpp's `step_buffer`) — the traffic a fused pair / chain tail is
// there to remove. A step writes the scratch when it is not the group's last step (the last one
// writes the domain's own value buffer, already priced by DomainCost::write_ns) AND it is not
// internal to a fused run; a later step's `Slot::Step` operand reads the scratch unless the step
// it names is internal (then the value is in a register). Purely structural, no plan-specific
// constant: with `pairings` empty this is exactly what the interpreter does with
// `Options::fuse_pairs = false`.
struct ScratchTouches {
  std::size_t writes = 0;
  std::size_t reads = 0;
  std::size_t total() const noexcept { return writes + reads; }
};

ScratchTouches scratch_touches(const ir::Group& grp, const std::vector<ir::StepPairing>& pairings) {
  ScratchTouches t;
  const std::vector<std::uint8_t> internal = internal_steps(grp, pairings);
  const std::size_t n = grp.steps.size();
  for (std::size_t k = 0; k < n; ++k) {
    if (k + 1 != n && internal[k] == 0) ++t.writes;
  }
  for (const ir::Step& st : grp.steps) {
    for (const ir::Slot* sl : {&st.a, &st.b, &st.c, &st.konst}) {
      if (sl->kind != ir::SlotKind::Step) continue;
      const std::size_t j = static_cast<std::size_t>(sl->index);
      if (j < n && internal[j] != 0) continue;  // read straight out of the register the pair left it in
      ++t.reads;
    }
  }
  return t;
}

std::size_t domain_readers(const std::vector<DomainFacts>& facts, ir::domain_id d) {
  // scan_readers is a subset of gather_readers (analyze() inserts both), so it is not counted
  // again here.
  const DomainFacts& f = facts[static_cast<std::size_t>(d)];
  return f.gather_readers.size() + f.segment_readers.size();
}

}  // namespace

DomainCost estimate_domain(const ir::Program& program, const std::vector<DomainFacts>& facts, const Plan& plan, ir::domain_id d, int L,
                            const CostModel& model) {
  DomainCost cost;
  const std::size_t di = static_cast<std::size_t>(d);
  const DomainPlan& dp = plan.of(d);
  const ir::Domain& dom = program.domains[di];
  const std::int64_t rows_total = std::max<std::int64_t>(0, dom.rows);

  std::int64_t priced_rows = rows_total;
  if (dp.treatment == Treatment::Inlined) {
    priced_rows = 0;
  } else if (dp.treatment == Treatment::FusedIntoReduction) {
    priced_rows = static_cast<std::int64_t>(dp.kept_rows);
  }
  if (priced_rows <= 0) return cost;  // nothing of this domain is dispatched / materialised on its own

  const double frac = rows_total > 0 ? static_cast<double>(priced_rows) / static_cast<double>(rows_total) : 0.0;
  cost.op_ns = domain_op_ns(program, d, L, model.coeffs) * frac;
  cost.gather_ns = domain_gather_ns(program, d, L, model.coeffs) * frac;

  const std::size_t tile = plan.tile > 0 ? static_cast<std::size_t>(plan.tile) : 1;
  // D63: one dispatch per KERNEL CALL per tile, not one per IR step — a run of steps the plan
  // fuses (DomainPlan::pairings) is a single call. The mutant is the pre-D63 model exactly: count
  // IR steps and charge no scratch traffic, which prices every fused pair identically to its
  // unfused twin.
  const bool pairing_unpriced = epykos::mutant("cost.pairing_unpriced");
  const std::size_t calls = pairing_unpriced ? program.groups[di].steps.size() : kernel_calls(program, plan, d);
  std::size_t units;
  if (dp.treatment == Treatment::Materialized) {
    units = domain_dispatch_units(program, d, plan.tile);
  } else {
    units = (static_cast<std::size_t>(priced_rows) + tile - 1) / tile;  // a fused domain's kept rows: plain tiles
  }
  cost.dispatch_ns = static_cast<double>(units * calls) * model.coeffs.dispatch_ns;

  // Every reader re-reads this domain's whole priced footprint once (a stated approximation,
  // see cost.hpp's header comment: no attempt at a combined reader+producer working set). The
  // cache tier is decided by one tile's worth (tile rows x L lanes), not the whole domain.
  const double bytes = static_cast<double>(priced_rows) * static_cast<double>(L) * 8.0;
  const std::size_t tile_rows = std::min<std::size_t>(tile, static_cast<std::size_t>(priced_rows));
  const double tier_bytes = static_cast<double>(tile_rows) * static_cast<double>(L) * 8.0;
  cost.write_ns = byte_cost(bytes, tier_bytes, model.coeffs);
  cost.read_ns = static_cast<double>(domain_readers(facts, d)) * byte_cost(bytes, tier_bytes, model.coeffs);

  // D63: the per-step tile scratch. Same ladder, same tier (one tile's worth of scratch is the
  // working set DESIGN.md §4 describes); the quantity is one 8-byte double per touch, per priced
  // row, per lane. A fused pair / chain tail removes its internal values' write and their one
  // reload, which is what makes a paired program cheaper than an unpaired one here.
  const ScratchTouches touches = scratch_touches(program.groups[di], dp.pairings);
  const double scratch_touches_total = static_cast<double>(touches.total()) * static_cast<double>(priced_rows) * static_cast<double>(L);
  cost.intermediate_ns = pairing_unpriced ? 0.0 : scratch_touches_total * model.coeffs.intermediate_ns;
  return cost;
}

double folded_cost_ns(const ir::Program& program, const std::vector<DomainFacts>& facts, const Plan& plan, ir::domain_id d, int L,
                       const CostModel& model) {
  const std::size_t di = static_cast<std::size_t>(d);
  const DomainPlan& dp = plan.of(d);
  if (dp.treatment == Treatment::Materialized) return 0.0;
  const ir::Domain& dom = program.domains[di];
  const double rows_total = static_cast<double>(std::max<std::int64_t>(0, dom.rows));
  const double full_op = domain_op_ns(program, d, L, model.coeffs);
  const double full_gather = domain_gather_ns(program, d, L, model.coeffs);
  if (dp.treatment == Treatment::FusedIntoReduction) {
    if (rows_total <= 0.0) return 0.0;
    const double frac = 1.0 - static_cast<double>(dp.kept_rows) / rows_total;
    return (full_op + full_gather) * std::max(0.0, frac);
  }
  // Inlined: DESIGN.md §7 recomputes the producer's steps once per gathered reference, not once
  // per row (the inliner permits up to ~1.25 references per row before it refuses to inline).
  if (rows_total <= 0.0) return 0.0;
  const double per_row = (full_op + full_gather) / rows_total;
  return per_row * static_cast<double>(facts[di].gather_refs);
}

ProgramCost estimate_program(const ir::Program& program, const Plan& plan, int B, const CostModel& model) {
  ProgramCost result;
  const std::vector<DomainFacts> facts = analyze(program);
  const std::size_t nd = program.domains.size();
  result.per_domain_ns.assign(nd, 0.0);
  const int lane_tile = plan.lane_tile > 0 ? plan.lane_tile : 1;

  int remaining = std::max(0, B);
  while (remaining > 0) {
    const int L = std::min(remaining, lane_tile);
    for (std::size_t d = 0; d < nd; ++d) {
      const DomainCost dc = estimate_domain(program, facts, plan, static_cast<ir::domain_id>(d), L, model);
      result.per_domain_ns[d] += dc.total();
    }
    for (std::size_t d = 0; d < nd; ++d) {
      const DomainPlan& dp = plan.of(static_cast<ir::domain_id>(d));
      if (dp.treatment == Treatment::Materialized || dp.consumers.empty()) continue;
      const double folded = folded_cost_ns(program, facts, plan, static_cast<ir::domain_id>(d), L, model);
      const double share = folded / static_cast<double>(dp.consumers.size());
      for (ir::domain_id c : dp.consumers) result.per_domain_ns[static_cast<std::size_t>(c)] += share;
    }
    remaining -= L;
  }
  for (double v : result.per_domain_ns) result.total_ns += v;
  return result;
}

ProgramCost estimate_program(const ir::Program& program, int B, int plan_tile, int plan_lane_tile, const CostModel& model) {
  const Plan plan = infer_plan(program, plan_tile, plan_lane_tile);
  return estimate_program(program, plan, B, model);
}

double estimate_jacobian_ns(double one_pass_ns, int n_inputs, int n_outputs, ADMode mode, const CostModel& model, double adjoint_multiplier) {
  switch (mode) {
    case ADMode::Forward:
      return one_pass_ns * static_cast<double>(std::max(0, n_inputs));
    case ADMode::Reverse:
      return one_pass_ns * adjoint_multiplier * static_cast<double>(std::max(0, n_outputs));
    case ADMode::ClosedFormAffine: {
      const int v = lane_variant_index(1);
      const double per_entry = model.coeffs.op_ns[static_cast<std::size_t>(v)][static_cast<std::size_t>(Op::Mul)] +
                                model.coeffs.op_ns[static_cast<std::size_t>(v)][static_cast<std::size_t>(Op::Add)];
      return per_entry * static_cast<double>(std::max(0, n_inputs)) * static_cast<double>(std::max(0, n_outputs));
    }
  }
  return 0.0;
}

// ---- persistence --------------------------------------------------------------------------

namespace {
constexpr const char* kFormat = "epykos-cost-model 1";
}

CostModel CostModel::load(const std::string& fingerprint_id, const std::string& results_dir) {
  namespace fs = std::filesystem;
  const fs::path path = fs::path(results_dir) / fingerprint_id / "cost_model.json";
  if (!fs::exists(path)) throw std::runtime_error("optimise::CostModel::load: no such file: " + path.string());
  const json::Value root = json::parse_file(path.string());
  const json::Value* fmt = root.find("format");
  if (fmt == nullptr || !fmt->is_string() || fmt->as_string() != kFormat) {
    throw json::JsonError("optimise::CostModel::load: " + path.string() + " is not a '" + std::string(kFormat) + "' document");
  }
  CostModel model;
  model.fingerprint = fingerprint_id;
  model.loaded_from_file = true;
  model.coeffs.l1_bytes = static_cast<std::size_t>(root.at("l1_bytes").as_int());
  model.coeffs.l2_bytes = static_cast<std::size_t>(root.at("l2_bytes").as_int());
  model.coeffs.l3_bytes = static_cast<std::size_t>(root.at("l3_bytes").as_int());
  model.coeffs.gather_ns = root.at("gather_ns").as_double();
  model.coeffs.dispatch_ns = root.at("dispatch_ns").as_double();
  model.coeffs.reduction_epilogue_ns = root.at("reduction_epilogue_ns").as_double();
  // D63 added this coefficient; a cost_model.json written before it has no such key. Read it when
  // present, otherwise leave the documented default rather than silently pricing fusion at zero
  // (the very defect D63 fixed) -- and say so, because a stale file is then only partly fitted.
  if (const json::Value* inter = root.find("intermediate_ns")) {
    model.coeffs.intermediate_ns = inter->as_double();
  } else {
    model.coeffs.intermediate_ns = CostCoefficients::defaults().intermediate_ns;
    std::cerr << "optimise::CostModel::load: " << path.string()
              << " predates D63 and has no 'intermediate_ns'; using CostCoefficients::defaults()'s "
              << model.coeffs.intermediate_ns << " ns for it, an ESTIMATE (re-run tools/costmodel/calibrate.py)\n";
  }
  const std::vector<json::Value>& byte_ns = root.at("byte_ns").as_array();
  if (byte_ns.size() != 4) throw json::JsonError("optimise::CostModel::load: byte_ns must have 4 entries");
  for (std::size_t i = 0; i < 4; ++i) model.coeffs.byte_ns[i] = byte_ns[i].as_double();
  const std::vector<json::Value>& op_ns = root.at("op_ns").as_array();
  if (static_cast<int>(op_ns.size()) != n_lane_variants) {
    throw json::JsonError("optimise::CostModel::load: op_ns must have " + std::to_string(n_lane_variants) + " rows");
  }
  for (int v = 0; v < n_lane_variants; ++v) {
    const std::vector<json::Value>& row = op_ns[static_cast<std::size_t>(v)].as_array();
    if (static_cast<int>(row.size()) != epykos::op_count) {
      throw json::JsonError("optimise::CostModel::load: op_ns row " + std::to_string(v) + " must have " + std::to_string(epykos::op_count) + " entries");
    }
    for (int o = 0; o < epykos::op_count; ++o) model.coeffs.op_ns[static_cast<std::size_t>(v)][static_cast<std::size_t>(o)] = row[static_cast<std::size_t>(o)].as_double();
  }
  return model;
}

CostModel CostModel::load_or_default(const std::string& fingerprint_id, const std::string& results_dir, std::ostream* warn) {
  try {
    return load(fingerprint_id, results_dir);
  } catch (const std::exception& e) {
    std::ostream& w = warn != nullptr ? *warn : std::cerr;
    w << "optimise::CostModel: no fitted coefficients for fingerprint '" << fingerprint_id << "' (" << e.what()
      << "); falling back to CostCoefficients::defaults() (an estimate, CLAUDE.md \"estimates are labelled\")\n";
    CostModel model;
    model.fingerprint = fingerprint_id;
    model.coeffs = CostCoefficients::defaults();
    model.loaded_from_file = false;
    return model;
  }
}

void CostModel::save(const std::string& results_dir) const {
  namespace fs = std::filesystem;
  const fs::path dir = fs::path(results_dir) / fingerprint;
  fs::create_directories(dir);
  json::Value root = json::Value::object();
  root.set("format", json::Value::string(kFormat));
  root.set("fingerprint", json::Value::string(fingerprint));
  root.set("l1_bytes", json::Value::number(static_cast<std::int64_t>(coeffs.l1_bytes)));
  root.set("l2_bytes", json::Value::number(static_cast<std::int64_t>(coeffs.l2_bytes)));
  root.set("l3_bytes", json::Value::number(static_cast<std::int64_t>(coeffs.l3_bytes)));
  root.set("gather_ns", json::Value::number(coeffs.gather_ns));
  root.set("dispatch_ns", json::Value::number(coeffs.dispatch_ns));
  root.set("reduction_epilogue_ns", json::Value::number(coeffs.reduction_epilogue_ns));
  root.set("intermediate_ns", json::Value::number(coeffs.intermediate_ns));
  std::vector<json::Value> byte_ns;
  for (double v : coeffs.byte_ns) byte_ns.push_back(json::Value::number(v));
  root.set("byte_ns", json::Value::array(std::move(byte_ns)));
  std::vector<json::Value> lane_variants_json;
  for (int lv : lane_variants) lane_variants_json.push_back(json::Value::number(static_cast<std::int64_t>(lv)));
  root.set("lane_variants", json::Value::array(std::move(lane_variants_json)));
  std::vector<json::Value> op_ns_outer;
  for (int v = 0; v < n_lane_variants; ++v) {
    std::vector<json::Value> row;
    for (int o = 0; o < epykos::op_count; ++o) row.push_back(json::Value::number(coeffs.op_ns[static_cast<std::size_t>(v)][static_cast<std::size_t>(o)]));
    op_ns_outer.push_back(json::Value::array(std::move(row)));
  }
  root.set("op_ns", json::Value::array(std::move(op_ns_outer)));
  json::write_file((dir / "cost_model.json").string(), root);
}

}  // namespace epykos::optimise
