// EpykosEngine — the differential tester (DESIGN.md §11; docs/WORKLOADS.md §M2 "State ball"):
// a compiled evaluation of a program against the templated maths instantiated on double, at
// random states in a ball around the record point.
//
// Pieces, each usable on its own:
//
//   StateBall     R states z^(r) = z + ρ·u^(r), u^(r) uniform on [−1, 1]^n, drawn from Philox
//                 sub-stream `substream_base + r` (draw index k = coordinate k, D17): draw r is a
//                 pure function of (seed, r, k), independent of R and of anything else drawn.
//                 The draws are computed in an E0 TU (src/verify/state_ball_e0.cpp, pinned to
//                 -ffp-contract=off in every preset, D25), so a state is the same bits on every
//                 preset and compiler and "the worst state" of a report is reproducible.
//   ScalarFn      the reference: one state in, n_outputs values out (price_book<double>, the
//                 tape replay, ...). Instantiate it in the TU whose flags you want: the harness
//                 never evaluates any maths of its own.
//   BatchFn       the compiled side: B states in, batch axis innermost (state[k·B + b], D15),
//                 n_outputs × B values out (out[o·B + b]) — exec::Interpreter::run's layout.
//                 interpreter_fn / replayer_fn / batch_of adapt the usual objects.
//   Tolerance     E0: bitwise equal. E1: |a − b| <= max(ulps · ulp(m), rel · m) with
//                 m = max(|a|, |b|, scale), where ulp(x) is the spacing of doubles at |x| and
//                 `scale` is an optional per-output, per-state scale (D26: for a value that is
//                 a difference of terms, the scale of the terms; without it the bound is relative
//                 to the value itself; an infinite scale exempts that value from the bound).
//                 ulps defaults to 4 and rel to 0 (Tolerance::e1); Tolerance::e1_relative(1e-12)
//                 is D26's bound as written. Measured on the M1 book (D29): the release
//                 preset's contraction of the reference alone reaches 13 ulps of the leg scale,
//                 so 4 ulps is a bound for kernels that share the reference's operations, not
//                 for a reference the compiler was free to contract.
//   differential  draws the ball through both sides, `batch` states per compiled call, and
//                 reports per output the max relative error and max ulp distance (both against
//                 m) with the draw they occur at, the counts of bitwise mismatches and tolerance
//                 violations, the worst (output, draw) overall with its state, and the
//                 pass/fail verdict.
//
// The harness holds no fixture knowledge: the M1 book's ball, reference and D26 scales are
// composed in fixtures/m1_differential.hpp (test-only).
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "epykos/exec/interpreter.hpp"
#include "epykos/tape/replay.hpp"

namespace epykos::verify {

// ---------------------------------------------------------------------------------------------
// The state ball
// ---------------------------------------------------------------------------------------------

struct BallOptions {
  double rho = 0.0050;                        // radius, docs/WORKLOADS.md §M2
  int draws = 256;                            // R
  std::uint64_t seed = 20260922;              // docs/WORKLOADS.md "Common conventions"
  std::uint64_t substream_base = 200000;      // draw r uses sub-stream substream_base + r
};

struct StateBall {
  int n_inputs = 0;
  int n_draws = 0;
  BallOptions options;
  std::vector<double> centre;   // n_inputs: the record point z
  std::vector<double> states;   // n_draws × n_inputs, draw r contiguous at [r·n_inputs, (r+1)·n_inputs)

  const double* state(int r) const {
    return states.data() + static_cast<std::size_t>(r) * static_cast<std::size_t>(n_inputs);
  }
  // Draws first .. first + B − 1 transposed into out[k·B + b] (the compiled side's layout).
  void soa(int first, int B, double* out) const;
};

// z^(r)_k = centre[k] + rho · u,  u = −1 + 2·U_k^(r),  U ~ Philox(seed, substream_base + r) draw k,
// for r in [0, draws). Throws std::invalid_argument for n_inputs < 1 or draws < 1.
StateBall make_state_ball(const double* centre, int n_inputs, const BallOptions& options = {});

// ---------------------------------------------------------------------------------------------
// The two sides
// ---------------------------------------------------------------------------------------------

// One state z[0..n_inputs) in, out[0..n_outputs) out.
using ScalarFn = std::function<void(const double* z, double* out)>;
// B states state[k·B + b] in, out[o·B + b] out, 1 <= B <= the batch the harness was asked for.
using BatchFn = std::function<void(const double* state, int B, double* out)>;
// Optional E1 scale: scale[0..n_outputs) for the state z (D26).
using ScaleFn = std::function<void(const double* z, double* scale)>;

// A scalar function as a batch function: evaluates the states one at a time (allocates its
// per-call scratch).
BatchFn batch_of(ScalarFn f, int n_inputs, int n_outputs);

// exec::Interpreter::run as a BatchFn (the interpreter must outlive the report); the batch
// passed to differential() must not exceed in.max_batch().
inline BatchFn interpreter_fn(const exec::Interpreter& in) {
  return [&in](const double* state, int B, double* out) { in.run(state, B, out); };
}

// Replayer::run as a ScalarFn (the replayer must outlive the report). Header-only, so the
// replay's arithmetic is compiled with the including TU's flags (like the templated maths).
inline ScalarFn replayer_fn(Replayer& rp) {
  return [&rp](const double* z, double* out) { rp.run(z, out); };
}

// ---------------------------------------------------------------------------------------------
// Tolerance and ulp arithmetic
// ---------------------------------------------------------------------------------------------

enum class Exactness : int { E0 = 0, E1 = 1 };

struct Tolerance {
  Exactness cls = Exactness::E0;
  double ulps = 4.0;  // E1: |a − b| <= ulps · ulp(m), m = max(|a|, |b|, scale)
  double rel = 0.0;   // E1: or |a − b| <= rel · m (whichever is looser; 0 = ulps only)

  static Tolerance e0() { return Tolerance{Exactness::E0, 0.0, 0.0}; }
  static Tolerance e1(double ulps = 4.0) { return Tolerance{Exactness::E1, ulps, 0.0}; }
  // D26's form: |a − b| <= rel · max(|a|, |b|, scale). ulps 0 unless given.
  static Tolerance e1_relative(double rel, double ulps = 0.0) { return Tolerance{Exactness::E1, ulps, rel}; }
};

// The spacing of doubles at |x|: nextafter(|x|, +inf) − |x| (the spacing below at DBL_MAX;
// +inf for a non-finite x; the smallest subnormal at 0).
double ulp(double x) noexcept;

// |a − b| / ulp(max(|a|, |b|, |scale|)): 0 when a and b are the same bits (or both zero of
// either sign), +inf when one is NaN or the two are different infinities; 0 for an infinite
// scale (the value is exempt).
double ulp_distance(double a, double b, double scale = 0.0) noexcept;

// |a − b| / max(|a|, |b|, |scale|): the relative error against the scale (0 when both are zero
// or the scale is infinite; +inf for a NaN or different infinities).
double relative_error(double a, double b, double scale = 0.0) noexcept;

// ulp_distance(a, b, scale) <= ulps.
bool within_ulps(double a, double b, double ulps, double scale = 0.0) noexcept;

// The E1 test: bitwise equal, or ulp_distance <= tol.ulps, or relative_error <= tol.rel.
bool within(double a, double b, const Tolerance& tol, double scale = 0.0) noexcept;

// ---------------------------------------------------------------------------------------------
// The report
// ---------------------------------------------------------------------------------------------

// Per output; "scale" below is the E1 scale of that draw when one was supplied, else 0, so
// both errors are against m = max(|a|, |b|, scale).
struct OutputStats {
  int mismatches = 0;         // draws where reference and compiled differ bitwise
  int violations = 0;         // draws failing the tolerance (E0: every mismatch)
  double max_abs = 0.0;       // max over draws of |a − b|
  double max_rel = 0.0;       // max over draws of relative_error(a, b, scale)
  int rel_draw = -1;          // the draw of max_rel (−1 when the output is bitwise equal everywhere)
  double max_ulps = 0.0;      // max over draws of ulp_distance(a, b, scale)
  int ulps_draw = -1;         // the draw of max_ulps
  double ref_at_worst = 0.0;  // reference and compiled values at ulps_draw
  double cmp_at_worst = 0.0;

  bool bitwise() const noexcept { return mismatches == 0; }
};

struct Report {
  int n_inputs = 0;
  int n_outputs = 0;
  int n_draws = 0;
  int batch = 1;
  bool scaled = false;  // an E1 scale was supplied
  Tolerance tolerance;

  std::vector<OutputStats> outputs;  // per output ordinal

  std::size_t mismatches = 0;  // (output, draw) pairs that differ bitwise
  std::size_t violations = 0;  // (output, draw) pairs failing the tolerance
  bool bitwise_equal = false;  // mismatches == 0
  bool passed = false;         // E0: bitwise_equal; E1: violations == 0

  double max_abs = 0.0;
  double max_rel = 0.0;        // with the (output, draw) it occurs at
  int max_rel_output = -1;
  int max_rel_draw = -1;
  double max_ulps = 0.0;       // with the (output, draw) it occurs at: the worst point
  int worst_output = -1;
  int worst_draw = -1;
  std::vector<double> worst_state;  // the state of worst_draw (n_inputs values; empty when bitwise)

  // One paragraph: sizes, tolerance, counts, the worst point, PASS / FAIL.
  std::string summary() const;
};

struct DifferentialOptions {
  Tolerance tolerance;   // default E0
  int batch = 1;         // states per compiled call (the last call may be shorter)
  ScaleFn scale;         // optional E1 scale per output and state (ignored for E0)
};

// Evaluates every draw of the ball through `reference` (one at a time) and `compiled` (`batch`
// at a time) and compares output by output. n_outputs is the number of outputs both produce.
// Throws std::invalid_argument for an empty ball, n_outputs < 1, batch < 1 or a missing callable.
Report differential(const StateBall& ball, int n_outputs, const ScalarFn& reference, const BatchFn& compiled,
                    const DifferentialOptions& options = {});

const char* to_string(Exactness cls) noexcept;

}  // namespace epykos::verify
