// tools/ladder/ — where the risk ladder's microseconds go.
//
// D90 reports the O3 ladder at 531.5 us against the other engine's 3,251.2 (6.12x ours), the
// engine's strongest number, and nobody has decomposed it. D91 decomposed the warm CALIBRATION by
// regression over a swept parameter and warned that isolated unit costs over-attribute (its first
// attempt got 111.5%); D92 showed that a one-row ladder gives a Jacobian share of 31-64% while a
// 256-row one gives 0.5%, so the row count is a first-order parameter. D102 measured the reverse
// pass's ARITHMETIC (a 1.76x ceiling on Stage A) and made no timing claim at all, so the share of
// the ladder that the reverse pass actually is has never been measured. This tool measures it.
//
// NOTE THE SHAPE D90's NUMBER IS. `tools/h2h/h2h_main.cpp` passes `ordinals = {book_output}`, so
// the 531.5 us headline is a ONE-ROW ladder: one book-level gradient, 25 buckets, B = 1. That is
// the quantity the other engine's `price_portfolio_risk(book).ladder` returns, so the comparison
// is sound -- but it means D92's "one row is an artefact" applies to the headline itself, and a
// decomposition of "the ladder" has to say which ladder. This tool reports both R = 1 and a swept
// R.
//
// Everything here times only PUBLIC API calls on the same `ImplicitProgram` objects the ladder
// itself runs on. Nothing under include/epykos/ or src/ is changed, so what is measured is the
// shipped path.
//
// ---- the phase ladder, by subtraction --------------------------------------------------------
//
// Per chunk of the ladder's row loop, at the same state and the same batch size the ladder uses:
//
//   F   exec::Interpreter::run(solved_state, B, out)              the forward pass
//   A   adjoint::Adjoint::run(solved_state, B, ob, out, sbar)     forward + reverse
//   R   ImplicitProgram::run(state, B, out)                       solve(no factors) + to_soa + F
//   D   ImplicitProgram::adjoint(state, B, ob, out, sbar)         solve(factors) + to_soa + A
//                                                                 + IFT per lane + copy-out
//   L   fixtures::compare_ois_ladder(...)                         D per chunk + the fixture's own
//                                                                 marshalling
//
//   reverse pass     <= A - F     an UPPER BOUND, not an estimate. adjoint_e0.cpp's forward()
//                                 materialises every domain's rows unconditionally while
//                                 exec::Interpreter fuses and inlines, so F understates the
//                                 forward half that sits inside A, and the difference over-counts
//                                 the reverse by exactly that much. Labelled as a bound wherever
//                                 it appears.
//   solve + IFT + .. =  D - A
//   marshalling      =  L - sum_chunks D
//
// Two independent routes to the exit Jacobian's share, because one would not be evidence:
//   * OPTION TOGGLE. SolveOptions::final_jacobian = false makes the solve hand back the shared
//     record-point factorisation instead of building J at the solution. At the record point those
//     are the same matrix, which is precisely why D85 §3 refuses this as a CORRECTNESS
//     configuration. Here it is an attribution device and nothing else; the tool prints the
//     ladder's own worst row difference under both so the reader can see that they coincide at
//     the record point and would not at a moved one.
//   * ISOLATED UNIT COST. ResidualProgram::jacobian + Factors::compute on a private
//     ResidualProgram, timed on its own. D91 §1 says this route over-attributes; the tool prints
//     the disagreement rather than choosing a winner.
//
// ---- the paired comparison with tools/revcollapse/, and what it says about D102 ---------------
//
// `ladderprobe --mode phases --trades 256` and `revcollapse "compare_ois(telescoped, 256)"` run
// the SAME program: both report 12,280 pinned nodes, 24 domains, 7,209 values, 34 inputs, 291
// outputs. (`record_compare_ois` records the naive coupon and compiles it; D102 §2 showed naive
// and telescoped reach the same pinned tape, so P, Q and the reverse are identical.) So the two
// tools' numbers can be divided, and that division is the test D102 asked for:
//
//   revcollapse   reverse-only arithmetic 42,791 OPS against the forward half's 15,645  = 2.74x
//   ladderprobe   reverse (A - F0) 26.43 us/row against the forward F0's 5.08 us/row    = 5.20x
//
// The reverse pass takes 5.20x the forward's TIME while the emitted reverse program does 2.74x
// the forward's ARITHMETIC, so per operation the reverse runs about 1.90x less efficiently than
// the forward. The candidate cause is named in `adjoint.hpp`'s own header and is deliberate
// (D31): the reverse RECOMPUTES every group's intermediate steps per tile --
// `adjoint_e0.cpp`'s reverse() calls forward_step for steps 0..last-1 -- arithmetic the emitted
// program materialises instead and which therefore never enters revcollapse's reverse-only count.
// That attribution is an ESTIMATE; nothing here counts the recomputed operations.
//
// The consequence for D102 cuts both ways and should not be quoted by halves. The denominator it
// lacked is LARGE -- the reverse pass is 75%-80% of a realistic ladder -- so its 1.76x-1.92x
// ceiling is not a D92. But that ceiling is a ratio of EMITTED-PROGRAM op counts, and the runtime
// executes more arithmetic than that program contains, so 1.92x of the count is not 1.92x of the
// time. D102's own closing line still stands unamended: the paired before/after of its two local
// edits is the only thing that settles it.
//
// Modes:
//   shape    structural facts: sizes, chunking, buffer bytes, RunStats. No timing.
//   phases   the subtraction ladder above, at each --rows value
//   sweep    the regression data: the ladder swept over row count at a fixed batch
//   batch    the same ladder at one row count swept over max_batch, which separates the
//            per-chunk terms from the per-lane ones independently of the row sweep
//   evict    D91 §3's residency probe: forced eviction between consecutive ladders
//   unit     isolated unit costs (the route D91 discarded), for the cross-check
//   toggle   the optimality diagnostic and the exit Jacobian switched off, as attribution devices
//   all      every mode above
//
// Reserved box only. The tool prints the 1-minute load before and after every timed block and
// REFUSES to time at all when the 1-minute load exceeds cores/2, which is bench/run.sh's own rule
// (D29, docs/WORKLOADS.md M1 Measurement); --ignore-load runs anyway and marks every line, for
// tooling tests only. Built whenever EPYKOS_BUILD_TOOLS is; never run by ctest or CI.
//
// Usage:
//   ladderprobe [--mode all|shape|phases|sweep|batch|evict|unit|toggle]
//               [--trades N] [--tenors a,b,c] [--rows 1,8,64,256] [--batch N] [--reps N]
//               [--evict-mb 0,1,4,16,64] [--seed N] [--ignore-load]

// D68's `_DEFAULT_SOURCE`, and it MUST come before every include: `getloadavg` is a BSD extension
// that glibc compiles out under `-std=c++20` (CMAKE_CXX_EXTENSIONS is OFF). Same dance as
// tools/costmodel/collect_main.cpp, for the same reason.
#define _DEFAULT_SOURCE 1

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <memory>
#include <stdlib.h>  // getloadavg (BSD extension; not in <cstdlib>'s guaranteed surface)
#include <string>
#include <thread>
#include <vector>

#include "epykos/adjoint/adjoint.hpp"
#include "epykos/exec/interpreter.hpp"
#include "epykos/fixtures/compare_ois.hpp"
#include "epykos/solver/implicit_program.hpp"
#include "epykos/solver/residual.hpp"
#include "epykos/version.hpp"

namespace {

namespace fixtures = epykos::fixtures;
namespace solver = epykos::solver;

using clock_type = std::chrono::steady_clock;

std::size_t uz(int v) { return static_cast<std::size_t>(v); }

double us_since(clock_type::time_point t) {
  return std::chrono::duration<double, std::micro>(clock_type::now() - t).count();
}

// ---- the box ---------------------------------------------------------------------------------
//
// Two implementations, as tools/costmodel/collect_main.cpp explains: Linux reads /proc/loadavg
// (no feature-test macro, no declaration), Darwin keeps `getloadavg`.
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

// bench/run.sh's rule, in the tool: refuse above cores/2 logical cores.
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
  double p90() const { return q(0.9); }
};

// A sink the optimiser cannot drop. Every timed body feeds it.
volatile double g_sink = 0.0;

// ---- the fixture -----------------------------------------------------------------------------

struct Config {
  int trades = 1000;
  std::vector<std::string> tenors;
  std::vector<int> rows = {1, 2, 4, 8, 16, 32, 48, 64, 96, 128, 192, 256};
  int batch = 64;
  int reps = 40;
  std::vector<int> evict_mb = {0, 1, 4, 16, 64};
  std::uint64_t seed = 20260925;
  // Rows below this are measured and printed but kept OUT of every regression. See the note the
  // `phases` mode prints: R = 1 is a different quantity, B = 2 takes a different kernel, and
  // below one lane_tile the per-row cost has not reached its asymptote.
  int fit_from = 8;
};

// One recording of the fixture, plus the ImplicitProgram shapes the modes need. `warm_start` and
// the chord policy are h2h's own settings for `full_warm` -- the object D90's ladder runs on.
struct Harness {
  fixtures::CompareOis s;
  fixtures::CompareOisTape tape;
  std::unique_ptr<solver::ImplicitProgram> prog;        // h2h's full_warm: warm start + chord
  // A SECOND interpreter over the same whole program with every fuse and inline switched OFF, so
  // it materialises every domain's rows -- which is what `adjoint_e0.cpp`'s forward() does, and
  // what `exec::Interpreter`'s DEFAULT does not. This is the tight proxy for the forward half
  // inside `Adjoint::run`: the shipped interpreter fuses and inlines, so the shipped forward
  // UNDERSTATES that half badly and `A - F` is a far looser bound than it looks.
  std::unique_ptr<epykos::exec::Interpreter> unfused;
  std::vector<double> solved_soa;                       // every tape input at the solved state, n_in x B
  int n_q = 0, n_in = 0, n_out = 0;

  Harness(const Config& c, int max_batch, bool diagnostic, bool final_jacobian) {
    fixtures::CompareOisOptions o;
    o.trades = c.trades;
    o.tenors = c.tenors;
    o.seed = c.seed;
    o.max_batch = max_batch;
    s = fixtures::make_compare_ois(o);
    tape = fixtures::record_compare_ois(s);
    // The attribution devices (see the header): the block's own SolveOptions, as recorded.
    for (solver::ImplicitBlock& b : tape.registry.blocks) {
      b.options.optimality_diagnostic = diagnostic;
      b.options.final_jacobian = final_jacobian;
    }
    solver::ProgramOptions po = fixtures::compare_ois_program_options(s);
    po.max_batch = max_batch;
    po.interpreter.max_batch = max_batch;
    po.adjoint.max_batch = max_batch;
    po.warm_start = true;
    po.jacobian = static_cast<int>(solver::JacobianPolicy::chord);
    prog = std::make_unique<solver::ImplicitProgram>(tape.tape, tape.registry, po);
    n_q = prog->n_state();
    n_in = prog->n_inputs();
    n_out = prog->n_outputs();
    // The materialising interpreter, with the ADJOINT's tiling so the comparison is like for
    // like: `adjoint::Options` defaults are tile 256, lane_tile 8, which the Interpreter shares.
    epykos::exec::Options uo;
    uo.max_batch = max_batch;
    uo.tile = prog->whole_adjoint().options().tile;
    uo.lane_tile = prog->whole_adjoint().options().lane_tile;
    uo.fuse_reductions = false;
    uo.fuse_pairs = false;
    uo.inline_producers = false;
    unfused = std::make_unique<epykos::exec::Interpreter>(prog->program(), uo);
  }

  // One run at the record quotes, so `full_state` exists, then the solved full state kept
  // lane-major.
  std::vector<double> solved_lane;
  void fill_solved(int) {
    std::vector<double> st(uz(n_q)), out(uz(n_out));
    for (int k = 0; k < n_q; ++k) st[uz(k)] = s.quotes[uz(k)];
    prog->run(st.data(), 1, out.data());
    solved_lane.assign(uz(n_in), 0.0);
    const double* lane = prog->full_state(0);
    for (int o = 0; o < n_in; ++o) solved_lane[uz(o)] = lane[o];
  }

  // The solved full state in the SoA layout exec::Interpreter and adjoint::Adjoint want, at
  // stride `B` -- EVERY buffer these engines read is strided by the B of the CALL, not by
  // max_batch, which is the trap the fixture avoids by sizing per chunk. Called outside every
  // clock.
  const double* solved_for(int B) {
    solved_soa.assign(uz(n_in) * uz(B), 0.0);
    for (int o = 0; o < n_in; ++o) {
      for (int b = 0; b < B; ++b) solved_soa[uz(o) * uz(B) + uz(b)] = solved_lane[uz(o)];
    }
    return solved_soa.data();
  }

  std::vector<int> ordinals(int R) const {
    std::vector<int> v;
    if (R == 1) {
      v.push_back(tape.book_output);
      return v;
    }
    // R per-TRADE rows (O2), which is what a desk ladder is; the book row is row 0's analogue and
    // is reported on its own at R = 1.
    for (int r = 0; r < R; ++r) v.push_back(tape.pv_outputs[uz(r % static_cast<int>(tape.pv_outputs.size()))]);
    return v;
  }
};

int chunks_of(int R, int B) { return (R + B - 1) / B; }

// ---- the timed bodies ------------------------------------------------------------------------
//
// Each returns the time for ONE whole ladder-equivalent at row count R: the chunk loop is the
// ladder's own, so the numbers are directly comparable and directly subtractable.

struct Buffers {
  std::vector<double> st, ob, out, sb, rows;
  void size(int n_q, int n_in, int n_out, int B, int R) {
    st.assign(uz(n_q) * uz(B), 0.0);
    ob.assign(uz(n_out) * uz(B), 0.0);
    out.assign(uz(n_out) * uz(B), 0.0);
    sb.assign(uz(n_in) * uz(B), 0.0);
    rows.assign(uz(R) * uz(n_q), 0.0);
  }
};

// L: the shipped ladder, exactly as h2h calls it.
double time_L(Harness& h, const std::vector<int>& ord, std::vector<double>* out) {
  const clock_type::time_point t = clock_type::now();
  const std::vector<double> rows = fixtures::compare_ois_ladder(*h.prog, h.s.quotes, ord, out);
  const double us = us_since(t);
  g_sink += rows.empty() ? 0.0 : rows[0];
  return us;
}

// D: ImplicitProgram::adjoint over the ladder's chunks, with the buffers pre-allocated and the
// seed re-set OUTSIDE the clock. Everything the ladder does except its own marshalling.
//
// `wide_zero` chooses how the seed is cleared, and the choice is itself a measurement. The fixture
// zeroes the WHOLE n_out x B out_bar buffer per chunk; a caller that remembered which entries it
// set would clear B of them. Both are timed (`D` and `Dmin`) and the difference prices that
// zeroing, which at 1,053 outputs and B = 64 is 539 KB a chunk.
double time_D(Harness& h, const std::vector<int>& ord, Buffers& bf, int B, bool wide_zero) {
  const int R = static_cast<int>(ord.size());
  double total = 0.0;
  std::vector<std::size_t> set_last;
  for (int r0 = 0; r0 < R; r0 += B) {
    const int b_n = std::min(B, R - r0);
    for (int k = 0; k < h.n_q; ++k) {
      for (int b = 0; b < b_n; ++b) bf.st[uz(k) * uz(b_n) + uz(b)] = h.s.quotes[uz(k)];
    }
    if (wide_zero) {
      std::fill(bf.ob.begin(), bf.ob.begin() + static_cast<std::ptrdiff_t>(uz(h.n_out) * uz(b_n)), 0.0);
    } else {
      for (std::size_t at : set_last) bf.ob[at] = 0.0;
      set_last.clear();
    }
    for (int b = 0; b < b_n; ++b) {
      const std::size_t at = uz(ord[uz(r0 + b)]) * uz(b_n) + uz(b);
      bf.ob[at] = 1.0;
      if (!wide_zero) set_last.push_back(at);
    }
    const clock_type::time_point t = clock_type::now();
    h.prog->adjoint(bf.st.data(), b_n, bf.ob.data(), bf.out.data(), bf.sb.data());
    total += us_since(t);
  }
  g_sink += bf.sb[0];
  return total;
}

// M: `compare_ois_ladder`'s body with the `program.adjoint(...)` call REMOVED and nothing else
// changed -- the same four `.assign()`s per chunk, the same seed loop, the same transposing row
// copy-out, the same returned vector. The fixture's own marshalling, measured DIRECTLY.
//
// It has to be direct. At R >= 64 the ladder is ~12 ms a chunk, so L - D is a difference of two
// 47,000 us medians and a 1% wobble on either swamps a 400 us term: the subtraction is below its
// own noise floor exactly where the marshalling matters most. Mirroring the body is duplication,
// and the duplication is the point -- it is the only way to put a clock around this phase without
// editing the fixture.
double time_M(Harness& h, const std::vector<int>& ord, int B) {
  const int n_q = h.n_q, n_out = h.n_out, n_in = h.n_in;
  const std::size_t R = ord.size();
  const clock_type::time_point t = clock_type::now();
  std::vector<double> rows(R * uz(n_q), 0.0);
  std::vector<double> out_v(uz(n_out), 0.0);
  std::vector<double> st, ob, o, sb;
  for (std::size_t r0 = 0; r0 < R; r0 += uz(B)) {
    const std::size_t b_n = std::min(uz(B), R - r0);
    st.assign(uz(n_q) * b_n, 0.0);
    ob.assign(uz(n_out) * b_n, 0.0);
    o.assign(uz(n_out) * b_n, 0.0);
    sb.assign(uz(n_in) * b_n, 0.0);
    for (std::size_t k = 0; k < uz(n_q); ++k) {
      for (std::size_t b = 0; b < b_n; ++b) st[k * b_n + b] = h.s.quotes[k];
    }
    for (std::size_t b = 0; b < b_n; ++b) ob[uz(ord[r0 + b]) * b_n + b] = 1.0;
    // program.adjoint(...) WOULD BE HERE.
    for (std::size_t b = 0; b < b_n; ++b) {
      for (std::size_t k = 0; k < uz(n_q); ++k) rows[(r0 + b) * uz(n_q) + k] = sb[k * b_n + b];
    }
    if (r0 == 0) {
      for (std::size_t oo = 0; oo < uz(n_out); ++oo) out_v[oo] = o[oo * b_n];
    }
  }
  const double us = us_since(t);
  g_sink += rows[0] + out_v[0];
  return us;
}

// R: ImplicitProgram::run over the same chunks -- solve WITHOUT factors, to_soa, the interpreter.
double time_Rrun(Harness& h, int R, Buffers& bf, int B) {
  double total = 0.0;
  for (int r0 = 0; r0 < R; r0 += B) {
    const int b_n = std::min(B, R - r0);
    for (int k = 0; k < h.n_q; ++k) {
      for (int b = 0; b < b_n; ++b) bf.st[uz(k) * uz(b_n) + uz(b)] = h.s.quotes[uz(k)];
    }
    const clock_type::time_point t = clock_type::now();
    h.prog->run(bf.st.data(), b_n, bf.out.data());
    total += us_since(t);
  }
  g_sink += bf.out[0];
  return total;
}

// A: adjoint::Adjoint::run at the solved state -- forward + reverse, no solve, no IFT, no to_soa.
double time_A(Harness& h, const std::vector<int>& ord, Buffers& bf, int B) {
  const int R = static_cast<int>(ord.size());
  double total = 0.0;
  for (int r0 = 0; r0 < R; r0 += B) {
    const int b_n = std::min(B, R - r0);
    const double* soa = h.solved_for(b_n);
    std::fill(bf.ob.begin(), bf.ob.begin() + static_cast<std::ptrdiff_t>(uz(h.n_out) * uz(b_n)), 0.0);
    for (int b = 0; b < b_n; ++b) bf.ob[uz(ord[uz(r0 + b)]) * uz(b_n) + uz(b)] = 1.0;
    const clock_type::time_point t = clock_type::now();
    h.prog->whole_adjoint().run(soa, b_n, bf.ob.data(), bf.out.data(), bf.sb.data());
    total += us_since(t);
  }
  g_sink += bf.sb[0];
  return total;
}

// F: exec::Interpreter::run at the solved state, as the ladder's own forward -- the SHIPPED,
// fused, inlined interpreter. A LOWER bound on the forward half inside A, and a loose one.
double time_F(Harness& h, int R, Buffers& bf, int B) {
  double total = 0.0;
  for (int r0 = 0; r0 < R; r0 += B) {
    const int b_n = std::min(B, R - r0);
    const double* soa = h.solved_for(b_n);
    const clock_type::time_point t = clock_type::now();
    h.prog->interpreter().run(soa, b_n, bf.out.data());
    total += us_since(t);
  }
  g_sink += bf.out[0];
  return total;
}

// F0: the same program through the MATERIALISING interpreter -- every domain's rows written, no
// fuse, no inline, the Adjoint's own tiling. This is the forward half that actually sits inside
// `Adjoint::run`, so A - F0 is the TIGHT upper bound on the reverse pass and A - F is not.
double time_F0(Harness& h, int R, Buffers& bf, int B) {
  double total = 0.0;
  for (int r0 = 0; r0 < R; r0 += B) {
    const int b_n = std::min(B, R - r0);
    const double* soa = h.solved_for(b_n);
    const clock_type::time_point t = clock_type::now();
    h.unfused->run(soa, b_n, bf.out.data());
    total += us_since(t);
  }
  g_sink += bf.out[0];
  return total;
}

// ---- eviction (D91 §3) ------------------------------------------------------------------------

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

// ---- reporting --------------------------------------------------------------------------------

void banner(const char* title) {
  const std::size_t len = std::strlen(title);
  std::printf("\n==== %s %s\n", title, std::string(len >= 76 ? 4 : 76 - len, '=').c_str());
}

void print_loads(const char* tag, double before, double after) {
  std::printf("  [%s] 1-minute load %.2f before, %.2f after; %d logical cores (threshold %.1f)\n", tag, before, after,
              logical_cores(), static_cast<double>(logical_cores()) / 2.0);
}

// Least squares for y = a*x + c. Two unknowns, closed form.
bool fit2(const std::vector<double>& x, const std::vector<double>& y, double* a, double* c) {
  const std::size_t n = y.size();
  if (n < 2) return false;
  double sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0;
  for (std::size_t i = 0; i < n; ++i) {
    sx += x[i];
    sy += y[i];
    sxx += x[i] * x[i];
    sxy += x[i] * y[i];
  }
  const double nn = static_cast<double>(n);
  const double den = nn * sxx - sx * sx;
  if (std::abs(den) < 1e-300) return false;
  *a = (nn * sxy - sx * sy) / den;
  *c = (sy - *a * sx) / nn;
  return true;
}

// Least squares for y = a*x1 + b*x2 + c over the rows of (x1, x2, y). Three unknowns, normal
// equations, Gauss-Jordan -- the same shape D91 fitted by hand.
bool fit3(const std::vector<double>& x1, const std::vector<double>& x2, const std::vector<double>& y, double* a,
          double* b, double* c) {
  const std::size_t n = y.size();
  if (n < 3) return false;
  double M[3][4] = {{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};
  for (std::size_t i = 0; i < n; ++i) {
    const double v[3] = {x1[i], x2[i], 1.0};
    for (int r = 0; r < 3; ++r) {
      for (int k = 0; k < 3; ++k) M[r][k] += v[r] * v[k];
      M[r][3] += v[r] * y[i];
    }
  }
  for (int col = 0; col < 3; ++col) {
    int piv = col;
    for (int r = col + 1; r < 3; ++r) {
      if (std::abs(M[r][col]) > std::abs(M[piv][col])) piv = r;
    }
    if (std::abs(M[piv][col]) < 1e-300) return false;
    if (piv != col) {
      for (int k = 0; k < 4; ++k) std::swap(M[col][k], M[piv][k]);
    }
    const double d = M[col][col];
    for (int k = 0; k < 4; ++k) M[col][k] /= d;
    for (int r = 0; r < 3; ++r) {
      if (r == col) continue;
      const double f = M[r][col];
      for (int k = 0; k < 4; ++k) M[r][k] -= f * M[col][k];
    }
  }
  *a = M[0][3];
  *b = M[1][3];
  *c = M[2][3];
  return true;
}

// ---- modes ------------------------------------------------------------------------------------

void mode_shape(const Config& c) {
  banner("shape -- structural facts, no timing");
  Harness h(c, c.batch, true, true);
  h.fill_solved(c.batch);
  std::printf("  %d trades, %d calibration quotes, seed %llu, max_batch %d\n", c.trades, h.n_q,
              static_cast<unsigned long long>(c.seed), c.batch);
  std::printf("  tape %zu nodes raw -> %zu after the passes; %d tape inputs, %d tape outputs\n", h.tape.nodes_raw,
              h.tape.nodes_after_passes, h.n_in, h.n_out);
  const epykos::ir::Program& p = h.prog->program();
  std::printf("  whole program %zu domains, %zu values\n", p.domains.size(), p.num_values());
  const epykos::adjoint::Adjoint& a = h.prog->whole_adjoint();
  std::printf("  adjoint buffers: values+vbar %zu B, edges %zu B, scratch %zu B, tables %zu B, total %.2f MB\n",
              a.value_bytes(), a.edge_bytes(), a.scratch_bytes(), a.table_bytes(),
              static_cast<double>(a.value_bytes() + a.edge_bytes() + a.scratch_bytes() + a.table_bytes()) / 1048576.0);
  const solver::ResidualProgram& rp = h.prog->residual_program(0);
  std::printf("  block 0: %d unknowns, %d residuals, %d parameters; residual IR %zu domains, %zu values\n",
              rp.n_unknowns(), rp.n_residuals(), rp.n_params(), rp.program().domains.size(), rp.program().num_values());
  std::printf("  O2 outputs available as ladder rows: %zu (plus the book row)\n", h.tape.pv_outputs.size());
  // The load-bearing assumption behind the reverse pass's share: that the UNFUSED interpreter
  // materialises what `adjoint_e0.cpp`'s forward() materialises. Counted here rather than
  // asserted, because the whole headline rests on it.
  // `Interpreter::describe` prints "<n> rows fused or inlined, never materialised" and one
  // "inlined into dK" per inlined domain. Both are what the proxy has to drive to zero.
  auto count = [](const std::string& s, const char* needle) {
    std::size_t n = 0;
    for (std::size_t at = s.find(needle); at != std::string::npos; at = s.find(needle, at + 1)) ++n;
    return n;
  };
  auto fused_rows = [](const std::string& s) {
    const char* key = " rows fused or inlined";
    const std::size_t at = s.find(key);
    if (at == std::string::npos) return std::string("?");
    std::size_t b = at;
    while (b > 0 && (std::isdigit(static_cast<unsigned char>(s[b - 1])) != 0)) --b;
    return s.substr(b, at - b);
  };
  const std::string fused = h.prog->interpreter().describe();
  const std::string plain = h.unfused->describe();
  std::printf("  forward proxy check, over %zu domains:\n", p.domains.size());
  std::printf("    shipped interpreter : %s rows fused-or-inlined, %zu domains inlined into another\n",
              fused_rows(fused).c_str(), count(fused, "inlined into d"));
  std::printf("    unfused interpreter : %s rows fused-or-inlined, %zu domains inlined into another\n",
              fused_rows(plain).c_str(), count(plain, "inlined into d"));
  std::printf("    The SECOND is the proxy for A's forward half, because adjoint_e0.cpp's forward()\n");
  std::printf("    materialises every domain's rows unconditionally. Both numbers must be 0 for the\n");
  std::printf("    proxy to be exact; any residue makes the proxy CHEAPER than A's forward and so\n");
  std::printf("    OVERSTATES the reverse pass's share. Reported either way.\n");
  std::printf("\n  %-8s %-8s %-10s %-12s %-14s %s\n", "rows", "chunks", "solves", "shared", "resid_evals", "jacobians");
  for (int R : c.rows) {
    const std::vector<int> ord = h.ordinals(R);
    std::vector<double> out;
    const std::vector<double> rows = fixtures::compare_ois_ladder(*h.prog, h.s.quotes, ord, &out);
    g_sink += rows[0];
    const solver::ImplicitProgram::RunStats& st = h.prog->last_run();
    std::printf("  %-8d %-8d %-10d %-12d %-14zu %zu     (last chunk's stats)\n", R, chunks_of(R, c.batch), st.solves,
                st.shared_lanes, st.residual_evaluations, st.jacobians);
  }
  std::printf("\n  NOTE: RunStats is per CALL, so the row above is the LAST chunk only. The ladder's\n");
  std::printf("  total solves over a call is one per chunk (dedup_lanes collapses the identical lanes).\n");
}

// Fit one phase's medians against (rows, chunks) and print the regression. D91's method: a
// parameter is swept so the counts vary, and least squares separates the terms. Here the sweep is
// the row count and the two counts are the rows (per-lane work) and the chunks (per-solve work),
// which the ceiling in `chunks_of` makes genuinely independent.
void fit_phase(const char* name, const std::vector<double>& xr, const std::vector<double>& xc,
               const std::vector<double>& y) {
  double a = 0.0, b = 0.0, k = 0.0;
  if (!fit3(xr, xc, y, &a, &b, &k)) {
    std::printf("  %-26s (fit failed)\n", name);
    return;
  }
  double worst = 0.0;
  for (std::size_t i = 0; i < y.size(); ++i) {
    if (y[i] <= 0.0) continue;
    worst = std::max(worst, 100.0 * std::abs(a * xr[i] + b * xc[i] + k - y[i]) / y[i]);
  }
  // The same data with the chunks regressor dropped. For a phase that is structurally per-lane
  // only -- A and F make one call per chunk and the chunking is the ladder's, not theirs -- the
  // chunks column is nearly collinear with the constant, and a two-term fit is the honest model.
  // Both are printed so the reader can see which model the data prefers rather than take mine.
  double a2 = 0.0, k2 = 0.0;
  double worst2 = -1.0;
  if (fit2(xr, y, &a2, &k2)) {
    worst2 = 0.0;
    for (std::size_t i = 0; i < y.size(); ++i) {
      if (y[i] <= 0.0) continue;
      worst2 = std::max(worst2, 100.0 * std::abs(a2 * xr[i] + k2 - y[i]) / y[i]);
    }
  }
  std::printf("  %-26s = %9.4f*rows + %8.2f*chunks + %8.2f us   worst err %6.2f%%", name, a, b, k, worst);
  if (worst2 >= 0.0) std::printf("   | rows-only: %8.4f*rows + %8.2f, worst %6.2f%%", a2, k2, worst2);
  std::printf("\n");
}

void mode_phases(const Config& c) {
  banner("phases -- the subtraction ladder, and a regression per phase");
  Harness h(c, c.batch, true, true);
  h.fill_solved(c.batch);
  Buffers bf;
  const double l0 = load_1min();
  require_quiet_box("phases");
  std::printf("  reps %d, batch %d; medians in us. Fit over rows >= %d only -- see the note below.\n", c.reps,
              c.batch, c.fit_from);
  std::printf("  L  compare_ois_ladder            the shipped ladder, as h2h calls it\n");
  std::printf("  D  ImplicitProgram::adjoint      solve(factors) + to_soa + A + IFT/lane + copy-out\n");
  std::printf("  D' the same with a narrow seed   D with B out_bar entries cleared, not n_out x B\n");
  std::printf("  A  Adjoint::run                  forward + reverse at the solved state\n");
  std::printf("  F  Interpreter::run              the SHIPPED fused/inlined forward (loose lower bound)\n");
  std::printf("  F0 Interpreter, no fuse/inline   every domain materialised: what A's forward half IS\n");
  std::printf("  R  ImplicitProgram::run          solve(no factors) + to_soa + F\n");
  std::printf("  M  the fixture body, no adjoint  the ladder's own marshalling, measured DIRECTLY\n\n");
  std::printf("  %-6s %-7s %9s %9s %9s %9s %9s %9s %9s %9s %8s %9s %9s %9s\n", "rows", "chunks", "L", "D", "D'", "A",
              "F0", "F", "R", "M", "L-D", "D-A", "A-F0", "A-F");
  std::vector<double> xr, xc, yL, yD, yDm, yA, yF, yF0, yR, yM, yDA, yAF0, yAF, yLD;
  for (int R : c.rows) {
    const std::vector<int> ord = h.ordinals(R);
    bf.size(h.n_q, h.n_in, h.n_out, c.batch, R);
    std::vector<double> out_v;
    Samples L, D, Dm, A, F, F0, Rr, M;
    // One untimed pass of each so nothing in the list is the first call of its kind.
    time_L(h, ord, &out_v);
    time_D(h, ord, bf, c.batch, true);
    time_D(h, ord, bf, c.batch, false);
    time_A(h, ord, bf, c.batch);
    time_F0(h, R, bf, c.batch);
    time_F(h, R, bf, c.batch);
    time_Rrun(h, R, bf, c.batch);
    time_M(h, ord, c.batch);
    for (int i = 0; i < c.reps; ++i) {
      L.add(time_L(h, ord, &out_v));
      D.add(time_D(h, ord, bf, c.batch, true));
      Dm.add(time_D(h, ord, bf, c.batch, false));
      A.add(time_A(h, ord, bf, c.batch));
      F0.add(time_F0(h, R, bf, c.batch));
      F.add(time_F(h, R, bf, c.batch));
      Rr.add(time_Rrun(h, R, bf, c.batch));
      M.add(time_M(h, ord, c.batch));
    }
    std::printf("  %-6d %-7d %9.1f %9.1f %9.1f %9.1f %9.1f %9.1f %9.1f %9.1f %8.1f %9.1f %9.1f %9.1f\n", R,
                chunks_of(R, c.batch), L.med(), D.med(), Dm.med(), A.med(), F0.med(), F.med(), Rr.med(), M.med(),
                L.med() - D.med(), D.med() - A.med(), A.med() - F0.med(), A.med() - F.med());
    if (R < c.fit_from) continue;
    xr.push_back(static_cast<double>(R));
    xc.push_back(static_cast<double>(chunks_of(R, c.batch)));
    yL.push_back(L.med());
    yD.push_back(D.med());
    yDm.push_back(Dm.med());
    yA.push_back(A.med());
    yF.push_back(F.med());
    yF0.push_back(F0.med());
    yR.push_back(Rr.med());
    yM.push_back(M.med());
    yDA.push_back(D.med() - A.med());
    yAF0.push_back(A.med() - F0.med());
    yAF.push_back(A.med() - F.med());
    yLD.push_back(L.med() - D.med());
  }
  const double l1 = load_1min();
  print_loads("phases", l0, l1);
  std::printf("\n  regressions over the %zu rows >= %d (D91's method: the row count is swept, least squares)\n",
              yL.size(), c.fit_from);
  fit_phase("L  ladder", xr, xc, yL);
  fit_phase("D  ImplicitProgram::adjoint", xr, xc, yD);
  fit_phase("D' narrow seed", xr, xc, yDm);
  fit_phase("A  Adjoint::run (fwd+rev)", xr, xc, yA);
  fit_phase("F0 materialising forward", xr, xc, yF0);
  fit_phase("F  shipped fused forward", xr, xc, yF);
  fit_phase("R  ImplicitProgram::run", xr, xc, yR);
  fit_phase("M  marshalling (direct)", xr, xc, yM);
  fit_phase("D-A  solve + IFT + marshal", xr, xc, yDA);
  fit_phase("A-F0 reverse pass (BOUND)", xr, xc, yAF0);
  fit_phase("A-F  rev + unfused fwd", xr, xc, yAF);
  fit_phase("L-D  fixture marshalling", xr, xc, yLD);
  std::printf("\n  WHY THE FIT EXCLUDES THE SMALL ROW COUNTS, and D91 §1 excluded rows for the same\n");
  std::printf("  kind of reason (it fitted the six chord-only points because a Jacobian refresh is a\n");
  std::printf("  different model). Here three things make the small counts a different model:\n");
  std::printf("    * R = 1 is a DIFFERENT QUANTITY -- the book PV row, which is what h2h asks for --\n");
  std::printf("      while R > 1 are per-trade rows. It is reported on its own, never fitted.\n");
  std::printf("    * B = 2 takes adjoint_e0.cpp's GENERIC Lanes<0> kernel: the switch specialises\n");
  std::printf("      1, 4, 8, 16, 32 and 64 lanes and nothing else, so R = 2 costs more than R = 4.\n");
  std::printf("    * below one full lane_tile the per-row cost has not reached its asymptote.\n");
  std::printf("  A-F0 is an UPPER BOUND on the reverse pass, not an estimate; A-F is a far looser one\n");
  std::printf("  and is printed only to show how much looser (see this file's header).\n");
  std::printf("  At --trades 256 with the default tenors this program is the one\n");
  std::printf("  `revcollapse \"compare_ois(telescoped, 256)\"` counts, so (A-F0)/F0 here and its\n");
  std::printf("  reverse-only/forward-half OPS ratio are directly divisible: see this file's header\n");
  std::printf("  for that comparison and what it does and does not license about D102.\n");
  std::printf("  In D-A the ROWS slope is the per-lane IFT contraction plus its bookkeeping and the\n");
  std::printf("  CHUNKS slope is the per-chunk solve (one residual evaluation, the diagnostic, the\n");
  std::printf("  exit Jacobian and its LU) plus to_soa. Neither needed an isolated call -- but rows\n");
  std::printf("  and chunks are near-collinear once chunks = R/batch, so a NEGATIVE chunks slope here\n");
  std::printf("  means the split is unidentified from this sweep alone and the 'batch' mode is what\n");
  std::printf("  separates them. Reported, not hidden.\n");
}

void mode_sweep(const Config& c) {
  banner("sweep -- the ladder over row count, and the regression");
  Harness h(c, c.batch, true, true);
  h.fill_solved(c.batch);
  const double l0 = load_1min();
  require_quiet_box("sweep");
  std::printf("  reps %d, batch %d\n\n", c.reps, c.batch);
  std::printf("  %-6s %-7s %10s %10s %10s %10s\n", "rows", "chunks", "min us", "med us", "p90 us", "us/row");
  std::vector<double> xr, xc, y;
  for (int R : c.rows) {
    const std::vector<int> ord = h.ordinals(R);
    std::vector<double> out_v;
    Samples L;
    time_L(h, ord, &out_v);
    for (int i = 0; i < c.reps; ++i) L.add(time_L(h, ord, &out_v));
    std::printf("  %-6d %-7d %10.1f %10.1f %10.1f %10.3f\n", R, chunks_of(R, c.batch), L.min(), L.med(), L.p90(),
                L.med() / static_cast<double>(R));
    xr.push_back(static_cast<double>(R));
    xc.push_back(static_cast<double>(chunks_of(R, c.batch)));
    y.push_back(L.med());
  }
  const double l1 = load_1min();
  print_loads("sweep", l0, l1);
  double a = 0.0, b = 0.0, k = 0.0;
  if (fit3(xr, xc, y, &a, &b, &k)) {
    std::printf("\n  fit: total = %.3f*rows + %.2f*chunks + %.2f us\n", a, b, k);
    double worst = 0.0;
    std::printf("  %-6s %-7s %10s %10s %9s\n", "rows", "chunks", "measured", "fitted", "err %");
    for (std::size_t i = 0; i < y.size(); ++i) {
      const double f = a * xr[i] + b * xc[i] + k;
      const double e = 100.0 * std::abs(f - y[i]) / y[i];
      worst = std::max(worst, e);
      std::printf("  %-6.0f %-7.0f %10.1f %10.1f %9.2f\n", xr[i], xc[i], y[i], f, e);
    }
    std::printf("  worst relative fit error %.2f%%\n", worst);
  }
}

void mode_batch(const Config& c) {
  banner("batch -- the same ladder over max_batch, which separates per-chunk from per-lane");
  const double l0 = load_1min();
  require_quiet_box("batch");
  const int R = c.rows.empty() ? 256 : c.rows.back();
  std::printf("  %d rows, reps %d; a NEW ImplicitProgram per batch size (max_batch sizes the buffers)\n\n", R, c.reps);
  std::printf("  %-10s %-8s %10s %10s %10s %12s\n", "max_batch", "chunks", "min us", "med us", "p90 us", "us/row");
  for (int B : {1, 2, 4, 8, 16, 32, 64}) {
    Harness h(c, B, true, true);
    h.fill_solved(B);
    const std::vector<int> ord = h.ordinals(R);
    std::vector<double> out_v;
    Samples L;
    time_L(h, ord, &out_v);
    for (int i = 0; i < c.reps; ++i) L.add(time_L(h, ord, &out_v));
    std::printf("  %-10d %-8d %10.1f %10.1f %10.1f %12.3f\n", B, chunks_of(R, B), L.min(), L.med(), L.p90(),
                L.med() / static_cast<double>(R));
  }
  const double l1 = load_1min();
  print_loads("batch", l0, l1);
}

void mode_evict(const Config& c) {
  banner("evict -- D91 section 3's residency probe, on the ladder");
  Harness h(c, c.batch, true, true);
  h.fill_solved(c.batch);
  const double l0 = load_1min();
  require_quiet_box("evict");
  std::printf("  reps %d, batch %d; a buffer of the stated size is walked between consecutive ladders\n\n", c.reps,
              c.batch);
  std::printf("  %-6s", "rows");
  for (int mb : c.evict_mb) std::printf(" %9d MB", mb);
  std::printf("   %10s %10s\n", "64MB/none", "last/none");
  for (int R : c.rows) {
    const std::vector<int> ord = h.ordinals(R);
    std::vector<double> out_v;
    std::vector<double> med;
    for (int mb : c.evict_mb) {
      const std::size_t bytes = static_cast<std::size_t>(mb) * 1048576u;
      Samples L;
      evict(bytes);
      time_L(h, ord, &out_v);
      for (int i = 0; i < c.reps; ++i) {
        evict(bytes);
        L.add(time_L(h, ord, &out_v));
      }
      med.push_back(L.med());
    }
    std::printf("  %-6d", R);
    for (double m : med) std::printf(" %12.1f", m);
    // Two ratios, because one of them is the comparable quantity and the other is not: D91 §3's
    // table tops out at 64 MB, so 64MB/none is what compares with its 2.52x, and last/none is
    // whatever `--evict-mb` happened to end on.
    double at64 = -1.0;
    for (std::size_t i = 0; i < c.evict_mb.size() && i < med.size(); ++i) {
      if (c.evict_mb[i] == 64) at64 = med[i];
    }
    const double base = (med.empty() || med[0] <= 0.0) ? 0.0 : med[0];
    if (at64 > 0.0 && base > 0.0) {
      std::printf("   %9.2fx", at64 / base);
    } else {
      std::printf("   %10s", "n/a");
    }
    std::printf(" %9.2fx\n", base > 0.0 ? med.back() / base : 0.0);
  }
  const double l1 = load_1min();
  print_loads("evict", l0, l1);
}

void mode_unit(const Config& c) {
  banner("unit -- isolated unit costs: the route D91 section 1 DISCARDED");
  Harness h(c, c.batch, true, true);
  h.fill_solved(c.batch);
  const double l0 = load_1min();
  require_quiet_box("unit");
  // A PRIVATE ResidualProgram over the same block, so nothing here perturbs the object the other
  // modes time.
  const solver::ImplicitBlock& blk = h.prog->block(0);
  solver::ResidualProgram rp(h.tape.tape, blk, h.prog->options().residual_passes, 64);
  const int n_z = rp.n_unknowns(), n_r = rp.n_residuals(), n_p = rp.n_params();
  std::vector<double> z(uz(n_z)), p(uz(n_p)), F(uz(n_r)), Jz(uz(n_r) * uz(n_z)), Jp(uz(n_r) * uz(n_p));
  const double* full = h.prog->full_state(0);
  for (int j = 0; j < n_z; ++j) z[uz(j)] = full[blk.unknowns[uz(j)]];
  for (int m = 0; m < n_p; ++m) p[uz(m)] = full[rp.param_ordinals()[uz(m)]];
  solver::Factors fac;
  rp.jacobian(z.data(), p.data(), F.data(), Jz.data(), n_p > 0 ? Jp.data() : nullptr);
  fac.compute(n_r, n_z, n_p, Jz.data(), n_p > 0 ? Jp.data() : nullptr);
  std::printf("  block 0: %d unknowns, %d residuals, %d parameters; Factors %zu bytes, square %s\n", n_z, n_r, n_p,
              fac.bytes(), fac.square() ? "yes" : "no");

  Samples ev, jac, lu, ift, st, jt;
  std::vector<double> zb(uz(n_z), 1.0), lam(uz(n_r), 0.0), pb(uz(n_p), 0.0), jtz(uz(n_z), 0.0), Ft(uz(n_r), 0.0);
  for (int i = 0; i < c.reps; ++i) {
    clock_type::time_point t = clock_type::now();
    rp.evaluate(z.data(), p.data(), F.data());
    ev.add(us_since(t));
    t = clock_type::now();
    rp.jacobian(z.data(), p.data(), F.data(), Jz.data(), n_p > 0 ? Jp.data() : nullptr);
    jac.add(us_since(t));
    t = clock_type::now();
    fac.compute(n_r, n_z, n_p, Jz.data(), n_p > 0 ? Jp.data() : nullptr);
    lu.add(us_since(t));
    t = clock_type::now();
    fac.ift_adjoint(zb.data(), lam.data(), pb.data());
    ift.add(us_since(t));
    t = clock_type::now();
    fac.solve_transposed(zb.data(), lam.data());
    st.add(us_since(t));
    t = clock_type::now();
    rp.jt_product(z.data(), p.data(), F.data(), Ft.data(), jtz.data(), nullptr);
    jt.add(us_since(t));
    g_sink += F[0] + Jz[0] + lam[0] + pb[0] + jtz[0];
  }
  const double l1 = load_1min();
  std::printf("\n  %-34s %10s %10s %10s\n", "isolated call", "min us", "med us", "p90 us");
  auto row = [](const char* n, const Samples& s) {
    std::printf("  %-34s %10.3f %10.3f %10.3f\n", n, s.min(), s.med(), s.p90());
  };
  row("ResidualProgram::evaluate", ev);
  row("ResidualProgram::jacobian (J_z, J_p)", jac);
  row("Factors::compute (LU)", lu);
  row("ResidualProgram::jt_product (diag)", jt);
  row("Factors::ift_adjoint (per lane)", ift);
  row("Factors::solve_transposed (per lane)", st);
  std::printf("\n  per-chunk solve, from these: evaluate + jacobian + LU + jt_product = %.2f us\n",
              ev.med() + jac.med() + lu.med() + jt.med());
  std::printf("  per-lane IFT, from these: ift_adjoint x batch = %.2f us at B = %d\n", ift.med() * c.batch, c.batch);
  print_loads("unit", l0, l1);
  std::printf("  D91 section 1: an isolated call runs COLDER than the same call inside the loop, so these\n");
  std::printf("  over-attribute. The 'phases' mode's subtraction is the number that stands.\n");
}

void mode_toggle(const Config& c) {
  banner("toggle -- the diagnostic and the exit Jacobian off, as ATTRIBUTION DEVICES only");
  const double l0 = load_1min();
  require_quiet_box("toggle");
  std::printf("  reps %d, batch %d. NEITHER configuration is a proposal: D85 section 3 shows a ladder\n", c.reps,
              c.batch);
  std::printf("  without the exit Jacobian is 6.5%%-24%% WRONG at a moved market, and D94 shows the\n");
  std::printf("  diagnostic off returns NaN. They are switched off here to price them and nothing else.\n\n");
  struct Row {
    const char* name;
    bool diag;
    bool fj;
  };
  const Row rows[] = {{"both on (shipped)", true, true},
                      {"diagnostic OFF", false, true},
                      {"exit Jacobian OFF", true, false},
                      {"both OFF", false, false}};
  std::printf("  %-22s", "configuration");
  for (int R : c.rows) std::printf(" %8d", R);
  std::printf("   rows\n");
  std::vector<std::vector<double>> meds;
  std::vector<double> ladder_ref;
  for (const Row& r : rows) {
    Harness h(c, c.batch, r.diag, r.fj);
    h.fill_solved(c.batch);
    std::vector<double> med;
    for (int R : c.rows) {
      const std::vector<int> ord = h.ordinals(R);
      std::vector<double> out_v;
      Samples L;
      time_L(h, ord, &out_v);
      for (int i = 0; i < c.reps; ++i) L.add(time_L(h, ord, &out_v));
      med.push_back(L.med());
    }
    // The ladder's own answer under this configuration, at R = 1, against the shipped one.
    const std::vector<int> ord1 = h.ordinals(1);
    std::vector<double> out_v;
    const std::vector<double> got = fixtures::compare_ois_ladder(*h.prog, h.s.quotes, ord1, &out_v);
    double worst = 0.0;
    if (ladder_ref.empty()) {
      ladder_ref = got;
    } else {
      double scale = 0.0;
      for (double v : ladder_ref) scale = std::max(scale, std::abs(v));
      if (scale <= 0.0) scale = 1.0;
      for (std::size_t i = 0; i < got.size() && i < ladder_ref.size(); ++i) {
        worst = std::max(worst, std::abs(got[i] - ladder_ref[i]) / scale);
      }
    }
    std::printf("  %-22s", r.name);
    for (double m : med) std::printf(" %8.1f", m);
    std::printf("   worst ladder diff vs shipped %.3e\n", worst);
    meds.push_back(med);
  }
  const double l1 = load_1min();
  print_loads("toggle", l0, l1);
  if (meds.size() == 4) {
    std::printf("\n  attributed, from the differences (shipped minus the switched-off configuration):\n");
    std::printf("  %-6s %12s %12s %12s %10s %10s\n", "rows", "shipped us", "diag us", "exit J us", "diag %", "exit J %");
    for (std::size_t i = 0; i < c.rows.size(); ++i) {
      const double base = meds[0][i];
      const double d = base - meds[1][i];
      const double j = base - meds[2][i];
      std::printf("  %-6d %12.1f %12.1f %12.1f %10.1f %10.1f\n", c.rows[i], base, d, j, 100.0 * d / base,
                  100.0 * j / base);
    }
  }
}

[[noreturn]] void die(const std::string& why) {
  std::cerr << "ladderprobe: " << why << "\n";
  std::exit(2);
}

std::vector<int> parse_ints(const std::string& list) {
  std::vector<int> v;
  for (std::size_t b = 0; b <= list.size();) {
    const std::size_t e = std::min(list.find(',', b), list.size());
    if (e > b) v.push_back(std::stoi(list.substr(b, e - b)));
    b = e + 1;
  }
  return v;
}

}  // namespace

int main(int argc, char** argv) {
  Config c;
  std::string mode = "all";
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> std::string {
      if (i + 1 >= argc) die("missing value for " + a);
      return argv[++i];
    };
    if (a == "--mode") mode = next();
    else if (a == "--trades") c.trades = std::stoi(next());
    else if (a == "--batch") c.batch = std::stoi(next());
    else if (a == "--reps") c.reps = std::stoi(next());
    else if (a == "--seed") c.seed = std::stoull(next());
    else if (a == "--rows") c.rows = parse_ints(next());
    else if (a == "--fit-from") c.fit_from = std::stoi(next());
    else if (a == "--evict-mb") c.evict_mb = parse_ints(next());
    else if (a == "--ignore-load") g_ignore_load = true;
    else if (a == "--tenors") {
      const std::string list = next();
      c.tenors.clear();
      for (std::size_t b = 0; b <= list.size();) {
        const std::size_t e = std::min(list.find(',', b), list.size());
        if (e > b) c.tenors.push_back(list.substr(b, e - b));
        b = e + 1;
      }
    } else die("unknown argument " + a);
  }
  if (c.rows.empty()) die("--rows given an empty list");

  try {
    std::printf("ladderprobe -- where the risk ladder's microseconds go\n");
    std::printf("  build %s, %s, flags %s%s\n", epykos::version(), epykos::build_config(), epykos::build_flags(),
                epykos::build_fp_contract_off() ? " (libepykos fp-contract off)" : "");
    std::printf("  %d logical cores, 1-minute load %.2f, refuse threshold %.1f%s\n", logical_cores(), load_1min(),
                static_cast<double>(logical_cores()) / 2.0, g_ignore_load ? "  (--ignore-load: NOT REPORTABLE)" : "");

    const bool all = mode == "all";
    if (all || mode == "shape") mode_shape(c);
    if (all || mode == "phases") mode_phases(c);
    if (all || mode == "sweep") mode_sweep(c);
    if (all || mode == "batch") mode_batch(c);
    if (all || mode == "unit") mode_unit(c);
    if (all || mode == "toggle") mode_toggle(c);
    if (all || mode == "evict") mode_evict(c);
    if (!all && mode != "shape" && mode != "phases" && mode != "sweep" && mode != "batch" && mode != "evict" &&
        mode != "unit" && mode != "toggle") {
      die("unknown mode '" + mode + "'");
    }
    std::printf("\n  final 1-minute load %.2f\n", load_1min());
    std::printf("  sink %.17g (ignore; it exists so the optimiser keeps the work)\n", static_cast<double>(g_sink));
  } catch (const std::exception& e) {
    std::cerr << "ladderprobe: " << e.what() << "\n";
    return 1;
  }
  return 0;
}
