// EpykosEngine — R3: elide trivial maps (M4/R-a; DESIGN.md §6, class E0).
//
// Target (unfilled — see stub_rule.hpp): a length-1 segment (a Sum / Affine over exactly one
// member) or an identity gather (index[r] == value_of(producer, r) for every row) merges its
// domain with the one it reads: the indirection disappears, not just its cost. SwapEngine
// analogue: `sub_is_identity`.
#pragma once

#include "epykos/rewrite/rule.hpp"
#include "epykos/rewrite/stub_rule.hpp"

namespace epykos::rewrite {

struct R3Tag {
  static const char* rule_name() { return "r3.elide_trivial_maps"; }
  static constexpr Exactness exactness() { return Exactness::E0; }
};

using R3ElideTrivialMaps = IdentityStubRule<R3Tag>;

}  // namespace epykos::rewrite
