// EpykosEngine — the consumer of ir::PlanAnnotations::JacobianBlockPlan (M4/EG "integration";
// PROBLEM.md §7, D47/D49/D50). D47 left `jacobian.mode` unpopulated and unconsumed by design; R7's
// own header (rewrite/r7_block_linmap.hpp) and D50 point 5 both say plainly why a rule alone is
// not enough: "a rule populating it cannot be checked by CLAUDE.md's own gates ... and a mutant of
// that logic could not be caught by them either ... exactly what 'every rewrite ships with its
// differential test AND its mutation test' exists to keep out of the tree." This file is that
// consumer: given a Program whose `plan.jacobian.mode` names a block's AD mode (rewrite::
// ADModePerBlockRule, rewrite/ad_mode_rule.hpp, decides it), it actually computes that block's
// dense Jacobian the named way, so a differential test can compare the three modes against each
// other and a mutation test can catch a wrong decision or a wrong closed-form product.
//
// Namespace/location: `epykos::adjoint` because two of the three modes ARE the adjoint engine
// (Reverse) or exist to be compared against it (Forward); this file depends on both
// `exec::Interpreter` and `adjoint::Adjoint` (a new, but non-cyclic, dependency direction --
// neither of those headers depends on this one) and, for the closed-form check, on
// `rewrite::is_linmap_domain` (ir-structural only, no rewrite/adjoint cycle either).
#pragma once

#include <string>
#include <vector>

#include "epykos/adjoint/adjoint.hpp"
#include "epykos/exec/interpreter.hpp"
#include "epykos/ir/program.hpp"

namespace epykos::adjoint {

struct BlockJacobianRequest {
  std::string block_name;                      // key of program.plan.jacobian.mode; Reverse if absent (the engine's own existing default)
  std::vector<ir::domain_id> output_domains;    // ClosedFormAffine eligibility only (ignored for Forward/Reverse)
  std::vector<int> input_ordinals;              // Jacobian columns, the caller's own order
  std::vector<int> output_ordinals;             // Jacobian rows, the caller's own order
  double fd_step = 1e-6;                        // Forward mode's one-sided finite-difference step
};

// Dense row-major Jacobian (request.output_ordinals.size() x request.input_ordinals.size()) of
// `program`'s named outputs w.r.t. its named inputs, at the single lane `state`
// (program.inputs.size() entries), by whichever epykos::ir::AdMode
// program.plan.jacobian.mode[request.block_name] names:
//
//   Reverse (the default when block_name is not a key of the plan, matching D47/annotate.hpp
//   point 3's "left empty, derive your own"): `adj` (an adjoint::Adjoint already built over
//   `program`), one lane per requested output -- the SAME machinery every other adjoint gate in
//   this codebase already trusts, exact to its own floating-point contract.
//
//   Forward: one-sided finite differences of `interp` (an exec::Interpreter already built over
//   `program`) over each input ordinal -- a STAND-IN for true forward-mode AD, labelled as such:
//   RESUME.md's own M3/G5 entry states "forward mode = Dual on the templated maths (no tangent
//   interpreter)" as a stated simplification, so no generic IR-level tangent evaluator exists yet
//   for this file to call instead. Its purpose is to give ir::AdMode::Forward a real, checkable,
//   IR-level consumer with the cost model's own priced shape (n_inputs extra passes,
//   optimise::estimate_jacobian_ns) -- not to match Dual's precision. Gate at FD tolerance (1e-6
//   relative), never bitwise.
//
//   ClosedFormAffine: reads the constant Affine coefficients directly off every domain of
//   `request.output_domains` -- each one must satisfy rewrite::is_linmap_domain (checked here, not
//   assumed) AND every member its rows read must itself be a Program Input (a narrower, STATED
//   scope than the fully general linmap case: a member that is itself a computed value from
//   another domain would need that domain's own Jacobian chained in first, which this file does
//   not attempt) -- exact, no evaluation at all beyond reading the Program's own tables, because a
//   linmap's weights ARE its Jacobian.
//
// Throws std::invalid_argument when the mode is ClosedFormAffine but either check above fails, or
// when an input/output ordinal is out of range.
void block_jacobian(const ir::Program& program, const exec::Interpreter& interp, const Adjoint& adj, const double* state,
                    const BlockJacobianRequest& request, double* jacobian);

}  // namespace epykos::adjoint
