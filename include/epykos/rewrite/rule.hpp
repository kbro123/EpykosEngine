// EpykosEngine — the rewrite rule interface (M4/R0; PROBLEM.md §7, DESIGN.md §6, D8, D47).
//
// A Rule is a pattern over the domain IR (a Program, plus the PlanAnnotations decided so far —
// ir/annotate.hpp) and a constructor of the rewritten form: either the Program itself
// (a structural rewrite: DESIGN.md §6's R1-R7 fold uniform columns, bucket rows, elide trivial
// maps, push ops through gathers, form groups, choose materialisation boundaries, block linmap)
// or an annotation of it (a planner decision: which domain is materialised / fused / inlined,
// which steps pair, which outputs are emitted, which Jacobian block uses which AD mode). Both
// kinds share one interface so a driver — a single GREEDY pass (rewrite/greedy.hpp: apply
// everywhere a rule matches, in program order) or an e-graph (M4/EG, D12: no external e-graph
// library, EG writes its own) — can run any rule without knowing which kind it is.
//
// ---------------------------------------------------------------------------------------------
// The contract, for rule authors
// ---------------------------------------------------------------------------------------------
//
// 1. `match` enumerates every site of `program` (as it stands, together with `plan`, the
//    annotations decided by whatever ran before this rule in the current pipeline) where
//    `propose` would produce a change. It is read-only and total: given the same `program` and
//    `plan`, it returns the same sites every time. A rule with no notion of "sites smaller than
//    the whole program" (most planner rules) returns at most one SiteKind::Program match.
//
// 2. `propose` builds the rewritten form at EXACTLY ONE site — one of `match`'s own results;
//    passing any other MatchSite is a caller error, not a condition `propose` is asked to detect.
//    It takes `program` and `plan` by const reference and returns its answer in a fresh
//    Proposal, touching neither input. This is what makes the same rule usable two ways without
//    change:
//      - a GREEDY pass (rewrite/greedy.hpp: apply_greedy) folds the Proposal onto its own live
//        copy of the program/plan and moves on — "discard the original, keep the rewrite";
//      - an e-graph driver keeps the ORIGINAL and the Proposal side by side as siblings of one
//        e-class and lets extraction choose later — "produce the alternative without discarding
//        the original" (this package's brief). Nothing in this header implements that driver
//        (M4/EG's job); the contract is written so EG only ever needs `match` and `propose`.
//
// 3. Proposals compose by MERGING, not replacing, when they are annotations-only (the common
//    case for planner rules): rewrite::merge_annotations (rewrite/greedy.hpp) folds a rule's
//    Proposal onto the running PlanAnnotations entry by entry, so a rule only ever needs to name
//    the domains / steps / values ITS OWN match sites concern, never the whole program's.
//
// 4. `name()` is "<pass>.<rule>", e.g. "planner.reduction_fusion", "r4a.push_unary_through_gathers".
//    A rule with its own mutants (CLAUDE.md: every rewrite ships a mutation test) guards its
//    defect with `epykos::mutant("rewrite." + name() + ".<defect>")` — the same mechanism as
//    every other pass (include/epykos/mutation/mutation.hpp, D32); registering the mutant is the
//    rule's own package's job (R-a/R-b/R-c land R1-R7's; this package's planner rules are
//    verified by rewrite/verifier.hpp and by the existing M1/M3 gates unchanged, so it registers
//    none of its own — see docs/RESUME.md's R0 landing entry for why).
//
// 5. `exactness_class()` is CLAUDE.md's E0 (bit-identical) or E1 (<= 1 ulp per op) — declared by
//    the rule, applied by rewrite::verify_rule (rewrite/verifier.hpp) and by the mutation gate.
//    Every one of R0's own rules (the five planner.* rules below, and the R1-R7 stubs) is E0: a
//    planner decision changes nothing about the arithmetic, and an identity stub changes nothing
//    at all.
//
// 6. A rule may cache read-only analysis of `program` between its own `match` / `propose` calls,
//    but must not assume either is called before the other, and must not cache ACROSS two
//    different Program objects (two Interpreters over the SAME Program with different Options
//    is the ordinary case this package exists to keep working, ir/annotate.hpp point 3) — R0's
//    own rules avoid the question by recomputing their (cheap, linear) analysis fresh in both
//    functions rather than caching at all.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "epykos/ir/annotate.hpp"
#include "epykos/ir/program.hpp"

namespace epykos::rewrite {

enum class Exactness : std::uint8_t { E0 = 0, E1 = 1 };

const char* to_string(Exactness e) noexcept;

// What part of the domain IR one match site refers to. `Program`-kind sites carry no index (a
// planner decision keyed on the whole program, or a rule that only ever has one possible site);
// `Domain` names one domain; `Step` names a run starting at one step of one domain's group (a
// fused-pair / tail candidate); `Gather` / `Segment` name one of those tables directly, for a
// rule pattern-matching on them (R4a pushes a unary op through a gather, R7 blocks a linmap by
// the column span its rows read).
enum class SiteKind : std::uint8_t { Program = 0, Domain = 1, Step = 2, Gather = 3, Segment = 4 };

const char* to_string(SiteKind k) noexcept;

struct MatchSite {
  SiteKind kind = SiteKind::Program;
  std::int32_t domain = -1;  // Domain, Step
  std::int32_t step = -1;    // Step: the first IR step index of the match
  std::int32_t index = -1;   // Gather, Segment: index into that table

  bool operator==(const MatchSite&) const = default;

  static MatchSite whole_program() noexcept { return MatchSite{}; }
  static MatchSite of_domain(std::int32_t d) noexcept { return MatchSite{SiteKind::Domain, d, -1, -1}; }
  static MatchSite of_step(std::int32_t d, std::int32_t s) noexcept { return MatchSite{SiteKind::Step, d, s, -1}; }
  static MatchSite of_gather(std::int32_t g) noexcept { return MatchSite{SiteKind::Gather, -1, -1, g}; }
  static MatchSite of_segment(std::int32_t s) noexcept { return MatchSite{SiteKind::Segment, -1, -1, s}; }
};

// A rule's proposal at one site. Exactly one of the two payloads is populated (never both, never
// neither: an empty Proposal from a site `match` returned is a matcher bug, not a legal "no
// change"). `program`: a structural IR rewrite (R1-R7) — the rewritten Program, standing on its
// own (the caller replaces its program with this one; it is not a diff). `annotations`: a
// planner-style rule — the annotation entries this site's decision fills in, meant to be
// MERGED onto the running plan (merge_annotations), not to replace it.
struct Proposal {
  std::optional<ir::Program> program;
  std::optional<ir::PlanAnnotations> annotations;

  bool is_structural() const noexcept { return program.has_value(); }
  bool is_annotation() const noexcept { return annotations.has_value(); }
  bool empty() const noexcept { return !program.has_value() && !annotations.has_value(); }
};

class Rule {
 public:
  virtual ~Rule() = default;

  virtual const std::string& name() const noexcept = 0;
  virtual Exactness exactness_class() const noexcept = 0;

  // Every site `propose` would currently succeed at. Empty: this rule does not apply right now.
  virtual std::vector<MatchSite> match(const ir::Program& program, const ir::PlanAnnotations& plan) const = 0;

  // Builds the rewritten form at `site` (one `match(program, plan)` returned). Never mutates
  // `program` or `plan`.
  virtual Proposal propose(const ir::Program& program, const ir::PlanAnnotations& plan, const MatchSite& site) const = 0;
};

}  // namespace epykos::rewrite
