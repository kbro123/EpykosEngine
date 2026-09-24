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
// out_bar), both AT `state` ONLY -- no ball.
inline RecordPointReport compare_at_record_point(const ir::Program& before, const ir::Program& after, const double* state, int n_inputs,
                                                 int n_outputs, int max_outputs_checked = 24) {
  RecordPointReport report;
  std::ostringstream detail;

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
