// EpykosEngine — R6: materialise only at domain boundaries (M4/R-c; DESIGN.md §6, class E0).
//
// Target (unfilled — see stub_rule.hpp): before a `linmap`, at a reduction, or on profitable
// fan-out — the STRUCTURAL counterpart of planner.reduction_fusion / planner.inline_producers
// (rewrite/planner_rules.hpp), which already make this call for the interpreter's own execution
// plan from measured thresholds; R6 would let the choice be baked into the IR itself (and so
// into a catalogue kernel, M4/C1) rather than re-decided by every Interpreter/Adjoint
// construction. DESIGN.md's own example: "materialise before a sparse reduce" turns a measured
// 1.28x trap unrepresentable.
#pragma once

#include "epykos/rewrite/rule.hpp"
#include "epykos/rewrite/stub_rule.hpp"

namespace epykos::rewrite {

struct R6Tag {
  static const char* rule_name() { return "r6.materialise_boundaries"; }
  static constexpr Exactness exactness() { return Exactness::E0; }
};

using R6MaterialiseBoundaries = IdentityStubRule<R6Tag>;

}  // namespace epykos::rewrite
