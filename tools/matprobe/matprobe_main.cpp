// tools/matprobe — DOES ANY STRUCTURAL SIGNAL PREDICT MATERIALISE-VS-RECOMPUTE? THE TIMING HALF.
//
// `tools/matrec/` measured the structure and found the central answer is a theorem rather than a
// correlation: `ir::infer` makes a tape node a domain value exactly when it is structural, is a
// segment member or an output, or has a use count other than one
// (`src/ir/signature.cpp`'s `initial_boundaries`), so **every intra-chain step has exactly one
// reader by construction** — 100.00% of 3,029 instances on `compare_ois`(256), 32,602 on
// `stage_a`, 526,340 on `exotic_path`(1024x252). Reader count is what CONSTRUCTS the partition, so
// inside the recomputed class it is the constant 1 and predicts nothing.
//
// What DOES vary, and varies between the two ends of the sharing spectrum, is the cone's
// transcendental content: 0 of 11,087 recomputed transcendentals on `stage_a`, 0 of 81 on
// `compare_ois`, and 50.1% on the path grid. This tool asks whether that predicts the winner.
//
// ---- the method, and why it is one binary ----------------------------------------------------
//
// Both arms are `adjoint::Options::materialise_steps` on and off, on the SAME `ir::Program`, in
// the SAME process, interleaved call by call, A,B,B,A per pair. That is D105's correction of a
// measurement error made on this very subsystem: a paired-alternating-BINARIES run reported a
// −6.6% "incidental codegen effect" which collapsed to +0.2% once both arms sat behind one option
// in one build, while exposing 4–13% regressions the two-binary method had diluted. Two builds
// differ in every inlining decision the linker makes; one build with one branch does not.
//
// `--swap` is the control for the other half of that confound: the two arms are two `Adjoint`
// objects, because `Options` is fixed at construction, so they differ not only in the option but
// in where their buffers sit. The sign of the delta must not depend on which was allocated first.
//
// ---- the denominator -------------------------------------------------------------------------
//
// `Adjoint::run` is a forward pass plus a reverse pass and this change touches BOTH — it adds a
// store pass to the forward and removes arithmetic from the reverse, which is exactly what makes
// it different from `seed_pull`. So the delta is reported against the whole call, and against
//
//     A − F0   where F0 = exec::Interpreter::run with fuse_pairs, fuse_reductions and
//              inline_producers ALL OFF
//
// D103 §2's reverse-pass bound. F0 and not the shipped interpreter: D103 §5 item 1 measured the
// shipped one over-attributing the reverse by 13%, because it fuses or inlines rows that
// `adjoint_e0.cpp`'s `forward()` materialises unconditionally. The quantity is a BOUND and is
// labelled one wherever it appears — and here it is a weaker bound than it was for `seed_pull`,
// because part of this change's cost lands in the forward, which is the F0 side.
//
// ---- residency against arithmetic (D91 §3, D103 §4) -----------------------------------------
//
// `--mode evict` runs the same paired arms with a forced cache flush of 0 / 1 / 4 / 16 / 64 MB
// between consecutive calls. D91 measured ~40% of the head-to-head calibration figure as cache
// refill and reproduced it this way; D103 measured 1.15x decaying to 1.01x on the ladder. The
// question here is sharper than either: materialising TRADES ARITHMETIC FOR FOOTPRINT, so if the
// winner flips with pressure then the deciding quantity is residency and no structural signal can
// carry the decision on its own.
//
// Never run by ctest or CI; reads the 1-minute load itself and refuses above cores/2, which is
// bench/run.sh's rule (D29). No -ffp-contract=off in this TU, deliberately and like tools/ladder/
// and tools/pullprobe/: every number here is a time under the shipped release flags, and it
// compares no bits — the bits are tests/adjoint/materialise_steps_e0_test.cpp's job.
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "epykos/adjoint/adjoint.hpp"
#include "epykos/exec/interpreter.hpp"
#include "epykos/fixtures/compare_ois.hpp"
#include "epykos/fixtures/exotic_path.hpp"
#include "epykos/fixtures/spike_telescoped.hpp"
#include "epykos/fixtures/stage_a.hpp"
#include "epykos/ir/program.hpp"
#include "epykos/ir/signature.hpp"
#include "epykos/version.hpp"

namespace {

using namespace epykos;
using ir::Program;
using clock_type = std::chrono::steady_clock;

double us_since(clock_type::time_point t) {
  return std::chrono::duration<double, std::micro>(clock_type::now() - t).count();
}

double load_1min() noexcept {
#if defined(__linux__)
  std::FILE* f = std::fopen("/proc/loadavg", "r");
  if (f == nullptr) return -1.0;
  double one = -1.0;
  const int n = std::fscanf(f, "%lf", &one);
  std::fclose(f);
  return n == 1 ? one : -1.0;
#else
  double avg[3] = {-1.0, -1.0, -1.0};
  if (::getloadavg(avg, 3) < 1) return -1.0;
  return avg[0];
#endif
}

int logical_cores() noexcept {
  const unsigned h = std::thread::hardware_concurrency();
  return h == 0 ? 1 : static_cast<int>(h);
}

bool g_ignore_load = false;
double g_sink = 0.0;

void require_quiet_box(const char* what) {
  const double l = load_1min();
  const double threshold = static_cast<double>(logical_cores()) / 2.0;
  if (l > threshold) {
    if (!g_ignore_load) {
      std::printf("\nREFUSED: 1-minute load %.2f exceeds cores/2 = %.1f (%d logical cores) before '%s'.\n", l,
                  threshold, logical_cores(), what);
      std::printf("Wait for the box, or --ignore-load for a run that is NOT reported (D29).\n");
      std::exit(2);
    }
    std::printf("  WARNING: load %.2f > cores/2 = %.1f; '%s' is DIRTY and must not be reported\n", l, threshold, what);
  }
}

double median(std::vector<double> v) {
  if (v.empty()) return 0.0;
  std::sort(v.begin(), v.end());
  const std::size_t n = v.size();
  return n % 2 == 1 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

// ---- forced eviction (D91 §3, D103 §4) -------------------------------------------------------
std::vector<char> g_evict;
void evict(std::size_t bytes) {
  if (bytes == 0) return;
  if (g_evict.size() < bytes) g_evict.assign(bytes, 1);
  double acc = 0.0;
  for (std::size_t i = 0; i < bytes; i += 64) {
    g_evict[i] = static_cast<char>(g_evict[i] + 1);
    acc += static_cast<double>(g_evict[i]);
  }
  g_sink += acc;
}

// ---- the structural census, printed beside the times so the signal and the winner are in one
// place and no one has to take tools/matrec/'s numbers on trust ---------------------------------
struct Census {
  long long domains = 0;
  long long multi_step_domains = 0;
  long long intra_instances = 0;   // (steps-1) * rows: what the arm stores, per lane
  long long one_reader = 0;        // ... of which exactly one in-group reader. THE THEOREM.
  long long trans_intra = 0;       // ... of which a transcendental: the expensive recompute
  long long trans_value = 0;       // transcendentals that are a group's VALUE: never recomputed
  long long segment_intra = 0;     // intra-chain Sum / Affine: the whole fold is removed
  long long flops_intra = 0;       // arithmetic the recompute arm pays per lane
  long long flops_value = 0;
};

Census census(const Program& p) {
  Census c;
  c.domains = static_cast<long long>(p.domains.size());
  for (std::size_t d = 0; d < p.domains.size(); ++d) {
    const ir::Group& g = p.groups[d];
    const int nsteps = static_cast<int>(g.steps.size());
    const int last = nsteps - 1;
    const long long rows = p.domains[d].rows;
    if (last > 0) ++c.multi_step_domains;
    // In-group readers of each step, through SlotKind::Step operand slots.
    std::vector<long long> rc(static_cast<std::size_t>(nsteps), 0);
    for (int j = 0; j < nsteps; ++j) {
      const ir::Step& s = g.steps[static_cast<std::size_t>(j)];
      for (const ir::Slot& sl : {s.a, s.b, s.c, s.konst}) {
        if (sl.kind == ir::SlotKind::Step) rc[static_cast<std::size_t>(sl.index)] += 1;
      }
    }
    for (int k = 0; k < nsteps; ++k) {
      const ir::Step& s = g.steps[static_cast<std::size_t>(k)];
      const bool trans = s.op == Op::Exp || s.op == Op::Log || s.op == Op::Sqrt;
      const bool seg = (s.op == Op::Sum || s.op == Op::Affine) && !ir::is_fixed_sum(s);
      long long fl = 1;
      if (seg) {
        long long m = 0;
        for (int r = 0; r < p.domains[d].rows; ++r) {
          m += p.segments[static_cast<std::size_t>(s.a.index)].offsets[static_cast<std::size_t>(r) + 1] -
               p.segments[static_cast<std::size_t>(s.a.index)].offsets[static_cast<std::size_t>(r)];
        }
        fl = s.op == Op::Affine ? 2 * m : std::max<long long>(m - rows, 0);
      } else {
        fl = rows * (s.op == Op::Fma ? 2 : 1);
      }
      if (k == last) {
        if (trans) c.trans_value += rows;
        c.flops_value += fl;
      } else {
        c.intra_instances += rows;
        if (rc[static_cast<std::size_t>(k)] == 1) c.one_reader += rows;
        if (trans) c.trans_intra += rows;
        if (seg) c.segment_intra += rows;
        c.flops_intra += fl;
      }
    }
  }
  return c;
}

// ---- one fixture's paired measurement --------------------------------------------------------

struct Config {
  int batch = 8;
  int reps = 25;
  int warmup = 5;
  int trades = 256;
  int lane_tile = 8;
  int tile = 256;
  int paths = 256;
  int steps = 52;
  // The payoff legs (fixtures/exotic_path.hpp's ExoticLegs). This is the WITHIN-FIXTURE test of
  // the transcendental hypothesis: `spot` alone is the GBM recurrence with no payoff, so almost
  // every intra-chain step is the step's own `exp`; each further leg adds non-transcendental
  // intra-chain steps and DILUTES the share. If the measured delta tracks the share across the
  // combinations, the signal is the transcendental and not "the path grid".
  unsigned legs = fixtures::exotic_legs_all;
  bool swap = false;
  std::vector<int> evict_mb = {0, 1, 4, 16, 64};
};

struct Result {
  double m_off = 0, m_on = 0, m_f0 = 0, d_paired = 0;
};

Result paired(const adjoint::Adjoint& ad_off, const adjoint::Adjoint& ad_on, const exec::Interpreter& f0,
              const std::vector<double>& state, int B, const std::vector<double>& out_bar, std::vector<double>* out,
              std::vector<double>* sbar, std::vector<double>* fout, const Config& cfg, std::size_t evict_bytes) {
  std::vector<double> t_off, t_on, t_f0, dp;
  for (int r = 0; r < cfg.reps; ++r) {
    const bool off_first = (r % 2) == 0;  // A,B,B,A across a pair
    double a = 0.0, b = 0.0;
    if (off_first) {
      evict(evict_bytes);
      const auto s0 = clock_type::now();
      ad_off.run(state.data(), B, out_bar.data(), out->data(), sbar->data());
      a = us_since(s0);
      evict(evict_bytes);
      const auto s1 = clock_type::now();
      ad_on.run(state.data(), B, out_bar.data(), out->data(), sbar->data());
      b = us_since(s1);
    } else {
      evict(evict_bytes);
      const auto s1 = clock_type::now();
      ad_on.run(state.data(), B, out_bar.data(), out->data(), sbar->data());
      b = us_since(s1);
      evict(evict_bytes);
      const auto s0 = clock_type::now();
      ad_off.run(state.data(), B, out_bar.data(), out->data(), sbar->data());
      a = us_since(s0);
    }
    t_off.push_back(a);
    t_on.push_back(b);
    dp.push_back(b - a);
    evict(evict_bytes);
    const auto s2 = clock_type::now();
    f0.run(state.data(), B, fout->data());
    t_f0.push_back(us_since(s2));
  }
  Result r;
  r.m_off = median(t_off);
  r.m_on = median(t_on);
  r.m_f0 = median(t_f0);
  r.d_paired = median(dp);
  return r;
}

void measure(const char* name, const Program& p, const Config& cfg, bool do_evict) {
  const int n_in = static_cast<int>(p.inputs.size());
  const int n_out = static_cast<int>(p.outputs.size());
  const int B = cfg.batch;
  const std::size_t Bs = static_cast<std::size_t>(B);

  adjoint::Options ao;
  ao.max_batch = B;
  ao.lane_tile = cfg.lane_tile;
  ao.tile = cfg.tile;
  ao.materialise_steps = false;
  adjoint::Options ao_mat = ao;
  ao_mat.materialise_steps = true;

  exec::Options eo;
  eo.max_batch = B;
  eo.lane_tile = cfg.lane_tile;
  eo.tile = cfg.tile;
  eo.fuse_pairs = false;       // D103 §5 item 1: the only forward proxy that materialises the
  eo.fuse_reductions = false;  // same rows adjoint_e0.cpp's forward() does
  eo.inline_producers = false;

  std::unique_ptr<adjoint::Adjoint> first, second;
  if (cfg.swap) {
    first = std::make_unique<adjoint::Adjoint>(p, ao_mat);
    second = std::make_unique<adjoint::Adjoint>(p, ao);
  } else {
    first = std::make_unique<adjoint::Adjoint>(p, ao);
    second = std::make_unique<adjoint::Adjoint>(p, ao_mat);
  }
  const adjoint::Adjoint& ad_off = cfg.swap ? *second : *first;
  const adjoint::Adjoint& ad_on = cfg.swap ? *first : *second;
  const exec::Interpreter f0(p, eo);

  std::vector<double> state(static_cast<std::size_t>(n_in) * Bs);
  std::vector<double> out_bar(static_cast<std::size_t>(n_out) * Bs, 1.0);
  std::vector<double> out(static_cast<std::size_t>(n_out) * Bs, 0.0);
  std::vector<double> sbar(static_cast<std::size_t>(n_in) * Bs, 0.0);
  std::vector<double> fout(static_cast<std::size_t>(n_out) * Bs, 0.0);
  for (int k = 0; k < n_in; ++k) {
    for (std::size_t b = 0; b < Bs; ++b) {
      state[static_cast<std::size_t>(k) * Bs + b] =
          p.input_values[static_cast<std::size_t>(k)] * (1.0 + 1e-7 * static_cast<double>(b));
    }
  }

  const Census c = census(p);
  const double mb = 1024.0 * 1024.0;
  std::printf("\n== %s  (B %d, lane_tile %d, tile %d, reps %d%s)\n", name, B, cfg.lane_tile, cfg.tile, cfg.reps,
              cfg.swap ? ", SWAPPED allocation order" : "");
  std::printf("   %lld values, %lld domains (%lld multi-step), %d in, %d out\n",
              static_cast<long long>(p.num_values()), c.domains, c.multi_step_domains, n_in, n_out);
  std::printf("   THE SIGNAL, structural: intra-chain instances %lld, of which exactly ONE in-group reader %lld "
              "(%.2f%%)\n",
              c.intra_instances, c.one_reader,
              c.intra_instances ? 100.0 * double(c.one_reader) / double(c.intra_instances) : 0.0);
  std::printf("     -- a theorem, not a measurement: ir::infer's boundary rule folds a node into a chain only when "
              "its use count is 1\n");
  std::printf("   recomputed transcendentals %lld of %lld in the program (%.1f%%)   <- THE signal that differs "
              "between fixtures\n",
              c.trans_intra, c.trans_intra + c.trans_value,
              (c.trans_intra + c.trans_value) ? 100.0 * double(c.trans_intra) / double(c.trans_intra + c.trans_value)
                                              : 0.0);
  std::printf("   recomputed segment folds %lld; recompute arithmetic %lld of %lld (%.1f%% of the forward's)\n",
              c.segment_intra, c.flops_intra, c.flops_intra + c.flops_value,
              (c.flops_intra + c.flops_value) ? 100.0 * double(c.flops_intra) / double(c.flops_intra + c.flops_value)
                                              : 0.0);
  std::printf("   footprint: values %.2f MB, edges %.2f MB, scratch %.2f MB off -> %.2f MB on (+%.2f MB, %+.1f%% of "
              "values+edges)\n",
              double(ad_off.value_bytes()) / mb, double(ad_off.edge_bytes()) / mb,
              double(ad_off.scratch_bytes()) / mb, double(ad_on.scratch_bytes()) / mb,
              double(ad_on.scratch_bytes() - ad_off.scratch_bytes()) / mb,
              100.0 * double(ad_on.scratch_bytes() - ad_off.scratch_bytes()) /
                  double(ad_off.value_bytes() + ad_off.edge_bytes()));

  for (int w = 0; w < cfg.warmup; ++w) {
    ad_off.run(state.data(), B, out_bar.data(), out.data(), sbar.data());
    ad_on.run(state.data(), B, out_bar.data(), out.data(), sbar.data());
    f0.run(state.data(), B, fout.data());
  }

  require_quiet_box(name);
  const double l0 = load_1min();

  if (!do_evict) {
    const Result r = paired(ad_off, ad_on, f0, state, B, out_bar, &out, &sbar, &fout, cfg, 0);
    const double l1 = load_1min();
    const double rev = r.m_off - r.m_f0;
    std::printf("   load %.2f -> %.2f\n", l0, l1);
    std::printf("   %-32s %10.2f us\n", "A, materialise_steps OFF (D31)", r.m_off);
    std::printf("   %-32s %10.2f us\n", "A, materialise_steps ON", r.m_on);
    std::printf("   %-32s %10.2f us\n", "F0, unfused forward", r.m_f0);
    std::printf("   %-32s %10.2f us   reverse-pass BOUND (A_off - F0)\n", "A_off - F0", rev);
    std::printf("   PAIRED DELTA (median of per-rep on - off)  %+10.3f us\n", r.d_paired);
    std::printf("   as a share of the whole call               %+10.2f %%\n", 100.0 * r.d_paired / r.m_off);
    if (rev > 0.0)
      std::printf("   as a share of the reverse-pass bound       %+10.2f %%   (a WEAKER bound here than for "
                  "seed_pull: part of this change's cost is in the forward, which is F0's side)\n",
                  100.0 * r.d_paired / rev);
    std::printf("   VERDICT: %s\n", r.d_paired < 0.0 ? "MATERIALISE wins" : "RECOMPUTE wins (D31 holds)");
    return;
  }

  // --mode evict: the same paired arms under pressure. If the winner FLIPS, the deciding quantity
  // is residency and no structural signal decides it on its own.
  std::printf("   load %.2f (eviction sweep; D91 section 3's technique)\n", l0);
  std::printf("   %-10s %12s %12s %12s %10s %10s\n", "evict MB", "off us", "on us", "paired us", "pct call",
              "verdict");
  for (const int m : cfg.evict_mb) {
    const std::size_t bytes = static_cast<std::size_t>(m) * 1024u * 1024u;
    const Result r = paired(ad_off, ad_on, f0, state, B, out_bar, &out, &sbar, &fout, cfg, bytes);
    std::printf("   %-10d %12.2f %12.2f %+12.3f %+10.2f %10s\n", m, r.m_off, r.m_on, r.d_paired,
                100.0 * r.d_paired / r.m_off, r.d_paired < 0.0 ? "MAT" : "RECOMP");
  }
  std::printf("   load after %.2f\n", load_1min());
}

// ---- the fixture table ------------------------------------------------------------------------

struct Entry {
  const char* name;
  std::function<Program(const Config&)> build;
};

const std::vector<Entry>& fixture_table() {
  static const std::vector<Entry> kAll = {
      // HIGH SHARING: the rates book.
      {"compare_ois",
       [](const Config& cfg) {
         fixtures::CompareOisOptions o;
         o.trades = cfg.trades;
         Tape t = fixtures::spike::record_compare_ois_spike(fixtures::make_compare_ois(o),
                                                            fixtures::spike::Form::telescoped, false)
                      .tape;
         compile(t);
         return ir::infer(t);
       }},
      {"stage_a",
       [](const Config&) {
         Tape t = fixtures::record_stage_a(fixtures::make_stage_a(), false).tape;
         compile(t);
         return ir::infer(t);
       }},
      // NO SHARING: D106's path grid.
      {"exotic_path",
       [](const Config& cfg) {
         Tape t = fixtures::record_exotic_path(
             fixtures::make_exotic_path(cfg.paths, cfg.steps, 20260922, cfg.legs), false, false);
         compile(t);
         return ir::infer(t);
       }},
  };
  return kAll;
}

[[noreturn]] void die(const std::string& m) {
  std::fprintf(stderr, "matprobe: %s\n", m.c_str());
  std::exit(2);
}

}  // namespace

int main(int argc, char** argv) {
  Config cfg;
  std::string which = "all", mode = "paired";
  std::vector<int> batches;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> std::string {
      if (i + 1 >= argc) die("missing value for " + a);
      return argv[++i];
    };
    if (a == "--mode") mode = next();
    else if (a == "--batch") batches.push_back(std::atoi(next().c_str()));
    else if (a == "--reps") cfg.reps = std::atoi(next().c_str());
    else if (a == "--trades") cfg.trades = std::atoi(next().c_str());
    else if (a == "--paths") cfg.paths = std::atoi(next().c_str());
    else if (a == "--steps") cfg.steps = std::atoi(next().c_str());
    else if (a == "--legs") {
      const std::string v = next();
      if (v == "spot") cfg.legs = fixtures::exotic_leg_spot;
      else if (v == "arith") cfg.legs = fixtures::exotic_leg_arith;
      else if (v == "geom") cfg.legs = fixtures::exotic_leg_geom;
      else if (v == "barrier") cfg.legs = fixtures::exotic_leg_barrier;
      else if (v == "all") cfg.legs = fixtures::exotic_legs_all;
      else die("--legs must be spot, arith, geom, barrier or all");
    }
    else if (a == "--lane-tile") cfg.lane_tile = std::atoi(next().c_str());
    else if (a == "--tile") cfg.tile = std::atoi(next().c_str());
    else if (a == "--swap") cfg.swap = true;
    else if (a == "--ignore-load") g_ignore_load = true;
    else if (a == "--list") {
      for (const Entry& e : fixture_table()) std::printf("%s\n", e.name);
      return 0;
    } else which = a;
  }
  if (batches.empty()) batches = {1, 8, 64};
  if (mode != "paired" && mode != "evict") die("--mode must be paired or evict");

  std::printf("matprobe -- materialise_steps, both arms in ONE process, interleaved (D105's pattern)\n");
  std::printf("build: %s\nflags: %s\n", epykos::version(), epykos::build_flags());
  std::printf("mode %s, %d logical cores, load threshold %.1f\n", mode.c_str(), logical_cores(),
              double(logical_cores()) / 2.0);

  bool any = false;
  for (const Entry& e : fixture_table()) {
    const std::string n = e.name;
    if (!(which == "all" || n == which)) continue;
    any = true;
    const Program p = e.build(cfg);
    for (const int B : batches) {
      Config c = cfg;
      c.batch = B;
      measure(e.name, p, c, mode == "evict");
    }
  }
  if (!any) die("no fixture matches \"" + which + "\" (try --list, or all)");
  return 0;
}
