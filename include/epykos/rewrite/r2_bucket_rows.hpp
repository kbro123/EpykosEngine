// EpykosEngine — R2: bucket rows by uniform-column signature (M4/R-a; DESIGN.md §6, class E0).
//
// Once R1 folds what it can, a domain's remaining (non-uniform) columns may still repeat: e.g. a
// "coupon" domain the signature pass formed from one op-tree shape holds both genuinely-fixed
// coupons and floating coupons with a realised fixing (DESIGN.md §5's own "Expected outcome" --
// both record the SAME tree, a constant in the rate slot, so they share a class), and some other
// column (a day-count basis flag, a compounding indicator, ...) may take only a few distinct
// values across many rows. R2 partitions such a domain's rows by the JOINT signature of every
// column that is not ALREADY uniform (an already-uniform column is R1's job and adds nothing to a
// bucketing key); each bucket becomes its own domain, in which those columns are now uniform for a
// LATER R1 pass to fold. SwapEngine analogue: per-kind batches.
//
// Scope of this landing (an honest restriction, not a magic number): a domain's rows keep their
// ORIGINAL relative order and value ids -- R2 buckets by MAXIMAL CONTIGUOUS RUNS of matching
// signature, never by an arbitrary sort/permutation of the rows. A permutation would require
// renumbering every value id downstream of the domain (any gather or segment anywhere in the
// program that reads one of its rows), program-wide; a contiguous-run split needs none of that
// (DESIGN.md §5's own value-id space stays contiguous either way -- rows just change which Domain
// entry owns them), which is why this package lands the safer half first. Consequently R2 fires
// only when the ORIGINAL RECORDING ORDER already groups some run of same-signature rows together;
// a domain whose kind-defining columns are shuffled relative to its row order will not benefit
// from this landing -- match() simply reports no site for it, per rule.hpp's "a rule that never
// fires is a finding to report, not to hide" (this package's own notes say what was measured on
// the M1 book and the Stage A tape).
//
// Non-recurrent, non-scan-class domains only (SiteKind::Domain): splitting a chain's rows across
// several domains would break the scan machinery's row-order contract (ir/program.hpp), so a scan
// or a level-split scan-candidate domain never matches.
//
// Two structural quality floors, neither a constant (r2_bucket_rows.cpp's own comment on
// run_buckets has the full account and the measurements): the split must CONSOLIDATE SOMETHING
// (fewer buckets than rows), and it must be a PER-KIND PARTITION (every distinct signature in
// exactly one contiguous run -- literally the "per-kind batches" of DESIGN.md §6). The gate read
// "reject any singleton run at all" from D52 until D65, on the strength of a singleton-heavy Stage
// A split that aborted with heap corruption during its adjoint; D65 found that fault in R-a's own
// test harness, not in src/adjoint/, so singleton buckets are allowed now and the floors say what
// they actually mean. Measured fire-counts with the floors as shipped: 0 of the M1 book's 10
// domains, 3 of the Stage A tape's 67. Measured with the per-kind floor OFF (the pure soundness
// gate): 5 of 10 and 23 of 67, all bit-exact, but fragmenting rather than batching -- and enough
// proposals to blow the e-graph's own node bound. D65 has the numbers.
#pragma once

#include <string>
#include <vector>

#include "epykos/rewrite/rule.hpp"

namespace epykos::rewrite {

class R2BucketRows final : public Rule {
 public:
  const std::string& name() const noexcept override;
  Exactness exactness_class() const noexcept override { return Exactness::E0; }
  std::vector<MatchSite> match(const ir::Program& program, const ir::PlanAnnotations& plan) const override;
  Proposal propose(const ir::Program& program, const ir::PlanAnnotations& plan, const MatchSite& site) const override;
};

}  // namespace epykos::rewrite
