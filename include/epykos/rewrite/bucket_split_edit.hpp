// EpykosEngine — shared low-level IR-editing helpers for R1-R3 (M4/R-a; DESIGN.md §6).
//
// R1 (fold uniform columns), R2 (bucket rows) and R3 (elide trivial maps) each need to do the
// same kind of bookkeeping once they have decided WHAT to change: renumber domain ids after a
// domain is split or removed, and, for R3, splice a domain out of the value space and redirect
// every reference to one of its rows. None of this is part of any rule's own PATTERN (that stays
// in each rule's own file); it is pure plumbing, kept here so each rule's file only has to say
// what it changes, not how the arrays stay consistent.
//
// `recompute_reads` (recomputing every domain's `Domain::reads`, ir/program.hpp, from the current
// gather/segment tables) is NOT declared here: M4/R-b's own `rewrite/ir_edit.hpp` landed the same
// function, to the same contract, for R4a/R4b/R5's own domain-insert/merge edits, so this file's
// own `.cpp` includes that header and reuses it rather than shipping a second implementation of
// the same recomputation in the same `epykos::rewrite::detail` namespace.
//
// Every function takes and returns / mutates a `ir::Program` value directly (never a `Rule`); a
// rule's `propose()` copies its input program once and calls into these to build the rewritten
// form. None of them touch `Program::plan` (DESIGN.md's own annotations are a separate concern,
// R0's package, untouched by structural rewrites).
#pragma once

#include <cstdint>
#include <vector>

#include "epykos/ir/program.hpp"

namespace epykos::rewrite::detail {

// True when every element of `values` has the same IEEE bit pattern as the first (E0: this is
// STRICTER than `==` on purpose -- +0.0 and -0.0 compare equal under `==` but are not the same
// value to fold into one literal, since `1.0 * -0.0` and `1.0 * 0.0` are not the same bits).
bool all_bit_identical(const std::vector<double>& values);

// Drops every Column / Gather / Segment owned by domain `d` (`.domain == d`) from `p`, and remaps
// every SURVIVING Step's Column / Gather / Segment slot (in every OTHER domain's group) to its new,
// shifted array index. Never touches `p.domains` / `p.groups` / value ids / domain ids -- the
// caller replaces or removes domain `d`'s own Group and Domain entries separately. Only a step
// belonging to domain `d` itself may reference an entry `.domain == d` owns (ir::validate); those
// steps are left unmodified (about to be discarded by the caller) since their slot indices no
// longer resolve to anything once this returns.
void drop_owned_entries(ir::Program& p, ir::domain_id d);

// Adds `delta` to every domain id STRICTLY GREATER than `threshold` referenced anywhere in `p`:
// Column::domain, Gather::domain, Segment::domain, Scan::domain, Group::domain, and every entry of
// every Domain::reads. Does not touch Domain::value_base (a value-id quantity, never a domain id)
// or `p.domains`' own length/order -- the caller inserts or erases domain entries separately, at
// the same threshold, so the ids this function rewrites land on the right entries once it does.
void shift_domain_ids(ir::Program& p, ir::domain_id threshold, std::int32_t delta);

// Removes domain `d` from `p` entirely, redirecting every reference anywhere in the program to one
// of its rows (value id `p.domains[d].value_base + r`) to `replacement[r]` -- a value id the
// caller guarantees is STRICTLY EARLIER than `d`'s own value_base (an eliminated domain must be a
// pure relabelling of already-computed values; validate() would reject a forward read otherwise).
// Every Column / Gather / Segment `d` owns is dropped (drop_owned_entries); every later domain id,
// gather/segment/column array index and value id closes the resulting gap; `recompute_reads` runs
// before returning. `replacement.size()` must equal `p.domains[d].rows`.
ir::Program eliminate_domain(const ir::Program& p, ir::domain_id d, const std::vector<ir::value_id>& replacement);

}  // namespace epykos::rewrite::detail
