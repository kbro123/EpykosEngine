// EpykosEngine — plan annotations carried on the domain IR (M4/R0, PROBLEM.md §7, DESIGN.md §6/§7, D47).
//
// A rewrite::Rule (include/epykos/rewrite/rule.hpp) rewrites either the Program itself (a
// structural IR rewrite: R1-R7, DESIGN.md §6) or, for the interpreter's own planning decisions
// (materialisation, fused-pair / tail grouping, emitted outputs, per-Jacobian-block AD mode), an
// ANNOTATION of it — a decision about how to execute the recorded structure that does not change
// the structure itself. PlanAnnotations is that second kind, gathered under Program::plan:
//
//   domain[d]   materialisation choice for domain d: materialise it, fold it into the whole-
//               domain reductions that read it, or evaluate it per tile of the one elementwise
//               domain that gathers it (Interpreter::Impl::decide_fusion / decide_inline before
//               M4/R0; DESIGN.md §7 "tiled interpreter").
//   emit        output rows written directly by a reduction block instead of copied from the
//               value buffer afterwards (the same decide_fusion, split out because it depends on
//               `domain` choices already made: emit_outputs runs after reduction_fusion).
//   group[d]    which consecutive steps of domain d's group run as one fused-pair kernel, and
//               which further steps are chained onto it as a tail (build_pair_step / add_tail_step
//               before M4/R0).
//   jacobian    forward vs reverse vs closed-form-affine per named Jacobian block (a curve's
//               quotes, a scenario ladder, ...): the slot PROBLEM.md §7's AD-mode rule and M4/EG's
//               cost model fill in. R0 defines the slot only; nothing in R0 populates or reads it
//               yet (RESUME.md §3 M4 table: "AD mode per Jacobian block is a rule" is EG's package).
//
// PlanAnnotations is deliberately NOT part of a Program's identity: Program::operator== treats
// every Program with a `plan` field as if it were unannotated (PlanAnnotations::operator==
// below always returns true), and neither ir::serialize nor ir::deserialize ever read or write
// it. Three reasons, all load-bearing:
//   1. round-trip identity (DESIGN.md §5.7, D10) is a property of the RECORDING (the domains,
//      groups, gathers, segments the signature pass produced), never of a plan for one execution
//      of it -- a Program that has been planned for lane_tile 32 and one planned for lane_tile 1
//      are the same recording and must compare equal.
//   2. a materialisation choice is Options-dependent (decide_inline's row_fusion_pays(lane_tile)
//      gate, DESIGN.md §7): the SAME Program is annotated differently by two exec::Interpreter
//      instances built with different Options over it, so the annotations cannot be a stored,
//      canonical property of the Program value the way domains/groups/gathers are.
//   3. exec::Interpreter and adjoint::Adjoint take `const ir::Program&` and do not own it (two
//      Interpreters over one Program, e.g. the M1 B=1 / B=64 bench points, are ordinary and must
//      not see each other's decisions) -- so nothing in rewrite:: or exec:: ever mutates a
//      caller's Program to attach a plan. A caller that DOES want to compute a plan once and
//      reuse it (M4/EG's extraction, or a test comparing two rules' output) sets `program.plan`
//      itself; exec::Interpreter / adjoint::Adjoint use it when it is not empty() and otherwise
//      derive their own, equivalent to today's Options-driven default (see rewrite/planner.hpp).
//
// Rule authors: `rewrite::Rule::match` / `propose` take the CURRENT annotations as an explicit
// parameter (not through `program.plan`) precisely so a multi-rule pipeline (run_pipeline,
// rewrite/greedy.hpp) never has to copy the Program between rules -- copying a Stage-A-sized
// Program (517,036 nodes) once per planner rule would be real, measurable overhead the M1 bench
// would see (PROBLEM.md §7's "the default plan must be ... time-identical to M1's"). See
// rewrite/rule.hpp for the full contract.
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace epykos::ir {

// Where an elementwise domain's rows live during one execution of a plan (DESIGN.md §7).
enum class Materialise : std::uint8_t {
  Materialize = 0,        // every row written to the value buffer: the default, always correct
  FuseIntoReduction = 1,  // evaluated inside the whole-domain Sum / Affine blocks that read it
  InlineIntoConsumer = 2, // evaluated per tile of the one elementwise domain that gathers it
};

const char* to_string(Materialise m) noexcept;

// One domain's materialisation choice, plus the bookkeeping the interpreter's plan builder needs
// to reproduce the chosen execution bit for bit:
//   FuseIntoReduction: `keep_rows` (ascending) are rows still materialised anyway (read by a
//     gather, a per-row Sum/Affine, or an output the reduction does not emit -- decide_fusion's
//     `keep`); every other row of the domain is folded into its readers' reduction blocks.
//   InlineIntoConsumer: `inline_consumer` is the one elementwise domain whose tiles evaluate it.
// Materialize needs neither field (both stay at their default).
struct DomainPlan {
  Materialise choice = Materialise::Materialize;
  std::vector<std::int32_t> keep_rows;
  std::int32_t inline_consumer = -1;

  bool operator==(const DomainPlan&) const = default;
};

// A run of consecutive steps of one elementwise group the interpreter's kernel builder treats as
// one call (DESIGN.md §7 "fused pairs" / "chain tails"): `first` and `second` (indices into the
// domain's Group::steps) chained through a value nothing else reads (build_pair_step's shape
// match); `tail` further step indices (Exp / Log applied in place: add_tail_step's shape match),
// applied after `second` (or after `first` alone when there is no pair: `second == -1`, a lone
// step that still grew a tail). A step of the group named by no StepPairing runs on its own,
// exactly as when Options::fuse_pairs (or the equivalent rule) is off.
struct StepPairing {
  std::int32_t first = -1;
  std::int32_t second = -1;
  std::vector<std::int32_t> tail;

  bool operator==(const StepPairing&) const = default;
};

// One domain's step groupings, in ascending `first` order, covering every step of the domain's
// group at most once (a step covered by no StepPairing is not fused with either neighbour).
struct GroupPlan {
  std::vector<StepPairing> pairings;

  bool operator==(const GroupPlan&) const = default;
};

// Reverse-mode adjoint vs forward-mode (Dual) vs a closed-form affine Jacobian, chosen per named
// Jacobian block (DESIGN.md §3.2, §7; PROBLEM.md §7). R0 defines this slot only -- no rule in
// this package populates it and no consumer reads it yet; the cost-model rule that will (M4/EG)
// keys blocks however its own caller does (a curve's free quotes, one scenario lane's ladder, ...)
// so this header does not hard-code Stage A's or any other problem's shape (HARD RULE 9: rules
// are keyed on IR structure or on a caller-supplied key, never on a magic instance count).
enum class AdMode : std::uint8_t { Forward = 0, Reverse = 1, ClosedFormAffine = 2 };

const char* to_string(AdMode m) noexcept;

struct JacobianBlockPlan {
  std::unordered_map<std::string, AdMode> mode;

  bool operator==(const JacobianBlockPlan&) const noexcept { return true; }  // see file header, point 1
};

// The full plan a rewrite::Rule pipeline (rewrite/greedy.hpp: run_pipeline) produces and
// exec::Interpreter / adjoint::Adjoint consume in place of deciding internally. Indexed by
// domain id where noted; a Program with N domains carries either `domain.size() == 0` (nothing
// decided: every consumer falls back to its own default rule pass) or `domain.size() == N`
// (every domain has an explicit, possibly-default, entry) -- there is no partial state in
// between that a consumer is expected to interpret; a rule pipeline that only touches some
// domains fills the rest with default-constructed entries before merge_annotations returns
// (rewrite/greedy.hpp).
struct PlanAnnotations {
  std::vector<DomainPlan> domain;
  std::vector<std::uint8_t> emitted;  // size == Program::num_values() when populated; row v emitted, not copied
  std::vector<GroupPlan> group;       // size == Program::domains.size() when populated; group[d] pairs domain d's steps
  JacobianBlockPlan jacobian;

  bool empty() const noexcept { return domain.empty() && emitted.empty() && group.empty() && jacobian.mode.empty(); }

  // See the file header: annotations are derived planning state, never part of a Program's
  // identity, so this is unconditionally true and Program::operator== (= default) is unaffected
  // by whatever plan two otherwise-identical Programs happen to carry.
  bool operator==(const PlanAnnotations&) const noexcept { return true; }
};

}  // namespace epykos::ir
