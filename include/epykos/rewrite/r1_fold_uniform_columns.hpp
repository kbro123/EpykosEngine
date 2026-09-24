// EpykosEngine — R1: fold uniform columns (M4/R-a; DESIGN.md §6, class E0).
//
// Target (unfilled — see stub_rule.hpp): a Column whose values are identical across every row of
// its domain (k == 1, konst == 0, w == 1 in DESIGN.md's table) becomes a Literal; the column
// disappears and every row's kernel reads a broadcast scalar instead of a per-row load.
// SwapEngine analogue: `cpn_is_plain`.
#pragma once

#include "epykos/rewrite/rule.hpp"
#include "epykos/rewrite/stub_rule.hpp"

namespace epykos::rewrite {

struct R1Tag {
  static const char* rule_name() { return "r1.fold_uniform_columns"; }
  static constexpr Exactness exactness() { return Exactness::E0; }
};

using R1FoldUniformColumns = IdentityStubRule<R1Tag>;

}  // namespace epykos::rewrite
