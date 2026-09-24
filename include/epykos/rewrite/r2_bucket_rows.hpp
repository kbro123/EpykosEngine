// EpykosEngine — R2: bucket rows by uniform-column signature (M4/R-a; DESIGN.md §6, class E0).
//
// Target (unfilled — see stub_rule.hpp): once R1 folds what it can, the rows of one domain that
// still differ only by which columns are uniform among them split into sub-domains — the
// per-kind batches a hand-written kernel would form (a coupon with a realised fixing has the
// fixed coupon's op tree with a constant in the rate slot, DESIGN.md §5's "Expected outcome").
// SwapEngine analogue: per-kind batches.
#pragma once

#include "epykos/rewrite/rule.hpp"
#include "epykos/rewrite/stub_rule.hpp"

namespace epykos::rewrite {

struct R2Tag {
  static const char* rule_name() { return "r2.bucket_rows"; }
  static constexpr Exactness exactness() { return Exactness::E0; }
};

using R2BucketRows = IdentityStubRule<R2Tag>;

}  // namespace epykos::rewrite
