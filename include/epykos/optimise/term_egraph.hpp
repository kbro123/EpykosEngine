// EpykosEngine — the TERM-level e-graph (P1/term-rewriting DESIGN ONLY; D73;
// docs/TERM_REWRITING.md §2 and §6). D62's own words: "A real term-level e-graph is the proper
// fix and is explicitly NOT attempted here." This is that fix, as an interface.
//
// STATUS: an INTERFACE PROPOSAL. Nothing implements it. `src/optimise/egraph.cpp` is untouched
// and the existing `optimise::EGraph` keeps running the layout rules (§7).
//
// ---------------------------------------------------------------------------------------------
// The size argument, which is the whole reason this is tractable
// ---------------------------------------------------------------------------------------------
//
// The instinct is that a term e-graph over the Stage A tape means 517,036 e-nodes. It does not,
// and the reason is the domain IR itself. `ir::infer` turns those 517,036 tape nodes into 67
// domains (80 after R2/R1 fire, D65) of a few Steps each — `ir::shape_string` switches to the
// "<op>[<steps>]" form only past 24 steps, and the Stage A groups are far shorter than that. A
// term e-graph over GROUPS is therefore on the order of a THOUSAND e-nodes, not half a million:
// the 517,036 : 67 collapse is the sharing, and it has already happened, before the optimiser is
// reached.
//
// That single fact is what separates this from the whole-Program e-graph it replaces. D62's
// cause (B) — "k independent local edits cost k whole clones and, over the rounds, 2^k of them" —
// is not mitigated here, it is DISSOLVED: k independent edits are k choice points in one graph,
// which is O(k) nodes. The 2^k plan-tier lattice that forced `RefirePolicy::PipelineOrderedPlans`
// has no analogue at the term tier.
//
// ---------------------------------------------------------------------------------------------
// What survives from D62, and what becomes unnecessary
// ---------------------------------------------------------------------------------------------
//
//   D62 (1) APPLICATION MEMO                 SURVIVES, re-keyed. The key becomes
//                                            (e-NODE, rule, binding), not (program node, rule,
//                                            site). E-nodes are immutable under hash-consing;
//                                            CLASSES merge, so a class-keyed memo would be
//                                            unsound the moment two classes union. Bindings are
//                                            canonicalised to class representatives at lookup, so
//                                            a merge makes the memo do redundant work at worst,
//                                            never wrong work. Still sound for exactly D62's
//                                            reason: TermRule::search / build are pure
//                                            (term_rule.hpp).
//
//   D62 (2) REPRESENTATIVE-ONLY MATCHING     UNNECESSARY, and becomes true by construction.
//                                            Hash-consing means two nodes with equal content ARE
//                                            one node. There is no congruent duplicate to skip.
//                                            D49's flagged "Known limitation" — plan tiers keyed
//                                            per node rather than per class — disappears with it
//                                            at this tier.
//
//   D62 (3) RefirePolicy::NoFreshCrossRule   UNNECESSARY, and SHOULD BE DELETED at this tier.
//                                            It existed solely to bound cause (B). D62 booked its
//                                            cost honestly: "any genuine optimum that needs two
//                                            different structural rewrites composed after both
//                                            have already fired once is no longer reachable at
//                                            all." That completeness is RECOVERED here. Mixed
//                                            chains of arbitrary length are what equality
//                                            saturation is for, and they are the normal case for
//                                            algebra (factor, then cancel, then contract).
//
//   D62 (4) RefirePolicy::PipelineOrderedPlans  SURVIVES UNCHANGED, in the PLAN tier, which this
//                                            design does not touch. Stated plainly because it is
//                                            the honest limit of this package: the plan tier is
//                                            still a whole-`PlanAnnotations`-valued e-graph with
//                                            a 2^k lattice, and term-level rewriting does nothing
//                                            for it. §7.3.
//
// What REPLACES the refire policy as the bound is a different and better-targeted set of
// mechanisms — no AC matching on Sum/Affine, bounded pattern depth, egg-style per-rule banning
// with backoff, and per-group scoping with a separately bounded cross-domain phase. §6 of the
// design document states each one and what completeness it costs, in D62's style.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "epykos/ir/program.hpp"
#include "epykos/rewrite/error_model.hpp"
#include "epykos/rewrite/term.hpp"
#include "epykos/rewrite/term_rule.hpp"

namespace epykos::optimise {

// An e-node's id. Immutable for the node's whole life — that immutability is what the memo is
// keyed on.
using TermNodeId = std::int64_t;
inline constexpr TermNodeId invalid_term_node = -1;

// --------------------------------------------------------------------------------------------
// Bounds
// --------------------------------------------------------------------------------------------

struct TermSaturationLimits {
  int max_rounds = 12;
  std::size_t max_nodes = 200000;   // whole graph, all domains
  std::size_t max_nodes_per_group = 4000;

  // ---- the bound that matters most -----------------------------------------------------------
  // ASSOCIATIVE-COMMUTATIVE MATCHING ON Op::Sum AND Op::Affine IS OFF, AND MUST STAY OFF.
  // Three independent reasons, any one of which is sufficient (design doc §3.4 and §6.2):
  //  (i)  Sum is a FIXED-ARITY LEFT FOLD in operand order, `((x_0 + x_1) + x_2) ...`
  //       (ir/program.hpp's evaluation contract). Reordering its members changes the rounding by
  //       an amount that is NOT bounded per rewrite — a sum of mixed-magnitude terms can lose
  //       everything to cancellation under one order and nothing under another — so it is an
  //       unbounded family whose error the propagator would have to price one permutation at a
  //       time. PRINCIPLES.md §4's retirement of bit-identity does NOT relax this: the objection
  //       was never "it changes bits", it is that the error is unbounded and the family infinite.
  //  (ii) D61 deliberately canonicalised a COMMUTATIVE STEP's two operands and deliberately did
  //       NOT canonicalise a Sum's member order. Introducing AC matching would silently reverse
  //       that decision and reopen `task_919ea449`'s class of compiler-dependent defect.
  //  (iii) The Stage A leg sums have segments of hundreds of members. An n-member AC term has
  //       n! orderings and Catalan(n-1) associations. There is no bound to set.
  // The field exists so that the decision is visible and a future package must change a default
  // rather than discover an absence.
  bool ac_matching_on_sum = false;

  // Patterns deeper than this are refused at registration, so search stays O(nodes x rules).
  int max_pattern_depth = 4;

  // egg-style rule banning with exponential backoff (Willsey et al. 2021; D12: no external
  // library, this is ours). A rule that adds more than `ban_threshold` nodes in one round is
  // banned for `ban_rounds`, doubling each time it re-offends. This is the mechanism D65's R2
  // needed and did not have: at 10,111 sites it would have been banned in round 1 with a logged
  // reason instead of running for 700 s and blowing the bound.
  std::size_t ban_threshold = 2000;
  int ban_rounds = 2;

  // Cross-domain rules (TermRule::crosses_domains) run in their own phase, after the within-group
  // graph has settled, for at most this many rounds. §6.4: pushing a unary through a gather
  // copies a sub-DAG into another group, which is the one term-level mechanism that can grow the
  // graph superlinearly.
  int max_cross_domain_rounds = 2;
  // Refuse to push through a gather whose producing domain has more than one reader. Duplicating
  // a shared producer into every reader is the inlining lattice again, in the term tier. Same
  // condition `r5.group_formation` already uses.
  bool cross_domain_single_reader_only = true;
};

struct TermSaturationLogEntry {
  int round = 0;
  std::string rule;
  std::size_t matches = 0;
  std::size_t nodes_added = 0;
  std::size_t memoised = 0;     // (node, rule, binding) applications already applied
  std::size_t declined = 0;     // build() returned false: a side condition failed
  bool banned = false;
  std::string note;             // the bound's name when cut, "" otherwise
};

struct TermSaturationReport {
  int rounds_run = 0;
  std::size_t total_nodes = 0;
  std::size_t total_classes = 0;
  bool reached_fixpoint = false;
  bool bound_hit = false;
  std::string bound_reason;
  std::vector<TermSaturationLogEntry> log;
};

// --------------------------------------------------------------------------------------------
// The graph
// --------------------------------------------------------------------------------------------

class TermEGraph : public rewrite::TermView {
 public:
  // Interns every group of `program` as terms. One class per distinct (op, child classes, leaf,
  // anchor) tuple. `anchor` is part of the key: two structurally identical terms in different
  // domains denote different row vectors (term.hpp).
  explicit TermEGraph(const ir::Program& program);
  ~TermEGraph() override;

  TermEGraph(const TermEGraph&) = delete;
  TermEGraph& operator=(const TermEGraph&) = delete;

  // ---- rewrite::TermView ---------------------------------------------------------------------
  const ir::Program& program() const noexcept override;
  std::int64_t num_classes() const noexcept override;
  std::vector<rewrite::TermNodeView> nodes_of(rewrite::TermClassId c) const override;
  ir::domain_id anchor_of(rewrite::TermClassId c) const noexcept override;
  rewrite::TermClassId class_of(rewrite::TermRef ref) const noexcept override;
  bool uniform_value(rewrite::TermClassId c, double& out) const override;
  bool provably_positive(rewrite::TermClassId c) const override;
  bool provably_nonzero(rewrite::TermClassId c) const override;
  bool provably_finite(rewrite::TermClassId c) const override;
  bool provably_not_negative_zero(rewrite::TermClassId c) const override;

  // ---- saturation ----------------------------------------------------------------------------
  // Deferred canonicalisation exactly as D49 described and for the same reason: a whole round's
  // matches are queued, then one `rebuild()` hash-conses and unions them all. Repeatable; a later
  // call resumes.
  TermSaturationReport saturate(const std::vector<const rewrite::TermRule*>& rules,
                                const TermSaturationLimits& limits = {});

  // Interns one rule's replacement and unions it with the site's class. The ONLY mutator a rule's
  // output reaches, and it is the driver that calls it, never the rule (term_rule.hpp).
  // Returns false when the replacement is rejected — a new gather reading a later domain, a new
  // column of the wrong length, a node budget hit — which is a logged event, never silent.
  bool apply(const rewrite::TermRewrite& rewrite);

  // ---- invariants (a gate test calls these directly) -------------------------------------------
  // Every pair of nodes in a class has equal content after canonicalisation, no two nodes with
  // equal content sit in different classes, and every class has exactly one anchor domain. The
  // last clause is this tier's own invariant and the one most likely to be broken by a careless
  // cross-domain rule.
  bool congruent();
  bool anchors_consistent() const;

  // Every rewrite ever applied, in order, with the class it fired on and its ErrorTerm. Extraction
  // reads it to build a candidate's CandidateError (error_model.hpp).
  struct AppliedRewrite {
    rewrite::TermClassId site = rewrite::invalid_term_class;
    ir::domain_id anchor = -1;
    std::int32_t step = -1;
    rewrite::ExactnessHint exactness = rewrite::ExactnessHint::Unknown;
    rewrite::ErrorTerm error{};
    std::string rule;
    std::string justification;
  };
  const std::vector<AppliedRewrite>& applied() const noexcept;

 private:
  struct Impl;
  Impl* impl_ = nullptr;
};

}  // namespace epykos::optimise
