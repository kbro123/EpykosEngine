// tools/exotics — THE EXOTICS KILL TEST: does D1's "No JIT" survive a path-dependent payoff?
//
// D1 (2026-09-22) carries its own revisit clause -- "revisit only if M1/M3 show the catalogue +
// interpreter cannot reach the gates" -- and M1/M3 are interest-rate swaps. This tool runs the
// clause against the shape `PRINCIPLES.md` §6 says the design targets and that no fixture in the
// tree had until `include/epykos/fixtures/exotic_path.hpp`: a scripted payoff over a path grid.
//
// **The pass/fail criterion was written down and committed BEFORE this tool existed**, in
// `docs/EXOTICS_KILL_TEST.md`. This tool prints the criterion it is being judged against on every
// run, so a reader cannot be handed the number without it.
//
// WHAT IT MEASURES, and the split between builds that `CLAUDE.md` requires:
//
//   * TIMINGS come from the `release` preset only. The deciding number is the compiled engine path
//     (`exec::Interpreter::run`, B = 1) against a straight `double` evaluation of the SAME Scalar
//     template (D3) -- one state of work on each side.
//   * COVERAGE and `catalogue::Coverage::time_fraction` are only populated under
//     `-DEPYKOS_EXEC_PROFILE` (the `profile` preset), which instruments every `Interpreter::run()`
//     and is never a gated or perf-gated build. So this tool PRINTS WHICH BUILD IT IS and refuses
//     to print a timing at all from a profile build. The two halves are run separately and never
//     mixed.
//
// Unlike tools/revcollapse/ this tool times, so like tools/ladder/ it reads the 1-minute load
// itself and refuses above cores/2 -- bench/run.sh's own rule (D29, docs/WORKLOADS.md M1
// Measurement). `--ignore-load` runs anyway and marks every line as DIRTY.
//
// Usage:
//   exoticsprobe [--mode all|shape|time|oracle|coverage] [--paths 64,256,1024] [--steps 12,52,252]
//                [--reps N] [--batch N] [--ignore-load]
//
// D68's `_DEFAULT_SOURCE`, and it MUST come before every include: `getloadavg` is a BSD extension
// that glibc compiles out under `-std=c++20` (CMAKE_CXX_EXTENSIONS is OFF).
#define _DEFAULT_SOURCE 1

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <stdlib.h>  // getloadavg (BSD extension; not in <cstdlib>'s guaranteed surface)
#include <string>
#include <thread>
#include <vector>

#include "epykos/catalogue/coverage.hpp"
#include "epykos/compile.hpp"
#include "epykos/exec/interpreter.hpp"
#include "epykos/fixtures/exotic_path.hpp"
#include "epykos/fixtures/m1_book.hpp"
#include "epykos/fixtures/record_m1.hpp"
#include "epykos/ir/program.hpp"
#include "epykos/ir/signature.hpp"
#include "epykos/tape/tape.hpp"
#include "epykos/version.hpp"

namespace {

using namespace epykos;
namespace fixtures = epykos::fixtures;
using clock_type = std::chrono::steady_clock;

std::size_t uz(int v) { return static_cast<std::size_t>(v); }

double us_since(clock_type::time_point t) {
  return std::chrono::duration<double, std::micro>(clock_type::now() - t).count();
}

// ---- the box (verbatim shape of tools/ladder/ladder_main.cpp, same reasons) -------------------
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
      std::printf("\nREFUSED: 1-minute load %.2f exceeds cores/2 = %.1f (%d logical cores) before '%s'.\n",
                  l, threshold, logical_cores(), what);
      std::printf("Wait for the box, or --ignore-load for a run that is NOT reported (D29).\n");
      std::exit(2);
    }
    std::printf("  WARNING: load %.2f > cores/2 = %.1f; '%s' is DIRTY and must not be reported\n", l, threshold, what);
  }
}

// Is the LIBRARY this binary links the profile build? A timing must never come out of one
// (CLAUDE.md Build: the preset instruments every Interpreter::run()).
//
// This CANNOT be a compile-time check, and the first version of this function was one and was
// WRONG -- it printed "build: release" while linked against a profile libepykos, which is exactly
// the mix-up the guard exists to prevent. The root cause: root CMakeLists.txt sets
// `target_compile_definitions(epykos PRIVATE EPYKOS_EXEC_PROFILE=1)`, and PRIVATE means the macro
// is visible only inside the library's own TUs. No consumer can see it. Any tool that guards
// itself the way CLAUDE.md requires has to do it at RUNTIME.
//
// The documented runtime signal is `catalogue::Coverage::time_fraction`, which coverage.hpp says
// is "only ever populated in a build compiled with -DEPYKOS_EXEC_PROFILE AND after at least one
// run has completed; -1.0 otherwise". So: build a one-row program, run it once, and look.
bool profile_build() {
  static const bool is_profile = [] {
    try {
      Tape t;
      {
        Tape::Scope s(t);
        const Rec x = make_input(t, 2.0);
        register_output(t, x * x);
      }
      const ir::Program p = ir::infer(t);
      exec::Options o;
      o.max_batch = 1;
      exec::Interpreter in(p, o);
      const double z = 2.0;
      double out = 0.0;
      in.run(&z, 1, &out);
      return in.coverage().time_fraction >= 0.0;
    } catch (const std::exception&) {
      return false;
    }
  }();
  return is_profile;
}

// A compiler barrier. BOTH arms need one and the `double` arm needs it more: its result is a local
// buffer nothing reads afterwards, so at -O3 with the template fully visible LLVM is entitled to
// delete the whole computation and hand back a timing of nothing. The engine arm goes through a
// non-inlinable `Interpreter::run` across a TU boundary and could not be elided anyway, so
// applying the barrier to both is what keeps them comparable rather than what protects the engine.
// Checked, not assumed: `--mode time` prints a checksum of each arm, and if either were elided its
// checksum would be zero.
void consume(const double* p, std::size_t n) {
  asm volatile("" : : "r"(p), "r"(n) : "memory");
}

struct Samples {
  std::vector<double> v;
  void add(double x) { v.push_back(x); }
  double q(double p) const {
    if (v.empty()) return 0.0;
    std::vector<double> s = v;
    std::sort(s.begin(), s.end());
    const double i = p * static_cast<double>(s.size() - 1);
    const std::size_t lo = static_cast<std::size_t>(i);
    const std::size_t hi = std::min(lo + 1, s.size() - 1);
    return s[lo] + (i - static_cast<double>(lo)) * (s[hi] - s[lo]);
  }
  double min() const { return q(0.0); }
  double med() const { return q(0.5); }
};

// ---- the shape of a recording -----------------------------------------------------------------
struct Shape {
  int paths = 0;
  int steps = 0;
  std::size_t nodes_recorded = 0;  // before the pin
  std::size_t nodes_pinned = 0;    // after compile()
  double collapse = 1.0;
  double record_ms = 0.0;
  double compile_ms = 0.0;
  double infer_ms = 0.0;
  std::size_t domains = 0;
  std::size_t rows = 0;
  std::size_t scan_domains = 0;
  std::size_t scan_rows = 0;
  std::size_t select_steps = 0;  // Select steps in the pinned program, by domain-weighted rows
  std::size_t select_rows = 0;
  ir::InferStats inf;
  std::size_t scan_class_doms = 0;
  catalogue::Coverage cov;
  bool ok = false;
  std::string error;
};

std::size_t count_op_rows(const ir::Program& p, Op op, std::size_t* step_count) {
  std::size_t rows = 0;
  *step_count = 0;
  for (std::size_t d = 0; d < p.domains.size(); ++d) {
    std::size_t n = 0;
    for (const ir::Step& s : p.groups[d].steps) {
      if (s.op == op) ++n;
    }
    if (n > 0) {
      *step_count += n;
      rows += static_cast<std::size_t>(p.domains[d].rows);
    }
  }
  return rows;
}

// Record -> pin -> infer -> plan, with every phase timed. `probe` keeps the tape and program alive
// for the caller (the Interpreter holds a reference to the program).
struct Built {
  fixtures::ExoticPathFixture f;
  Tape tape_raw;   // passes OFF: the recording, for the node count before the pin
  Tape tape_pin;   // through compile(): the pinned tape
  ir::Program prog;
  std::unique_ptr<exec::Interpreter> interp;
  Shape shape;
};

std::unique_ptr<Built> build(int paths, int steps, bool output_paths, int max_batch,
                             unsigned legs = fixtures::exotic_legs_all) {
  auto b = std::make_unique<Built>();
  b->shape.paths = paths;
  b->shape.steps = steps;
  b->f = fixtures::make_exotic_path(paths, steps, 20260922, legs);
  try {
    clock_type::time_point t0 = clock_type::now();
    b->tape_raw = fixtures::record_exotic_path(b->f, output_paths, /*run_passes=*/false);
    b->shape.record_ms = us_since(t0) / 1000.0;
    b->shape.nodes_recorded = b->tape_raw.size();

    // The pin, timed on its own recording so the two counts are of the same program.
    b->tape_pin = fixtures::record_exotic_path(b->f, output_paths, /*run_passes=*/false);
    t0 = clock_type::now();
    const PassResult pr = compile(b->tape_pin);
    b->shape.compile_ms = us_since(t0) / 1000.0;
    b->shape.nodes_pinned = pr.nodes_after;
    b->shape.collapse = pr.nodes_after == 0
                            ? 0.0
                            : static_cast<double>(pr.nodes_before) / static_cast<double>(pr.nodes_after);

    t0 = clock_type::now();
    b->prog = ir::infer(b->tape_pin, &b->shape.inf);
    b->shape.infer_ms = us_since(t0) / 1000.0;
    ir::validate(b->prog);

    b->shape.domains = b->prog.domains.size();
    b->shape.rows = b->prog.num_values();
    for (const ir::Domain& d : b->prog.domains) {
      if (d.scan >= 0) {
        ++b->shape.scan_domains;
        b->shape.scan_rows += static_cast<std::size_t>(d.rows);
      }
      if (d.scan_class) ++b->shape.scan_class_doms;
    }
    std::size_t sel_steps = 0;
    b->shape.select_rows = count_op_rows(b->prog, Op::Select, &sel_steps);
    b->shape.select_steps = sel_steps;

    exec::Options o;
    o.max_batch = max_batch;
    b->interp = std::make_unique<exec::Interpreter>(b->prog, o);
    b->shape.cov = b->interp->coverage();
    b->shape.ok = true;
  } catch (const std::exception& e) {
    b->shape.error = e.what();
    b->shape.ok = false;
  }
  return b;
}

// ---- correctness: the engine against the templated double, and against the closed form ---------
//
// This comparison CROSSES the pin (PRINCIPLES.md §5.2a case 2): one side is the recording as
// written, the other the collapsed tape, so they are two roundings of the same real number. It is
// a tolerance comparison, and the measured divergence is printed rather than only the verdict.
double max_rel_diff(const std::vector<double>& a, const std::vector<double>& b, double* abs_scale) {
  double worst = 0.0;
  *abs_scale = 0.0;
  for (std::size_t i = 0; i < a.size(); ++i) {
    const double scale = std::max(std::abs(a[i]), std::abs(b[i]));
    *abs_scale = std::max(*abs_scale, scale);
    const double d = std::abs(a[i] - b[i]);
    if (scale > 1e-300) worst = std::max(worst, d / scale);
    else worst = std::max(worst, d);
  }
  return worst;
}

void print_criterion() {
  std::printf("THE CRITERION (docs/EXOTICS_KILL_TEST.md, committed before this tool existed)\n");
  std::printf("  deciding number: engine / double at B = 1, every swept point.\n");
  std::printf("    >= 1.00x everywhere ........ PASS     D1 holds for path-dependent payoffs\n");
  std::printf("    0.80x - 1.00x anywhere ..... MARGINAL parity with naive scalar C++, not a validation\n");
  std::printf("    <  0.80x anywhere .......... FAIL     D1's revisit clause has fired\n");
  std::printf("  diagnostic, NOT deciding: catalogue time_fraction >= 0.25 reaches / < 0.10 does not.\n");
  std::printf("  geometric Asian vs its exact closed form: within 3 MC standard errors.\n\n");
}

void print_box(const char* tag) {
  std::printf("  [%s] 1-minute load %.2f; %d logical cores (threshold %.1f); build %s\n", tag, load_1min(),
              logical_cores(), static_cast<double>(logical_cores()) / 2.0, profile_build() ? "PROFILE" : "release");
}

std::vector<int> parse_list(const char* s) {
  std::vector<int> v;
  const char* p = s;
  while (*p != '\0') {
    char* end = nullptr;
    const long x = std::strtol(p, &end, 10);
    if (end == p) break;
    v.push_back(static_cast<int>(x));
    p = end;
    while (*p == ',' || *p == ' ') ++p;
  }
  return v;
}

}  // namespace

int main(int argc, char** argv) {
  std::string mode = "all";
  std::vector<int> paths = {64, 256, 1024};
  std::vector<int> steps = {12, 52, 252};
  int reps = 7;
  int batch = 64;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    const char* next = (i + 1 < argc) ? argv[i + 1] : nullptr;
    if (a == "--mode" && next != nullptr) mode = argv[++i];
    else if (a == "--paths" && next != nullptr) paths = parse_list(argv[++i]);
    else if (a == "--steps" && next != nullptr) steps = parse_list(argv[++i]);
    else if (a == "--reps" && next != nullptr) reps = std::atoi(argv[++i]);
    else if (a == "--batch" && next != nullptr) batch = std::atoi(argv[++i]);
    else if (a == "--ignore-load") g_ignore_load = true;
    else {
      std::printf("unknown argument '%s'\n", a.c_str());
      return 2;
    }
  }

  std::printf("exoticsprobe -- the exotics kill test (D1's revisit clause)\n");
  std::printf("  %s\n", epykos::build_flags());
  std::printf("  build: %s%s\n", profile_build() ? "PROFILE (-DEPYKOS_EXEC_PROFILE)" : "release",
              profile_build() ? "  -- NO TIMING IS PRINTED FROM THIS BUILD" : "");
  print_box("start");
  std::printf("\n");
  print_criterion();

  const bool want_shape = (mode == "all" || mode == "shape" || mode == "coverage");
  const bool want_time = (mode == "all" || mode == "time");
  const bool want_oracle = (mode == "all" || mode == "oracle");

  // ---- 0. the moments of ln G, and the same check over independent seeds ----------------------
  //
  // WHY THIS IS HERE AND NOT ONLY THE PRICE. The first run of --mode oracle returned z = -2.1 to
  // -3.2 on all six sweep points, and because the sweep nests one draw stream it is ONE
  // observation, not six -- an unlucky stream and a biased path generator look identical in it.
  // Two discriminators, both cheap:
  //
  //   * the MOMENTS of ln G against their exact analytic values. ln G is exactly normal here, its
  //     mean and variance are known in closed form, and they have far less MC error than the
  //     option price (no max(), so no mass at zero and no skew). A path-machinery defect shows
  //     up here and an unlucky stream does not.
  //   * the SAME price check over independent seeds. Under the null the z-scores scatter about 0.
  if (mode == "all" || mode == "moments" || mode == "oracle") {
    std::printf("== THE MOMENTS OF ln G, AND THE SAME CHECK OVER INDEPENDENT SEEDS ==\n");
    std::printf("  ln G is exactly normal (exact log increments), so its mean and variance have\n");
    std::printf("  closed forms and much less MC error than the price. mu = ln S0 + (r-q-s^2/2)*tbar.\n");
    std::printf("  %6s %6s | %12s %12s %8s | %12s %12s %8s\n", "paths", "steps", "mean(lnG)", "analytic mu", "z",
                "var(lnG)", "analytic s2", "z");
    for (int P : paths) {
      for (int K : steps) {
        const fixtures::ExoticPathFixture f = fixtures::make_exotic_path(P, K);
        // ln G per path, from the same template the payoff uses: recompute it here rather than
        // adding an output, so the fixture stays the payoff and nothing is special-cased for a
        // test (the per-path geometric payoff would be clipped by the max()).
        const double n = static_cast<double>(K);
        const double dt = f.dt();
        const double drift = (f.r - f.q - 0.5 * f.sigma * f.sigma) * dt;
        const double vol = f.sigma * std::sqrt(dt);
        std::vector<double> lng(uz(P), 0.0);
        for (int p = 0; p < P; ++p) {
          double ls = std::log(f.s0);
          double acc = 0.0;
          for (int k = 0; k < K; ++k) {
            ls += drift + vol * f.eps[uz(k) * uz(P) + uz(p)];
            acc += ls;
          }
          lng[uz(p)] = acc / n;
        }
        double m = 0.0;
        for (double x : lng) m += x;
        m /= static_cast<double>(P);
        double s2 = 0.0, m4 = 0.0;
        for (double x : lng) {
          s2 += (x - m) * (x - m);
          m4 += std::pow(x - m, 4.0);
        }
        s2 /= static_cast<double>(P - 1);
        m4 /= static_cast<double>(P);
        const double tbar = dt * (n + 1.0) / 2.0;
        const double mu_a = std::log(f.s0) + (f.r - f.q - 0.5 * f.sigma * f.sigma) * tbar;
        const double s2_a = f.sigma * f.sigma * dt * (n + 1.0) * (2.0 * n + 1.0) / (6.0 * n);
        const double se_m = std::sqrt(s2 / static_cast<double>(P));
        const double se_v = std::sqrt((m4 - s2 * s2) / static_cast<double>(P));
        std::printf("  %6d %6d | %12.8f %12.8f %+8.2f | %12.8f %12.8f %+8.2f\n", P, K, m, mu_a, (m - mu_a) / se_m, s2,
                    s2_a, (s2 - s2_a) / se_v);
      }
    }
    std::printf("\n  the price check over 8 INDEPENDENT seeds (paths %d, steps %d), z about 0 under the null:\n",
                paths.back(), steps.back());
    double zsum = 0.0;
    int zn = 0;
    for (std::uint64_t sd = 0; sd < 8; ++sd) {
      const fixtures::ExoticPathFixture f = fixtures::make_exotic_path(paths.back(), steps.back(), 20260922 + sd * 7919);
      const std::vector<double> out = fixtures::exotic_path_oracle(f, f.record_state(), false);
      const double cf = fixtures::geometric_asian_call_closed_form(f);
      const double se = fixtures::geometric_asian_std_error(f);
      const double z = (out[1] - cf) / se;
      std::printf("    seed %12llu : MC %12.8f  closed %12.8f  z %+6.2f\n",
                  static_cast<unsigned long long>(20260922 + sd * 7919), out[1], cf, z);
      zsum += z;
      ++zn;
    }
    std::printf("    mean z over %d independent seeds: %+.3f (expected 0 +/- %.3f)\n", zn, zsum / zn,
                1.0 / std::sqrt(static_cast<double>(zn)));
    // And the cause, localised to the draws themselves rather than left as an inference: the
    // sample mean of the eps grid under the DEFAULT seed WORKLOADS.md fixes (20260922, sub-stream
    // exotic_path_substream_base). The seed is not changed to get a prettier number -- it is
    // fixed by the fixture discipline -- it is reported for what it is.
    std::printf("\n  the DEFAULT stream's own draws (seed 20260922, sub-stream %llu), which is the cause:\n",
                static_cast<unsigned long long>(fixtures::exotic_path_substream_base));
    for (int P : paths) {
      for (int K : steps) {
        const fixtures::ExoticPathFixture f = fixtures::make_exotic_path(P, K);
        const double n = static_cast<double>(f.eps.size());
        double m = 0.0;
        for (double e : f.eps) m += e;
        m /= n;
        double v = 0.0;
        for (double e : f.eps) v += (e - m) * (e - m);
        v /= n - 1.0;
        std::printf("    paths %5d steps %4d : %9.0f draws, mean %+.6f (%.2f SE from 0), var %.6f\n", P, K, n, m,
                    m / (1.0 / std::sqrt(n)), v);
      }
    }
    std::printf("\n");
  }

  // ---- 1. the oracle check, which needs no quiet box (it compares numbers, not times) ---------
  if (want_oracle) {
    std::printf("== THE INDEPENDENT CHECK: geometric Asian against its exact closed form ==\n");
    std::printf("  no dependency added (standard library only), so no D12 entry is needed.\n");
    std::printf("  %6s %6s  %14s %14s %12s %10s %8s\n", "paths", "steps", "MC estimate", "closed form", "rel diff",
                "std err", "z");
    for (int P : paths) {
      for (int K : steps) {
        const fixtures::ExoticPathFixture f = fixtures::make_exotic_path(P, K);
        const std::vector<double> out = fixtures::exotic_path_oracle(f, f.record_state(), false);
        const double cf = fixtures::geometric_asian_call_closed_form(f);
        const double se = fixtures::geometric_asian_std_error(f);
        const double rel = std::abs(out[1] - cf) / std::abs(cf);
        const double z = se > 0.0 ? (out[1] - cf) / se : 0.0;
        std::printf("  %6d %6d  %14.8f %14.8f %12.3e %10.2e %+8.2f%s\n", P, K, out[1], cf, rel, se, z,
                    std::abs(z) <= 3.0 ? "" : "   <-- OUTSIDE 3 SE");
      }
    }
    std::printf("\n");
  }

  // ---- 2. shape, collapse, domains, scan fraction, coverage -----------------------------------
  // No timing here beyond the pipeline phases themselves, which are reported as ms and labelled.
  std::vector<std::unique_ptr<Built>> built;
  if (want_shape) {
    std::printf("== SHAPE, COLLAPSE AND COVERAGE ==\n");
    std::printf("  collapse = recorded nodes / pinned nodes. compare_ois(16 trades) is 2,565x (D81).\n");
    std::printf("  %6s %6s %12s %12s %9s | %6s %9s %9s %7s | %7s %7s %9s\n", "paths", "steps", "recorded", "pinned",
                "collapse", "doms", "rows", "scan rows", "scan%", "grp cov", "row cov", "time cov");
    for (int P : paths) {
      for (int K : steps) {
        auto b = build(P, K, /*output_paths=*/false, batch);
        if (!b->shape.ok) {
          std::printf("  %6d %6d  REFUSED: %s\n", P, K, b->shape.error.c_str());
          built.push_back(std::move(b));
          continue;
        }
        const Shape& s = b->shape;
        // One run so that time_fraction has something to report (profile build only).
        {
          std::vector<double> st(uz(s.paths > 0 ? fixtures::exotic_n_inputs : 0), 0.0);
          const std::vector<double> z = b->f.record_state();
          std::vector<double> out(uz(b->interp->n_outputs()));
          b->interp->run(z.data(), 1, out.data());
        }
        const catalogue::Coverage cov = b->interp->coverage();
        const double scan_pct = s.rows == 0 ? 0.0 : 100.0 * static_cast<double>(s.scan_rows) / static_cast<double>(s.rows);
        std::printf("  %6d %6d %12zu %12zu %8.3fx | %6zu %9zu %9zu %6.1f%% | %6.1f%% %6.1f%% ", P, K,
                    s.nodes_recorded, s.nodes_pinned, s.collapse, s.domains, s.rows, s.scan_rows, scan_pct,
                    100.0 * cov.group_fraction(), 100.0 * cov.row_fraction());
        if (cov.time_fraction < 0.0) std::printf("%9s\n", "n/a");
        else std::printf("%8.1f%%\n", 100.0 * cov.time_fraction);
        built.push_back(std::move(b));
      }
    }
    std::printf("\n");
    // The Select sites, and the rates baseline for coverage, so the exotics number has something
    // to be compared with that was measured in THIS binary (D103 §1: a number without its
    // denominator means nothing).
    std::printf("  Select steps in the pinned program (the recording discipline, made countable):\n");
    for (const auto& b : built) {
      if (!b->shape.ok) continue;
      std::printf("    paths %4d steps %4d : %zu Select steps over %zu rows\n", b->shape.paths, b->shape.steps,
                  b->shape.select_steps, b->shape.select_rows);
    }
    // WHY THERE IS NO SCAN. `scan%` above is 0.0 at every point, which is either "the recurrence
    // was never detected" or "it was detected and could not be laid out". ir::InferStats
    // distinguishes them: `chains`/`scan_classes` are what detection found, `scan_rounds` > 1 and
    // `scan_retries` are the layout giving up and retrying the class as straight-line rows.
    std::printf("\n  WHY NO SCAN DOMAIN (ir::InferStats: detection vs layout):\n");
    for (const auto& b : built) {
      if (!b->shape.ok) continue;
      const ir::InferStats& s = b->shape.inf;
      std::printf("    paths %4d steps %4d : classes %zu -> domains %zu; chains %zu (%zu nodes),"
                  " scan_classes %zu, scan_rounds %zu, scan_class doms %zu\n",
                  b->shape.paths, b->shape.steps, s.classes, s.domains, s.chains, s.chain_nodes,
                  s.scan_classes, s.scan_rounds, b->shape.scan_class_doms);
      if (!s.scan_retries.empty()) {
        std::printf("      retries:%s\n", s.scan_retries.c_str());
      }
    }
    // HOW MANY COUPLED RECURRENCES THE SCAN DETECTOR TOLERATES. The full payoff carries four (the
    // spot plus three accumulators that all READ it) and gets no scan. `spot` alone is one
    // recurrence with nothing reading it, which is affine_scan's shape and the control. Whatever
    // the threshold is, it is the number a payoff language would have to stay under.
    std::printf("\n  THE SCAN-LAYOUT THRESHOLD, by leg (paths 64, steps 52):\n");
    std::printf("    %-42s %6s %9s %9s %7s %8s %7s\n", "legs", "doms", "rows", "scan rows", "scan%",
                "row cov", "chains");
    struct LegCase {
      const char* name;
      unsigned legs;
    };
    const LegCase legcases[] = {
        {"spot only (1 recurrence, nothing reads it)", fixtures::exotic_leg_spot},
        {"spot + arith (2, one reads the spot)", fixtures::exotic_leg_spot | fixtures::exotic_leg_arith},
        {"arith only", fixtures::exotic_leg_arith},
        {"geom only", fixtures::exotic_leg_geom},
        {"barrier only (a select in the recurrence)", fixtures::exotic_leg_barrier},
        {"arith + geom", fixtures::exotic_leg_arith | fixtures::exotic_leg_geom},
        {"arith + barrier", fixtures::exotic_leg_arith | fixtures::exotic_leg_barrier},
        {"all three (the full payoff)", fixtures::exotic_legs_all},
    };
    for (const LegCase& lc : legcases) {
      auto b = build(64, 52, false, batch, lc.legs);
      if (!b->shape.ok) {
        std::printf("    %-42s REFUSED: %s\n", lc.name, b->shape.error.c_str());
        continue;
      }
      const std::vector<double> z = b->f.record_state();
      std::vector<double> out(uz(b->interp->n_outputs()));
      b->interp->run(z.data(), 1, out.data());
      const catalogue::Coverage c = b->interp->coverage();
      const Shape& sh = b->shape;
      std::printf("    %-42s %6zu %9zu %9zu %6.1f%% %7.1f%% %7zu%s\n", lc.name, sh.domains, sh.rows, sh.scan_rows,
                  sh.rows == 0 ? 0.0 : 100.0 * static_cast<double>(sh.scan_rows) / static_cast<double>(sh.rows),
                  100.0 * c.row_fraction(), sh.inf.chains, sh.scan_rows > 0 ? "  <-- SCANS" : "");
      if (!sh.inf.scan_retries.empty()) std::printf("        retries:%s\n", sh.inf.scan_retries.c_str());
    }

    std::printf("\n  THE RATES BASELINE, same binary, so the coverage numbers above divide:\n");
    try {
      const fixtures::Book book = fixtures::make_m1_book();
      Tape t = fixtures::record_m1(book);
      const ir::Program p = ir::infer(t);
      exec::Options o;
      o.max_batch = batch;
      exec::Interpreter in(p, o);
      std::vector<double> out(uz(in.n_outputs()));
      std::vector<double> z(book.z0.begin(), book.z0.end());
      in.run(z.data(), 1, out.data());
      const catalogue::Coverage c = in.coverage();
      std::printf("    m1_book: %zu domains, %zu rows; grp cov %.1f%%, row cov %.1f%%, time cov ",
                  p.domains.size(), p.num_values(), 100.0 * c.group_fraction(), 100.0 * c.row_fraction());
      if (c.time_fraction < 0.0) std::printf("n/a\n");
      else std::printf("%.1f%%\n", 100.0 * c.time_fraction);
    } catch (const std::exception& e) {
      std::printf("    m1_book: unavailable (%s)\n", e.what());
    }
    std::printf("\n");
  }

  // ---- 3. THE DECIDING NUMBER: engine against templated double, release build only -------------
  if (want_time) {
    if (profile_build()) {
      std::printf("== TIMING REFUSED ==\n");
      std::printf("  This is the PROFILE build. CLAUDE.md: that preset is never a gated or perf-gated\n");
      std::printf("  build -- it instruments every Interpreter::run(). Run --mode time from `release`.\n");
      return 0;
    }
    std::printf("== THE DECIDING NUMBER: engine / templated double ==\n");
    require_quiet_box("the engine-vs-double sweep");
    std::printf("  B = 1 is the criterion (one state of work on each side). B = %d is reported as the\n", batch);
    std::printf("  amortised figure and is NOT the criterion: the double path has no batch axis.\n");
    std::printf("  reps %d, min of reps (the figure bench/run.sh reports), load read before and after.\n", reps);
    const double load_before = load_1min();
    std::printf("  %6s %6s | %13s %13s %9s | %13s %9s | %10s\n", "paths", "steps", "double us", "engine B=1 us",
                "ratio", "eng B=N /st", "ratio", "verdict");
    bool any_fail = false;
    bool any_marginal = false;
    for (int P : paths) {
      for (int K : steps) {
        auto b = build(P, K, /*output_paths=*/false, batch);
        if (!b->shape.ok) {
          std::printf("  %6d %6d  REFUSED: %s\n", P, K, b->shape.error.c_str());
          continue;
        }
        // TWO interpreters, each right-sized for the arm it serves. Measured: running the B = 1
        // arm on the max_batch = 64 instance costs it 41% at paths 1024 / steps 252 (9,861 us
        // against 6,979) -- the value buffer is 64x larger than the lanes in use and the program
        // does not fit, which is D91's residency finding in a new place. Charging the engine for a
        // buffer the caller asked for and never used would be an unfair comparison, so the
        // criterion's arm gets its own instance.
        exec::Options o1;
        o1.max_batch = 1;
        exec::Interpreter in1(b->prog, o1);

        const std::vector<double> z = b->f.record_state();
        const int nout_d = b->f.n_outputs(false);
        std::vector<double> ref(uz(nout_d));
        std::vector<double> eng(uz(in1.n_outputs()));
        std::vector<double> engN(uz(b->interp->n_outputs()) * uz(batch));
        std::vector<double> stateN(uz(fixtures::exotic_n_inputs) * uz(batch));
        for (int k = 0; k < fixtures::exotic_n_inputs; ++k) {
          for (int l = 0; l < batch; ++l) stateN[uz(k) * uz(batch) + uz(l)] = z[uz(k)];
        }

        // Correctness before timing: a ratio against a wrong answer is worthless.
        fixtures::exotic_path_evaluate<double>(b->f, z.data(), ref.data(), false);
        in1.run(z.data(), 1, eng.data());
        double scale = 0.0;
        const std::vector<double> eng3(eng.begin(), eng.begin() + nout_d);
        const double rel = max_rel_diff(ref, eng3, &scale);

        Samples d_us, e1_us, eN_us;
        double sum_d = 0.0, sum_e = 0.0;
        for (int rr = 0; rr < reps; ++rr) {
          // The state MOVES every rep, identically for both arms, so neither computation is loop
          // invariant and neither can be hoisted out of the rep loop. The perturbation is tiny
          // (1e-9 relative on the spot) so the work is the same work: no path changes which side
          // of the barrier or the strike it falls on.
          std::vector<double> zr = z;
          zr[fixtures::exotic_s0] *= 1.0 + 1e-9 * static_cast<double>(rr);
          for (int l = 0; l < batch; ++l) stateN[uz(fixtures::exotic_s0) * uz(batch) + uz(l)] = zr[0];

          // Interleaved, so a drift in the box moves both arms together (D89's paired discipline).
          clock_type::time_point t0 = clock_type::now();
          fixtures::exotic_path_evaluate<double>(b->f, zr.data(), ref.data(), false);
          consume(ref.data(), ref.size());
          d_us.add(us_since(t0));
          t0 = clock_type::now();
          in1.run(zr.data(), 1, eng.data());
          consume(eng.data(), eng.size());
          e1_us.add(us_since(t0));
          t0 = clock_type::now();
          b->interp->run(stateN.data(), batch, engN.data());
          consume(engN.data(), engN.size());
          eN_us.add(us_since(t0) / static_cast<double>(batch));
          sum_d += ref[0] + ref[1] + ref[2];
          sum_e += eng[0] + eng[1] + eng[2];
        }
        const double r1 = d_us.min() / e1_us.min();
        const double rN = d_us.min() / eN_us.min();
        const char* verdict = r1 >= 1.0 ? "PASS" : (r1 >= 0.8 ? "MARGINAL" : "FAIL");
        if (r1 < 0.8) any_fail = true;
        else if (r1 < 1.0) any_marginal = true;
        std::printf("  %6d %6d | %13.1f %13.1f %8.3fx | %13.3f %8.3fx | %-10s  (agree %.2e, sums %.6f/%.6f)\n", P, K,
                    d_us.min(), e1_us.min(), r1, eN_us.min(), rN, verdict, rel, sum_d, sum_e);
      }
    }
    const double load_after = load_1min();
    std::printf("  loads: %.2f before, %.2f after (threshold %.1f)\n", load_before, load_after,
                static_cast<double>(logical_cores()) / 2.0);
    std::printf("\n  VERDICT ON D1, against the criterion committed before the measurement: %s\n",
                any_fail ? "FAIL -- the revisit clause has fired"
                         : (any_marginal ? "MARGINAL -- parity only, not a validation" : "PASS"));

    // ---- THE DECISIVE FOLLOW-UP: does a payoff that DOES get a scan domain pass the ratio? -----
    //
    // `--mode shape` finds that the scan layout survives every leg combination EXCEPT the barrier:
    // a Select inside a recurrence (the running extremum) makes ir::infer give up, and it takes the
    // spot's own `mul` chain down with it. So the sweep above is not a measurement of "the
    // catalogue + interpreter on a path-dependent payoff" -- it is a measurement of that one gap.
    //
    // This separates them. The SAME path grid, the SAME transcendentals, the SAME reduction, with
    // and without the leg that defeats the scan. If the scanning payoffs also lose, D1's revisit
    // clause has fired on its own terms. If they win, the shortfall is the scan gap and D1 is not
    // what the measurement is about. Either way the headline verdict above is unchanged: it was
    // taken on the full payoff, which is the instrument the brief asked for.
    std::printf("\n  == IS IT D1, OR IS IT THE SELECT-IN-A-RECURRENCE GAP? ==\n");
    std::printf("  Same grid, same transcendentals, same reduction; only the legs change.\n");
    std::printf("  %-40s %6s %9s %7s | %10s %10s %8s | %10s %8s %8s\n", "legs", "doms", "scan rows", "row cov",
                  "double us", "eng B=1", "ratio", "eng B=N/st", "ratio", "ns/step");
    {
      struct LegCase {
        const char* name;
        unsigned legs;
      };
      const LegCase legcases[] = {
          {"spot only (scans)", fixtures::exotic_leg_spot},
          {"arith only -- arithmetic Asian (scans)", fixtures::exotic_leg_arith},
          {"geom only -- geometric Asian (scans)", fixtures::exotic_leg_geom},
          {"arith + geom (scans)", fixtures::exotic_leg_arith | fixtures::exotic_leg_geom},
          {"barrier only -- NO scan", fixtures::exotic_leg_barrier},
          {"all three -- NO scan", fixtures::exotic_legs_all},
      };
      const int P = paths.back(), K = steps.back();
      for (const LegCase& lc : legcases) {
        const fixtures::ExoticPathFixture f = fixtures::make_exotic_path(P, K, 20260922, lc.legs);
        Tape t = fixtures::record_exotic_path(f, false, true);
        ir::Program prog = ir::infer(t);
        exec::Options o;
        o.max_batch = 1;
        exec::Interpreter in(prog, o);
        exec::Options oN;
        oN.max_batch = batch;
        exec::Interpreter inN(prog, oN);
        std::size_t scan_rows = 0;
        for (const ir::Domain& d : prog.domains) {
          if (d.scan >= 0) scan_rows += static_cast<std::size_t>(d.rows);
        }
        const std::vector<double> z = f.record_state();
        std::vector<double> ref(uz(f.n_outputs(false)));
        std::vector<double> eng(uz(in.n_outputs()));
        in.run(z.data(), 1, eng.data());
        const catalogue::Coverage c = in.coverage();
        std::vector<double> engN(uz(inN.n_outputs()) * uz(batch));
        std::vector<double> stateN(uz(fixtures::exotic_n_inputs) * uz(batch));
        for (int k = 0; k < fixtures::exotic_n_inputs; ++k) {
          for (int l = 0; l < batch; ++l) stateN[uz(k) * uz(batch) + uz(l)] = z[uz(k)];
        }
        Samples d_us, e_us, eN_us;
        for (int rr = 0; rr < reps; ++rr) {
          std::vector<double> zr = z;
          zr[fixtures::exotic_s0] *= 1.0 + 1e-9 * static_cast<double>(rr);
          for (int l = 0; l < batch; ++l) stateN[uz(fixtures::exotic_s0) * uz(batch) + uz(l)] = zr[0];
          clock_type::time_point t0 = clock_type::now();
          fixtures::exotic_path_evaluate<double>(f, zr.data(), ref.data(), false);
          consume(ref.data(), ref.size());
          d_us.add(us_since(t0));
          t0 = clock_type::now();
          in.run(zr.data(), 1, eng.data());
          consume(eng.data(), eng.size());
          e_us.add(us_since(t0));
          t0 = clock_type::now();
          inN.run(stateN.data(), batch, engN.data());
          consume(engN.data(), engN.size());
          eN_us.add(us_since(t0) / static_cast<double>(batch));
        }
        const double r = d_us.min() / e_us.min();
        const double rN = d_us.min() / eN_us.min();
        // The mechanism, made a number: the engine's excess over the double arm, per path-step.
        // A path grid has one row per path-step and no sharing, so there is nothing to amortise a
        // per-row interpretive cost against -- which is what this column shows and what the rates
        // book, where 1,000 trades share one curve, never exposed.
        const double ns_step = 1000.0 * (e_us.min() - d_us.min()) / (static_cast<double>(P) * static_cast<double>(K));
        std::printf("  %-40s %6zu %9zu %6.1f%% | %10.1f %10.1f %7.3fx | %10.2f %7.3fx %7.2f%s\n", lc.name,
                    prog.domains.size(), scan_rows, 100.0 * c.row_fraction(), d_us.min(), e_us.min(), r, eN_us.min(),
                    rN, ns_step, r >= 1.0 ? "  <-- clears 1.00x" : "");
      }
      std::printf("  paths %d steps %d, max_batch = 1 on every row, min of %d reps.\n", P, K, reps);
    }

    // ---- IS THE SHORTFALL A FIXABLE DEFECT BELOW THE PIN, OR IS IT STRUCTURAL? ----------------
    //
    // The criterion's FAIL clause says the shortfall must not be attributable to a fixable defect
    // below the pin. Every knob exec::Options carries is swept here at the worst swept point, on
    // the SAME program, so a tuning answer would show up as a point that clears 1.0x. If none
    // does, the result is not a mis-set knob. This is the discharge of that clause, not a search
    // for a better number: the default configuration is what the verdict is taken from either way.
    std::printf("\n  == DISCHARGING THE 'fixable defect below the pin' CLAUSE ==\n");
    std::printf("  Every exec::Options knob, same program, worst swept point. A tuning answer would\n");
    std::printf("  show as a configuration clearing 1.00x; the verdict is taken from the DEFAULT either way.\n");
    {
      const int P = paths.back(), K = steps.back();
      const fixtures::ExoticPathFixture f = fixtures::make_exotic_path(P, K);
      Tape t = fixtures::record_exotic_path(f, false, true);
      const ir::Program p = ir::infer(t);
      const std::vector<double> z = f.record_state();
      std::vector<double> ref(uz(f.n_outputs(false)));

      Samples d_us;
      for (int rr = 0; rr < reps; ++rr) {
        std::vector<double> zr = z;
        zr[fixtures::exotic_s0] *= 1.0 + 1e-9 * static_cast<double>(rr);
        const clock_type::time_point t0 = clock_type::now();
        fixtures::exotic_path_evaluate<double>(f, zr.data(), ref.data(), false);
        consume(ref.data(), ref.size());
        d_us.add(us_since(t0));
      }
      std::printf("  paths %d steps %d; the double arm is %.1f us (min of %d)\n", P, K, d_us.min(), reps);
      std::printf("  %-44s %12s %9s\n", "configuration", "engine us", "ratio");

      // `name` is a std::string, not a const char*: an earlier version of this loop pushed names
      // into a static vector and stored .c_str(), which dangles the moment the vector reallocates
      // -- and did, printing two blank rows.
      struct Cfg {
        std::string name;
        exec::Options o;
      };
      std::vector<Cfg> cfgs;
      auto base = [] { exec::Options o; o.max_batch = 1; return o; };
      cfgs.push_back({"default", base()});
      for (int tile : {16, 64, 1024, 4096}) {
        exec::Options o = base();
        o.tile = tile;
        cfgs.push_back({"tile = " + std::to_string(tile), o});
      }
      for (int lt : {1, 4}) {
        exec::Options o = base();
        o.lane_tile = lt;
        cfgs.push_back({"lane_tile = " + std::to_string(lt), o});
      }
      {
        exec::Options o = base(); o.split_lane_chunks = false; cfgs.push_back({"split_lane_chunks = false", o});
      }
      {
        exec::Options o = base(); o.fuse_pairs = false;        cfgs.push_back({"fuse_pairs = false", o});
      }
      {
        exec::Options o = base(); o.fuse_reductions = false;   cfgs.push_back({"fuse_reductions = false", o});
      }
      {
        exec::Options o = base(); o.inline_producers = false;  cfgs.push_back({"inline_producers = false", o});
      }
      {
        exec::Options o = base(); o.use_catalogue = false;     cfgs.push_back({"use_catalogue = false", o});
      }
      {
        exec::Options o = base(); o.tile = 4096; o.fuse_reductions = false;
        cfgs.push_back({"tile = 4096 + fuse_reductions = false", o});
      }
      {
        // NOT currently a gated mode and NOT tier 2 either: exp_poly is <= 1 ulp of std::exp, so
        // PRINCIPLES.md §5.3 ("a vector or otherwise different transcendental of equal or better
        // accuracy is a legal implementation below the pin") is the route by which it could become
        // the declared policy. It is swept here because it is the only knob that moves the
        // transcendental cost, and that turns out to be the largest single item -- see the report.
        exec::Options o = base(); o.exp = exec::ExpMode::poly;
        cfgs.push_back({"exp = poly (not a gated mode; §5.3 is its route)", o});
      }
      double best = 0.0;
      std::string best_name;
      for (const Cfg& c : cfgs) {
        try {
          exec::Interpreter in(p, c.o);
          std::vector<double> out(uz(in.n_outputs()));
          Samples e;
          for (int rr = 0; rr < reps; ++rr) {
            std::vector<double> zr = z;
            zr[fixtures::exotic_s0] *= 1.0 + 1e-9 * static_cast<double>(rr);
            const clock_type::time_point t0 = clock_type::now();
            in.run(zr.data(), 1, out.data());
            consume(out.data(), out.size());
            e.add(us_since(t0));
          }
          const double r = d_us.min() / e.min();
          if (r > best) { best = r; best_name = c.name; }
          std::printf("  %-44s %12.1f %8.3fx%s\n", c.name.c_str(), e.min(), r, r >= 1.0 ? "  <-- clears 1.00x" : "");
        } catch (const std::exception& ex) {
          std::printf("  %-44s %12s  (%s)\n", c.name.c_str(), "refused", ex.what());
        }
      }
      std::printf("\n  BEST of %zu configurations: %.3fx (%s).\n", cfgs.size(), best, best_name.c_str());
      std::printf("  %s\n", best >= 1.0
                                ? "A knob clears 1.00x: the shortfall IS tunable and the FAIL is NOT structural."
                                : "No configuration clears 1.00x, so the shortfall is NOT a mis-set knob.");
    }
  }

  print_box("end");
  return 0;
}
