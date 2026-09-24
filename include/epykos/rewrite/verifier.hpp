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
#include <vector>

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

// Verifies an e-graph extraction's PROGRAM against the TRUE, unrewritten root it came from -- not
// merely that its OWN PlanAnnotations are transparent given whatever structural rewrites already
// produced it. D57 (a review finding on the M4-gate-1 landing): every shipped
// tests/optimise/egraph_*_test.cpp called only `verify_annotations(result.program, result.plan,
// ...)`, which compares `result.program` against ITSELF (plan cleared vs the same plan) --
// verify_annotations' own documented contract, correct for what IT checks, but never a comparison
// against the original tape, so a composed chain of more than one E1-classed structural rewrite
// (fma_contraction and R4bSharedReciprocal are both landed as E1, D51) could in principle drift
// arbitrarily from ground truth with no gate ever noticing. Call this ALONGSIDE verify_annotations
// (which still has its own job: catching a planner-rule regression), not instead of it.
//
// `history` is the rule-name trail the extraction followed (ExtractResult::history, or the
// program-history prefix of it: a plan-only rule changes no value an E0/E1 differential can see,
// so including plan history in `history` costs nothing but adds no real risk either);
// `e1_rule_names` is the subset of names in `history` that the CALLER knows are E1-classed --
// taken as a parameter, never hard-coded here (HARD RULE 9), typically `rule->name()` for every
// `rule->exactness_class() == Exactness::E1` in whatever rule set built the e-graph. The tolerance
// actually used is the UNION (widen, never narrow) of `options.interpreter_tolerance` /
// `.adjoint_tolerance` as given and a bound derived from `history`: bitwise (E0) when none of
// `history` names an E1 rule, else `verify::Tolerance::e1(4.0 * count)` with `count` = how many
// `history` entries are E1-classed -- an explicit, stated way to bound a CHAIN of independently
// ~4-ulp-bounded roundings without pretending accumulation cannot happen. Passing default-
// constructed `options` (E0) therefore gets exactly the history-derived bound; passing an already-
// looser tolerance for a reason orthogonal to `history` (e.g. `exec::ExpMode::poly`'s own E1
// deviation from `std::exp`) keeps it, widened further only if `history` needs more. This is still
// an UNSCALED ulps-of-value bound, so it inherits fma_contraction.hpp's own D57 caveat wherever
// `history` includes a rewrite (fma_contraction today) whose soundness depends on a scale other
// than the output: a caller whose composed history may hit catastrophic cancellation should check
// that case separately, scaled, rather than trust this function's default bound to catch it (D57's
// own fma_contraction regression test does exactly that, directly, without going through this
// function at all).
VerifyReport verify_extraction(const ir::Program& original, const ir::Program& extracted, const ir::PlanAnnotations& extracted_plan,
                               const std::vector<std::string>& history, const std::vector<std::string>& e1_rule_names,
                               const double* state, int n_inputs, int n_outputs, VerifyOptions options = {});

}  // namespace epykos::rewrite
