// EpykosEngine — tools/costmodel/collect_main.cpp (M4/CM, docs/PROBLEM.md §7).
//
// Builds ONE (case, B, tile, lane_tile) interpreter and runs it `--reps` times so
// exec::Interpreter's per-domain profile table (compiled in only under the `profile` preset,
// EPYKOS_EXEC_PROFILE=ON) accumulates enough samples that this process's own one-time
// construction cost (Stage A's record-time solves and dependency discovery run their own,
// smaller interpreters, which share the same process-global profile table) is a small fraction
// of the total. Prints its report to stderr at normal process exit (the profile table is a
// process-lifetime global, D45's earlier "each (B, lane_tile) its own filtered process" made
// reusable); tools/costmodel/calibrate.py captures it into bench/results/<fingerprint>/costmodel_raw/.
//
// A plain (non-profile) build runs this tool too — it just has nothing to say (no #ifdef code
// runs), which is a cheap way to check a config parses and the program builds before spending a
// profile-preset rebuild on it.
//
// D63: `--fuse-pairs 0` runs the SAME program with exec::Options::fuse_pairs off (one kernel per
// step, no chain tails). That contrast is the only thing in the grid that varies step pairing
// while holding rows, lanes and the op mix fixed, and it is what makes `dispatch_ns` and the
// byte ladder identifiable against the per-op rates at all: every other feature the model has is
// proportional to rows x lanes inside a domain, so before this point the fit could not tell an
// op's cost from the scratch traffic around it (byte_ns[L1] fitted to 2.8e-4 ns/byte, i.e. zero).
//
// `--fuse-reductions 0` and `--inline-producers 0` are the same contrast for the interpreter's
// other two planning knobs. They are NOT in calibrate.py's grid (they change a domain's
// TREATMENT, which fit_main's plan construction would have to mirror before such a capture could
// enter a fit); they are here because the D63 pairing contrast immediately raised the question of
// how much the planner's OTHER decisions are worth on this fingerprint, and that is a question
// one process per knob answers directly. See D63's "next binding constraint".
//
// Usage: costmodel_collect --case m1|stage_a --B N --tile N --lane-tile N [--reps N] [--trades N]
//                          [--fuse-pairs 0|1] [--fuse-reductions 0|1] [--inline-producers 0|1]
#include <algorithm>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <vector>

#include "epykos/exec/interpreter.hpp"
#include "epykos/fixtures/m1_book.hpp"
#include "epykos/fixtures/record_m1.hpp"
#include "epykos/fixtures/stage_a.hpp"
#include "epykos/ir/program.hpp"
#include "epykos/ir/signature.hpp"
#include "epykos/solver/implicit_program.hpp"
#include "epykos/tape/tape.hpp"

namespace {

namespace ir = epykos::ir;
namespace fixtures = epykos::fixtures;
namespace exec = epykos::exec;
namespace solver = epykos::solver;

struct Args {
  std::string case_name;
  int B = 1;
  int tile = 256;
  int lane_tile = 8;
  long reps = 0;    // 0 -> a case-specific default
  int trades = -1;  // stage_a only; -1 = the blueprint's own count
  bool fuse_pairs = true;         // D63: exec::Options::fuse_pairs
  bool fuse_reductions = true;    // D63: exec::Options::fuse_reductions
  bool inline_producers = true;   // D63: exec::Options::inline_producers
};

// D63: the lane-chunk width the interpreter will actually PLAN with, which is not `--lane-tile`.
// `exec::Interpreter`'s constructor computes `Lt = min(options.lane_tile, options.max_batch)` and
// hands THAT to `rewrite::planner::default_plan` — so at B=1 a `--lane-tile 8` run plans at Lt=1,
// where `row_fusion_pays` is true and the planner inlines, while `--lane-tile 8` on its own says
// it is false and nothing inlines. Every capture records it so `costmodel_fit` builds its feature
// matrix for the plan the interpreter ran, not for the one its command line suggests.
int effective_lane_tile(const exec::Options& o) noexcept { return std::min(o.lane_tile, o.max_batch); }

[[noreturn]] void usage_error(const std::string& msg) {
  std::cerr << "costmodel_collect: " << msg << "\n"
            << "usage: costmodel_collect --case m1|stage_a --B N --tile N --lane-tile N [--reps N] [--trades N] [--fuse-pairs 0|1] [--fuse-reductions 0|1] [--inline-producers 0|1]\n";
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
    if (arg == "--case") a.case_name = next("--case");
    else if (arg == "--B") a.B = std::stoi(next("--B"));
    else if (arg == "--tile") a.tile = std::stoi(next("--tile"));
    else if (arg == "--lane-tile") a.lane_tile = std::stoi(next("--lane-tile"));
    else if (arg == "--reps") a.reps = std::stol(next("--reps"));
    else if (arg == "--trades") a.trades = std::stoi(next("--trades"));
    else if (arg == "--fuse-pairs") a.fuse_pairs = std::stoi(next("--fuse-pairs")) != 0;
    else if (arg == "--fuse-reductions") a.fuse_reductions = std::stoi(next("--fuse-reductions")) != 0;
    else if (arg == "--inline-producers") a.inline_producers = std::stoi(next("--inline-producers")) != 0;
    else usage_error("unknown argument '" + arg + "'");
  }
  if (a.case_name != "m1" && a.case_name != "stage_a") usage_error("--case must be m1 or stage_a");
  if (a.B < 1 || a.tile < 1 || a.lane_tile < 1) usage_error("--B, --tile, --lane-tile must be >= 1");
  return a;
}

void run_m1(const Args& a) {
  const fixtures::Book book = fixtures::make_m1_book();
  const epykos::Tape tape = fixtures::record_m1(book);
  const ir::Program program = ir::infer(tape);
  exec::Options opt;
  opt.tile = a.tile;
  opt.lane_tile = a.lane_tile;
  opt.max_batch = a.B;
  opt.fuse_pairs = a.fuse_pairs;
  opt.fuse_reductions = a.fuse_reductions;
  opt.inline_producers = a.inline_producers;
  const exec::Interpreter interp(program, opt);
  const int lt = effective_lane_tile(opt);
  const std::vector<double> all = tape.input_values();
  std::vector<double> state(all.size() * static_cast<std::size_t>(a.B));
  for (std::size_t k = 0; k < all.size(); ++k) {
    for (int b = 0; b < a.B; ++b) state[k * static_cast<std::size_t>(a.B) + static_cast<std::size_t>(b)] = all[k];
  }
  std::vector<double> out(static_cast<std::size_t>(program.outputs.size()) * static_cast<std::size_t>(a.B));
  const long reps = a.reps > 0 ? a.reps : 200000;
  std::cerr << "costmodel_collect: case=m1 B=" << a.B << " tile=" << a.tile << " lane_tile=" << a.lane_tile
            << " Lt=" << lt << " fuse_pairs=" << (a.fuse_pairs ? 1 : 0) << " fuse_reductions=" << (a.fuse_reductions ? 1 : 0)
            << " inline_producers=" << (a.inline_producers ? 1 : 0) << " domains=" << program.domains.size() << " reps=" << reps << "\n";
  for (long r = 0; r < reps; ++r) interp.run(state.data(), a.B, out.data());
}

void run_stage_a(const Args& a) {
  fixtures::StageAOptions so;
  so.trades = a.trades;
  const fixtures::StageA s = fixtures::make_stage_a(so);
  const fixtures::StageATape t = fixtures::record_stage_a(s);
  solver::ProgramOptions po = fixtures::stage_a_program_options(s, std::max(64, a.B));
  po.interpreter.tile = a.tile;
  po.interpreter.lane_tile = a.lane_tile;
  po.interpreter.fuse_pairs = a.fuse_pairs;
  po.interpreter.fuse_reductions = a.fuse_reductions;
  po.interpreter.inline_producers = a.inline_producers;
  const int lt = effective_lane_tile(po.interpreter);
  solver::ImplicitProgram prog(t.tape, t.registry, po);
  const std::vector<double> all = t.tape.input_values();  // the record point: quotes, solved knots, diagnostics
  const std::size_t n_in = all.size();
  const std::size_t n_out = static_cast<std::size_t>(prog.n_outputs());
  std::vector<double> state(n_in * static_cast<std::size_t>(a.B));
  for (std::size_t k = 0; k < n_in; ++k) {
    for (int b = 0; b < a.B; ++b) state[k * static_cast<std::size_t>(a.B) + static_cast<std::size_t>(b)] = all[k];
  }
  std::vector<double> out(n_out * static_cast<std::size_t>(a.B));
  const long reps = a.reps > 0 ? a.reps : 300;
  std::cerr << "costmodel_collect: case=stage_a B=" << a.B << " tile=" << a.tile << " lane_tile=" << a.lane_tile
            << " Lt=" << lt << " fuse_pairs=" << (a.fuse_pairs ? 1 : 0) << " fuse_reductions=" << (a.fuse_reductions ? 1 : 0)
            << " inline_producers=" << (a.inline_producers ? 1 : 0) << " domains=" << prog.program().domains.size() << " reps=" << reps << "\n";
  for (long r = 0; r < reps; ++r) prog.interpreter().run(state.data(), a.B, out.data());
}

}  // namespace

int main(int argc, char** argv) {
  const Args a = parse_args(argc, argv);
  try {
    if (a.case_name == "m1") {
      run_m1(a);
    } else {
      run_stage_a(a);
    }
  } catch (const std::exception& e) {
    std::cerr << "costmodel_collect: " << e.what() << "\n";
    return 1;
  }
  return 0;
}
