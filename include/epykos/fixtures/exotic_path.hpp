// EpykosEngine — the exotics fixture: a scripted PATH-DEPENDENT payoff over a path grid
// (test-only, D28; the kill test of `docs/EXOTICS_KILL_TEST.md`).
//
// WHY THIS EXISTS. Every fixture in the tree before this one is an interest-rate swap: affine,
// shallow, and so heavily shared that D81's algebra collapses it 2,565x. D1's revisit clause --
// "revisit only if M1/M3 show the catalogue + interpreter cannot reach the gates" -- was therefore
// never tested on the shape `PRINCIPLES.md` §6 says the design targets and `PROBLEM.md` §8 calls
// Stage D. `include/epykos/mc/` is an empty directory. This fixture is the first recorded
// path-dependent payoff in the repository.
//
// THE MODEL. Black-Scholes GBM under the risk-neutral measure, simulated with EXACT log
// increments, which is the exact GBM law and not an Euler approximation of it -- so there is no
// discretisation bias in the path, which is what makes the geometric closed form below an exact
// check rather than an approximate one:
//
//   dt      = T / K,            t_k = k·dt,   k = 1..K
//   S_{k,p} = S_{k-1,p} · exp((r − q − sigma²/2)·dt + sigma·sqrt(dt)·eps_{k,p})
//
// eps is drawn once from the seed and is STRUCTURE (a Column, like affine_scan's eps), not an
// input: the inputs are the six market and contract parameters, in ordinal order
//
//   0: S0    1: r    2: q    3: sigma    4: strike X    5: up-barrier Bu
//
// THE PAYOFFS, all three over the same path, discounted at exp(−r·T). Written as a payoff SCRIPT
// would write them -- running accumulators inside the time loop, not a stored path folded
// afterwards -- because that is the shape a payoff language emits and the shape that stresses the
// recording discipline:
//
//   A_p = sum_k S_{k,p}                      arithmetic Asian:  DF·max(A_p/K − X, 0)
//   L_p = sum_k log S_{k,p}                  geometric Asian:   DF·max(exp(L_p/K) − X, 0)
//   M_p = max_k S_{k,p}                      up-and-out call:   DF·(M_p < Bu ? max(S_{K,p} − X, 0) : 0)
//
// Outputs: the three Monte Carlo means over paths, in that order; then, when `output_paths`, every
// per-path payoff (3·P more, payoff-major). The means are written as the natural left-associated
// add chain and left for `fold_sum` to bucket.
//
// WHERE THE RECORDING DISCIPLINE BITES, and what was written instead (CLAUDE.md; the evidence this
// fixture exists to produce). Four sites, every one of them a `select`, none a C++ `if`:
//
//   1. `max(A/K − X, 0)` and `max(S_K − X, 0)` -- a call payoff. NOT `std::max`: `epykos::max` on a
//      Scalar records a Select with both arms evaluated (scalar/rec.hpp). Found unqualified by
//      enclosing-namespace lookup, so the same source is the `double` reference.
//   2. The running extremum `M_k = max(S_k, M_{k-1})`. This puts a Select INSIDE a recurrence,
//      which no fixture in the tree did before.
//   3. The barrier's survival test `M_K < Bu`. Bu is an INPUT, so the predicate is tainted and
//      `structural_if` would throw by construction -- the discipline is enforced by the recorder
//      here, not by the author remembering. It is a `select`, both arms recorded.
//   4. `log(S_k)` for the geometric leg is unconditional, but note it is `log` of a product chain:
//      the engine is handed the definition and may collapse it or not (it does not; see the
//      kill-test results).
//
// Header-only so the including TU's contraction setting governs the arithmetic (D28): `double` is
// the reference oracle, `Rec` records, `Dual<N>` is the forward-mode check.
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "epykos/compile.hpp"
#include "epykos/rng/philox.hpp"
#include "epykos/scalar/rec.hpp"
#include "epykos/scalar/select.hpp"
#include "epykos/tape/passes.hpp"
#include "epykos/tape/tape.hpp"

namespace epykos::fixtures {

inline constexpr int exotic_path_steps = 52;
inline constexpr int exotic_path_paths = 256;
inline constexpr std::uint64_t exotic_path_substream_base = 530000;

// The six input ordinals, named so a caller never indexes by a bare integer.
enum ExoticInput : int { exotic_s0 = 0, exotic_r = 1, exotic_q = 2, exotic_sigma = 3, exotic_strike = 4, exotic_barrier = 5 };
inline constexpr int exotic_n_inputs = 6;

// WHICH LEGS ARE PRICED, as a bitmask. Not a convenience: the full payoff carries FOUR interleaved
// recurrences per path (the spot, the running sum, the running log sum, the running extremum) and
// `ir::infer` lays out NO scan domain for it, reporting "scan class 6 (mul) reads itself through
// other classes". Being able to price one leg at a time is what turns that from an observation
// into a measurement -- it says how many coupled recurrences the scan detector tolerates before it
// gives up, which is the number a payoff language would have to stay under. `spot` is the control:
// one recurrence and nothing reading it, which is affine_scan's shape.
enum ExoticLegs : unsigned {
  exotic_leg_spot = 1u,     // output the mean terminal spot only: ONE recurrence, no payoff
  exotic_leg_arith = 2u,    // + the running sum   -> the arithmetic Asian
  exotic_leg_geom = 4u,     // + the running log sum -> the geometric Asian
  exotic_leg_barrier = 8u,  // + the running extremum -> the up-and-out call
  exotic_legs_all = exotic_leg_arith | exotic_leg_geom | exotic_leg_barrier,
};
inline int exotic_n_legs(unsigned legs) {
  return ((legs & exotic_leg_spot) ? 1 : 0) + ((legs & exotic_leg_arith) ? 1 : 0) +
         ((legs & exotic_leg_geom) ? 1 : 0) + ((legs & exotic_leg_barrier) ? 1 : 0);
}

struct ExoticPathFixture {
  int n_steps = exotic_path_steps;
  int n_paths = exotic_path_paths;
  double maturity = 1.0;            // T in years
  std::vector<double> eps;          // n_steps × n_paths, [k·n_paths + p]: structure, not an input

  // Record-point state, in ordinal order.
  double s0 = 100.0;
  double r = 0.03;
  double q = 0.01;
  double sigma = 0.20;
  double strike = 100.0;
  double barrier = 130.0;           // up-and-out, above the forward so the knock-out bites

  unsigned legs = exotic_legs_all;

  double dt() const { return maturity / static_cast<double>(n_steps); }
  int n_inputs() const { return exotic_n_inputs; }
  int n_legs() const { return exotic_n_legs(legs); }
  int n_outputs(bool output_paths) const { return n_legs() * (1 + (output_paths ? n_paths : 0)); }
  std::vector<double> record_state() const { return {s0, r, q, sigma, strike, barrier}; }
};

inline ExoticPathFixture make_exotic_path(int n_paths = exotic_path_paths, int n_steps = exotic_path_steps,
                                          std::uint64_t seed = 20260922, unsigned legs = exotic_legs_all) {
  ExoticPathFixture f;
  f.n_paths = n_paths;
  f.n_steps = n_steps;
  f.legs = legs;
  // One sub-stream for the whole grid, drawn step-major so that a grid with more steps extends the
  // draws of a shorter one rather than reshuffling them (D17: draws independent of generation
  // order, and a sweep over K then nests).
  rng::Philox g(seed, exotic_path_substream_base);
  f.eps.resize(static_cast<std::size_t>(n_steps) * static_cast<std::size_t>(n_paths));
  for (double& e : f.eps) e = g.gaussian();
  return f;
}

// The evaluation on Scalar: in[0..6) -> out[0..n_outputs(output_paths)).
//
// `max` and `select` are unqualified: epykos::max / epykos::select are found by enclosing-namespace
// lookup for `double` (scalar/select.hpp) and by ADL for Rec and Dual, so this is ONE source for
// the reference and the recording (D3). std::max on a Scalar is forbidden (CLAUDE.md) and would not
// compile on Rec anyway, because RecBool does not convert to bool.
template <class Scalar>
void exotic_path_evaluate(const ExoticPathFixture& f, const Scalar* in, Scalar* out, bool output_paths) {
  using std::exp;
  using std::log;
  using std::sqrt;
  const std::size_t P = static_cast<std::size_t>(f.n_paths);
  const Scalar s0 = in[exotic_s0];
  const Scalar r = in[exotic_r];
  const Scalar q = in[exotic_q];
  const Scalar sigma = in[exotic_sigma];
  const Scalar strike = in[exotic_strike];
  const Scalar barrier = in[exotic_barrier];

  const double dt = f.dt();
  const double inv_k = 1.0 / static_cast<double>(f.n_steps);
  // Shared by every path and every step: the engine sees these once.
  const Scalar drift = (r - q - 0.5 * sigma * sigma) * dt;
  const Scalar vol = sigma * sqrt(dt);
  const Scalar df = exp(-r * f.maturity);

  // Up to four interleaved recurrences per path: the spot, the running sum, the running log sum
  // and the running extremum, the last three all READING the first. Time outer, paths inner, so
  // the chains interleave on the tape exactly as affine_scan's do and the signature pass has to
  // lay them out chain-major (D41). `legs` selects which of the three accumulators exist, which
  // is how the scan-layout threshold is measured rather than asserted.
  const bool want_a = (f.legs & exotic_leg_arith) != 0u;
  const bool want_g = (f.legs & exotic_leg_geom) != 0u;
  const bool want_b = (f.legs & exotic_leg_barrier) != 0u;
  std::vector<Scalar> s(P, s0);
  std::vector<Scalar> acc(P, Scalar(0.0));
  std::vector<Scalar> lacc(P, Scalar(0.0));
  std::vector<Scalar> run_max(P, s0);
  for (int k = 0; k < f.n_steps; ++k) {
    for (std::size_t p = 0; p < P; ++p) {
      const Scalar z = drift + vol * f.eps[static_cast<std::size_t>(k) * P + p];
      s[p] = s[p] * exp(z);
      // `want_*` are compile-time-constant structure: they come from `legs`, never from a Scalar,
      // so a C++ `if` on them is legitimate and `structural_if` is not needed (it takes a RecBool).
      // Nothing here branches on a VALUE: every value branch below is a select.
      if (want_a) acc[p] = acc[p] + s[p];
      if (want_g) lacc[p] = lacc[p] + log(s[p]);
      // SITE 2: a Select inside a recurrence. `max` records it; a C++ `if` does not compile on Rec.
      if (want_b) run_max[p] = max(s[p], run_max[p]);
    }
  }

  std::vector<Scalar> pay_a(P), pay_g(P), pay_b(P);
  for (std::size_t p = 0; p < P; ++p) {
    // SITE 1: the call payoff. max(x, 0) is a select with both arms recorded, never std::max.
    if (want_a) pay_a[p] = df * max(acc[p] * inv_k - strike, Scalar(0.0));
    if (want_g) pay_g[p] = df * max(exp(lacc[p] * inv_k) - strike, Scalar(0.0));
    if (want_b) {
      const Scalar vanilla = max(s[p] - strike, Scalar(0.0));
      // SITE 3: the knock-out. `barrier` is an input, so the predicate is tainted and
      // structural_if throws on it by construction; both arms are recorded.
      pay_b[p] = df * select(run_max[p] < barrier, vanilla, Scalar(0.0));
    }
  }

  // The per-leg MC means, as the natural left-associated chain; fold_sum buckets it. Leg order is
  // always spot, arithmetic, geometric, barrier, over the legs actually present.
  const double inv_p = 1.0 / static_cast<double>(f.n_paths);
  std::size_t o = 0;
  auto emit = [&](const std::vector<Scalar>& v) {
    Scalar t = v[0];
    for (std::size_t p = 1; p < P; ++p) t = t + v[p];
    out[o++] = t * inv_p;
  };
  if ((f.legs & exotic_leg_spot) != 0u) emit(s);
  if (want_a) emit(pay_a);
  if (want_g) emit(pay_g);
  if (want_b) emit(pay_b);
  if (output_paths) {
    auto emit_paths = [&](const std::vector<Scalar>& v) {
      for (std::size_t p = 0; p < P; ++p) out[o++] = v[p];
    };
    if ((f.legs & exotic_leg_spot) != 0u) emit_paths(s);
    if (want_a) emit_paths(pay_a);
    if (want_g) emit_paths(pay_g);
    if (want_b) emit_paths(pay_b);
  }
}

inline std::vector<double> exotic_path_oracle(const ExoticPathFixture& f, const std::vector<double>& z,
                                              bool output_paths) {
  std::vector<double> out(static_cast<std::size_t>(f.n_outputs(output_paths)));
  exotic_path_evaluate<double>(f, z.data(), out.data(), output_paths);
  return out;
}

// The recording at the record point, through the pin unless run_passes is false.
inline Tape record_exotic_path(const ExoticPathFixture& f, bool output_paths, bool run_passes = true) {
  Tape tape;
  {
    Tape::Scope scope(tape);
    const std::vector<double> z0 = f.record_state();
    std::vector<Rec> in;
    in.reserve(z0.size());
    for (double v : z0) in.push_back(make_input(tape, v));
    std::vector<Rec> out(static_cast<std::size_t>(f.n_outputs(output_paths)));
    exotic_path_evaluate<Rec>(f, in.data(), out.data(), output_paths);
    for (const Rec& x : out) register_output(tape, x);
  }
  tape.validate();
  if (run_passes) {
    compile(tape);
    tape.validate();
  }
  return tape;
}

// ---- the independent check: the geometric Asian's exact closed form ---------------------------
//
// Standard library only -- NO dependency is added, so no D12 entry is needed. D2 makes the
// engine's own templated-`double` path the oracle, which for a path-dependent payoff is circular:
// a bug in the path machinery moves both sides together. This is the external maths.
//
// With exact log increments the path is exactly GBM, so ln G = (1/n)·sum_k ln S_{t_k} is exactly
// normal -- there is no discretisation bias to acknowledge, unlike the barrier (see the header):
//
//   mu    = ln S0 + (r − q − sigma²/2)·tbar,    tbar = (1/n)·sum_k t_k = dt·(n+1)/2
//   s²    = (sigma²/n²)·sum_i sum_j min(t_i,t_j) = sigma²·dt·n(n+1)(2n+1)/(6 n²)
//   price = exp(−rT)·[ exp(mu + s²/2)·Phi(d_plus) − X·Phi(d_minus) ]
//   d_minus = (mu − ln X)/s,   d_plus = d_minus + s
//
// sum_i sum_j min(i,j) = n(n+1)(2n+1)/6 over i,j = 1..n (check n=2: 1+1+1+2 = 5 = 2·3·5/6).
inline double normal_cdf(double x) noexcept { return 0.5 * std::erfc(-x / std::sqrt(2.0)); }

inline double geometric_asian_call_closed_form(const ExoticPathFixture& f) {
  const double n = static_cast<double>(f.n_steps);
  const double dt = f.dt();
  const double tbar = dt * (n + 1.0) / 2.0;
  const double var = f.sigma * f.sigma * dt * (n + 1.0) * (2.0 * n + 1.0) / (6.0 * n);
  const double sd = std::sqrt(var);
  const double mu = std::log(f.s0) + (f.r - f.q - 0.5 * f.sigma * f.sigma) * tbar;
  const double d_minus = (mu - std::log(f.strike)) / sd;
  const double d_plus = d_minus + sd;
  return std::exp(-f.r * f.maturity) * (std::exp(mu + 0.5 * var) * normal_cdf(d_plus) - f.strike * normal_cdf(d_minus));
}

// The sample standard error of the geometric leg's MC mean, from the same paths the estimate used:
// the width of the test above, so the reader sees it rather than taking a tolerance on trust.
inline double geometric_asian_std_error(const ExoticPathFixture& f) {
  const std::vector<double> out = exotic_path_oracle(f, f.record_state(), /*output_paths=*/true);
  const std::size_t P = static_cast<std::size_t>(f.n_paths);
  // The geometric leg's per-path block, found from `legs` rather than hardcoded: the leg order is
  // spot, arithmetic, geometric, barrier, over the legs present, and the per-path blocks follow
  // the means in the same order.
  int before = ((f.legs & exotic_leg_spot) != 0u ? 1 : 0) + ((f.legs & exotic_leg_arith) != 0u ? 1 : 0);
  const double* pay = out.data() + static_cast<std::size_t>(f.n_legs()) + static_cast<std::size_t>(before) * P;
  double mean = 0.0;
  for (std::size_t p = 0; p < P; ++p) mean += pay[p];
  mean /= static_cast<double>(P);
  double s2 = 0.0;
  for (std::size_t p = 0; p < P; ++p) s2 += (pay[p] - mean) * (pay[p] - mean);
  s2 /= static_cast<double>(P - 1);
  return std::sqrt(s2 / static_cast<double>(P));
}

// A ball of states around the record point, for a differential test: draw rr scales every input by
// U(0.9, 1.1) from sub-stream exotic_path_substream_base + 100 + rr; draw 0 is the record point.
// The band is deliberately narrower than affine_scan's U(0.8, 1.2) because scaling the barrier and
// the strike moves which paths knock out, and a U(0.8,1.2) draw on `sigma` with a 0.8 draw on `Bu`
// makes the barrier leg identically zero -- a state that tests the select but not the arithmetic.
inline std::vector<std::vector<double>> exotic_path_states(const ExoticPathFixture& f, int n,
                                                            std::uint64_t seed = 20260922) {
  std::vector<std::vector<double>> states;
  const std::vector<double> z0 = f.record_state();
  for (int rr = 0; rr < n; ++rr) {
    std::vector<double> z = z0;
    if (rr > 0) {
      rng::Philox g(seed, exotic_path_substream_base + 100 + static_cast<std::uint64_t>(rr));
      for (double& v : z) v *= g.uniform_range(0.9, 1.1);
    }
    states.push_back(z);
  }
  return states;
}

}  // namespace epykos::fixtures
