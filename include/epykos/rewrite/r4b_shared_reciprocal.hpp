// EpykosEngine — R4b: a/b -> a * recip(b), shared reciprocal (M4/R-b; DESIGN.md §6, class E1).
//
// Target: a step `y = Div(a, gather(g))` where `g` reads a single, homogeneous source domain `S`
// (see r4a_push_unary_through_gathers.hpp's header for why heterogeneous gathers are out of
// scope). Every such step across the WHOLE program that shares the same `S` is handled by ONE
// proposal: a new domain `S_recip` (rows = S.rows, one `Recip` step over an identity gather into
// `S`) is inserted right after `S` (rewrite/ir_edit.hpp), and every matching step becomes
// `a * gather(S_recip)` in place. This is a real IEEE rounding-order change (`a / b` is not
// bit-identical to `a * (1/b)` in general -- DESIGN.md §12's own residual, "two IEEE divisions per
// forward element vs one shared reciprocal"), hence class **E1** (checked at 4 ulps, D26), never
// E0. A site is included only when relocating is actually profitable: its OWN gather reuses at
// least one `S` row more than once (real per-site redundancy, r4a's own "toward the smaller
// domain"), OR at least one OTHER `Div`-of-`gather(S)` site exists elsewhere in the program (the
// reciprocal is then genuinely SHARED, `S_recip`'s cost amortised over more than one reader,
// this rule's whole point) -- a single, non-redundant division by a domain nobody else reads is
// left alone (E1 for no measured benefit is not worth taking).
#pragma once

#include <string>
#include <vector>

#include "epykos/rewrite/rule.hpp"

namespace epykos::rewrite {

class R4bSharedReciprocal final : public Rule {
 public:
  const std::string& name() const noexcept override;
  Exactness exactness_class() const noexcept override { return Exactness::E1; }
  std::vector<MatchSite> match(const ir::Program& program, const ir::PlanAnnotations& plan) const override;
  Proposal propose(const ir::Program& program, const ir::PlanAnnotations& plan, const MatchSite& site) const override;
};

}  // namespace epykos::rewrite
