// EpykosEngine — R5: group formation (M4/R-b; DESIGN.md §6, class E0).
//
// Target: a "producer" domain `P`, read by exactly one gather anywhere in the program, owned by
// exactly one "consumer" domain `D`, and that gather is the plain IDENTITY over `P` (`D.rows ==
// P.rows`, row `r` of `D` reads row `r` of `P`, nothing reordered or repeated -- the case R4a/R4b
// deliberately do NOT touch, since there is no redundancy to remove here, only an unnecessary
// domain boundary: `P`'s whole row is materialised to the value buffer and then immediately read
// straight back by the one domain that gathers it) -- and `D` itself feeds a Segment used by a
// `Sum`/`Affine` step somewhere (DESIGN.md §6's own "with ... a segment-sum epilogue": every
// Segment table exists only to serve a Sum or Affine step, program.hpp's own "segment: the
// members of a variadic op", so "`D` is read by any segment" already means "`D` feeds a
// segment-sum"). `P`'s whole step sequence is spliced onto the FRONT of `D`'s own group
// (rewrite::detail::merge_producer_into_consumer, rewrite/ir_edit.hpp) and `P` disappears: the
// interpreter tiles `D`'s (now longer) group directly instead of materialising `P`'s rows to the
// value buffer and gathering them straight back, and a later CATALOGUE kernel (DESIGN.md §7 tier
// 1, M4/C1) sees the fused region ahead of time (this rule's stub-file comment, restated).
//
// One application merges one (P, D) pair; a chain of more than two elementwise domains is formed
// by re-matching after each merge (D, now longer, may itself be a fresh producer for the domain
// that reads IT) -- rewrite::apply_greedy / a caller's own fixpoint loop, not a single match()
// call, is what makes the merged region "maximal" (DESIGN.md §6's word) rather than exactly one
// pair. Recurrent (scan) domains are out of scope on both sides, and neither domain's rows may be
// a direct Program output (merging would otherwise need to redirect an output ordinal, not just a
// gather -- rewrite::detail::merge_producer_into_consumer's own precondition).
#pragma once

#include <string>
#include <vector>

#include "epykos/rewrite/rule.hpp"

namespace epykos::rewrite {

class R5GroupFormation final : public Rule {
 public:
  const std::string& name() const noexcept override;
  Exactness exactness_class() const noexcept override { return Exactness::E0; }
  std::vector<MatchSite> match(const ir::Program& program, const ir::PlanAnnotations& plan) const override;
  Proposal propose(const ir::Program& program, const ir::PlanAnnotations& plan, const MatchSite& site) const override;
};

}  // namespace epykos::rewrite
