// M4/EG "integration" — experiment (4) E1 EXTRACTION's measured half (RESUME.md §3 EG row): the
// M1 book, every landed M4 rule, extracted at E1 (fma / R4bSharedReciprocal now eligible), run
// with exec::Options::exp = poly (exp_poly, E1, D27), timed at B=1 and B=64 -- an INFORMATIONAL
// row against bench/hand's own v0 numbers (a different binary, a different exp mode axis
// cost.hpp cannot price, D9: never gated against it, only reported alongside it). Never run by
// ctest or CI; bench/run.sh writes bench/results/<fingerprint>/egraph_e1_extract_m1.json.
#include <benchmark/benchmark.h>

#include <memory>
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
using epykos::exec::ExpMode;

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

const ir::Program& e1_extracted_program() {
  static const ir::Program p = [] {
    static FullRuleSet s;
    s.planner = std::make_unique<rewrite::planner::DefaultPlanner>(rewrite::planner::DefaultPlanOptions{true, true, true, kLaneTile});
    s.rules = {&s.r1, &s.r2, &s.r3, &s.r4a, &s.r4b, &s.r5, &s.r6, &s.r7, &s.fma};
    for (const rewrite::Rule* r : s.planner->rules()) s.rules.push_back(r);
    optimise::EGraph g(program());
    g.saturate(s.rules, optimise::SaturationLimits{24, 200000, 2000});
    optimise::ExtractOptions options;
    options.max_exactness = rewrite::Exactness::E1;
    options.B = 1;
    options.tile = 256;
    options.lane_tile = kLaneTile;
    const optimise::CostModel model{optimise::CostCoefficients::defaults(), "test", false};
    const optimise::ExtractResult result = optimise::extract(g, model, options);
    ir::Program out = result.program;
    out.plan = result.plan;
    return out;
  }();
  return p;
}

Options exec_options() {
  Options o;
  o.tile = 256;
  o.lane_tile = kLaneTile;
  o.max_batch = fixtures::n_states;
  o.exp = ExpMode::poly;
  return o;
}

void set_counters(benchmark::State& state, int B) {
  state.counters["B"] = static_cast<double>(B);
  state.counters["ns_per_state"] =
      benchmark::Counter(static_cast<double>(B), benchmark::Counter::kIsIterationInvariantRate | benchmark::Counter::kInvert);
}

void BM_E1ExtractedB1(benchmark::State& state) {
  const Interpreter in(e1_extracted_program(), exec_options());
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

void BM_E1ExtractedB64(benchmark::State& state) {
  constexpr int B = 64;
  const Interpreter in(e1_extracted_program(), exec_options());
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

BENCHMARK(BM_E1ExtractedB1);
BENCHMARK(BM_E1ExtractedB64);

}  // namespace
