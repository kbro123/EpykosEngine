// EpykosEngine — R3: elide trivial maps (M4/R-a; DESIGN.md §6, class E0).
//
// A domain whose ENTIRE group is one `Sum` over a segment with exactly one member per row computes
// nothing: row r's value is that member's value id, unchanged. Such a domain can only arise from a
// signature-pass BOUNDARY (fan-out > 1, an output, ... DESIGN.md §5.3) that gave a plain relabelling
// of an earlier value its own class -- the general form of the "identity gather" DESIGN.md §6 names
// (a `Sub` domain that just reads a `DF` row 1:1: its Sum segment's one member IS that DF value id,
// with nothing else in the group to say otherwise). R3 removes such a domain outright, redirecting
// every reference anywhere in the program to one of its rows to the member's value id directly
// (`rewrite::detail::eliminate_domain`, ir_edit.hpp): the indirection disappears, not just its cost
// -- no Column, Gather or Segment table is left behind for it, unlike R1's folded-but-present
// column. SwapEngine analogue: `sub_is_identity`.
//
// Scope of this landing (an honest restriction; see also r2_bucket_rows.hpp's own scope note): only
// a domain whose group has EXACTLY ONE step is eligible. A length-1 Affine (`konst + c_0·x_0`) is
// identity only in the special case `konst == 0` and `c_0 == 1`, and the tape's own affine_collapse
// pass (src/tape/passes.cpp, mutant `affine.single_term_unscaled`) already emits a bare atom instead
// of an Affine node whenever that holds -- so a length-1 Affine surviving into the domain IR is, by
// construction, never trivial, and this landing does not touch Affine at all (DESIGN.md §6's "Sum /
// Affine over exactly one member" is the general STRUCTURAL shape a length-1 segment can take;
// folding one to identity is only ever sound for Sum).
//
// Non-recurrent, non-scan-class domains only (SiteKind::Domain): `eliminate_domain` redirects a
// row's readers to a value STRICTLY EARLIER in evaluation order, which a scan's own carry gather
// (a row reading the PREVIOUS ROW OF ITS OWN DOMAIN) is not.
#pragma once

#include <string>
#include <vector>

#include "epykos/rewrite/rule.hpp"

namespace epykos::rewrite {

class R3ElideTrivialMaps final : public Rule {
 public:
  const std::string& name() const noexcept override;
  Exactness exactness_class() const noexcept override { return Exactness::E0; }
  std::vector<MatchSite> match(const ir::Program& program, const ir::PlanAnnotations& plan) const override;
  Proposal propose(const ir::Program& program, const ir::PlanAnnotations& plan, const MatchSite& site) const override;
};

}  // namespace epykos::rewrite
