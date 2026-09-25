// EpykosEngine — shared structural-surgery primitives for M4/R-b's IR rewrites (R4a, R4b, R5).
//
// ir::Program lays every domain's rows out contiguously in one flat value space (value id =
// domain.value_base + row, program.hpp's own file header): inserting or removing a domain shifts
// every value id, gather/segment table `.domain` field and `Domain::reads` entry at or after that
// point. Three rewrites in this package need exactly this bookkeeping and none should reimplement
// it independently, so it lives here, tested on its own (tests/rewrite/ir_edit_test.cpp) before
// any rule builds on it:
//
//   insert_domain_after   R4a (push a unary op through a gather) and R4b (shared reciprocal) both
//                         add ONE new elementwise domain, reading only domains already present,
//                         at a position right after the domain it reads -- nothing existing is
//                         removed, so no gather/segment table entry is ever dropped or reordered:
//                         only value ids and domain ids at or after the insertion point shift.
//   merge_producer_into_consumer
//                         R5 (group formation) folds a single-reader elementwise producer's whole
//                         step sequence into its one reader's group (Step-slot references replace
//                         the gather that used to cross the domain boundary) and then the producer
//                         domain disappears -- the one case in this package where a domain is
//                         actually removed, so gather/segment table POSITIONS are compacted too
//                         (dropping the producer's own, now-orphaned, tables) and every surviving
//                         step's Slot{Gather,*}/{Segment,*} indices are renumbered along with it.
//
// Both rebuild the WHOLE Program (never mutate the input) and finish with `ir::validate` implied
// by the caller (rule tests call it explicitly); `Domain::reads` is always recomputed from the
// final gather/segment tables rather than patched incrementally, which is the one part of this
// file worth getting right once: a hand-patched `reads` list is exactly the kind of one-line
// mistake `ir::validate`'s "reads a later domain" / "reads an unknown domain" checks exist to
// catch, so recomputation is preferred over invariant-preservation-by-inspection.
#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "epykos/ir/program.hpp"

namespace epykos::rewrite::detail {

// Inserts `new_domain` (rows, recurrent/scan_class left as given; value_base and reads are
// computed here) immediately after `after` (0 <= after < src.domains.size()) and its `new_group`
// (domain field is ignored and set to the new domain's own final id). `new_gathers` /
// `new_segments` are the tables the new group's own steps reference, given with `.domain` set to
// `new_domain_marker()` (this function rewrites that field to the new domain's real id once it is
// known) -- they are appended to the program's own tables; every EXISTING table entry keeps its
// position (insertion never drops or reorders a gather/segment, only shifts the value ids and
// domain ids they hold). Contract for `new_group`'s own steps: `new_gathers[k]` lands at index
// `src.gathers.size() + k` and `new_segments[k]` at `src.segments.size() + k` -- build
// `new_group`'s Slot{Gather,*}/{Segment,*} operands against those indices. Every value id `>= src.domains[after].value_base + src.domains[after].rows`
// (i.e. belonging to a domain positioned after `after`) shifts up by `new_domain.rows`; every
// domain id `> after` shifts up by one. `new_domain`'s own `reads` is set to read `after` (and
// nothing else the caller did not name via `new_gathers` / `new_segments`, which this function
// also folds in when computing it). Returns the new domain's final id alongside the rewritten
// Program.
inline constexpr ir::domain_id new_domain_marker() noexcept { return -2; }

// Recomputes every domain's `Domain::reads` from the CURRENT gather/segment tables (sorted,
// unique, self excluded unless recurrent) -- both structural edits above use it, and a rule that
// edits a single domain's group in place (R4a, R4b: adding one gather without touching domain
// layout at all) calls it too rather than hand-patching the affected domain's `reads` entry.
void recompute_reads(ir::Program& p);

struct InsertResult {
  ir::Program program;
  ir::domain_id new_id = -1;   // where `new_domain` ended up
  ir::value_id new_base = -1;  // its new value_base
};

InsertResult insert_domain_after(const ir::Program& src, ir::domain_id after, ir::Domain new_domain, ir::Group new_group,
                                  std::vector<ir::Gather> new_gathers, std::vector<ir::Segment> new_segments);

// Folds `producer`'s whole step sequence into `consumer`'s group (producer's steps first,
// renumbered to occupy positions [0, producer_steps.size()); consumer's own original steps
// follow, renumbered by that same offset) and then removes `producer` entirely. Preconditions
// the CALLER (a Rule's match()) is responsible for, not re-checked here:
//   - `producer` is read by exactly one gather in the whole program, owned by `consumer`, and
//     that gather is the identity over `producer` (rows equal, index[r] == value_of(producer, r));
//     `consumer_gather` names it.
//   - neither domain is recurrent; `producer` is positioned before `consumer` (validate()'s own
//     "no forward reads" requirement, unaffected by this rewrite).
// Every reference to `consumer_gather` within `consumer`'s OWN steps is replaced by a Step-slot
// naming the producer's own last (renumbered) step -- the row-for-row value the identity gather
// used to fetch. `producer`'s own gather/segment tables (used only by its now-absorbed steps) are
// dropped and every surviving step's Slot{Gather,*}/{Segment,*} is renumbered to the compacted
// tables; every value id and domain id from `producer`'s old position onward shifts down.
// `consumer_gather` is an INDEX into `src.gathers` (the identity gather `consumer`'s own group
// uses to read `producer`), not a domain id.
ir::Program merge_producer_into_consumer(const ir::Program& src, ir::domain_id producer, ir::domain_id consumer,
                                          std::int32_t consumer_gather);

}  // namespace epykos::rewrite::detail
