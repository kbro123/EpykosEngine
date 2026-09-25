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
// D68: two additions that make this tool a WALL-CLOCK instrument as well as a profile-capture
// one, so the plan-level ceiling can be measured in a `release` build rather than inferred from
// the `profile` build's instrumented per-domain sums.
//
//   * Every run now times its own rep loop with a steady_clock around it and prints
//     `wall_us_per_rep` / `wall_us_total` plus the 1-minute load before and after (D9: state the
//     load alongside the number). That line is printed in EVERY preset -- it is not inside the
//     EPYKOS_EXEC_PROFILE #ifdef -- so the knob-off contrast can be taken on the uninstrumented
//     release binary, where a domain's own ProfileScope pair of steady_clock::now() calls is not
//     part of what is being compared.
//   * `--mode adjoint` runs the same program through `adjoint::Adjoint` instead of
//     `exec::Interpreter`. That is the O3 risk-ladder engine. Note what the flag combination then
//     means, because it is the whole point of offering it: `adjoint::Options` has NO fuse_pairs /
//     fuse_reductions / inline_producers (adjoint/adjoint.hpp: "the forward pass below
//     materialises every domain's rows unconditionally ... Adjoint applies none of the
//     fuse/inline optimisations of its own"), so --mode adjoint accepts the three knobs and is
//     BY CONSTRUCTION unaffected by them. The measurement exists to put a number on that
//     structural fact rather than to leave it as an argument from reading the header.
//
// Usage: costmodel_collect --case m1|stage_a --B N --tile N --lane-tile N [--reps N] [--trades N]
//                          [--mode forward|adjoint]
//                          [--fuse-pairs 0|1] [--fuse-reductions 0|1] [--inline-producers 0|1]
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <exception>
#include <stdexcept>
#include <iostream>
#include <memory>
#include <stdlib.h>  // getloadavg (POSIX/BSD; not in <cstdlib>'s guaranteed surface)
#include <string>
#include <vector>

#include "epykos/adjoint/adjoint.hpp"
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
namespace adjoint = epykos::adjoint;
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
  bool adjoint_mode = false;      // D68: --mode adjoint -> adjoint::Adjoint, the O3 ladder's engine
  int ceiling_rounds = 0;         // D68: --ceiling R -> the interleaved knob sweep, R rounds (0 = off)
};

// D68: the 1-minute load average, the quantity bench/run.sh gates on (D29) and the one
// tools/costmodel/calibrate.py already records per capture. Printed before and after the timed
// loop so a reader can see whether the machine moved under the measurement.
double load_1min() noexcept {
  double avg[3] = {-1.0, -1.0, -1.0};
  if (::getloadavg(avg, 3) < 1) return -1.0;
  return avg[0];
}

// D68: one timed rep loop, reported identically for every (case, mode) so the four knob settings
// of a ceiling sweep are directly comparable. `body` runs one repetition.
template <typename F>
void timed(const char* header, long reps, F&& body) {
  const double load_before = load_1min();
  const std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
  for (long r = 0; r < reps; ++r) body();
  const std::chrono::steady_clock::time_point t1 = std::chrono::steady_clock::now();
  const double load_after = load_1min();
  const double total_us = std::chrono::duration<double, std::micro>(t1 - t0).count();
  std::fprintf(stderr, "costmodel_collect: %s wall_us_total=%.3f wall_us_per_rep=%.4f load_1min_before=%.2f load_1min_after=%.2f\n",
               header, total_us, total_us / static_cast<double>(reps), load_before, load_after);
}

// D63: the lane-chunk width the interpreter will actually PLAN with, which is not `--lane-tile`.
// `exec::Interpreter`'s constructor computes `Lt = min(options.lane_tile, options.max_batch)` and
// hands THAT to `rewrite::planner::default_plan` — so at B=1 a `--lane-tile 8` run plans at Lt=1,
// where `row_fusion_pays` is true and the planner inlines, while `--lane-tile 8` on its own says
// it is false and nothing inlines. Every capture records it so `costmodel_fit` builds its feature
// matrix for the plan the interpreter ran, not for the one its command line suggests.
int effective_lane_tile(const exec::Options& o) noexcept { return std::min(o.lane_tile, o.max_batch); }

[[noreturn]] void usage_error(const std::string& msg) {
  std::cerr << "costmodel_collect: " << msg << "\n"
            << "usage: costmodel_collect --case m1|stage_a --B N --tile N --lane-tile N [--reps N] [--trades N] [--mode forward|adjoint] [--fuse-pairs 0|1] [--fuse-reductions 0|1] [--inline-producers 0|1]\n";
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
    else if (arg == "--mode") {
      const std::string m = next("--mode");
      if (m == "adjoint") a.adjoint_mode = true;
      else if (m == "forward") a.adjoint_mode = false;
      else usage_error("--mode must be forward or adjoint");
    }
    else if (arg == "--ceiling") a.ceiling_rounds = std::stoi(next("--ceiling"));
    else usage_error("unknown argument '" + arg + "'");
  }
  if (a.case_name != "m1" && a.case_name != "stage_a") usage_error("--case must be m1 or stage_a");
  if (a.B < 1 || a.tile < 1 || a.lane_tile < 1) usage_error("--B, --tile, --lane-tile must be >= 1");
  if (a.ceiling_rounds < 0) usage_error("--ceiling must be >= 0");
  if (a.ceiling_rounds > 0 && a.reps <= 0) usage_error("--ceiling needs an explicit --reps (the sweep must not silently change its own loop length between configurations)");
  return a;
}

// ---- D68: the plan-level ceiling sweep ------------------------------------------------------
//
// One process, one fixture, all four knob settings interleaved with their own baseline, so a
// contrast is never taken across a fixture construction or (on Stage A) across the ~seconds of
// recording and record-time solving that a per-process sweep would put between its two halves.
// D63 §2(f) learned the cross-process version of this lesson the hard way: ~12 minutes of load
// drift between the on-points and the off-points landed straight on the contrast, and made Stage
// A look 0.65x FASTER with pairing off, uniformly, including in the output copy no interpreter
// option can touch. Here every "off" run is separated from its "on" twin by one Interpreter
// construction.
//
// The `all_three_off` row is the one D63 did not have: it is the whole plan-level ceiling for a
// configuration, i.e. what the interpreter's ENTIRE planning stage is worth against materialising
// every domain and dispatching one kernel per step. A cost-model-driven search that re-plans can
// win at most that much by re-planning.
struct KnobSetting {
  const char* name;
  bool fuse_pairs;
  bool fuse_reductions;
  bool inline_producers;
};

// Alternating: every contrast is immediately preceded by its own baseline.
const KnobSetting kCeilingSettings[] = {
    {"all_on", true, true, true},
    {"no_fuse_pairs", false, true, true},
    {"all_on", true, true, true},
    {"no_fuse_reductions", true, false, true},
    {"all_on", true, true, true},
    {"no_inline_producers", true, true, false},
    {"all_on", true, true, true},
    {"all_three_off", false, false, false},
};

void ceiling_sweep(const Args& a, const ir::Program& program, const std::vector<double>& state, int planner_max_batch) {
  const std::size_t Bs = static_cast<std::size_t>(a.B);
  const std::size_t n_out = program.outputs.size();
  std::vector<double> out(n_out * Bs, 0.0);
  const long reps = a.reps;

  // The adjoint side builds ONE engine and times it under every label. adjoint::Options has no
  // fuse/inline knob (adjoint/adjoint.hpp), so the eight rows are the same object doing the same
  // work eight times: their spread is this harness's own noise floor at this configuration, and
  // it is the number the forward side's ratios have to clear to mean anything.
  std::unique_ptr<adjoint::Adjoint> ad;
  std::vector<double> out_bar, state_bar;
  if (a.adjoint_mode) {
    adjoint::Options ao;
    ao.tile = a.tile;
    ao.lane_tile = a.lane_tile;
    ao.max_batch = a.B;
    ad = std::make_unique<adjoint::Adjoint>(program, ao);
    if (static_cast<std::size_t>(ad->n_outputs()) != n_out ||
        static_cast<std::size_t>(ad->n_inputs()) * Bs != state.size()) {
      throw std::runtime_error("costmodel_collect --ceiling --mode adjoint: buffer lengths disagree with the Adjoint");
    }
    out_bar.assign(n_out * Bs, 0.0);
    for (int b = 0; b < a.B; ++b) out_bar[static_cast<std::size_t>(b)] = 1.0;  // output 0 per lane
    state_bar.assign(static_cast<std::size_t>(ad->n_inputs()) * Bs, 0.0);
  }

  for (int round = 1; round <= a.ceiling_rounds; ++round) {
    for (const KnobSetting& k : kCeilingSettings) {
      exec::Options opt;
      opt.tile = a.tile;
      opt.lane_tile = a.lane_tile;
      opt.max_batch = planner_max_batch;
      opt.fuse_pairs = k.fuse_pairs;
      opt.fuse_reductions = k.fuse_reductions;
      opt.inline_producers = k.inline_producers;
      const std::string header = std::string("CEILING case=") + a.case_name + " mode=" + (a.adjoint_mode ? "adjoint" : "forward") +
                                 " B=" + std::to_string(a.B) + " tile=" + std::to_string(a.tile) +
                                 " lane_tile=" + std::to_string(a.lane_tile) + " Lt=" + std::to_string(effective_lane_tile(opt)) +
                                 " round=" + std::to_string(round) + " setting=" + k.name + " reps=" + std::to_string(reps);
      if (a.adjoint_mode) {
        timed(header.c_str(), reps, [&] { ad->run(state.data(), a.B, out_bar.data(), out.data(), state_bar.data()); });
      } else {
        const exec::Interpreter interp(program, opt);
        timed(header.c_str(), reps, [&] { interp.run(state.data(), a.B, out.data()); });
      }
    }
  }
}

void run_m1(const Args& a) {
  const fixtures::Book book = fixtures::make_m1_book();
  const epykos::Tape tape = fixtures::record_m1(book);
  const ir::Program program = ir::infer(tape);
  if (a.ceiling_rounds > 0) {
    const std::vector<double> all0 = tape.input_values();
    std::vector<double> st(all0.size() * static_cast<std::size_t>(a.B));
    for (std::size_t k = 0; k < all0.size(); ++k) {
      for (int b = 0; b < a.B; ++b) st[k * static_cast<std::size_t>(a.B) + static_cast<std::size_t>(b)] = all0[k];
    }
    ceiling_sweep(a, program, st, a.B);
    return;
  }
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
  const std::string header = std::string("case=m1 mode=") + (a.adjoint_mode ? "adjoint" : "forward") +
                             " B=" + std::to_string(a.B) + " tile=" + std::to_string(a.tile) +
                             " lane_tile=" + std::to_string(a.lane_tile) + " Lt=" + std::to_string(lt) +
                             " fuse_pairs=" + (a.fuse_pairs ? "1" : "0") + " fuse_reductions=" + (a.fuse_reductions ? "1" : "0") +
                             " inline_producers=" + (a.inline_producers ? "1" : "0") +
                             " domains=" + std::to_string(program.domains.size()) + " reps=" + std::to_string(reps);
  std::cerr << "costmodel_collect: " << header << "\n";
  if (!a.adjoint_mode) {
    timed(header.c_str(), reps, [&] { interp.run(state.data(), a.B, out.data()); });
    return;
  }
  // D68: the adjoint engine on the same program. adjoint::Options carries no fuse/inline knob --
  // the three flags above are recorded in the header and cannot reach this path.
  adjoint::Options ao;
  ao.tile = a.tile;
  ao.lane_tile = a.lane_tile;
  ao.max_batch = a.B;
  const adjoint::Adjoint ad(program, ao);
  const std::size_t n_out = static_cast<std::size_t>(ad.n_outputs());
  std::vector<double> out_bar(n_out * static_cast<std::size_t>(a.B), 0.0);
  for (int b = 0; b < a.B; ++b) out_bar[static_cast<std::size_t>(b)] = 1.0;  // output 0 (the book PV) per lane
  std::vector<double> state_bar(static_cast<std::size_t>(ad.n_inputs()) * static_cast<std::size_t>(a.B), 0.0);
  timed(header.c_str(), reps, [&] { ad.run(state.data(), a.B, out_bar.data(), out.data(), state_bar.data()); });
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
  if (a.ceiling_rounds > 0) {
    // The planner's max_batch on Stage A is the ImplicitProgram's, not `--B`: stage_a_program_options
    // is built at max(64, B), so Lt = min(lane_tile, that) -- which is why Stage A's inlining row
    // is a genuine Lt=8 measurement where the M1 book's B=1 row is an Lt=1 one (D63 (d2)).
    ceiling_sweep(a, prog.program(), state, po.interpreter.max_batch);
    return;
  }
  std::vector<double> out(n_out * static_cast<std::size_t>(a.B));
  const long reps = a.reps > 0 ? a.reps : 300;
  const std::string header = std::string("case=stage_a mode=") + (a.adjoint_mode ? "adjoint" : "forward") +
                             " B=" + std::to_string(a.B) + " tile=" + std::to_string(a.tile) +
                             " lane_tile=" + std::to_string(a.lane_tile) + " Lt=" + std::to_string(lt) +
                             " fuse_pairs=" + (a.fuse_pairs ? "1" : "0") + " fuse_reductions=" + (a.fuse_reductions ? "1" : "0") +
                             " inline_producers=" + (a.inline_producers ? "1" : "0") +
                             " domains=" + std::to_string(prog.program().domains.size()) + " reps=" + std::to_string(reps);
  std::cerr << "costmodel_collect: " << header << "\n";
  if (!a.adjoint_mode) {
    timed(header.c_str(), reps, [&] { prog.interpreter().run(state.data(), a.B, out.data()); });
    return;
  }
  // D68: the O3 risk ladder's own engine, on the SAME program and the SAME record-point state as
  // the forward row above, so the two are directly comparable. solver::ImplicitProgram::adjoint
  // wraps this with the block solves and the IFT pull; neither reaches exec::Interpreter (it
  // calls im.adj->run, never im.interp->run), which is why a ladder measurement and this one
  // agree that the three knobs cannot touch the reverse path.
  adjoint::Options ao;
  ao.tile = a.tile;
  ao.lane_tile = a.lane_tile;
  ao.max_batch = a.B;
  const adjoint::Adjoint ad(prog.program(), ao);
  // D65's lesson, applied rather than re-learned: every buffer length comes from the engine, and
  // the shared ones are checked against it instead of assumed.
  if (static_cast<std::size_t>(ad.n_outputs()) != n_out || static_cast<std::size_t>(ad.n_inputs()) != n_in) {
    throw std::runtime_error("costmodel_collect --mode adjoint: the Adjoint's input/output counts do not match the ImplicitProgram's");
  }
  std::vector<double> out_bar(n_out * static_cast<std::size_t>(a.B), 0.0);
  for (int b = 0; b < a.B; ++b) out_bar[static_cast<std::size_t>(b)] = 1.0;  // output 0 per lane
  std::vector<double> state_bar(static_cast<std::size_t>(ad.n_inputs()) * static_cast<std::size_t>(a.B), 0.0);
  timed(header.c_str(), reps, [&] { ad.run(state.data(), a.B, out_bar.data(), out.data(), state_bar.data()); });
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
