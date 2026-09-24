// EpykosEngine — fma contraction: a*b+c chains folded into one Op::Fma step (M4/R-b; class E1).
//
// Target: within one domain's group, a Step `x = Mul(a, b)` used by exactly one later Step of the
// SAME group, `y = Add(x, c)` (or `Add(c, x)`, Add is commutative), where nothing else in the
// group (and nothing outside it: Mul/Add operands are always Step/Literal/Column/Gather/Segment
// slots, never read across domains except through a gather, so "nothing else in the group" is the
// whole check) reads `x`. Fusing gives `y = fma(a, b, c)` in ONE step instead of two, with ONE
// rounding instead of two -- `std::fma` differs from `a*b` (rounded) `+ c` (rounded again) in the
// last bit for some operands (DESIGN.md §10), hence class **E1** (<= 1 ulp), never E0, and this
// rewrite is deliberately left OFF an E0 extraction (CLAUDE.md "no -ffast-math"; this is the one
// FP-contraction the ENGINE chooses to make, explicitly, rather than the one a compiler flag would
// make silently and inconsistently across GCC/clang -- D25/D46's whole point).
//
// This is not one of DESIGN.md §6's R1-R7 (it has no SwapEngine analogue named there); it is the
// package's own second rewrite named directly in PROBLEM.md §7 / RESUME.md's M4 table text ("an
// fma-contraction rule (E1) for a*b+c chains where the recorded order permits one rounding
// change"). It never touches gathers, segments or domain boundaries: purely a local edit of one
// group's Step list (remove the Mul step, replace the Add step in place, renumber the group's own
// Step-kind slot indices) -- no other domain, gather or segment table changes at all.
#pragma once

#include <string>
#include <vector>

#include "epykos/rewrite/rule.hpp"

namespace epykos::rewrite {

class FmaContractionRule final : public Rule {
 public:
  const std::string& name() const noexcept override;
  Exactness exactness_class() const noexcept override { return Exactness::E1; }
  std::vector<MatchSite> match(const ir::Program& program, const ir::PlanAnnotations& plan) const override;
  Proposal propose(const ir::Program& program, const ir::PlanAnnotations& plan, const MatchSite& site) const override;
};

}  // namespace epykos::rewrite
