// EpykosEngine — R7: block linmap by the column span its rows read (M4/R-c; DESIGN.md §6, E0).
//
// Target (unfilled — see stub_rule.hpp): a `linmap` (DESIGN.md §3.1: `y = W*x`, W block-sparse,
// structure-only) whose rows each read only a narrow span of `x`'s columns splits into
// independent per-curve blocks — the analytic calibration Jacobian's own block structure
// (DESIGN.md §7: "the `linmapᵀ` at the Times boundary gives the analytic calibration Jacobian").
// SwapEngine analogue: per-curve block GEMV.
#pragma once

#include "epykos/rewrite/rule.hpp"
#include "epykos/rewrite/stub_rule.hpp"

namespace epykos::rewrite {

struct R7Tag {
  static const char* rule_name() { return "r7.block_linmap"; }
  static constexpr Exactness exactness() { return Exactness::E0; }
};

using R7BlockLinmap = IdentityStubRule<R7Tag>;

}  // namespace epykos::rewrite
