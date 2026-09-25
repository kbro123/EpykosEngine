// Shared helper for tests/rewrite/r*_e0_test.cpp's Stage A checks.
//
// rewrite::verify_rule / compare_programs perturb the record point through verify::make_state_ball
// (rho > 0, at least one draw) before evaluating exec::Interpreter / adjoint::Adjoint directly over
// the result -- exactly right for a program whose Inputs are genuinely free (the M1 book, and every
// hand-built synthetic program in these tests). The Stage A tape's Inputs are its QUOTES, but the
// tape's own book depends on the CALIBRATED KNOTS an implicit block solves for from those quotes
// (fixtures/stage_a.hpp); a raw exec::Interpreter does not re-solve that block, so perturbing the
// quotes without going through solver::ImplicitProgram (as tests/stage_a/*_test.cpp do) walks the
// book off the solved point -- not what this file's rules are being asked to preserve, and, in
// practice, sometimes into `log`/`sqrt`/`div`'s undefined region entirely (measured: NaN outputs).
// Checking the exact record point sidesteps this: it is by construction the one state the recorded
// tape is self-consistent at, for a plain exec::Interpreter / adjoint::Adjoint exactly as well as
// for solver::ImplicitProgram, so it is enough to confirm a structural rewrite changed no bits
// there, without teaching this package's rule tests solver::ImplicitProgram's own API.
//
// The record point and the buffer sizes are taken FROM THE PROGRAM (`before.input_values`,
// `before.inputs.size()`, `before.outputs.size()`), never from the caller -- see D65. This helper
// originally took `state` / `n_inputs` / `n_outputs` as parameters and R-a's own Stage A tests
// passed `StageATape::record_quotes()` for all three. That vector is the 70 calibration QUOTES,
// a strict subset of the Stage A tape's 148 Inputs (the quotes plus the realised fixings recorded
// alongside them). `exec::Interpreter::run` and `adjoint::Adjoint::run` take exactly
// `program.inputs.size()` entries and, per their own contract (adjoint.hpp: "zero allocations in
// run()"), are handed raw pointers with no length to check -- so the 70-entry vector was read 78
// doubles past its end by the forward pass and, fatally, WRITTEN 78 doubles past the end of the
// `state_bar` vector sized from the same 70. That 624-byte heap write overflow is the
// "heap-corruption-shaped abort some distance past the actual fault inside src/adjoint/" D52
// point 4 attributed to the adjoint and walled R2 off for; it is a caller-side sizing defect and
// nothing in `src/adjoint/` is wrong (D65). M4/R-b and R-c hit the same SIGABRT independently and
// fixed it caller-side in their own tests (r4a_push_unary_e0_test.cpp, r5_group_formation_e0_test.cpp,
// fma_contraction_verify_test.cpp all carry a comment saying to use `program.input_values`);
// deriving it here instead of documenting it again is what makes the whole class unreachable.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#include "epykos/adjoint/adjoint.hpp"
#include "epykos/exec/interpreter.hpp"
#include "epykos/ir/program.hpp"

namespace epykos::test {

namespace record_point_detail {
inline std::uint64_t bits_of(double v) {
  std::uint64_t u;
  std::memcpy(&u, &v, sizeof u);
  return u;
}
}  // namespace record_point_detail

struct RecordPointReport {
  bool passed = true;
  std::string detail;
};

// Bitwise-compares exec::Interpreter(before) vs exec::Interpreter(after) over every output, and
// adjoint::Adjoint(before) vs adjoint::Adjoint(after) over a sample of seeded outputs (one-hot
// out_bar), both AT THE RECORD POINT ONLY (`before.input_values`) -- no ball, and no caller-chosen
// state or buffer length (see this file's header).
inline RecordPointReport compare_at_record_point(const ir::Program& before, const ir::Program& after, int max_outputs_checked = 24) {
  RecordPointReport report;
  std::ostringstream detail;

  // A structural rewrite preserves the recording's own interface; anything else is a bug in the
  // rule, not something to paper over with a differently sized buffer.
  const int n_inputs = static_cast<int>(before.inputs.size());
  const int n_outputs = static_cast<int>(before.outputs.size());
  if (before.input_values.size() != before.inputs.size() || after.inputs.size() != before.inputs.size() ||
      after.outputs.size() != before.outputs.size()) {
    report.passed = false;
    report.detail = "record point: the before/after programs disagree on their input or output count";
    return report;
  }
  const double* const state = before.input_values.data();

  exec::Interpreter interp_before(before, {});
  exec::Interpreter interp_after(after, {});
  std::vector<double> out_before(static_cast<std::size_t>(n_outputs)), out_after(static_cast<std::size_t>(n_outputs));
  interp_before.run(state, 1, out_before.data());
  interp_after.run(state, 1, out_after.data());
  int interp_mismatches = 0;
  for (int o = 0; o < n_outputs; ++o) {
    if (record_point_detail::bits_of(out_before[static_cast<std::size_t>(o)]) != record_point_detail::bits_of(out_after[static_cast<std::size_t>(o)])) {
      ++interp_mismatches;
    }
  }
  if (interp_mismatches > 0) {
    report.passed = false;
    detail << "interpreter: " << interp_mismatches << "/" << n_outputs << " output(s) differ at the record point; ";
  }

  adjoint::Adjoint adj_before(before, {});
  adjoint::Adjoint adj_after(after, {});
  std::vector<double> out_bar(static_cast<std::size_t>(n_outputs), 0.0);
  std::vector<double> ob_before(static_cast<std::size_t>(n_outputs)), ob_after(static_cast<std::size_t>(n_outputs));
  std::vector<double> sb_before(static_cast<std::size_t>(n_inputs)), sb_after(static_cast<std::size_t>(n_inputs));
  const int step = n_outputs > max_outputs_checked ? n_outputs / max_outputs_checked : 1;
  int adjoint_mismatches = 0, checked = 0;
  for (int o = 0; o < n_outputs; o += step) {
    std::fill(out_bar.begin(), out_bar.end(), 0.0);
    out_bar[static_cast<std::size_t>(o)] = 1.0;
    adj_before.run(state, 1, out_bar.data(), ob_before.data(), sb_before.data());
    adj_after.run(state, 1, out_bar.data(), ob_after.data(), sb_after.data());
    ++checked;
    for (int i = 0; i < n_inputs; ++i) {
      if (record_point_detail::bits_of(sb_before[static_cast<std::size_t>(i)]) != record_point_detail::bits_of(sb_after[static_cast<std::size_t>(i)])) {
        ++adjoint_mismatches;
      }
    }
    for (int i = 0; i < n_outputs; ++i) {
      if (record_point_detail::bits_of(ob_before[static_cast<std::size_t>(i)]) != record_point_detail::bits_of(ob_after[static_cast<std::size_t>(i)])) {
        ++adjoint_mismatches;
      }
    }
  }
  if (adjoint_mismatches > 0) {
    report.passed = false;
    detail << "adjoint: " << adjoint_mismatches << " value(s) differ over " << checked << " seeded output(s) at the record point";
  }
  report.detail = detail.str();
  return report;
}

}  // namespace epykos::test
