// EpykosEngine — R7: block linmap by the column span its rows read (M4/R-c; DESIGN.md §6, E0).
//
// A `linmap` (DESIGN.md §3.1: `y = W·x`, W block-sparse, structure-only) is, as the signature
// pass actually records one (D22: "Knots —linmap→ Times"), a domain whose group is exactly one
// `Affine` step over a Segment: row r's value is `c_0 + Σ k_i·x_{m_i}`, the members `m_i` value
// ids of an earlier domain. R7's pattern: partition such a domain's rows, IN THEIR EXISTING
// ORDER, into the maximal runs ("blocks") whose members all fall inside one advancing span of
// that earlier domain's rows — DESIGN.md's own example is the per-curve case (each curve's own
// knot span is read by exactly one contiguous run of Times rows) but the rule is keyed on the
// value-id spans alone (HARD RULE 9: no curve count, no instrument count, nothing but IR
// structure) — then gives each block its own domain, each with its own (sliced) Segment and, if
// the Affine's `c_0` varies by row, its own (sliced) Column. Splitting changes nothing else:
// value ids keep their global numbering exactly (row order is not touched — DESIGN.md §6's "per-
// row order unchanged" — so every block's `value_base` is simply the running total of the
// earlier blocks' row counts, block 0 keeping the original domain's own `value_base`), so no
// gather or segment ANYWHERE ELSE in the program needs to change: only the domain HEADERS (and
// the bookkeeping fields — Group/Column/Gather/Segment/Scan `.domain`, Domain.reads — that name a
// domain by its position) move. E0: this is bookkeeping, not arithmetic (ir/evaluate.hpp's
// contract is per-row and does not read which domain a row's header claims it as).
//
// Why a domain, not just an annotation: unlike R6 (a planner-style choice PlanAnnotations already
// has a slot for), "this Affine domain is really K independent blocks" has no annotation slot —
// and DESIGN.md's own R1-R7 table lists it as a structural rewrite alongside R1-R5, not a
// planner decision (see rewrite/planner_rules.hpp's file header for that distinction). Exposing
// the blocks as real domains is also what lets every OTHER domain-keyed decision (R6's own
// per-domain test, a future dense per-block GEMV catalogue kernel, M4/C1) see the block structure
// without re-deriving it.
//
// Scoped OUT of this file, deliberately (a finding, not an oversight): DESIGN.md §6's own words
// for R7 also name "the calibration Jacobian blocks the IFT uses (F_z is an Affine-derived block
// on linear schemes: expose it as a closed form the AD-mode rule can pick)" — i.e. populating
// ir::PlanAnnotations::JacobianBlockPlan::mode with AdMode::ClosedFormAffine for a linmap's block.
// ir/annotate.hpp is explicit that this slot has NO CONSUMER YET ("R0 defines this slot only ...
// the cost-model rule that will [read it] is M4/EG's [job]"): nothing in exec::Interpreter,
// adjoint::Adjoint or solver:: reads `jacobian.mode` today, so a rule proposing it cannot be
// checked by ANY of CLAUDE.md's gates (a differential/round-trip/adjoint comparison is blind to
// an annotation nothing consumes) and a mutant of that logic could not be caught by them either —
// exactly the situation CLAUDE.md's "every rewrite ships with its differential test AND its
// mutation test" rule exists to keep code out of. `is_linmap_domain` below (every block this file
// produces is one) is the fact M4/EG needs to make that call once it has a consumer to gate it
// with; wiring AdMode::ClosedFormAffine itself is left to whichever package adds that consumer.
//
// Site granularity: SiteKind::Domain. A structural rewrite returns at most one applied site per
// rewrite::apply_greedy call (rewrite/greedy.hpp) — match() may enumerate every currently
// splittable domain, but splitting one shifts every later domain's id, so a caller wanting every
// block split applies the rule repeatedly (see rewrite/greedy.hpp's own file header) until
// match() returns empty.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "epykos/ir/program.hpp"
#include "epykos/rewrite/rule.hpp"

namespace epykos::rewrite {

struct BlockLinmapParams {
  // A block of fewer than this many rows is not worth its own domain header (table overhead,
  // per-domain dispatch) — a rule parameter, never a magic instance count (HARD RULE 9): the
  // default keeps every block found (1 = "any block is worth splitting out").
  std::int32_t min_block_rows = 1;
};

// One [lo, hi) row range of a linmap domain's rows that reads only one advancing span of its
// source domain (see the file header). Exposed for tests and for propose()'s own reuse.
struct LinmapBlock {
  ir::row_id lo = 0;
  ir::row_id hi = 0;
};

// True when `d`'s group is exactly the pattern R7 targets: one step, Op::Affine over a Segment,
// the domain itself not recurrent (a scan's compounding step is never a linmap).
bool is_linmap_domain(const ir::Program& program, ir::domain_id d) noexcept;

// The column-span blocks of a linmap domain's rows (file header): every row assigned to the
// block whose running [lo,hi) span it fits inside or extends, a new block starting whenever a
// row's members come from a different source domain than the running block's, or from row
// indices strictly beyond the running block's span. Empty when `d` is not a linmap domain.
std::vector<LinmapBlock> linmap_blocks(const ir::Program& program, ir::domain_id d);

class R7BlockLinmap final : public Rule {
 public:
  explicit R7BlockLinmap(BlockLinmapParams params = {});

  const std::string& name() const noexcept override;
  Exactness exactness_class() const noexcept override { return Exactness::E0; }
  std::vector<MatchSite> match(const ir::Program& program, const ir::PlanAnnotations& plan) const override;
  Proposal propose(const ir::Program& program, const ir::PlanAnnotations& plan, const MatchSite& site) const override;

 private:
  BlockLinmapParams params_;
  std::string name_;
};

// The structural rewrite itself, exposed as a pure function (propose() is a thin wrapper): splits
// domain `d` of `program` into one domain per entry of `blocks` (ascending, covering every row of
// `d` exactly once), preserving every value id, and returns the resulting Program. Throws
// std::invalid_argument if `d` is not a linmap domain or `blocks` does not exactly partition its
// rows in order.
ir::Program split_linmap_domain(const ir::Program& program, ir::domain_id d, const std::vector<LinmapBlock>& blocks);

}  // namespace epykos::rewrite
