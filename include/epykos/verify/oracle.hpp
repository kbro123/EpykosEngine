// EpykosEngine — error against truth (PRINCIPLES.md §4; D72).
//
// verify/differential.hpp compares two `double` evaluations with each other. This header compares
// ONE `double` evaluation with the recorded expression evaluated at 106 significand bits, which
// PRINCIPLES.md §4 makes ground truth:
//
//     "Ground truth is the recorded expression evaluated without rounding. Not the `double`
//      evaluation of it, which is merely one realisation and has never had its own error
//      characterised."
//
// The oracle is `epykos::Wide` (scalar/wide.hpp) and it is an INSTANTIATION of the templated maths,
// never a second copy of it (D3): `price_book<Wide>` and `price_book<double>` are the same source
// text. The harness itself evaluates no maths of its own — it is handed two callables.
//
// WHY THE COMPARISON IS CLEAN. A ball's states are doubles. They promote into `Wide` EXACTLY —
// `Wide(x)` is `{x, 0.0}`, no rounding — so the two sides are given bit-identical inputs and the
// only thing that can differ is the arithmetic. Nothing here needs to argue about input error.
//
// Pieces:
//   OracleFn       one state in as exactly-promoted Wides, n_outputs Wides out. Instantiate it in
//                  whichever TU you like: Wide's result does not depend on -ffp-contract (see
//                  scalar/wide.hpp), which is what lets the oracle be the SAME number everywhere.
//   BatchFn        the path under test, differential.hpp's own type, so an exec::Interpreter, a
//                  Replayer, a catalogued kernel or the templated maths on double all drop in
//                  unchanged. That is not incidental: PRINCIPLES.md §4 folded execution into the
//                  mathematical tier, so "the interpreter, the catalogue kernels and the adjoint
//                  are implementations of the recorded maths like any rewrite, and are judged the
//                  same way". Measuring an execution path against truth therefore costs a caller
//                  and no new code here (tests/maths/m1_oracle_error_test.cpp does it for the
//                  interpreter).
//   OutputClass    a named run of output ordinals. PRINCIPLES.md §4 makes tolerances per output
//                  class ("valuation is held tight; sensitivities are allowed more"), so the
//                  report is per class and never one global number.
//   error_against_truth   the measurement.
//   oracle_jacobian       the sensitivity channel: a central difference taken AT THE ORACLE'S
//                  precision, which is a different thing from a finite difference in double.
//
// §4 also states the preference this header exists to serve: "prefer measuring each path against
// the oracle over comparing the two paths to each other, where the oracle is affordable: it is the
// stronger statement and it says which path is wrong, not merely that they differ."
// verify/differential.hpp is the cheap path-against-path form and remains right where the oracle is
// too expensive; this is the strong form.
//
// NO VERDICT IS RETURNED, deliberately. §4 says the per-class tolerances "belong in PROBLEM.md
// beside the outputs they govern", and this package is the first ever measurement of what the
// naive path's error actually is. Setting a tolerance before that measurement existed is how the
// engine ended up gating against an uncharacterised approximation in the first place. The harness
// measures; the owner sets the numbers from what it reports.
#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "epykos/scalar/wide.hpp"
#include "epykos/verify/differential.hpp"

namespace epykos::verify {

// ------------------------------------------------------------------------------------------------
// The oracle side
// ------------------------------------------------------------------------------------------------

// One state z[0..n_inputs) in (already promoted, exactly, from the ball's doubles), out[0..n_outputs)
// out. The Wide instantiation of the same templated maths the `double` side runs.
using OracleFn = std::function<void(const Wide* z, Wide* out)>;

// PRINCIPLES.md §4's output class: a named, contiguous run of output ordinals held to one
// tolerance. `first + count` must not exceed n_outputs; classes may not overlap.
struct OutputClass {
  std::string name;
  int first = 0;
  int count = 0;
};

// ------------------------------------------------------------------------------------------------
// The measurement
// ------------------------------------------------------------------------------------------------

// One output's error against truth over the states of a ball. "scale" is the optional per-output,
// per-state D26 scale; m = max(|truth|, scale) is what `rel` and `ulps` are taken against, and
// `rel_self` is the same error taken against |truth| alone. Both are reported because for a value
// that is a difference of cancelling terms they answer different questions: `rel_self` is how
// wrong the ANSWER is, `rel` is how well the ARITHMETIC was done.
struct OutputTruth {
  int n_states = 0;
  int degraded = 0;  // states where the oracle itself was not at full precision (wide.hpp)

  double max_abs = 0.0;
  double max_rel = 0.0;       // max over states of |approx − truth| / max(|truth|, scale)
  int max_rel_state = -1;
  double max_rel_self = 0.0;  // max over states of |approx − truth| / |truth|
  int max_rel_self_state = -1;
  double max_ulps = 0.0;      // max_abs in units of ulp(max(|truth|, scale))
  double rms_rel = 0.0;       // root mean square of the per-state `rel`

  double truth_at_worst = 0.0;   // at max_rel_state
  double approx_at_worst = 0.0;
  double scale_at_worst = 0.0;

  bool exact() const noexcept { return max_abs == 0.0; }
};

// The same, reduced over a class's outputs (PRINCIPLES.md §4).
struct ClassTruth {
  std::string name;
  int first = 0;
  int count = 0;

  double max_rel = 0.0;       // the worst output's worst state
  int max_rel_output = -1;
  int max_rel_state = -1;
  double max_rel_self = 0.0;
  int max_rel_self_output = -1;
  double max_ulps = 0.0;
  double rms_rel = 0.0;       // over every (output, state) of the class
  double median_rel = 0.0;    // over outputs, of each output's max_rel
  int exact_outputs = 0;      // outputs that were bitwise right at every state
  int degraded = 0;

  double truth_at_worst = 0.0;
  double approx_at_worst = 0.0;
  double scale_at_worst = 0.0;
};

struct TruthReport {
  int n_inputs = 0;
  int n_outputs = 0;
  int n_states = 0;
  int batch = 1;
  bool scaled = false;

  // Stated with every report, so a number always carries the instrument it was taken with.
  int oracle_mantissa_bits = 0;
  int headroom_bits = 0;  // oracle bits − double's 53

  std::vector<OutputTruth> outputs;  // per output ordinal
  std::vector<ClassTruth> classes;   // per declared class ("" = one class over everything)

  std::size_t degraded = 0;  // (output, state) pairs where the oracle was not at full precision
  double max_rel = 0.0;
  int max_rel_output = -1;
  int max_rel_state = -1;
  std::vector<double> worst_state;  // the state of max_rel_state

  // One paragraph: sizes, the oracle's headroom, the worst point, and the degraded count.
  std::string summary() const;
  // One line per output class: the table PRINCIPLES.md §4 asks for.
  std::string per_class_table() const;
};

struct TruthOptions {
  int batch = 1;                     // states per call of the path under test
  ScaleFn scale;                     // optional per-output, per-state D26 scale
  std::vector<OutputClass> classes;  // empty = one unnamed class over every output
};

// Evaluates every state of the ball through `approx` (`batch` at a time) and `truth` (one at a
// time, at the oracle's precision) and reports, per output and per class, how far the `double`
// path is from the expression's true value.
//
// Throws std::invalid_argument for an empty ball, n_outputs < 1, batch < 1, a missing callable, or
// classes that overlap or run past n_outputs.
TruthReport error_against_truth(const StateBall& ball, int n_outputs, const BatchFn& approx, const OracleFn& truth,
                                const TruthOptions& options = {});

// ------------------------------------------------------------------------------------------------
// The sensitivity channel
// ------------------------------------------------------------------------------------------------

// The oracle's Jacobian by a central difference taken at 106 bits.
//
// This is an approximation of the derivative, and calling it anything else would be dishonest.
// What makes it usable is that its error is bounded far below the thing being measured, for a
// reason that does not apply in double. A central difference carries a truncation error
// ~h^2*|f'''|/6 and a roundoff error ~u*|f|/h. In double (u = 1.1e-16) the best achievable is
// ~1e-11 relative, which is why M2 gated the adjoint against finite differences at 1e-6 (D30).
// At the oracle's u = 2^-106 the same balance lands ten decades lower.
//
// MEASURED, on d(1+z)^64/dz at z = 0.03 and on d exp(z)/dz, against the analytic derivatives
// (tests/verify/oracle_truth_test.cpp restates these every run):
//
//                                 (1+z)^64    exp(z)
//   plain central difference       6.8e-19   2.0e-21   relative
//   Richardson-extrapolated        2.6e-24   3.1e-21   relative   (the default)
//   the SAME difference in double  2.9e-08             relative
//
// The plain form's 6.8e-19 on the polynomial is not a defect: it is h^2*|f'''|/6 with
// f'''/f' = 3,684 for a 64th-order polynomial, and oracle_jacobian_step cannot know that. The
// Richardson pair (4*D(h) - D(2h))/3 cancels the h^2 term and with it the dependence on f''',
// for twice the evaluations.
//
// Note the second column: on exp(z), where f'''/f' = 1, the plain difference is already at its
// roundoff floor (~u/h = 4e-22) and Richardson is MARGINALLY WORSE, because averaging two
// differences amplifies roundoff by 5/3. That is the whole trade and it is why Richardson is
// still the default: a truncation term scales with a derivative the instrument cannot see, while
// the roundoff floor is fixed, known and 1.67x from optimal at worst. PRINCIPLES.md §4 holds
// sensitivities to a looser tolerance than valuation, and the instrument must not be what sets
// that floor. Pass `richardson = false` to halve the cost on a fixture where it does not matter.
struct JacobianOptions {
  double h = 0.0;           // 0 = oracle_jacobian_step(|z_k|) per input
  bool richardson = true;   // cancel the h^2 truncation term: 4 oracle evaluations per input, not 2
  std::vector<int> inputs;  // empty = every input; otherwise the input ordinals to take
};

// The balanced central-difference step at the oracle's precision for an input of magnitude |z|:
// cbrt(3u)*max(|z|, 1) with u = 2^-106, which is about 3.3e-11.
double oracle_jacobian_step(double z) noexcept;

// jac[o * n_taken + j] = d out[o] / d z[inputs[j]] at `z`, to the accuracy measured above.
// `n_taken` is inputs.size() (or n_inputs when empty). Costs 4 (Richardson) or 2 oracle
// evaluations per taken input, which on a large fixture is the dominant cost of a measurement.
std::vector<Wide> oracle_jacobian(const OracleFn& truth, const double* z, int n_inputs, int n_outputs,
                                  const JacobianOptions& options = {});

// ------------------------------------------------------------------------------------------------
// The contraction-independence probe
// ------------------------------------------------------------------------------------------------

// scalar/wide.hpp claims a Wide result does not depend on the -ffp-contract setting of the TU that
// computed it, which is what lets one oracle figure be THE figure on every preset and compiler.
// D46 is the entry recording that exactly this assumption failed for header-only templates before,
// so it is measured here rather than asserted: this function is defined in
// src/verify/wide_probe.cpp, an ORDINARY library TU that the release preset is free to contract,
// and tests/scalar/wide_test.cpp computes the identical expression in its own
// -ffp-contract=off TU and compares the two bitwise.
//
// `out` receives 3 values: an add/multiply chain, a division chain, and an exp/log chain, each
// written in the `a·b + c` shapes a compiler would fuse.
void wide_contraction_probe(double seed, Wide* out /* 3 values */);

}  // namespace epykos::verify
