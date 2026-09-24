// EpykosEngine — the cost model implementation (M4/CM; see include/epykos/optimise/cost.hpp for
// what is priced and what is not).
#include "epykos/optimise/cost.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <set>
#include <stdexcept>

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

namespace {

bool is_reduction_shape(const ir::Program& program, std::size_t d) {
  const std::vector<ir::Step>& steps = program.groups[d].steps;
  if (steps.empty()) return false;
  const ir::Step& last = steps.back();
  return op_is_variadic(last.op) && last.a.kind == ir::SlotKind::Segment;
}

bool is_scan(const ir::Program& program, std::size_t d) { return program.domains[d].scan >= 0; }

bool row_fusion_pays(int lane_tile) noexcept { return lane_tile == 1 || lane_tile >= 16; }

// exec/interpreter.cpp's decide_inline(): a whole-domain Sum/Affine producer is still eligible
// to be inlined into its one consumer's tiles when every row has the SAME member count (an
// interpolation: two knot values per row is the common case, DESIGN.md §6 R4a/R7) — it is not
// the reduction-vs-elementwise distinction that decides eligibility, it is whether the per-row
// work is uniform enough to vectorise inside the consumer's own tile loop.
bool has_uniform_row_length(const ir::Program& program, std::size_t d) {
  const std::vector<ir::Step>& steps = program.groups[d].steps;
  if (steps.empty() || steps.back().a.kind != ir::SlotKind::Segment) return false;
  const ir::Segment& seg = program.segments[static_cast<std::size_t>(steps.back().a.index)];
  if (seg.offsets.size() < 2) return true;
  const std::int32_t len0 = seg.offsets[1] - seg.offsets[0];
  for (std::size_t r = 0; r + 1 < seg.offsets.size(); ++r) {
    if (seg.offsets[r + 1] - seg.offsets[r] != len0) return false;
  }
  return true;
}

}  // namespace

Plan infer_plan(const ir::Program& program, int tile, int lane_tile) {
  const std::vector<DomainFacts> facts = analyze(program);
  const std::size_t nd = program.domains.size();

  Plan plan;
  plan.tile = tile;
  plan.lane_tile = lane_tile;
  plan.domains.resize(nd);
  for (std::size_t d = 0; d < nd; ++d) plan.domains[d].domain = static_cast<ir::domain_id>(d);

  // Pass 1: fused-into-reduction producers (DESIGN.md §7): elementwise, not a scan, read ONLY as
  // Sum/Affine members, no gather reader at all, no output rows of its own.
  std::vector<std::uint8_t> fused(nd, 0);
  for (std::size_t d = 0; d < nd; ++d) {
    const bool ok = !is_reduction_shape(program, d) && !is_scan(program, d) && facts[d].gather_readers.empty() &&
                    !facts[d].segment_readers.empty() && facts[d].output_rows == 0;
    if (ok) {
      fused[d] = 1;
      plan.domains[d].treatment = Treatment::FusedIntoReduction;
      plan.domains[d].consumers = facts[d].segment_readers;
      // infer_plan cannot see which rows are ALSO read elsewhere without a second structural
      // pass it does not perform (see the header comment): kept_rows stays 0 here.
    }
  }

  // Pass 2: inlined producers: not a scan, not already fused, read ONLY through the gathers of
  // exactly one consumer that is itself materialised and elementwise (not a whole-domain
  // reduction, not a scan, not fused), row fusion paying at this lane_tile, and not
  // over-referenced (exec/interpreter.cpp's inline_max_refs_per_row = 1.25). A whole-domain
  // Sum / Affine producer qualifies too, but only with a uniform row length (has_uniform_row_length,
  // above) — exec/interpreter.cpp's own `decide_inline` rule, not a reduction-vs-elementwise one.
  const bool pays = row_fusion_pays(lane_tile);
  for (std::size_t d = 0; d < nd; ++d) {
    if (fused[d] != 0) continue;
    if (is_scan(program, d)) continue;
    if (is_reduction_shape(program, d) && !has_uniform_row_length(program, d)) continue;
    if (!facts[d].segment_readers.empty() || facts[d].output_rows != 0) continue;
    if (facts[d].gather_readers.size() != 1) continue;
    const std::size_t c = static_cast<std::size_t>(facts[d].gather_readers[0]);
    if (is_reduction_shape(program, c) || is_scan(program, c) || fused[c] != 0) continue;
    if (!pays) continue;
    const double rows = static_cast<double>(program.domains[d].rows);
    if (rows > 0.0 && static_cast<double>(facts[d].gather_refs) > 1.25 * rows) continue;
    plan.domains[d].treatment = Treatment::Inlined;
    plan.domains[d].consumers = {facts[d].gather_readers[0]};
  }
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
  const std::size_t steps = program.groups[di].steps.size();
  std::size_t units;
  if (dp.treatment == Treatment::Materialized) {
    units = domain_dispatch_units(program, d, plan.tile);
  } else {
    units = (static_cast<std::size_t>(priced_rows) + tile - 1) / tile;  // a fused domain's kept rows: plain tiles
  }
  cost.dispatch_ns = static_cast<double>(units * steps) * model.coeffs.dispatch_ns;

  // Every reader re-reads this domain's whole priced footprint once (a stated approximation,
  // see cost.hpp's header comment: no attempt at a combined reader+producer working set). The
  // cache tier is decided by one tile's worth (tile rows x L lanes), not the whole domain.
  const double bytes = static_cast<double>(priced_rows) * static_cast<double>(L) * 8.0;
  const std::size_t tile_rows = std::min<std::size_t>(tile, static_cast<std::size_t>(priced_rows));
  const double tier_bytes = static_cast<double>(tile_rows) * static_cast<double>(L) * 8.0;
  cost.write_ns = byte_cost(bytes, tier_bytes, model.coeffs);
  cost.read_ns = static_cast<double>(domain_readers(facts, d)) * byte_cost(bytes, tier_bytes, model.coeffs);
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
