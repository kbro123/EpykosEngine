// M4/EG "integration" -- experiment (1) REDISCOVERY's measured half (PROBLEM.md §7 / RESUME.md §3
// EG row): "the extracted plan's B=1 and B=64 time vs M1's greedy plan (target within 1.02x;
// report the plan diff)", with EVERY landed M4 rule in the e-graph (R1-R7, fma, the five planner
// rules -- egraph_m1_bench.cpp, M4/EG "core", ran this same comparison with only the five planner
// rules, before R-a/R-b/R-c landed R1-R7/fma for real). Single lane_tile (8: M1's own baseline
// configuration) -- unlike egraph_m1_bench.cpp's own 4-lane_tile sweep (that package's own
// row-fusion-lane-tile question), which this file does not repeat. Never run by ctest or CI
// (CLAUDE.md); bench/run.sh writes bench/results/<fingerprint>/egraph_full_rules_m1.json.
#include <benchmark/benchmark.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "epykos/exec/interpreter.hpp"
#include "epykos/fixtures/m1_book.hpp"
#include "epykos/fixtures/record_m1.hpp"
#include "epykos/ir/program.hpp"
#include "epykos/ir/signature.hpp"
#include "epykos/optimise/cost.hpp"
#include "epykos/optimise/egraph.hpp"
#include "epykos/optimise/extract.hpp"
#include "epykos/rewrite/fma_contraction.hpp"
#include "epykos/rewrite/planner_rules.hpp"
#include "epykos/rewrite/r1_fold_uniform_columns.hpp"
#include "epykos/rewrite/r2_bucket_rows.hpp"
#include "epykos/rewrite/r3_elide_trivial_maps.hpp"
#include "epykos/rewrite/r4a_push_unary_through_gathers.hpp"
#include "epykos/rewrite/r4b_shared_reciprocal.hpp"
#include "epykos/rewrite/r5_group_formation.hpp"
#include "epykos/rewrite/r6_materialise_boundaries.hpp"
#include "epykos/rewrite/r7_block_linmap.hpp"
#include "epykos/tape/tape.hpp"

namespace {

namespace fixtures = epykos::fixtures;
namespace ir = epykos::ir;
namespace rewrite = epykos::rewrite;
namespace optimise = epykos::optimise;
using epykos::exec::Interpreter;
using epykos::exec::Options;

constexpr int kLaneTile = 8;

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

namespace fs = std::filesystem;

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
  static const optimise::CostModel m =
      optimise::CostModel::load_or_default(current_fingerprint_id(), (repo_root() / "bench" / "results").string(), &std::cerr);
  return m;
}

struct FullRuleSet {
  std::unique_ptr<rewrite::planner::DefaultPlanner> planner;
  rewrite::R1FoldUniformColumns r1;
  rewrite::R2BucketRows r2;
  rewrite::R3ElideTrivialMaps r3;
  rewrite::R4aPushUnaryThroughGathers r4a;
  rewrite::R4bSharedReciprocal r4b;
  rewrite::R5GroupFormation r5;
  rewrite::R6MaterialiseBoundaries r6;
  rewrite::R7BlockLinmap r7;
  rewrite::FmaContractionRule fma;
  std::vector<const rewrite::Rule*> rules;
};

const optimise::EGraph& saturated_graph() {
  static const optimise::EGraph graph = [] {
    static FullRuleSet s;
    s.planner = std::make_unique<rewrite::planner::DefaultPlanner>(
        rewrite::planner::DefaultPlanOptions{true, true, true, kLaneTile});
    s.rules = {&s.r1, &s.r2, &s.r3, &s.r4a, &s.r4b, &s.r5, &s.r6, &s.r7, &s.fma};
    for (const rewrite::Rule* r : s.planner->rules()) s.rules.push_back(r);
    optimise::EGraph g(program());
    const optimise::SaturationReport report =
        g.saturate(s.rules, optimise::SaturationLimits{/*max_iterations=*/24, /*max_plan_nodes=*/200000, /*max_program_nodes=*/2000});
    std::fprintf(stderr, "egraph_full_rules_m1_bench: saturation %s (iterations=%d, programs=%zu, plan_nodes=%zu)\n",
                 report.bound_hit ? ("BOUND HIT: " + report.bound_reason).c_str() : "reached a fixpoint", report.iterations_run,
                 static_cast<std::size_t>(g.num_programs()), report.total_plan_nodes);
    return g;
  }();
  return graph;
}

ir::Program with_plan(ir::Program p, ir::PlanAnnotations plan) {
  p.plan = std::move(plan);
  return p;
}

const ir::Program& default_program() {
  static const ir::Program p =
      with_plan(program(), rewrite::planner::default_plan(program(), rewrite::planner::DefaultPlanOptions{true, true, true, kLaneTile}));
  return p;
}

const ir::Program& extracted_program() {
  static const ir::Program p = [] {
    optimise::ExtractOptions options;
    options.max_exactness = rewrite::Exactness::E0;
    options.B = 1;
    options.tile = 256;
    options.lane_tile = kLaneTile;
    const optimise::ExtractResult result = optimise::extract(saturated_graph(), cost_model(), options);
    std::fprintf(stderr, "egraph_full_rules_m1_bench: extracted program %d (%zu domains), estimated %.1f ns, history:",
                 result.program_id, result.program.domains.size(), result.estimated_ns);
    for (const std::string& h : result.history) std::fprintf(stderr, " %s", h.c_str());
    std::fprintf(stderr, "\n");
    return with_plan(result.program, result.plan);
  }();
  return p;
}

Options exec_options() {
  Options o;
  o.tile = 256;
  o.lane_tile = kLaneTile;
  o.max_batch = fixtures::n_states;
  return o;
}

void set_counters(benchmark::State& state, int B) {
  state.counters["B"] = static_cast<double>(B);
  state.counters["ns_per_state"] =
      benchmark::Counter(static_cast<double>(B), benchmark::Counter::kIsIterationInvariantRate | benchmark::Counter::kInvert);
}

void run_b1(benchmark::State& state, const ir::Program& p) {
  const Interpreter in(p, exec_options());
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
  set_counters(state, 1);
}

void run_b64(benchmark::State& state, const ir::Program& p) {
  constexpr int B = 64;
  const Interpreter in(p, exec_options());
  std::vector<double> z(12u * B), out(1001u * B);
  double bump = 0.0;
  for (auto _ : state) {
    for (std::size_t j = 0; j < z.size(); ++j) z[j] = batch().z[j] + bump;
    bump += 1e-9;
    in.run(z.data(), B, out.data());
    benchmark::DoNotOptimize(out.data());
    benchmark::ClobberMemory();
  }
  set_counters(state, B);
}

void BM_DefaultPlanB1(benchmark::State& state) { run_b1(state, default_program()); }
void BM_ExtractedFullB1(benchmark::State& state) { run_b1(state, extracted_program()); }
void BM_DefaultPlanB64(benchmark::State& state) { run_b64(state, default_program()); }
void BM_ExtractedFullB64(benchmark::State& state) { run_b64(state, extracted_program()); }

struct Registrar {
  Registrar() {
    benchmark::RegisterBenchmark("BM_DefaultPlanB1", BM_DefaultPlanB1);
    benchmark::RegisterBenchmark("BM_ExtractedFullB1", BM_ExtractedFullB1);
    benchmark::RegisterBenchmark("BM_DefaultPlanB64", BM_DefaultPlanB64);
    benchmark::RegisterBenchmark("BM_ExtractedFullB64", BM_ExtractedFullB64);
  }
} registrar;

}  // namespace
