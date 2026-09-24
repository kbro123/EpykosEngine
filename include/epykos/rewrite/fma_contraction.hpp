// EpykosEngine — fma contraction: a*b+c chains folded into one Op::Fma step (M4/R-b; class E1).
//
// Target: within one domain's group, a Step `x = Mul(a, b)` used by exactly one later Step of the
// SAME group, `y = Add(x, c)` (or `Add(c, x)`, Add is commutative), where nothing else in the
// group (and nothing outside it: Mul/Add operands are always Step/Literal/Column/Gather/Segment
// slots, never read across domains except through a gather, so "nothing else in the group" is the
// whole check) reads `x`. Fusing gives `y = fma(a, b, c)` in ONE step instead of two, with ONE
// rounding instead of two -- `std::fma` differs from `a*b` (rounded) `+ c` (rounded again) in the
// last bit for some operands (DESIGN.md §10), hence class **E1**, never E0, and this rewrite is
// deliberately left OFF an E0 extraction (CLAUDE.md "no -ffast-math"; this is the one
// FP-contraction the ENGINE chooses to make, explicitly, rather than the one a compiler flag would
// make silently and inconsistently across GCC/clang -- D25/D46's whole point).
//
// UNSOUND AT A FLAT ULPS-OF-VALUE BOUND (D57, a review finding on the M4-gate-1 landing; fixed by
// documenting the real bound rather than the class label alone, per D26's own precedent for
// exactly this shape). "<= 1 ulp" is only true relative to the PRE-FUSION operands, `|a*b| + |c|`
// -- because `fma(a,b,c)` has exactly one rounding of the exact `a*b+c` while the unfused path has
// two (of `a*b`, then of the sum), each bounded in ABSOLUTE terms by half a ulp of ITS OWN
// intermediate magnitude, D26's exact reasoning for "a difference of legs". It is NOT bounded
// relative to the RESULT `y`: under catastrophic cancellation (the Sum's two members close in
// magnitude and opposite in sign -- common in finance, e.g. DF/notional*rate differences), `y` can
// land arbitrarily close to zero while the two roundings it absorbs do not shrink with it, so the
// ulp distance between the fused and unfused values, measured against `y`, is unbounded -- proved
// directly in tests/rewrite/fma_contraction_verify_test.cpp's own
// `PlainE1ToleranceIsUnsoundUnderCatastrophicCancellation` test (a=2^27+1, b=2^27-1, c=-2^54:
// unfused = 0.0 exactly, fused = fma(a,b,c) = -1.0 exactly, the true, mathematically correct
// answer -- an unscaled 4-ulp check on that pair fails outright, while the SAME pair checked with
// scale = |a*b| + |c| is 0.125 ulps, comfortably inside 1). Any caller verifying (or extracting
// at) this rule's E1 class MUST supply that scale -- `epykos::verify::Tolerance::e1_relative` /
// `within` with `scale = |a*b| + |c|` computed from the SAME state the check runs at, never a bare
// `Tolerance::e1()` with no scale -- exactly as `fixtures::m1_differential.hpp` already does for
// D26's leg-scale case. A bare, unscaled E1 check of this rule (as every OTHER landed E1 rewrite
// is checked, because none of them can divide by a value that shrinks towards zero) is a false
// sense of safety, not a stricter one.
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
