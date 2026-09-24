// EpykosEngine — R4a: push pure unary ops through gathers toward the smaller domain, then CSE
// (M4/R-b; DESIGN.md §6, class E0).
//
// Target (unfilled — see stub_rule.hpp): `exp(gather(df_times))` read by several consumers moves
// the `exp` to the smaller `df_times` domain and CSEs it there, so it runs once per distinct time
// rather than once per reader row. SwapEngine analogue: `exp` once per time.
#pragma once

#include "epykos/rewrite/rule.hpp"
#include "epykos/rewrite/stub_rule.hpp"

namespace epykos::rewrite {

struct R4aTag {
  static const char* rule_name() { return "r4a.push_unary_through_gathers"; }
  static constexpr Exactness exactness() { return Exactness::E0; }
};

using R4aPushUnaryThroughGathers = IdentityStubRule<R4aTag>;

}  // namespace epykos::rewrite
