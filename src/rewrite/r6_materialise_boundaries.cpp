#include "epykos/rewrite/r6_materialise_boundaries.hpp"

#include <unordered_set>

#include "epykos/mutation/mutation.hpp"
#include "epykos/rewrite/greedy.hpp"

namespace epykos::rewrite {

R6MaterialiseBoundaries::R6MaterialiseBoundaries(MaterialiseBoundariesParams params)
    : params_(params), name_("r6.materialise_boundaries") {}

const std::string& R6MaterialiseBoundaries::name() const noexcept { return name_; }

// The local (single-domain) half of R6's test: `d`'s sole consumer if inlining `d` into it can
// never be wrong, independent of what any OTHER domain decides -- the cross-domain "is that
// consumer itself about to be inlined away" check lives in match() (it needs every domain's
// verdict at once). Mirrors planner::inline_producers_plan's own safety checks (rewrite/planner.cpp)
// exactly, so the two rules never disagree about what "safe to inline" means -- only about
// row_fusion_pays(lane_tile), which this rule does not consult at all.
ir::domain_id materialise_boundaries_consumer_of(const ir::Program& program, const planner::InlineAnalysis& analysis,
                                                 ir::domain_id d, const MaterialiseBoundariesParams& params) {
  const std::size_t ud = static_cast<std::size_t>(d);
  const ir::Domain& dom = program.domains[ud];
  if (dom.recurrent || dom.rows < 1) return -1;
  // Not "an intermediate" at all: an Input domain's rows already come from the caller's own
  // array (ir::SlotKind::Input, resolved straight off `ordinal_[]` -- ir/evaluate.hpp's `fetch`),
  // as cheap as a value gets. R6's own scope (this file's header) is single-use INTERMEDIATES;
  // whether the interpreter's InlineIntoConsumer path is even exercised for a raw Input producer
  // is untested territory this rule does not need to enter to do its job.
  if (program.groups[ud].steps.back().op == Op::Input) return -1;
  // "at a reduction" also means: a domain that IS a whole-domain Sum/Affine reduction never
  // inlines away itself, only ordinary elementwise producers do. planner::inline_producers_plan
  // (rewrite/planner.cpp) allows a whole-segment producer to inline too, under EXTRA conditions
  // (uniform segment length, and every one of ITS OWN segment's members independently
  // materialised) this rule does not replicate -- R6 is the unconditionally-safe subset, not a
  // reproduction of every case R0's cost-driven rule accepts.
  if (planner::is_whole_segment(program.groups[ud])) return -1;
  // "before a linmap, at a reduction" (the boundary from the READER's side): a Sum/Affine segment
  // member, or an output, must stay materialised -- planner::analyze_inline's own `blocked` flag.
  if (epykos::mutant("r6.ignore_reduction_boundary")) {
    // Defect: the boundary before a linmap/reduction is never checked, so a domain feeding one
    // gets inlined away anyway -- its consumer's Segment then reads value-buffer rows the
    // interpreter never wrote (an InlineIntoConsumer domain is evaluated only per-tile of ITS
    // named consumer, never into the shared value buffer another domain's segment reads from).
  } else {
    if (analysis.blocked[ud]) return -1;
  }
  if (analysis.readers[ud].empty()) return -1;
  // "on fan-out above a threshold": every reader gather of `d` must belong to the SAME one
  // consumer domain -- Materialise::InlineIntoConsumer names exactly one, so more than one
  // distinct consumer is never representable, at any threshold.
  const std::int32_t c = analysis.gather_user[analysis.readers[ud][0]];
  if (c < 0 || c == d) return -1;
  bool single_consumer = true;
  if (epykos::mutant("r6.ignore_fanout_boundary")) {
    // Defect: only the FIRST reader gather's user is checked, so a domain read by several
    // distinct consumer domains is still treated as having "the" one consumer `c` -- every OTHER
    // consumer then reads a row the interpreter never materialised (it only evaluates `d`'s rows
    // inside `c`'s own tiles).
  } else {
    for (std::size_t k : analysis.readers[ud]) single_consumer &= analysis.gather_user[k] == c;
  }
  if (!single_consumer) return -1;
  if (static_cast<double>(analysis.refs[ud]) > params.max_refs_per_row * static_cast<double>(dom.rows)) return -1;
  // The consumer itself must be a plain elementwise anchor: not a whole-domain reduction (it has
  // no "per tile" of its own to evaluate `d` inside), not recurrent (a scan's carry gather is not
  // an ordinary reader), matching planner::inline_producers_plan's own consumer-side checks.
  const std::size_t uc = static_cast<std::size_t>(c);
  if (planner::is_whole_segment(program.groups[uc]) || program.domains[uc].recurrent) return -1;
  return c;
}

std::vector<MatchSite> R6MaterialiseBoundaries::match(const ir::Program& program, const ir::PlanAnnotations& plan) const {
  const planner::InlineAnalysis analysis = planner::analyze_inline(program);
  const std::size_t nd = program.domains.size();
  std::vector<ir::domain_id> consumer(nd, -1);
  for (std::size_t d = 0; d < nd; ++d) {
    consumer[d] = materialise_boundaries_consumer_of(program, analysis, static_cast<ir::domain_id>(d), params_);
  }
  // A domain about to be inlined away cannot also serve as a stable consumer anchor for another
  // one (rewrite/planner.cpp's own left-to-right pass excludes this one level of chaining the
  // same way, via n_refs_to_consumer); R6 has no ordering to exploit it incrementally, so it
  // simply declines both ends of any such pair -- safe (never wrong), only potentially less
  // thorough than a fixed-point search would be.
  std::unordered_set<ir::domain_id> being_inlined;
  for (std::size_t d = 0; d < nd; ++d) {
    if (consumer[d] >= 0) being_inlined.insert(static_cast<ir::domain_id>(d));
  }
  std::vector<MatchSite> sites;
  for (std::size_t d = 0; d < nd; ++d) {
    if (consumer[d] < 0) continue;
    if (being_inlined.count(consumer[d]) != 0) continue;
    if (d < plan.domain.size() && !(plan.domain[d] == ir::DomainPlan{})) continue;  // already decided upstream
    sites.push_back(MatchSite::of_domain(static_cast<ir::domain_id>(d)));
  }
  return sites;
}

Proposal R6MaterialiseBoundaries::propose(const ir::Program& program, const ir::PlanAnnotations&, const MatchSite& site) const {
  Proposal p;
  if (site.kind != SiteKind::Domain || site.domain < 0 || static_cast<std::size_t>(site.domain) >= program.domains.size()) {
    return p;  // empty: not a site this rule would ever return from match()
  }
  const planner::InlineAnalysis analysis = planner::analyze_inline(program);
  const ir::domain_id c = materialise_boundaries_consumer_of(program, analysis, site.domain, params_);
  if (c < 0) return p;  // the program changed since match(); nothing to propose at a stale site
  ir::PlanAnnotations out;
  out.domain.assign(static_cast<std::size_t>(site.domain) + 1, ir::DomainPlan{});
  out.domain[static_cast<std::size_t>(site.domain)] = ir::DomainPlan{ir::Materialise::InlineIntoConsumer, {}, c};
  p.annotations = std::move(out);
  return p;
}

}  // namespace epykos::rewrite
