// M4/EG "core" -- the package's own "first experiment" (PROBLEM.md §7 / RESUME.md §3's EG row:
// "rediscovers M1's three fusions with the planner's hard-coded rules off"; report in notes, not
// yet the gate). Two `exec::Interpreter`s over the M1 book, both built with the interpreter's own
// Options-driven internal plan derivation bypassed (an EXPLICIT `program.plan` attached before
// construction -- ir/annotate.hpp point 3's "a caller sets it explicitly"), so the comparison is
// entirely about WHICH plan, never about who derives one:
//
//   "default"    rewrite::planner::default_plan(program, DefaultPlanOptions{...}) -- the SAME
//                fixed-order five-rule greedy pipeline exec::Interpreter derives internally when
//                given no explicit plan (D47): M1's own baseline, one specific point.
//   "extracted"  optimise::EGraph saturated with ONLY those same five rules (there is no other
//                planner left to switch off, D47; R1-R7 are still identity stubs, R-a/R-b/R-c's
//                job), instantiated at FOUR lane_tile choices at once (1, 8, 16, 32 -- the M3 gate
//                note this experiment answers: "row-fusion's lane-tile gate ... is a cost-model
//                decision waiting to be made, not yet a rule"), extracted by CM's cost model at
//                the SAME (tile, lane_tile, B) the benchmark then runs at.
//
// Never run by ctest or CI (CLAUDE.md); run by hand via bench/run.sh, JSON results are
// informational (this experiment is not a perf-gate baseline: no --accept, no bench/targets.json
// entry).
#include <benchmark/benchmark.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "epykos/exec/interpreter.hpp"
#include "epykos/fixtures/m1_book.hpp"
#include "epykos/fixtures/record_m1.hpp"
#include "epykos/ir/program.hpp"
#include "epykos/ir/signature.hpp"
#include "epykos/optimise/cost.hpp"
#include "epykos/optimise/egraph.hpp"
#include "epykos/optimise/extract.hpp"
#include "epykos/rewrite/planner_rules.hpp"
#include "epykos/tape/tape.hpp"

namespace {

namespace fixtures = epykos::fixtures;
namespace ir = epykos::ir;
namespace rewrite = epykos::rewrite;
namespace optimise = epykos::optimise;
using epykos::exec::Interpreter;
using epykos::exec::Options;

const fixtures::Book& book() {
  static const fixtures::Book b = fixtures::make_m1_book();
  return b;
}
const fixtures::Batch& batch() {
  static const fixtures::Batch b = fixtures::make_m1_batch();
  return b;
}
const ir::Program& program() {
  static const ir::Program p = [] {
    const epykos::Tape t = fixtures::record_m1(book());
    return ir::infer(t);
  }();
  return p;
}

const std::vector<int>& lane_tile_candidates() {
  static const std::vector<int> v = {1, 8, 16, 32};
  return v;
}

// Owns the DefaultPlanner objects (one per lane_tile candidate) for the lifetime of the process:
// DefaultPlanner's own `rules()` returns pointers to ITS OWN member Rule objects, so these must
// never move once constructed -- unique_ptr, never a plain vector<DefaultPlanner> that could
// reallocate and silently dangle every one of those pointers.
const optimise::EGraph& saturated_graph() {
  static const optimise::EGraph graph = [] {
    static std::vector<std::unique_ptr<rewrite::planner::DefaultPlanner>> planners;
    std::vector<const rewrite::Rule*> rules;
    for (int lt : lane_tile_candidates()) {
      planners.push_back(std::make_unique<rewrite::planner::DefaultPlanner>(
          rewrite::planner::DefaultPlanOptions{/*fuse_reductions=*/true, /*fuse_pairs=*/true,
                                               /*inline_producers=*/true, /*lane_tile=*/lt}));
      for (const rewrite::Rule* r : planners.back()->rules()) rules.push_back(r);
    }
    optimise::EGraph g(program());
    const optimise::SaturationReport report = g.saturate(rules, optimise::SaturationLimits{/*max_iterations=*/16,
                                                                                            /*max_plan_nodes=*/200000,
                                                                                            /*max_program_nodes=*/1000});
    std::fprintf(stderr,
                 "egraph_m1_bench: saturation %s (iterations=%d, plan_nodes=%zu, program_nodes=%zu)\n",
                 report.bound_hit ? ("BOUND HIT: " + report.bound_reason).c_str() : "reached a fixpoint",
                 report.iterations_run, report.total_plan_nodes, report.total_program_nodes);
    return g;
  }();
  return graph;
}

namespace fs = std::filesystem;

// tests/optimise/cost_test.cpp's own popen pattern (test/bench-only; not engine API): this
// machine's fingerprint, so the fitted bench/results/<fingerprint>/cost_model.json committed for
// it (D48) is used when present, rather than silently costing everything against
// CostCoefficients::defaults() and never noticing a calibrated model was sitting right there.
fs::path repo_root() {
  if (const char* e = std::getenv("EPYKOS_SOURCE_DIR")) return fs::path(e);
  return fs::path(__FILE__).parent_path().parent_path().parent_path();
}

std::string shell_quote(const std::string& s) {
  std::string r = "'";
  for (char c : s) {
    if (c == '\'') r += "'\\''";
    else r += c;
  }
  return r + "'";
}

std::string current_fingerprint_id() {
  const fs::path script = repo_root() / "scripts" / "fingerprint.sh";
  if (!fs::exists(script)) return "";
  const std::string cmd = shell_quote(script.string()) + " --id 2>/dev/null";
  FILE* p = popen(cmd.c_str(), "r");
  if (p == nullptr) return "";
  std::string out;
  char buf[256];
  while (std::size_t n = std::fread(buf, 1, sizeof buf, p)) out.append(buf, n);
  pclose(p);
  while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ')) out.pop_back();
  return out;
}

const optimise::CostModel& cost_model() {
  // D9: a fingerprint's own fitted coefficients, or CostCoefficients::defaults() with a warning
  // (CostModel::load_or_default's own contract) -- either way, this experiment's numbers are only
  // ever compared against each other on the SAME run, never across fingerprints.
  static const optimise::CostModel m =
      optimise::CostModel::load_or_default(current_fingerprint_id(), (repo_root() / "bench" / "results").string(), &std::cerr);
  return m;
}

ir::Program with_plan(ir::PlanAnnotations plan) {
  ir::Program p = program();
  p.plan = std::move(plan);
  return p;
}

const ir::Program& default_program_for(int lane_tile) {
  static std::vector<std::pair<int, ir::Program>> cache;
  for (auto& kv : cache)
    if (kv.first == lane_tile) return kv.second;
  const ir::PlanAnnotations plan =
      rewrite::planner::default_plan(program(), rewrite::planner::DefaultPlanOptions{true, true, true, lane_tile});
  cache.emplace_back(lane_tile, with_plan(plan));
  return cache.back().second;
}

const ir::Program& extracted_program_for(int lane_tile) {
  static std::vector<std::pair<int, ir::Program>> cache;
  for (auto& kv : cache)
    if (kv.first == lane_tile) return kv.second;
  optimise::ExtractOptions options;
  options.max_exactness = rewrite::Exactness::E0;
  options.B = 1;
  options.tile = 256;
  options.lane_tile = lane_tile;
  const optimise::ExtractResult result = optimise::extract(saturated_graph(), cost_model(), options);
  cache.emplace_back(lane_tile, with_plan(result.found ? result.plan : ir::PlanAnnotations{}));
  return cache.back().second;
}

Options exec_options(int lane_tile) {
  Options o;
  o.tile = 256;
  o.lane_tile = lane_tile;
  o.max_batch = fixtures::n_states;
  return o;
}

void set_counters(benchmark::State& state, const Interpreter& in, int B, int lane_tile) {
  state.SetLabel("lane_tile " + std::to_string(lane_tile));
  state.counters["B"] = static_cast<double>(B);
  state.counters["lane_tile"] = static_cast<double>(lane_tile);
  state.counters["ns_per_state"] =
      benchmark::Counter(static_cast<double>(B), benchmark::Counter::kIsIterationInvariantRate | benchmark::Counter::kInvert);
  benchmark::DoNotOptimize(in.num_values());
}

void run_b1(benchmark::State& state, const ir::Program& p, int lane_tile) {
  const Interpreter in(p, exec_options(lane_tile));
  std::vector<double> out(1001);
  double z[12];
  double bump = 0.0;
  for (auto _ : state) {
    for (int kk = 0; kk < 12; ++kk) z[kk] = book().z0[static_cast<std::size_t>(kk)] + bump;
    bump += 1e-9;
    in.run(z, 1, out.data());
    benchmark::DoNotOptimize(out.data());
    benchmark::ClobberMemory();
  }
  set_counters(state, in, 1, lane_tile);
}

void run_b64(benchmark::State& state, const ir::Program& p, int lane_tile) {
  constexpr int B = 64;
  const Interpreter in(p, exec_options(lane_tile));
  std::vector<double> z(12u * B), out(1001u * B);
  double bump = 0.0;
  for (auto _ : state) {
    for (std::size_t j = 0; j < z.size(); ++j) z[j] = batch().z[j] + bump;
    bump += 1e-9;
    in.run(z.data(), B, out.data());
    benchmark::DoNotOptimize(out.data());
    benchmark::ClobberMemory();
  }
  set_counters(state, in, B, lane_tile);
}

void BM_DefaultPlanB1(benchmark::State& state) { run_b1(state, default_program_for(static_cast<int>(state.range(0))), static_cast<int>(state.range(0))); }
void BM_ExtractedPlanB1(benchmark::State& state) { run_b1(state, extracted_program_for(static_cast<int>(state.range(0))), static_cast<int>(state.range(0))); }
void BM_DefaultPlanB64(benchmark::State& state) { run_b64(state, default_program_for(static_cast<int>(state.range(0))), static_cast<int>(state.range(0))); }
void BM_ExtractedPlanB64(benchmark::State& state) { run_b64(state, extracted_program_for(static_cast<int>(state.range(0))), static_cast<int>(state.range(0))); }

void register_all() {
  for (int lt : lane_tile_candidates()) {
    benchmark::RegisterBenchmark("BM_DefaultPlanB1", BM_DefaultPlanB1)->Arg(lt);
    benchmark::RegisterBenchmark("BM_ExtractedPlanB1", BM_ExtractedPlanB1)->Arg(lt);
    benchmark::RegisterBenchmark("BM_DefaultPlanB64", BM_DefaultPlanB64)->Arg(lt);
    benchmark::RegisterBenchmark("BM_ExtractedPlanB64", BM_ExtractedPlanB64)->Arg(lt);
  }
}

struct Registrar {
  Registrar() { register_all(); }
} registrar;

}  // namespace
