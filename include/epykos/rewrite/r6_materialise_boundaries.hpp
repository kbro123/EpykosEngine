// EpykosEngine — R6: materialise only at domain boundaries (M4/R-c; DESIGN.md §6, class E0).
//
// planner.inline_producers (rewrite/planner_rules.hpp, M4/R0) already inlines a single-use
// elementwise producer into its one consumer, but only when the interpreter's own
// `row_fusion_pays(lane_tile)` gate says the per-row dispatch overhead is worth amortising at
// THIS run's lane_tile (DESIGN.md §7's measured note) — a decision that must be re-made every
// time an exec::Interpreter / adjoint::Adjoint is built over the program, and that a catalogue
// kernel (M4/C1, generated once, not "per lane_tile") has no `lane_tile` to make it with. R6 is
// the lane_tile-INDEPENDENT half of the same call: a domain inlines when doing so can never be
// wrong, full stop — never at a domain boundary DESIGN.md §6 names outright:
//   - before a `linmap` / at a reduction: the domain is a member of some Sum or Affine segment
//     (planner::InlineAnalysis::blocked — the same flag planner.inline_producers already
//     consults, recomputed here rather than shared state, per rewrite/rule.hpp point 6);
//   - on fan-out above a threshold: read by more than one distinct consumer domain (structural —
//     `Materialise::InlineIntoConsumer` names exactly one consumer, so this is never representable
//     regardless of any threshold), or referenced more than `params.max_refs_per_row` times per
//     row by its one consumer (the rule's own parameter — R0's inline_producers defaults this
//     to 1.25 as a PROFITABILITY margin; R6 defaults to 1.0, "read exactly once per row", because
//     it has no lane_tile to weigh against and is meant to be unconditionally safe).
// Everywhere else, R6 proposes exactly planner.inline_producers' own InlineIntoConsumer /
// inline_consumer pair, so the two rules can never disagree about what "safe to inline" means —
// only about when it is worth doing.
//
// Site granularity: SiteKind::Domain, one site per producer domain R6 would inline (DESIGN.md §6's
// rewrites are local; see rewrite/rule.hpp and rewrite/planner_rules.hpp's own file header for why
// this differs from the five whole-program planner rules).
#pragma once

#include <string>
#include <vector>

#include "epykos/rewrite/planner.hpp"
#include "epykos/rewrite/rule.hpp"

namespace epykos::rewrite {

struct MaterialiseBoundariesParams {
  // R6's own fan-out threshold (a rule parameter, DESIGN.md §6): a producer domain read more than
  // this many times per row (averaged over its rows) by its one consumer stays materialised.
  // R0's planner.inline_producers uses 1.25 (a profitability margin against lane_tile dispatch
  // cost); R6 has no such cost to weigh, so its default is the strict "exactly once" boundary.
  double max_refs_per_row = 1.0;
};

class R6MaterialiseBoundaries final : public Rule {
 public:
  explicit R6MaterialiseBoundaries(MaterialiseBoundariesParams params = {});

  const std::string& name() const noexcept override;
  Exactness exactness_class() const noexcept override { return Exactness::E0; }
  std::vector<MatchSite> match(const ir::Program& program, const ir::PlanAnnotations& plan) const override;
  Proposal propose(const ir::Program& program, const ir::PlanAnnotations& plan, const MatchSite& site) const override;

 private:
  MaterialiseBoundariesParams params_;
  std::string name_;
};

// The pure decision shared by match() and propose() (rewrite/rule.hpp point 6: cheap enough to
// recompute rather than cache): domain `d`'s sole consumer domain if R6 would inline it, else -1.
// Exposed for tests and for R7/EG callers that want the same structural test without an
// exec::Options-shaped Rule around it.
ir::domain_id materialise_boundaries_consumer_of(const ir::Program& program, const planner::InlineAnalysis& analysis,
                                                 ir::domain_id d, const MaterialiseBoundariesParams& params);

}  // namespace epykos::rewrite
