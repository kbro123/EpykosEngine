// M3/G6 gate: the remaining PROBLEM.md section 6 gates on the Stage A tape, each an explicit
// boolean with its worst number ("[  GATE    ]" lines):
//
//   scenario_lanes   O4: 32 seeded scenario lanes of the 1,000, run as the grid runs them (batches
//                    of 64 lanes through one ImplicitProgram), each bitwise an independent
//                    single-scenario run (a separate single-lane ImplicitProgram): every output,
//                    the solved state, the solve reports' iteration counts;
//   optimality       O1: ‖Jᵀr‖∞ < 1e-12 on every block at the record point, at the O2 run and on
//                    every sampled scenario lane;
//   recovery         O1: with the quote noise at 0 the quotes are the generating curves' par
//                    quotes and every solved knot is the generating zero rate (record-time
//                    Gauss–Newton and run-time chord solves alike);
//   one_tape         O1, O2 and the O3 / O4 runs come from the one recording: the ImplicitProgram
//                    that produces O2 and O4 and the adjoint that produces O3 are built from the
//                    same Tape and ImplicitRegistry, and the output layout accounts for every
//                    output of that tape.
//
// The last test also writes the interpreter's plan (Interpreter::describe()) and the IR facts
// per domain (rows, readers by kind, output rows) to files in the test's working directory —
// stage_a_plan.txt and stage_a_domains.csv — which scripts/exec_coverage.py joins into the
// fusion-coverage table of the G6 report (optionally with -DEPYKOS_EXEC_PROFILE timings).
//
// Not a gate of scripts/mutation_test.sh (stage_a_outputs_e0_test holds the lane E0 gate there).
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "epykos/ir/program.hpp"
#include "epykos/rng/philox.hpp"
#include "epykos/solver/implicit_program.hpp"
#include "epykos/tape/op.hpp"
#include "stage_a/stage_a_test_helpers.hpp"

namespace fixtures = epykos::fixtures;
namespace ir = epykos::ir;
namespace solver = epykos::solver;
using epykos::test::bits;
using epykos::test::lane_knots;
using epykos::test::stage_a;
using epykos::test::stage_a_tape;

namespace {

constexpr std::uint64_t gate_seed = 20260923;
constexpr int n_lanes = 32;

std::vector<int> sample_lanes() {
  const fixtures::StageA& s = stage_a();
  epykos::rng::Philox g(gate_seed, 3);
  std::set<int> picked;
  while (static_cast<int>(picked.size()) < n_lanes) {
    const int l = 1 + static_cast<int>(g.uniform_range(0.0, static_cast<double>(s.n_scenarios() - 1))) % (s.n_scenarios() - 1);
    picked.insert(l);
  }
  return std::vector<int>(picked.begin(), picked.end());
}

}  // namespace

TEST(StageAGateLanes, SampledScenarioLanesAreBitwiseIndependentSingleRuns) {
  const fixtures::StageA& s = stage_a();
  const fixtures::StageATape& t = stage_a_tape();
  const std::vector<int> lanes = sample_lanes();
  ASSERT_EQ(lanes.size(), static_cast<std::size_t>(n_lanes));
  // The grid's program: batches of 64 lanes, the chunks the grid itself runs.
  solver::ImplicitProgram grid(t.tape, t.registry, fixtures::stage_a_program_options(s, 64));
  const int chunk = grid.max_batch();
  const int n_out = grid.n_outputs(), n_in = grid.n_inputs();
  std::map<int, std::vector<double>> batched_out, batched_state;
  std::map<int, std::vector<int>> batched_iterations;
  std::set<int> chunks;
  for (int l : lanes) chunks.insert(l / chunk);
  double worst_jtr = 0.0;
  int not_converged = 0;
  epykos::test::clock_type::time_point t0 = epykos::test::clock_type::now();
  for (int c : chunks) {
    const int b0 = c * chunk;
    const int B = std::min(chunk, s.n_scenarios() - b0);
    const std::vector<std::vector<double>> q(s.scenario_quotes.begin() + b0, s.scenario_quotes.begin() + b0 + B);
    const std::vector<double> o = fixtures::run_lanes(grid, q);
    for (int l : lanes) {
      if (l / chunk != c) continue;
      const int b = l - b0;
      std::vector<double>& out = batched_out[l];
      out.resize(static_cast<std::size_t>(n_out));
      for (int oo = 0; oo < n_out; ++oo) out[static_cast<std::size_t>(oo)] = o[static_cast<std::size_t>(oo) * static_cast<std::size_t>(B) + static_cast<std::size_t>(b)];
      batched_state[l].assign(grid.full_state(b), grid.full_state(b) + n_in);
      for (int k = 0; k < grid.n_blocks(); ++k) {
        const solver::SolveReport& r = grid.report(k, b);
        batched_iterations[l].push_back(r.iterations);
        not_converged += !r.converged;
        worst_jtr = std::max(worst_jtr, r.jtr_inf);
      }
    }
  }
  const double t_batched = epykos::test::seconds_since(t0);
  // Independent single runs: a separate single-lane program.
  solver::ImplicitProgram single(t.tape, t.registry, fixtures::stage_a_program_options(s, 1));
  std::size_t output_mismatches = 0, state_mismatches = 0, iteration_mismatches = 0;
  int worst_lane = -1;
  t0 = epykos::test::clock_type::now();
  for (int l : lanes) {
    const std::vector<double> o = fixtures::run_lanes(single, {s.scenario_quotes[static_cast<std::size_t>(l)]});
    std::size_t mism = 0;
    for (int oo = 0; oo < n_out; ++oo) mism += bits(o[static_cast<std::size_t>(oo)]) != bits(batched_out[l][static_cast<std::size_t>(oo)]);
    for (int k = 0; k < n_in; ++k) state_mismatches += bits(single.full_state(0)[k]) != bits(batched_state[l][static_cast<std::size_t>(k)]);
    for (int k = 0; k < single.n_blocks(); ++k) {
      const solver::SolveReport& r = single.report(k, 0);
      iteration_mismatches += r.iterations != batched_iterations[l][static_cast<std::size_t>(k)];
      not_converged += !r.converged;
      worst_jtr = std::max(worst_jtr, r.jtr_inf);
    }
    if (mism > 0 && worst_lane < 0) worst_lane = l;
    output_mismatches += mism;
  }
  const double t_single = epykos::test::seconds_since(t0);
  std::cout << "[  lanes   ] " << n_lanes << " sampled lanes in " << chunks.size() << " chunks of " << chunk << " (" << t_batched << " s batched) vs " << n_lanes
            << " single runs (" << t_single << " s): output mismatches " << output_mismatches << " of " << static_cast<std::size_t>(n_lanes) * static_cast<std::size_t>(n_out)
            << ", state mismatches " << state_mismatches << ", iteration mismatches " << iteration_mismatches << ", not converged " << not_converged << ", worst |J^T r|_inf "
            << worst_jtr << "; lanes:";
  for (int l : lanes) std::cout << ' ' << l;
  std::cout << '\n';
  const bool ok = output_mismatches == 0 && state_mismatches == 0 && iteration_mismatches == 0 && not_converged == 0;
  std::cout << "[  GATE    ] scenario_lanes ok=" << (ok ? 1 : 0) << " lanes=" << n_lanes << " output_mismatches=" << output_mismatches << " state_mismatches=" << state_mismatches
            << " first_mismatching_lane=" << worst_lane << '\n';
  std::cout << "[  GATE    ] optimality_lanes ok=" << (worst_jtr < 1e-12 && not_converged == 0 ? 1 : 0) << " worst=" << worst_jtr << " gate=1e-12 lanes=" << n_lanes
            << " blocks=" << grid.n_blocks() << '\n';
  EXPECT_EQ(output_mismatches, 0u);
  EXPECT_EQ(state_mismatches, 0u);
  EXPECT_EQ(iteration_mismatches, 0u);
  EXPECT_EQ(not_converged, 0);
  EXPECT_LT(worst_jtr, 1e-12);
}

TEST(StageAGateLanes, OptimalityPerCurveAtTheRecordPointAndAtTheRun) {
  const fixtures::StageA& s = stage_a();
  const fixtures::StageATape& t = stage_a_tape();
  solver::ImplicitProgram prog(t.tape, t.registry, fixtures::stage_a_program_options(s, 8));
  const std::vector<double> out = fixtures::run_lanes(prog, {s.quotes});
  double worst = 0.0;
  bool all_converged = true;
  std::cout << "[  optimal ] per block (record-time Gauss-Newton | run-time chord): ";
  for (int k = 0; k < prog.n_blocks(); ++k) {
    const solver::SolveReport& rec = t.record_reports[static_cast<std::size_t>(k)];
    const solver::SolveReport& run = prog.report(k, 0);
    all_converged = all_converged && rec.converged && run.converged;
    worst = std::max({worst, rec.jtr_inf, run.jtr_inf, out[static_cast<std::size_t>(t.registry.blocks[static_cast<std::size_t>(k)].diag_jtr_output)]});
    std::cout << (k ? "; " : "") << t.registry.blocks[static_cast<std::size_t>(k)].name << " " << rec.jtr_inf << " (" << rec.iterations << " it) | " << run.jtr_inf << " ("
              << run.iterations << " it, |F| " << run.residual_inf << ")";
    EXPECT_LT(rec.jtr_inf, 1e-12) << "block " << k;
    EXPECT_LT(run.jtr_inf, 1e-12) << "block " << k;
    EXPECT_TRUE(rec.converged && run.converged) << "block " << k;
    for (int o : t.registry.blocks[static_cast<std::size_t>(k)].residuals) EXPECT_LT(std::fabs(out[static_cast<std::size_t>(o)]), s.options.solve_tol) << "block " << k;
  }
  std::cout << '\n';
  std::cout << "[  GATE    ] optimality ok=" << (worst < 1e-12 && all_converged ? 1 : 0) << " worst=" << worst << " gate=1e-12 blocks=" << prog.n_blocks() << '\n';
}

TEST(StageAGateLanes, RecoversTheGeneratingCurvesFromTheirParQuotes) {
  fixtures::StageAOptions o;
  o.quote_noise_bp = 0.0;
  o.trades = 20;
  o.scenarios = 0;
  const fixtures::StageA exact = fixtures::make_stage_a(o);
  const fixtures::StageATape t = fixtures::record_stage_a(exact);
  double worst_record = 0.0, worst_run = 0.0;
  for (std::size_t c = 0; c < t.record_knots.size(); ++c) {
    ASSERT_EQ(t.record_knots[c].size(), exact.generating[c].size());
    for (std::size_t k = 0; k < t.record_knots[c].size(); ++k) worst_record = std::max(worst_record, std::fabs(t.record_knots[c][k] - exact.generating[c][k]));
  }
  solver::ImplicitProgram prog(t.tape, t.registry, fixtures::stage_a_program_options(exact, 1));
  const std::vector<double> out = fixtures::run_lanes(prog, {exact.quotes});
  const std::vector<std::vector<double>> z = lane_knots(prog, t, 0);
  for (std::size_t c = 0; c < z.size(); ++c) {
    for (std::size_t k = 0; k < z[c].size(); ++k) worst_run = std::max(worst_run, std::fabs(z[c][k] - exact.generating[c][k]));
  }
  bool converged = true;
  for (std::size_t k = 0; k < t.record_reports.size(); ++k) converged = converged && t.record_reports[k].converged && prog.report(static_cast<int>(k), 0).converged;
  std::cout << "[  recover ] noise 0: worst |z - generating| record-time " << worst_record << ", run-time " << worst_run << " (zero-rate units) over " << exact.n_quotes() << " knots\n";
  std::cout << "[  GATE    ] recovery ok=" << (worst_record < 1e-12 && worst_run < 1e-12 && converged ? 1 : 0) << " worst=" << std::max(worst_record, worst_run) << " gate=1e-12\n";
  EXPECT_TRUE(converged);
  EXPECT_LT(worst_record, 1e-12);
  EXPECT_LT(worst_run, 1e-12);
}

TEST(StageAGateLanes, OneTapeAndThePlan) {
  const fixtures::StageA& s = stage_a();
  const fixtures::StageATape& t = stage_a_tape();
  solver::ImplicitProgram prog(t.tape, t.registry, fixtures::stage_a_program_options(s, 64));
  // One tape: every output group is a range of the one tape's outputs, and the program that
  // runs O2 / O4 and the adjoint that runs O3 are built over that tape.
  const std::vector<std::vector<int>> groups = t.output_groups();
  std::size_t covered = 0;
  for (const std::vector<int>& g : groups) covered += g.size();
  const bool one_tape = static_cast<int>(covered) == t.layout.n_outputs && t.layout.n_outputs == prog.n_outputs() && prog.n_state() == s.n_quotes() &&
                        prog.n_blocks() == s.n_curves() && &prog.whole_adjoint().program() == &prog.program() && &prog.interpreter().program() == &prog.program();
  std::cout << "[  GATE    ] one_tape ok=" << (one_tape ? 1 : 0) << " outputs=" << t.layout.n_outputs << " groups=" << groups.size() << " (block " << groups[0].size() << ", O1 "
            << groups[1].size() << ", O2 " << groups[2].size() << (groups.size() > 3 ? ", O6 " + std::to_string(groups[3].size()) : std::string()) << ") inputs=" << prog.n_inputs()
            << " free=" << prog.n_state() << " blocks=" << prog.n_blocks() << " nodes=" << t.stats.nodes_after_passes << '\n';
  EXPECT_TRUE(one_tape);
  // The plan and the IR facts, for scripts/exec_coverage.py.
  const ir::Program& p = prog.program();
  const std::string plan = prog.interpreter().describe();
  {
    std::ofstream f("stage_a_plan.txt");
    f << prog.describe() << plan;
  }
  // Readers by kind: for every domain, the domains reading it through gathers and through
  // segments (Sum / Affine members), and which of those readers are scans; output rows per domain.
  const std::size_t nd = p.domains.size();
  std::vector<std::set<ir::domain_id>> gather_readers(nd), segment_readers(nd), scan_readers(nd);
  std::vector<std::size_t> output_rows(nd, 0), gather_slots(nd, 0), segment_slots(nd, 0), gather_refs(nd, 0);
  for (std::size_t d = 0; d < nd; ++d) {
    for (const ir::Step& st : p.groups[d].steps) {
      for (const ir::Slot* sl : {&st.a, &st.b, &st.c}) {
        if (sl->kind == ir::SlotKind::Gather) {
          ++gather_slots[d];
          std::set<ir::domain_id> seen;
          for (ir::value_id v : p.gathers[static_cast<std::size_t>(sl->index)].index) {
            const ir::domain_id r = p.domain_of(v);
            seen.insert(r);
            if (r != static_cast<ir::domain_id>(d)) ++gather_refs[static_cast<std::size_t>(r)];
          }
          for (ir::domain_id r : seen) {
            if (r == static_cast<ir::domain_id>(d)) continue;   // a scan's carry
            gather_readers[static_cast<std::size_t>(r)].insert(static_cast<ir::domain_id>(d));
            if (p.domains[d].scan >= 0) scan_readers[static_cast<std::size_t>(r)].insert(static_cast<ir::domain_id>(d));
          }
        } else if (sl->kind == ir::SlotKind::Segment) {
          ++segment_slots[d];
          std::set<ir::domain_id> seen;
          for (ir::value_id v : p.segments[static_cast<std::size_t>(sl->index)].members) seen.insert(p.domain_of(v));
          for (ir::domain_id r : seen) segment_readers[static_cast<std::size_t>(r)].insert(static_cast<ir::domain_id>(d));
        }
      }
    }
  }
  for (ir::value_id v : p.outputs) ++output_rows[static_cast<std::size_t>(p.domain_of(v))];
  {
    std::ofstream f("stage_a_domains.csv");
    f << "domain,shape,rows,level,scan,chains,last_op,steps,reads,gather_readers,segment_readers,scan_readers,output_rows,gather_refs\n";
    for (std::size_t d = 0; d < nd; ++d) {
      const ir::Domain& dom = p.domains[d];
      auto list = [](const std::set<ir::domain_id>& x) {
        std::string s2;
        for (ir::domain_id r : x) s2 += (s2.empty() ? "" : " ") + std::to_string(r);
        return s2;
      };
      std::string reads;
      for (ir::domain_id r : dom.reads) reads += (reads.empty() ? "" : " ") + std::to_string(r);
      f << d << ",\"" << ir::shape_string(p, static_cast<ir::domain_id>(d)) << "\"," << dom.rows << ',' << dom.level << ',' << (dom.scan >= 0 ? 1 : 0) << ','
        << (dom.scan >= 0 ? p.scans[static_cast<std::size_t>(dom.scan)].chains() : 0) << ',' << epykos::to_string(p.groups[d].steps.back().op) << ',' << p.groups[d].steps.size() << ",\""
        << reads << "\",\"" << list(gather_readers[d]) << "\",\"" << list(segment_readers[d]) << "\",\"" << list(scan_readers[d]) << "\"," << output_rows[d] << ',' << gather_refs[d] << '\n';
    }
  }
  std::cout << "[  plan    ] written stage_a_plan.txt (" << plan.size() << " bytes) and stage_a_domains.csv (" << nd << " domains) in the test's working directory\n";
  std::cout << plan;
  EXPECT_GT(plan.size(), 0u);
  // The same program planned at other lane tiles: the inliner and the exp tails are gated by
  // the lane count (row fusion pays at L = 1 or L >= 16, exec/interpreter.cpp), so the plan
  // the grid runs (lane_tile 8, the default of stage_a_program_options) differs from the B = 1
  // and the wide-batch plans; written as stage_a_plan_L<lane_tile>.txt for the coverage report.
  for (int lt : {1, 32}) {
    epykos::exec::Options o;
    o.max_batch = 64;
    o.lane_tile = lt;
    const epykos::exec::Interpreter in(p, o);
    std::ofstream f("stage_a_plan_L" + std::to_string(lt) + ".txt");
    f << in.describe();
    std::cout << "[  plan L" << lt << " ] " << in.num_fused_values() << " of " << in.num_values() << " rows fused or inlined (never materialised) at lane_tile " << lt
              << " vs " << prog.interpreter().num_fused_values() << " at lane_tile " << prog.interpreter().options().lane_tile << '\n';
  }
}
