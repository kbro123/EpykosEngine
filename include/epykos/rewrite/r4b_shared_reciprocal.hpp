// EpykosEngine — R4b: a/b -> a * recip(b), shared reciprocal (M4/R-b; DESIGN.md §6, class E1).
//
// Target (unfilled — see stub_rule.hpp): the same R4a relocation applied to `recip(b)` so several
// divisions by the same `b` (e.g. the same discount factor) share one reciprocal — a real IEEE
// rounding-order change, hence E1, not E0 (DESIGN.md §12: the residual gap to the hand kernel
// after libm parity is exactly this, two IEEE divisions per forward element vs one shared
// reciprocal). SwapEngine analogue: shared `INV`.
#pragma once

#include "epykos/rewrite/rule.hpp"
#include "epykos/rewrite/stub_rule.hpp"

namespace epykos::rewrite {

struct R4bTag {
  static const char* rule_name() { return "r4b.shared_reciprocal"; }
  static constexpr Exactness exactness() { return Exactness::E1; }
};

using R4bSharedReciprocal = IdentityStubRule<R4bTag>;

}  // namespace epykos::rewrite
