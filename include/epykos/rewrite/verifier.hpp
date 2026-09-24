// EpykosEngine — the per-rule verifier (M4/R0; PROBLEM.md §7 "every extracted program passes §6
// at its declared class"; CLAUDE.md D8 "every rewrite ships with its differential test").
//
// Reuses the M2 differential harness (verify/differential.hpp) for the interpreter half of a
// before/after comparison and adds an equivalent for the adjoint (no existing driver has quite
// its shape: a seeded reverse pass rather than an n-outputs-at-a-state forward one). "Before" and
// "after" are always TWO ways of building an exec::Interpreter / adjoint::Adjoint over the SAME
// recorded structure — either the same ir::Program with two different PlanAnnotations
// (verify_annotations, the common case for a planner-style rule) or two different ir::Programs
// (verify_rule, for a structural R1-R7-style rule's Proposal::program) — never a change to what
// the templated-double oracle itself would compute, which is a differential-tester question
// (verify/differential.hpp), not this file's.
#pragma once

#include <string>

#include "epykos/adjoint/adjoint.hpp"
#include "epykos/exec/interpreter.hpp"
#include "epykos/ir/annotate.hpp"
#include "epykos/ir/program.hpp"
#include "epykos/rewrite/rule.hpp"
#include "epykos/verify/differential.hpp"

namespace epykos::rewrite {

struct VerifyOptions {
  exec::Options interpreter{};
  adjoint::Options adjoint{};
  verify::BallOptions ball{};
  verify::Tolerance interpreter_tolerance = verify::Tolerance::e0();
  verify::Tolerance adjoint_tolerance = verify::Tolerance::e0();
  bool check_adjoint = true;
  int adjoint_ball_draws = 8;     // the adjoint check is O(draws x outputs_checked): kept small
  int max_outputs_checked = 32;   // outputs sampled evenly when the program has more than this
};

struct AdjointCheckResult {
  bool ran = false;
  bool passed = true;
  double max_out_ulps = 0.0;
  double max_state_bar_ulps = 0.0;
  int worst_output = -1;
  int worst_draw = -1;  // -1: the record point itself

  std::string summary() const;
};

struct VerifyReport {
  verify::Report interpreter;
  AdjointCheckResult adjoint;

  bool passed() const noexcept { return interpreter.passed && (!adjoint.ran || adjoint.passed); }
  std::string summary() const;
};

// Compares exec::Interpreter(before, options.interpreter) against
// exec::Interpreter(after, options.interpreter) (and, unless options.check_adjoint is false, the
// two equivalent adjoint::Adjoint objects) over `state` and an M2 ball around it. `before` /
// `after` stand on their own: attach whatever PlanAnnotations you want checked to `.plan` before
// calling (verify_annotations, below, does this for the common "same program, two plans" case).
VerifyReport compare_programs(const ir::Program& before, const ir::Program& after, const double* state, int n_inputs,
                              int n_outputs, const VerifyOptions& options = {});

// The common case: `program` unannotated (its own `.plan`, if any, is ignored) vs a copy of it
// carrying `plan`. Used to check that one rule's (or one pipeline's) PlanAnnotations output
// changes nothing an E0/E1 gate can see.
VerifyReport verify_annotations(const ir::Program& program, const ir::PlanAnnotations& plan, const double* state,
                                int n_inputs, int n_outputs, const VerifyOptions& options = {});

// Runs `rule` at every site match(program, base_plan) returns and verifies each one: an
// annotation Proposal via verify_annotations (merged onto `base_plan`); a structural Proposal via
// compare_programs(program, *proposal.program, ...) directly. Returns the FIRST failing site's
// report, or the last (trivially passing) one if `rule` does not match at all — check
// `sites_checked` to tell "passed" from "nothing to check". This is this package's own gate
// ("the verifier catches a deliberately wrong rule", PROBLEM.md §7): rewrite/verifier_test.cpp
// exercises it against a deliberately wrong rule and against the identity stubs.
struct RuleVerifyReport {
  VerifyReport report;
  int sites_checked = 0;
  bool passed() const noexcept { return sites_checked == 0 || report.passed(); }
};

RuleVerifyReport verify_rule(const Rule& rule, const ir::Program& program, const double* state, int n_inputs,
                            int n_outputs, const ir::PlanAnnotations& base_plan = {}, const VerifyOptions& options = {});

}  // namespace epykos::rewrite
