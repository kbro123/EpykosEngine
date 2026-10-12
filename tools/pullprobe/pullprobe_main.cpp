// tools/pullprobe/ — what seeding the reverse pass's accumulator is worth, paired, in one process.
//
// D102 §4 item 1 priced `pull()`'s zero start at 165,245 of 627,968 reverse-only operations on
// Stage A (26%) and said in its last line that only a paired before/after on a reserved box
// settles it. D103 §3 then showed why the op count is a CEILING and not a forecast: on the same
// program the reverse pass is 2.74x the forward in arithmetic and 5.20x in time, so a count
// converts to a time at no fixed rate in either direction. This tool supplies the time.
//
// WHAT IT FOUND, so the reader is not left to re-derive it (fingerprint d448afd70180, release
// flags, load 2.4-3.0, 31 reps, both allocation orders): at the shipped `lane_tile = 8` seeding is
// SLOWER — `stage_a` +10.0% to +10.6% of the whole call at B = 1, `compare_ois`(256) +12.5% to
// +13.2%, and +1.3% to +4.2% at B = 8. The op count did not keep its SIGN, let alone its
// magnitude. The reason is in the shape of the two costs and the `--lane-tile` sweep shows it: the
// zero fill saved is L stores per row, while finding the first contribution costs one four-way
// branch and eight CSR offset loads per row whatever L is, so the net scales as 1/L — as a share
// of the reverse-pass bound at B = 64, `compare_ois` runs +20.8% at lane_tile 1, +5.4% at 8, ~0 at
// 16, -9.6% at 32 and ~0 at 64, and `stage_a` +1.4% / -1.4% / -7.8% / +0.2% at 8 / 16 / 32 / 64.
// `adjoint::Options::seed_pull` is therefore off by default and switching it on is a `lane_tile`
// decision. One caution on reading the B = 8 rows of a `--lane-tile` sweep: `Adjoint` sizes its
// buffers for `min(lane_tile, max_batch)`, so at B = 8 every lane_tile >= 8 is the SAME
// configuration and the spread between those rows is this tool's own run-to-run noise (+0.15% to
// +4.23% on `stage_a`, measured) rather than an effect.
//
// ---- the method, and why it is one binary ----------------------------------------------------
//
// Both arms are `adjoint::Options::seed_pull` on and off, on the SAME `ir::Program`, in the SAME
// process, interleaved call by call. That is deliberate and it is the correction of a measurement
// error made on this subsystem the day before: a paired-alternating-BINARIES run reported −6.6% at
// B = 1 and labelled it "an incidental codegen effect", and re-measuring with both arms behind one
// option in one binary sent that effect to +0.2% while exposing 4–13% regressions the two-binary
// method had diluted into the spread. Two builds differ in every inlining decision the linker
// makes; one build with one branch does not.
//
// The arms alternate A,B,B,A per pair of reps (a "bounce"), so a monotone drift in the box — a
// thermal ramp, a background job starting — lands on both arms equally instead of on whichever ran
// second. Each printed figure is a median over `--reps` samples of each arm and the delta is the
// median of the PAIRED per-rep differences, which is not the difference of the medians and is the
// more conservative of the two.
//
// ---- the denominator -------------------------------------------------------------------------
//
// `Adjoint::run` is a forward pass plus a reverse pass and this change touches only the reverse.
// So the tool reports the delta twice: against the whole call, and against
//
//     A − F0   where F0 = exec::Interpreter::run with fuse_pairs, fuse_reductions and
//              inline_producers ALL OFF
//
// which is D103 §2's reverse-pass bound. F0 and not the shipped interpreter: D103 §5 item 1
// measured the shipped one over-attributing the reverse by 13%, because it fuses or inlines rows
// that `adjoint_e0.cpp`'s `forward()` materialises unconditionally, and with the three switches off
// it materialises the same rows the adjoint's forward does. The quantity is still a BOUND — the
// two forward passes are different code — and is labelled one wherever it appears.
//
// ---- the op-count census ---------------------------------------------------------------------
//
// Printed beside the times, from `AdjointPlan`'s four CSR reader lists, so the ceiling and the
// measurement are in one place and the reader need not take D102's numbers on trust: non-empty
// pulls (one leading `+ 0.0` each, which is what the arithmetic saving is), single-reader pulls,
// single-reader-at-unit-coefficient pulls (D102's 79.4% on Stage A, for which the whole pull is
// one copy pass), and the lane-element stores the zero fill costs per `run` at the given B.
//
// Never run by ctest or CI; reads the 1-minute load itself and refuses above cores/2, which is
// bench/run.sh's rule (D29). No -ffp-contract=off in this TU, deliberately and like tools/ladder/:
// every number here is a time under the shipped release flags, and it compares no bits — the bits
// are tests/adjoint/seed_pull_e0_test.cpp's job.
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

// ---- the census ------------------------------------------------------------------------------

struct Census {
  long long values = 0;
  long long pulls_nonempty = 0;  // one leading `+ 0.0` each: the arithmetic the seeding removes
  long long pulls_empty = 0;     // no readers at all: the zero fill stays, it IS the answer
  long long single = 0;          // exactly one contribution
  long long single_unit = 0;     // exactly one contribution, an Affine at coefficient 1.0 or a
                                 // non-Affine reader (which carries no coefficient)
  long long readers = 0;         // total contributions over every value
  long long input_rows = 0;      // the carve-out's rows: these keep the zero start
};

Census census(const adjoint::AdjointPlan& plan, const Program& p) {
  Census c;
  c.values = static_cast<long long>(plan.num_values);
  for (std::size_t d = 0; d < p.domains.size(); ++d) {
    if (plan.domains[d].is_input) c.input_rows += p.domains[d].rows;
  }
  for (std::size_t v = 0; v < plan.num_values; ++v) {
    const std::int32_t n_out = plan.output_offsets[v + 1] - plan.output_offsets[v];
    const std::int32_t n_gat = plan.gather_offsets[v + 1] - plan.gather_offsets[v];
    const std::int32_t n_sum = plan.sum_offsets[v + 1] - plan.sum_offsets[v];
    const std::int32_t a0 = plan.affine_offsets[v], a1 = plan.affine_offsets[v + 1];
    const std::int32_t n = n_out + n_gat + n_sum + (a1 - a0);
    c.readers += n;
    if (n == 0) {
      ++c.pulls_empty;
      continue;
    }
    ++c.pulls_nonempty;
    if (n != 1) continue;
    ++c.single;
    if (a1 == a0 || plan.affine_coefs[static_cast<std::size_t>(a0)] == 1.0) ++c.single_unit;
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
  // Construct the seed_pull = true object FIRST. The two arms are two `Adjoint` objects because
  // `Options` is fixed at construction, and each owns its own value / adjoint buffers — so the
  // arms differ not only in the option but in where their buffers sit. That is the two-binary
  // confound wearing a different hat, and `--swap` is the control: run the measurement both ways
  // and the sign of the delta must not depend on which object was allocated first.
  bool swap = false;
};

void measure(const char* name, const Program& p, const Config& cfg) {
  const int n_in = static_cast<int>(p.inputs.size());
  const int n_out = static_cast<int>(p.outputs.size());
  const int B = cfg.batch;
  const std::size_t Bs = static_cast<std::size_t>(B);

  adjoint::Options ao;
  ao.max_batch = B;
  ao.lane_tile = cfg.lane_tile;
  ao.tile = cfg.tile;
  ao.seed_pull = false;
  adjoint::Options ao_seed = ao;
  ao_seed.seed_pull = true;

  exec::Options eo;
  eo.max_batch = B;
  eo.lane_tile = cfg.lane_tile;
  eo.tile = cfg.tile;
  eo.fuse_pairs = false;        // D103 §5 item 1: the only forward proxy that materialises the
  eo.fuse_reductions = false;   // same rows adjoint_e0.cpp's forward() does
  eo.inline_producers = false;

  // Allocation order is the thing `--swap` controls, so build them in the requested order and
  // then refer to them by arm.
  std::unique_ptr<adjoint::Adjoint> first, second;
  if (cfg.swap) {
    first = std::make_unique<adjoint::Adjoint>(p, ao_seed);
    second = std::make_unique<adjoint::Adjoint>(p, ao);
  } else {
    first = std::make_unique<adjoint::Adjoint>(p, ao);
    second = std::make_unique<adjoint::Adjoint>(p, ao_seed);
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

  const Census cen = census(ad_off.plan(), p);
  std::printf("\n== %s  (B %d, lane_tile %d, tile %d, reps %d%s)\n", name, B, cfg.lane_tile, cfg.tile, cfg.reps,
              cfg.swap ? ", SWAPPED allocation order" : "");
  std::printf("   %lld values, %lld inputs, %lld outputs; %lld domains\n", static_cast<long long>(cen.values),
              static_cast<long long>(n_in), static_cast<long long>(n_out),
              static_cast<long long>(p.domains.size()));
  std::printf("   pulls: %lld non-empty (%lld reader entries), %lld empty, %lld Input rows (keep the zero start)\n",
              cen.pulls_nonempty, cen.readers, cen.pulls_empty, cen.input_rows);
  std::printf("   single-reader %lld (%.1f%% of non-empty), of which unit coefficient %lld (%.1f%%)\n", cen.single,
              100.0 * static_cast<double>(cen.single) / static_cast<double>(std::max<long long>(cen.pulls_nonempty, 1)),
              cen.single_unit,
              100.0 * static_cast<double>(cen.single_unit) /
                  static_cast<double>(std::max<long long>(cen.pulls_nonempty, 1)));
  std::printf("   the zero fill is %lld lane-element stores per run at B = %d; the leading adds it feeds are %lld\n",
              (cen.pulls_nonempty + cen.pulls_empty) * B, B, cen.pulls_nonempty * B);

  for (int w = 0; w < cfg.warmup; ++w) {
    ad_off.run(state.data(), B, out_bar.data(), out.data(), sbar.data());
    ad_on.run(state.data(), B, out_bar.data(), out.data(), sbar.data());
    f0.run(state.data(), B, fout.data());
  }

  require_quiet_box(name);
  const double l0 = load_1min();

  std::vector<double> t_off, t_on, t_f0, paired;
  for (int r = 0; r < cfg.reps; ++r) {
    // A,B,B,A: a monotone drift across a pair lands on both arms equally.
    const bool off_first = (r % 2) == 0;
    double a = 0.0, b = 0.0;
    if (off_first) {
      const auto s0 = clock_type::now();
      ad_off.run(state.data(), B, out_bar.data(), out.data(), sbar.data());
      a = us_since(s0);
      const auto s1 = clock_type::now();
      ad_on.run(state.data(), B, out_bar.data(), out.data(), sbar.data());
      b = us_since(s1);
    } else {
      const auto s1 = clock_type::now();
      ad_on.run(state.data(), B, out_bar.data(), out.data(), sbar.data());
      b = us_since(s1);
      const auto s0 = clock_type::now();
      ad_off.run(state.data(), B, out_bar.data(), out.data(), sbar.data());
      a = us_since(s0);
    }
    t_off.push_back(a);
    t_on.push_back(b);
    paired.push_back(b - a);
    const auto s2 = clock_type::now();
    f0.run(state.data(), B, fout.data());
    t_f0.push_back(us_since(s2));
  }
  const double l1 = load_1min();

  const double m_off = median(t_off), m_on = median(t_on), m_f0 = median(t_f0);
  const double d_paired = median(paired);
  const double rev_off = m_off - m_f0;
  std::printf("   load %.2f -> %.2f\n", l0, l1);
  std::printf("   %-28s %10.2f us   (min %.2f)\n", "A, seed_pull OFF", m_off, *std::min_element(t_off.begin(), t_off.end()));
  std::printf("   %-28s %10.2f us   (min %.2f)\n", "A, seed_pull ON", m_on, *std::min_element(t_on.begin(), t_on.end()));
  std::printf("   %-28s %10.2f us\n", "F0, unfused forward", m_f0);
  std::printf("   %-28s %10.2f us   reverse-pass BOUND (A_off - F0)\n", "A_off - F0", rev_off);
  std::printf("   PAIRED DELTA (median of per-rep b - a)  %+10.3f us\n", d_paired);
  std::printf("   as a share of the whole call            %+10.2f %%\n", 100.0 * d_paired / m_off);
  if (rev_off > 0.0) {
    std::printf("   as a share of the reverse-pass bound    %+10.2f %%   <- compare with D102's 26%% op ceiling\n",
                100.0 * d_paired / rev_off);
  }
  std::printf("   median-of-medians delta                 %+10.3f us (%+.2f %% of the call)\n", m_on - m_off,
              100.0 * (m_on - m_off) / m_off);
}

[[noreturn]] void die(const std::string& m) {
  std::fprintf(stderr, "pullprobe: %s\n", m.c_str());
  std::exit(2);
}

}  // namespace

int main(int argc, char** argv) {
  Config cfg;
  std::string which = "all";
  std::vector<int> batches;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> std::string {
      if (i + 1 >= argc) die("missing value for " + a);
      return argv[++i];
    };
    if (a == "--fixture") which = next();
    else if (a == "--batch") batches.push_back(std::stoi(next()));
    else if (a == "--reps") cfg.reps = std::stoi(next());
    else if (a == "--trades") cfg.trades = std::stoi(next());
    else if (a == "--lane-tile") cfg.lane_tile = std::stoi(next());
    else if (a == "--tile") cfg.tile = std::stoi(next());
    else if (a == "--swap") cfg.swap = true;
    else if (a == "--ignore-load") g_ignore_load = true;
    else die("unknown argument " + a);
  }
  if (batches.empty()) batches = {1, 8, 64};

  try {
    std::printf("pullprobe — seed_pull on against off, paired, one process\n");
    std::printf("  flags: %s\n", epykos::build_flags());
    std::printf("  %d logical cores, 1-minute load %.2f, refuse threshold %.1f%s\n", logical_cores(), load_1min(),
                static_cast<double>(logical_cores()) / 2.0, g_ignore_load ? "  (--ignore-load: NOT reportable)" : "");

    const bool all = which == "all";
    if (all || which == "compare_ois") {
      fixtures::CompareOisOptions o;
      o.trades = cfg.trades;
      const Program p = ir::infer(fixtures::record_compare_ois(fixtures::make_compare_ois(o)).tape);
      for (const int B : batches) {
        Config c = cfg;
        c.batch = B;
        measure(("compare_ois(" + std::to_string(cfg.trades) + " trades)").c_str(), p, c);
      }
    }
    if (all || which == "stage_a") {
      const Program p = ir::infer(fixtures::record_stage_a(fixtures::make_stage_a()).tape);
      for (const int B : batches) {
        Config c = cfg;
        c.batch = B;
        measure("stage_a(default)", p, c);
      }
    }
    std::printf("\n  final 1-minute load %.2f\n", load_1min());
  } catch (const std::exception& e) {
    die(std::string("exception: ") + e.what());
  }
  return 0;
}
