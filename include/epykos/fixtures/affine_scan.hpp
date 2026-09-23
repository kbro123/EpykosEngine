// EpykosEngine — the affine scan fixture (M3/G3; test-only, D28): x_{k+1} = a_k·x_k + b_k over
// n_steps steps on n_paths paths, the shape M6's short-rate simulation will record (a mean-
// reverting step with a per-step coefficient and increment), written as the natural time loop
// with the paths inside it — so the chains interleave on the tape and the signature pass must
// lay the scan's rows out chain-major (D37).
//
//   a_k       = exp(−κ·Δt_k)
//   drift_k   = θ·(1 − a_k)                      (shared by every path)
//   b_kp      = drift_k + σ·sqrt(Δt_k)·ε_kp      (ε seeded standard normals, structure)
//   x_{k+1,p} = a_k·x_kp + b_kp                  written as a_k * x + drift_k + noise_kp
//
// Inputs (ordinal order): x_0 of every path, then κ, θ, σ. Outputs: every x_kp in time-major
// order when `output_path` (the path is what an exposure grid reads: every step a boundary),
// else only the final x of every path (the intermediates used once: the chain runs through
// inner nodes); then the mean of the finals. Both recordings must produce one scan domain of
// n_paths chains of n_steps steps. Synthetic: Δt_k ~ U(0.5, 1.5)/52 and ε from the seed
// (sub-streams affine_scan_substream_base + 0 and + 1), no data files. Header-only so the
// including TU's contraction setting governs the arithmetic; double is the oracle, Rec
// records, Dual<N> is the forward-mode check.
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "epykos/rng/philox.hpp"
#include "epykos/scalar/rec.hpp"
#include "epykos/tape/passes.hpp"
#include "epykos/tape/tape.hpp"

namespace epykos::fixtures {

inline constexpr int affine_scan_steps = 100;
inline constexpr int affine_scan_paths = 4;
inline constexpr std::uint64_t affine_scan_substream_base = 520000;

struct AffineScanFixture {
  int n_steps = affine_scan_steps;
  int n_paths = affine_scan_paths;
  std::vector<double> dt;    // n_steps
  std::vector<double> eps;   // n_steps × n_paths, [k·n_paths + p]
  std::vector<double> x0;    // n_paths: record-point initial values
  double kappa = 0.5;
  double theta = 0.04;
  double sigma = 0.01;

  int n_inputs() const { return n_paths + 3; }
  int n_outputs(bool output_path) const { return (output_path ? n_steps * n_paths : n_paths) + 1; }
  // The record-point state in input ordinal order.
  std::vector<double> record_state() const {
    std::vector<double> z(x0.begin(), x0.end());
    z.push_back(kappa);
    z.push_back(theta);
    z.push_back(sigma);
    return z;
  }
};

inline AffineScanFixture make_affine_scan(std::uint64_t seed = 20260922) {
  AffineScanFixture f;
  rng::Philox g_dt(seed, affine_scan_substream_base);
  rng::Philox g_eps(seed, affine_scan_substream_base + 1);
  f.dt.resize(static_cast<std::size_t>(f.n_steps));
  for (double& d : f.dt) d = g_dt.uniform_range(0.5, 1.5) / 52.0;
  f.eps.resize(static_cast<std::size_t>(f.n_steps) * static_cast<std::size_t>(f.n_paths));
  for (double& e : f.eps) e = g_eps.gaussian();
  f.x0.resize(static_cast<std::size_t>(f.n_paths));
  for (int p = 0; p < f.n_paths; ++p) f.x0[static_cast<std::size_t>(p)] = 0.02 + 0.005 * p;
  return f;
}

// The evaluation on Scalar: in[0..n_inputs) -> out[0..n_outputs(output_path)).
template <class Scalar>
void affine_scan_evaluate(const AffineScanFixture& f, const Scalar* in, Scalar* out, bool output_path) {
  using std::exp;
  using std::sqrt;
  const std::size_t P = static_cast<std::size_t>(f.n_paths);
  const Scalar kappa = in[P];
  const Scalar theta = in[P + 1];
  const Scalar sigma = in[P + 2];
  std::vector<Scalar> x(in, in + P);
  std::size_t o = 0;
  for (int k = 0; k < f.n_steps; ++k) {
    const double dt = f.dt[static_cast<std::size_t>(k)];
    const Scalar a = exp(-kappa * dt);
    const Scalar drift = theta * (1.0 - a);
    const Scalar vol = sigma * sqrt(dt);
    for (std::size_t p = 0; p < P; ++p) {
      const Scalar noise = vol * f.eps[static_cast<std::size_t>(k) * P + p];
      x[p] = a * x[p] + drift + noise;
      if (output_path) out[o++] = x[p];
    }
  }
  if (!output_path) {
    for (std::size_t p = 0; p < P; ++p) out[o++] = x[p];
  }
  Scalar mean = x[0];
  for (std::size_t p = 1; p < P; ++p) mean = mean + x[p];
  out[o] = mean / static_cast<double>(f.n_paths);
}

inline std::vector<double> affine_scan_oracle(const AffineScanFixture& f, const std::vector<double>& z, bool output_path) {
  std::vector<double> out(static_cast<std::size_t>(f.n_outputs(output_path)));
  affine_scan_evaluate<double>(f, z.data(), out.data(), output_path);
  return out;
}

// The recording at the record point, with the E0 standard passes unless run_passes is false.
inline Tape record_affine_scan(const AffineScanFixture& f, bool output_path, bool run_passes = true) {
  Tape tape;
  {
    Tape::Scope scope(tape);
    const std::vector<double> z0 = f.record_state();
    std::vector<Rec> in;
    for (double v : z0) in.push_back(make_input(tape, v));
    std::vector<Rec> out(static_cast<std::size_t>(f.n_outputs(output_path)));
    affine_scan_evaluate<Rec>(f, in.data(), out.data(), output_path);
    for (const Rec& r : out) register_output(tape, r);
  }
  tape.validate();
  if (run_passes) {
    standard_passes(tape);
    tape.validate();
  }
  return tape;
}

// A ball of states around the record point: draw r scales every input by U(0.8, 1.2) from
// sub-stream affine_scan_substream_base + 100 + r; draw 0 is the record point.
inline std::vector<std::vector<double>> affine_scan_states(const AffineScanFixture& f, int n, std::uint64_t seed = 20260922) {
  std::vector<std::vector<double>> states;
  const std::vector<double> z0 = f.record_state();
  for (int r = 0; r < n; ++r) {
    std::vector<double> z = z0;
    if (r > 0) {
      rng::Philox g(seed, affine_scan_substream_base + 100 + static_cast<std::uint64_t>(r));
      for (double& v : z) v *= g.uniform_range(0.8, 1.2);
    }
    states.push_back(z);
  }
  return states;
}

}  // namespace epykos::fixtures
