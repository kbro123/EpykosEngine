// EpykosEngine — R5: group formation (M4/R-b; DESIGN.md §6, class E0).
//
// Target (unfilled — see stub_rule.hpp): the maximal elementwise region of one domain (gathers in,
// a segment-sum epilogue out) becomes one fused group — the IR-level counterpart of what
// planner.fused_pairs / planner.chain_tails (rewrite/planner_rules.hpp) already do at the
// execution-plan level for the interpreter's own kernel calls; R5 is the structural rewrite that
// would let a CATALOGUE kernel (DESIGN.md §7 tier 1, M4/C1) see the same fused region ahead of
// time instead of the interpreter discovering it at plan-build time on every construction.
// SwapEngine analogue: the fused coupon -> leg loop.
#pragma once

#include "epykos/rewrite/rule.hpp"
#include "epykos/rewrite/stub_rule.hpp"

namespace epykos::rewrite {

struct R5Tag {
  static const char* rule_name() { return "r5.group_formation"; }
  static constexpr Exactness exactness() { return Exactness::E0; }
};

using R5GroupFormation = IdentityStubRule<R5Tag>;

}  // namespace epykos::rewrite
