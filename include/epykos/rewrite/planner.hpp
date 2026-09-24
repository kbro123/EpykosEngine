// EpykosEngine — the M1 planner's decisions as pure functions (M4/R0; DESIGN.md §7, D47).
//
// Before M4/R0 these lived inside exec::Interpreter::Impl (decide_fusion, decide_inline,
// build_pair_step, add_tail_step, src/exec/interpreter.cpp) as one constructor's worth of
// decide-and-build code. This header is the SINGLE SOURCE OF TRUTH for the "decide" half of each
// — a pure function of an ir::Program (and, where a later decision depends on an earlier one, of
// the ir::PlanAnnotations decided so far), taking no exec::Interpreter state. Two things consume
// it, and must never disagree because they call the very same functions:
//   - the five rewrite::planner::*Rule classes (planner_rules.hpp), each a rewrite::Rule whose
//     `propose` wraps one function below into a PlanAnnotations;
//   - exec::Interpreter's own plan builder (src/exec/interpreter.cpp), which now reads
//     annotations (either a caller's `program.plan`, or its own default rule pass over Options,
//     built via these same functions) instead of deciding internally, and keeps the "build"
//     half — resolving Slots to runtime Operands, looking up kernel function pointers — which
//     needs exec::Interpreter::Impl's own tables and so cannot live here.
//
// Every function is a pure read of its ir::Program argument (and PlanAnnotations argument, where
// present): no allocation beyond its own return value, no reference to any exec:: or adjoint::
// type, safe to call from a test or from a future e-graph driver (M4/EG) without constructing an
// Interpreter at all.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "epykos/ir/annotate.hpp"
#include "epykos/ir/program.hpp"
#include "epykos/tape/op.hpp"

namespace epykos::rewrite::planner {

// A group that runs as a whole-domain Sum / Affine pass (DESIGN.md §7): exactly one step, a Sum
// or Affine over a segment. Shared with exec::Interpreter (src/exec/interpreter.cpp defines its
// own copy no longer; it calls this one) so the fusion, inlining and pairing decisions below and
// the interpreter's plan builder can never classify a group differently.
bool is_whole_segment(const ir::Group& g) noexcept;

// Chain tails and inlined producers amortise their per-row dispatch at lane_tile 1 or >= 16
// (DESIGN.md §7 measured note; RESUME.md §5 M3 result: "row-fusion's lane-tile gate ... is a
// cost-model decision waiting to be made, not yet a rule") -- unmoved from interpreter.cpp, kept
// here as the threshold planner.fused_pairs / planner.chain_tails / planner.inline_producers
// apply until M4/CM's cost model supersedes it with a real rule.
bool row_fusion_pays(int lane_tile) noexcept;

// ---------------------------------------------------------------------------------------------
// planner.reduction_fusion / planner.emit_outputs (decide_fusion before M4/R0)
// ---------------------------------------------------------------------------------------------

struct ReductionFusionParams {
  double max_refs_per_row = 2.0;    // interpreter.cpp: fuse_max_refs_per_row
  double max_kept_fraction = 0.5;   // fuse_max_kept_fraction
  // NOT independent of planner.emit_outputs' own threshold, despite the two being separate rules:
  // decide_fusion (before M4/R0) computed the keep set and the emit set in the SAME pass because
  // an output row that will be emitted from the reduction block need not also be kept
  // (materialised) — an output row that WON'T be emitted (rows * lane_tile * 8 bytes below this)
  // must be kept, or its value is written nowhere. Both rules default to the same threshold; a
  // caller that changes one without the other gets exactly that behaviour change, deliberately.
  std::size_t emit_min_bytes = std::size_t{64} << 10;
};

struct EmitOutputsParams {
  std::size_t emit_min_bytes = std::size_t{64} << 10;  // interpreter.cpp: emit_min_bytes
};

// One pass over program.gathers / program.segments classifying every value as `marked` (read by
// a gather or a per-row Sum/Affine: must stay materialised) or a `whole_member` (a member of a
// whole-domain Sum/Affine group, with `member_refs[domain]` counting how many times), plus which
// values are `is_output`. Shared by reduction_fusion_plan and emit_outputs_plan so both use the
// same classification (decide_fusion computed it once for both; splitting the two decisions into
// two rules must not let them silently drift apart).
struct FusionAnalysis {
  std::vector<std::uint8_t> marked;
  std::vector<std::uint8_t> whole_member;
  std::vector<std::uint8_t> is_output;
  std::vector<std::size_t> member_refs;  // per domain
};

FusionAnalysis analyze_fusion(const ir::Program& program);

// Domain `d` folds into the whole-domain reductions that read it (decide_fusion's `fused[d] = 1`
// before M4/R0): not recurrent, has rows, not itself a whole-domain reduction, referenced as a
// member at most `params.max_refs_per_row` times per row, and — once `keep_rows` (ascending: the
// rows read some other way, or an output row the domain cannot emit at `lane_tile`) is computed —
// at most `params.max_kept_fraction` of its rows kept. `keep_rows` is populated regardless of the
// verdict (so a caller can inspect why a domain was rejected); the return value is decide_fusion's
// own accept/reject test. `lane_tile` is the interpreter's lane-chunk width (Lt): see
// ReductionFusionParams::emit_min_bytes for why this rule needs it too.
bool decide_reduction_fusion(const ir::Program& program, const FusionAnalysis& analysis, ir::domain_id d,
                             const ReductionFusionParams& params, int lane_tile, std::vector<std::int32_t>* keep_rows);

// The full planner.reduction_fusion decision: one DomainPlan per domain (FuseIntoReduction with
// its keep_rows, or the default Materialize), sized to program.domains.size().
ir::PlanAnnotations reduction_fusion_plan(const ir::Program& program, int lane_tile, const ReductionFusionParams& params = {});

// planner.emit_outputs: reads `plan.domain[d].choice` (planner.reduction_fusion must already
// have run) and decide_fusion's own per-row test — an output row that is a whole-domain member,
// not `marked`, of a domain whose region is at least `params.emit_min_bytes` (rows * lane_tile *
// 8) — to mark PlanAnnotations::emitted. `lane_tile` is the interpreter's lane-chunk width (Lt).
ir::PlanAnnotations emit_outputs_plan(const ir::Program& program, const ir::PlanAnnotations& plan, int lane_tile,
                                     const EmitOutputsParams& params = {});

// ---------------------------------------------------------------------------------------------
// planner.inline_producers (decide_inline before M4/R0)
// ---------------------------------------------------------------------------------------------

struct InlineProducersParams {
  double max_refs_per_row = 1.25;  // interpreter.cpp: inline_max_refs_per_row
};

// decide_inline's per-gather / per-domain bookkeeping: `blocked[d]` is true when domain d is read
// other than through a gather (an output, or a segment member); `gather_user[g]` is the one
// domain whose steps read gather g, or -2 when several do; `readers[d]` are the gather slots
// holding some of domain d's row ids, and `refs[d]` the total row references across them.
struct InlineAnalysis {
  std::vector<std::uint8_t> blocked;
  std::vector<std::int32_t> gather_user;
  std::vector<std::vector<std::size_t>> readers;
  std::vector<std::size_t> refs;
};

InlineAnalysis analyze_inline(const ir::Program& program);

// planner.inline_producers: reads `plan.domain[*].choice` for domains planner.reduction_fusion
// already chose to fuse (never also inlined) and decides, in Program order (so a later domain's
// test can see an earlier one's acceptance, matching decide_inline's single left-to-right pass),
// which domains inline into which consumer. Empty (as if it had not run) when
// !row_fusion_pays(lane_tile) — decide_inline's own early return.
ir::PlanAnnotations inline_producers_plan(const ir::Program& program, const ir::PlanAnnotations& plan, int lane_tile,
                                         const InlineProducersParams& params = {});

// ---------------------------------------------------------------------------------------------
// planner.fused_pairs / planner.chain_tails (build_pair_step / add_tail_step before M4/R0)
// ---------------------------------------------------------------------------------------------

// Per-step use counts: how many times step index k of `grp` is read by a later step of the same
// group as a Slot::Step operand. Shared with exec::Interpreter's plan builder.
std::vector<int> step_uses(const ir::Group& grp);

// An operand's source, at the granularity build_pair_step's kernel table distinguishes (a
// literal or column is interchangeable there — "Scalar" — a gather is not): mirrors
// exec::detail::PK without depending on exec/ (rewrite/ has no exec dependency, by design).
enum class OperandKind : std::uint8_t { Scalar = 0, Gathered = 1, None = 2 };

OperandKind operand_kind_of(const ir::Slot& s) noexcept;

// The shape of a fused pair at (k, k+1) of `grp`: step k's op and operands (already
// swap-normalised for a commutative op, exactly as build_pair_step did — `a`/`ka` first, `b`/`kb`
// second, `b`/`kb` at None for Neg) and step k+1's op and its one non-`prev` operand (`c`/`kc`,
// None for Neg), with `prev_right` set when that operand is on the LEFT of a non-commutative
// op2 (so the pair computes `c op2 prev`, not `prev op2 c`). std::nullopt: no match (build_pair_
// step's own shape test returning false, before M4/R0).
struct PairShape {
  Op op1 = Op::Const;
  Op op2 = Op::Const;
  ir::Slot a, b, c;
  OperandKind ka = OperandKind::None, kb = OperandKind::None, kc = OperandKind::None;
  bool prev_right = false;
};

std::optional<PairShape> match_pair(const ir::Group& grp, const std::vector<int>& uses, std::size_t k);

// Step `st` (immediately after step `prev` of the same group) continues the chain as a tail:
// `n_tail_so_far < max_tail`, `st.op` is Exp or Log, and its one operand is exactly step `prev`'s
// value (Slot{Step, prev}).
bool match_tail(const ir::Step& st, std::size_t prev, int n_tail_so_far, int max_tail);

// planner.fused_pairs: one StepPairing{first = k, second = k + 1} per accepted match_pair, domain
// by domain, scanning left to right and skipping both consumed steps (build_pair_step's greedy
// order before M4/R0); a whole-segment group or a fixed-arity Sum step (ir::is_fixed_sum, scan
// groups only) is never a candidate, matching build_group's own control flow. `tail` is left
// empty (planner.chain_tails' job).
ir::PlanAnnotations fused_pairs_plan(const ir::Program& program);

// planner.chain_tails: extends `plan`'s existing pairings (planner.fused_pairs must already have
// run — a domain with no GroupPlan entry is left alone) with trailing tail step indices, while
// row_fusion_pays(lane_tile) and each next step is used exactly once and match_tail accepts it
// (`max_tail`: StepPairing has no hard-coded limit, but interpreter.cpp's StepPlan::max_tail is
// 2 — callers building an interpreter plan from this pass `2`; a test exploring the rule in
// isolation may pass any bound).
ir::PlanAnnotations chain_tails_plan(const ir::Program& program, const ir::PlanAnnotations& plan, int lane_tile,
                                    int max_tail);

}  // namespace epykos::rewrite::planner
