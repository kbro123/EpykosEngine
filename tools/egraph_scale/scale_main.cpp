// EpykosEngine — tools/egraph_scale/scale_main.cpp (docs/DECISIONS.md D62; D54's own scaling
// finding is what this tool exists to measure and re-measure).
//
// Saturates a Stage A recording of `--trades` trades with the full rule set (R1-R7, fma, the five
// planner rules — the SAME set tests/optimise/egraph_full_rules_stage_a_test.cpp runs), ONE ROUND
// AT A TIME (`saturate` with max_iterations=1, which egraph.hpp documents as resumable), and
// prints for every round: cumulative program nodes, cumulative plan nodes, that round's
// wall-clock milliseconds, the process peak resident set, and the per-rule breakdown of sites
// matched / nodes added / proposals the application memo skipped.
//
// Nothing here is gated (D9): these are informational numbers for one machine, quoted with their
// fingerprint in the decision entry that reports them, never compared across fingerprints.
//
// Usage: egraph_scale [--trades N] [--rounds N] [--max-program-nodes N] [--max-plan-nodes N]
//                     [--lane-tile N] [--policy all-rules|no-fresh-cross-rule|pipeline-ordered-plans] [--extract]
//                     [--no-dedup] [--fingerprint ID]
//
// `--policy all-rules --no-dedup` reproduces the pre-D62 behaviour exactly: that is how the
// "before" column of D62's own table was measured, in this same binary.
#include <sys/resource.h>

#include <chrono>
#include <cstdlib>
#include <exception>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "epykos/fixtures/stage_a.hpp"
#include "epykos/ir/program.hpp"
#include "epykos/ir/signature.hpp"
#include "epykos/optimise/cost.hpp"
#include "epykos/optimise/egraph.hpp"
#include "epykos/optimise/extract.hpp"
#include "epykos/optimise/plan_bridge.hpp"
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
#include "epykos/rewrite/rule.hpp"

namespace {

namespace ir = epykos::ir;
namespace fixtures = epykos::fixtures;
namespace optimise = epykos::optimise;
namespace rewrite = epykos::rewrite;

struct Args {
  int trades = 60;
  int rounds = 8;
  std::size_t max_program_nodes = 200000;
  std::size_t max_plan_nodes = 2000000;
  int lane_tile = 8;
  std::string policy = "pipeline-ordered-plans";
  bool extract = false;
  bool dedup = true;
  std::string fingerprint;
};

[[noreturn]] void usage_error(const std::string& msg) {
  std::cerr << "egraph_scale: " << msg
            << "\nusage: egraph_scale [--trades N] [--rounds N] [--max-program-nodes N]"
               " [--max-plan-nodes N] [--lane-tile N] [--policy all-rules|no-fresh-cross-rule|pipeline-ordered-plans]"
               " [--no-dedup] [--extract] [--fingerprint ID]\n";
  std::exit(2);
}

Args parse_args(int argc, char** argv) {
  Args a;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto next = [&](const char* name) -> std::string {
      if (i + 1 >= argc) usage_error(std::string("missing value for ") + name);
      return argv[++i];
    };
    if (arg == "--trades") a.trades = std::stoi(next("--trades"));
    else if (arg == "--rounds") a.rounds = std::stoi(next("--rounds"));
    else if (arg == "--max-program-nodes") a.max_program_nodes = static_cast<std::size_t>(std::stoll(next("--max-program-nodes")));
    else if (arg == "--max-plan-nodes") a.max_plan_nodes = static_cast<std::size_t>(std::stoll(next("--max-plan-nodes")));
    else if (arg == "--lane-tile") a.lane_tile = std::stoi(next("--lane-tile"));
    else if (arg == "--policy") a.policy = next("--policy");
    else if (arg == "--extract") a.extract = true;
    else if (arg == "--no-dedup") a.dedup = false;
    else if (arg == "--fingerprint") a.fingerprint = next("--fingerprint");
    else usage_error("unknown argument '" + arg + "'");
  }
  return a;
}

// Peak resident set of this process, in MiB. macOS reports ru_maxrss in bytes, Linux in KiB.
double peak_rss_mib() {
  struct rusage ru {};
  getrusage(RUSAGE_SELF, &ru);
#if defined(__APPLE__)
  return static_cast<double>(ru.ru_maxrss) / (1024.0 * 1024.0);
#else
  return static_cast<double>(ru.ru_maxrss) / 1024.0;
#endif
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

std::unique_ptr<FullRuleSet> make_full_rule_set(int lane_tile) {
  auto s = std::make_unique<FullRuleSet>();
  s->planner = std::make_unique<rewrite::planner::DefaultPlanner>(
      rewrite::planner::DefaultPlanOptions{true, true, true, lane_tile});
  s->rules = {&s->r1, &s->r2, &s->r3, &s->r4a, &s->r4b, &s->r5, &s->r6, &s->r7, &s->fma};
  for (const rewrite::Rule* r : s->planner->rules()) s->rules.push_back(r);
  return s;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const Args args = parse_args(argc, argv);
    optimise::RefirePolicy policy = optimise::RefirePolicy::NoFreshCrossRule;
    if (args.policy == "all-rules") policy = optimise::RefirePolicy::AllRules;
    else if (args.policy == "pipeline-ordered-plans") policy = optimise::RefirePolicy::PipelineOrderedPlans;
    else if (args.policy != "no-fresh-cross-rule") usage_error("unknown --policy '" + args.policy + "'");

    auto t_build = std::chrono::steady_clock::now();
    fixtures::StageAOptions opt;
    opt.trades = args.trades;
    opt.scenarios = 0;
    const fixtures::StageA s = fixtures::make_stage_a(opt);
    const fixtures::StageATape tape = fixtures::record_stage_a(s);
    const ir::Program program = ir::infer(tape.tape);
    const double build_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_build).count();

    std::size_t ir_nodes = 0;
    for (const ir::Domain& d : program.domains) ir_nodes += static_cast<std::size_t>(d.rows);
    std::cout << "== stage_a trades=" << args.trades << " domains=" << program.domains.size()
              << " tape_nodes=" << tape.tape.size() << " ir_rows=" << ir_nodes
              << " policy=" << args.policy << " dedup=" << (args.dedup ? "on" : "off")
              << " lane_tile=" << args.lane_tile
              << " (built in " << std::fixed << std::setprecision(1) << build_ms << " ms)\n";

    const std::unique_ptr<FullRuleSet> rule_set = make_full_rule_set(args.lane_tile);
    optimise::EGraph graph(program);

    optimise::SaturationLimits limits;
    limits.max_iterations = 1;
    limits.max_program_nodes = args.max_program_nodes;
    limits.max_plan_nodes = args.max_plan_nodes;
    limits.refire = policy;
    limits.dedup_before_construction = args.dedup;

    std::cout << "round  prog_nodes  prog_classes  plan_nodes    ms   peak_RSS_MiB  note\n";
    std::cout << "    0  " << std::setw(10) << graph.num_programs() << "  " << std::setw(12)
              << graph.program_class_representatives().size() << "  " << std::setw(10) << 1 << "  "
              << std::setw(6) << 0 << "  " << std::setw(12) << std::setprecision(1) << peak_rss_mib() << "\n";

    for (int round = 1; round <= args.rounds; ++round) {
      const auto t0 = std::chrono::steady_clock::now();
      const optimise::SaturationReport report = graph.saturate(rule_set->rules, limits);
      const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

      std::size_t plan_nodes = 0;
      for (int p = 0; p < graph.num_programs(); ++p) plan_nodes += static_cast<std::size_t>(graph.num_plan_nodes(p));

      std::string note;
      if (report.bound_hit) note = "BOUND " + report.bound_reason;
      std::size_t queued = 0;
      std::size_t skipped = 0;
      std::size_t blocked = 0;
      for (const optimise::SaturationLogEntry& e : report.log) {
        queued += e.nodes_added;
        skipped += e.proposals_memoised;
        blocked += e.nodes_blocked;
      }
      if (!note.empty()) note += ' ';
      note += "queued=" + std::to_string(queued) + " memoised=" + std::to_string(skipped) +
              " blocked=" + std::to_string(blocked);
      if (queued == 0) note = "FIXPOINT " + note;

      std::cout << std::setw(5) << round << "  " << std::setw(10) << graph.num_programs() << "  " << std::setw(12)
                << graph.program_class_representatives().size() << "  " << std::setw(10) << plan_nodes << "  "
                << std::setw(6) << std::setprecision(0) << ms << "  " << std::setw(12) << std::setprecision(1)
                << peak_rss_mib() << "  " << note << "\n";
      std::cout << "       per-rule (sites/added/memoised):";
      for (const optimise::SaturationLogEntry& e : report.log) {
        if (e.sites_matched == 0 && e.nodes_added == 0 && e.proposals_memoised == 0) continue;
        std::cout << ' ' << e.rule << '=' << e.sites_matched << '/' << e.nodes_added << '/' << e.proposals_memoised;
      }
      std::cout << "\n";
      std::cout.flush();

      if (queued == 0 || report.bound_hit) break;
    }

    std::cout << "== congruent=" << (graph.congruent() ? "true" : "false") << " peak_rss_mib=" << std::setprecision(1)
              << peak_rss_mib() << "\n";

    if (args.extract) {
      const optimise::CostModel synthetic{optimise::CostCoefficients::defaults(), "synthetic", false};
      const optimise::CostModel fitted =
          args.fingerprint.empty() ? synthetic
                                   : optimise::CostModel::load_or_default(args.fingerprint, "bench/results", &std::cerr);
      std::cout << "== cost model: fingerprint '" << args.fingerprint << "' fitted="
                << (fitted.loaded_from_file ? "LOADED" : "NOT FOUND (synthetic defaults)") << "\n";
      optimise::ExtractOptions options;
      options.max_exactness = rewrite::Exactness::E0;
      options.B = 1;
      options.tile = 256;
      options.lane_tile = args.lane_tile;
      const auto t0 = std::chrono::steady_clock::now();
      const optimise::ExtractResult result = optimise::extract(graph, fitted, options);
      const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
      if (!result.found) {
        std::cout << "== extract: nothing found\n";
        return 0;
      }
      const ir::PlanAnnotations default_plan =
          rewrite::planner::default_plan(program, rewrite::planner::DefaultPlanOptions{true, true, true, args.lane_tile});
      const optimise::Plan default_bridged =
          optimise::plan_from_annotations(program, default_plan, options.tile, options.lane_tile);
      const optimise::ProgramCost default_cost = optimise::estimate_program(program, default_bridged, options.B, fitted);
      std::cout << "== extract: " << result.candidates_considered << " candidates in " << std::setprecision(0) << ms
                << " ms; program node " << result.program_id << " (" << result.program.domains.size()
                << " domains), estimated " << std::setprecision(6) << result.estimated_ns << " ns vs default "
                << default_cost.total_ns << " ns, ratio " << (result.estimated_ns / default_cost.total_ns)
                << "\n== history:";
      for (const std::string& h : result.history) std::cout << ' ' << h;
      std::cout << "\n";
    }
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "egraph_scale: " << e.what() << "\n";
    return 1;
  }
}
