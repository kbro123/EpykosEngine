// EpykosEngine — R4a: push pure unary ops through gathers toward the smaller domain, then CSE
// (M4/R-b; DESIGN.md §6, class E0).
//
// Target: a step `y = op(gather(g))` (op one of Exp/Log/Sqrt/Neg; the step's ONLY operand is a
// gather, nothing else) where `g` reads a single, homogeneous source domain `S` (every row of the
// gather comes from `S`; a gather mixing several source domains -- program.hpp §"the value may
// come from any domain, including several domains for one gather" -- is not this rewrite's
// target: there is no single smaller domain to push the op onto) AND that gather is not already
// injective (some `S` row is read more than once: real redundancy, `op` computed once per
// consuming row instead of once per DISTINCT `S` row it actually needs -- DESIGN.md's own framing,
// "toward the SMALLER domain"). All matching steps that share the same `(S, op)` are handled by
// ONE proposal: a single new domain `S_op` (rows = S.rows, one elementwise step `op` over an
// identity gather into `S`) is inserted right after `S` (rewrite/ir_edit.hpp), and every matching
// step becomes `gather(S_op) * 1.0` in place -- a literal multiply by the exact IEEE identity
// (`x * 1.0 == x` for every finite `x`, signed zero and NaN alike), so the class stays **E0**
// without needing to eliminate the (possibly still directly-read) original domain, and so the
// rewrite is a single local edit at each site rather than a value-space-wide renumbering there
// too (only `insert_domain_after`'s own bookkeeping renumbers anything, and only once per
// proposal regardless of how many sites share `(S, op)`).
//
// Recurrent (scan) domains are out of scope on the READING side (a scan's carry/wave semantics
// are not something this rewrite reasons about); `S` itself may be anything, recurrent or not.
#pragma once

#include <string>
#include <vector>

#include "epykos/rewrite/rule.hpp"

namespace epykos::rewrite {

class R4aPushUnaryThroughGathers final : public Rule {
 public:
  const std::string& name() const noexcept override;
  Exactness exactness_class() const noexcept override { return Exactness::E0; }
  std::vector<MatchSite> match(const ir::Program& program, const ir::PlanAnnotations& plan) const override;
  Proposal propose(const ir::Program& program, const ir::PlanAnnotations& plan, const MatchSite& site) const override;
};

}  // namespace epykos::rewrite
