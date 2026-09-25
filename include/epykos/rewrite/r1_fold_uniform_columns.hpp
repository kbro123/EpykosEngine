// EpykosEngine — R1: fold uniform columns (M4/R-a; DESIGN.md §6, class E0).
//
// A Column whose values are bit-identical across every row of its domain contributes nothing a
// per-row load buys: every row's kernel would read the same double. R1 folds such a column into a
// Literal (shared by every row) and rewrites the ONE slot of the domain's group that read it (a
// domain's group is the shared op sequence of every row, so a Column is read by at most one slot
// per operand position, never once per row) to read the new literal instead; the Column entry
// itself is left in `Program::columns`, unreferenced (like a dead constant after DCE elsewhere in
// the pipeline: ir::validate does not require every column be read by some step, only that a
// column a step DOES read belongs to that step's domain and has the right length). SwapEngine
// analogue: `cpn_is_plain`.
//
// One domain is one MatchSite (SiteKind::Domain): a domain with more than one uniform column folds
// all of them in a single `propose()` call, since folding one does not change whether another one
// of the SAME domain is uniform (columns are independent per-row facts). `match` returns a site
// only when folding would change something: the domain has at least one column that is (a) its
// own (`Column::domain == d`), (b) bit-identical across every row and (c) actually read by one of
// the domain's own steps -- a column nothing reads is already dead and folding it would be a
// Proposal that changes nothing, which rule.hpp's contract for `match` forbids.
#pragma once

#include <string>
#include <vector>

#include "epykos/rewrite/rule.hpp"

namespace epykos::rewrite {

class R1FoldUniformColumns final : public Rule {
 public:
  const std::string& name() const noexcept override;
  Exactness exactness_class() const noexcept override { return Exactness::E0; }
  std::vector<MatchSite> match(const ir::Program& program, const ir::PlanAnnotations& plan) const override;
  Proposal propose(const ir::Program& program, const ir::PlanAnnotations& plan, const MatchSite& site) const override;
};

}  // namespace epykos::rewrite
