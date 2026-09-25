// EpykosEngine — the equality-saturation e-graph over the domain IR (M4/EG "core";
// docs/PROBLEM.md §7, docs/DESIGN.md §7's own words: "produce the alternative without discarding
// the original", docs/DECISIONS.md D47).
//
// rewrite::Rule (rewrite/rule.hpp) already gives every rewrite — R0's five planner decisions
// today, R1-R7 (currently rewrite::stub_rule identity placeholders) once R-a/R-b/R-c land them —
// one shape: `match` names every site a rewrite applies, `propose` builds the alternative at one
// site as EITHER a whole rewritten ir::Program (a structural rewrite) OR an ir::PlanAnnotations
// delta to merge onto the plan decided so far (a planning decision). rewrite::apply_greedy folds
// each Proposal onto a single live (program, plan) pair, discarding the road not taken. This file
// is the other consumer rule.hpp was written for (D47's own words, rule.hpp point 2): an e-graph
// that keeps the ORIGINAL and every Proposal side by side and lets extraction (extract.hpp) pick
// the cheapest one later, at the caller's requested exactness class.
//
// Two tiers, because the two Proposal payloads are equivalence classes of two different things:
//
//   PROGRAM tier   e-classes of ir::Program values reachable by structural Proposals (R1-R7).
//                  Hash-consed on ir::Program::operator== (which already ignores `.plan`, see
//                  ir/annotate.hpp) via `ir::serialize` as the map key — exact identity, not a
//                  hash liable to collide, since serialize's own contract is
//                  `deserialize(serialize(p)) == p`. R1-R7 are still `stub_rule::IdentityStubRule`
//                  (`match` always empty) as this package lands, so this tier holds exactly one
//                  node — the input program — until a real structural rule matches something;
//                  the machinery is real and tested against a synthetic rule (egraph_test.cpp),
//                  not yet exercised by a real one (see this header's "Known limitation" below).
//
//   PLAN tier      one PER PROGRAM NODE (a materialisation / fusion / emission plan is a decision
//                  about how to execute ONE specific recording, ir/annotate.hpp point 1 — a plan
//                  for program A is not a candidate for a structurally different program B).
//                  e-classes of ir::PlanAnnotations values reachable by annotation Proposals,
//                  merged onto the running plan by rewrite::merge_annotations exactly as
//                  rewrite::apply_greedy would, EXCEPT every merge result is added as a new
//                  sibling rather than replacing its parent. `ir::PlanAnnotations::operator==` is
//                  unconditionally `true` by design (annotate.hpp: plans are not part of a
//                  Program's identity), so hash-consing here uses this file's OWN structural
//                  equality/hash over `domain` / `group` / `emitted` / `jacobian.mode` — never
//                  the type's own `operator==`.
//
// Saturation (`EGraph::saturate`) runs BOTH tiers to a fixpoint or a stated bound (iterations,
// e-node count), one round at a time (BFS by rule-application distance from the root, standard
// equality-saturation practice): every node that existed at the START of a round is matched
// against every rule and every site; every resulting Proposal is folded (merge_annotations, for
// an annotation Proposal) and QUEUED, never replacing the node it came from and never itself
// re-matched until next round. A round ends with `rebuild()`, which hash-conses the whole queue
// at once — hash_cons_plan / hash_cons_program compute each pending node's canonical content key
// and UNION it onto an existing node's class when one with equal content is already known —
// the deferred-canonicalisation pattern real e-graph implementations use so a whole round's worth
// of matches can be queued before any union-find work happens (egg, Willsey et al. 2021; D12: no
// external e-graph library, this is EG's own). `EGraph::congruent()` is the invariant a test can
// check directly: every pair of nodes in one class has equal content, and no two nodes with equal
// content sit in different classes.
//
// A rule never needs this header. `saturate` takes `const std::vector<const rewrite::Rule*>&` —
// the exact same objects a greedy pipeline runs (rewrite/planner_rules.hpp's
// DefaultPlanner::rules(), rewrite/stub_rule.hpp's IdentityStubRule<Tag> instances, or a rule this
// package has never heard of) — so PROBLEM.md §7's "design so adding a rule needs no e-graph
// changes" is the file's own contract, not a note in a commit message: nothing here names R0,
// R1-R7 or planner.* by name; the experiment code that DOES (egraph_m1_bench.cpp,
// tests/optimise/egraph_m1_extract_test.cpp) lives outside this header entirely.
//
// Known limitation (report honestly, D9 / HARD RULE 10): a plan tier is keyed by the PROGRAM
// NODE that created it, not by that node's program-tier CLASS — two structurally-rewritten
// program nodes the program tier later discovers are identical (hash-consed / unioned) still keep
// two separate plan tiers rather than sharing one combined search space. With R1-R7 still stub
// rules (`match` always empty) this never happens in practice, so it costs nothing today; it is a
// real gap for whichever package lands R1-R7's first real structural rewrite to close (share one
// plan tier per program CLASS, not per node) — flagged here rather than silently assumed away.
// egraph_test.cpp's synthetic structural rule exercises the program tier's own hash-consing
// directly, without depending on this unfinished reconciliation.
//
// ------------------------------------------------------------------------------------------------
// Scaling (D62, closing D54's own "single most significant open finding of M4", D59)
// ------------------------------------------------------------------------------------------------
// As first landed (D49), `saturate` re-ran `match` AND `propose` for every rule at every site of
// every node accumulated so far, EVERY round, and only then discovered — after a full Program
// clone, an `ir::validate` and an `ir::serialize` — that the content already existed. Measured on
// a 60-trade Stage A recording over four rounds: 1,064 candidate Programs fully built, validated
// and serialised, of which 234 (22.0%) were thrown away at `program_known` as content the graph
// already held — and that is only the waste the old code could SEE, since every accepted node's
// own (node, rule, site) triples were re-matched and re-proposed in every later round too, and
// the plan tier re-merged every delta onto every plan node of its tier every round. Program
// nodes went 7 -> 39 -> 191 -> 831 at 429 / 2,485 / 11,722 / 49,006 ms and 2.3 GB resident, with
// no fixpoint. Two mechanisms address it:
//
//   (A) DEDUP BEFORE CONSTRUCTION, `SaturationLimits::dedup_before_construction` (default on), in
//       two halves. First, an APPLICATION MEMO: `Rule::match` and `Rule::propose` are PURE
//       functions of (program, plan, site) — rule.hpp's own contract points 1 and 2 — and both of
//       a program node's inputs here are FIXED for its whole life: `programs_[pid].program` is
//       never mutated, and the plan context a structural match sees is always that node's own
//       plan-tier root, the empty `ir::PlanAnnotations{}`, which nothing ever writes to. So a
//       given (program node, rule, site) triple has exactly ONE answer, for ever. The memo
//       computes each node's site list once, applies each structural site once, and for an
//       annotation site remembers how far up its node's (growing) plan tier it has already merged.
//       Second, only a program CLASS's REPRESENTATIVE is matched at all: a non-representative node
//       is congruent to the one `extract` already visits, so its sites and proposals are identical
//       by the same purity argument, and matching it only bought a duplicate plan tier (D49 point
//       3's own flagged "Known limitation" above, in its cheapest sound form). Both halves remove
//       re-derivation ENTIRELY and give up no completeness, because every skipped call would have
//       returned content the graph already holds.
//
//   (B) A RE-FIRING POLICY, `SaturationLimits::refire`. The memo does not bound the node COUNT: k
//       independent local edits on one node still clone the whole Program k times and then 2^k
//       times over the rounds, because this is a WHOLE-PROGRAM-valued e-graph (every e-node is a
//       full `ir::Program`; `Rule::propose` has no way to return a localized sub-term edit into
//       shared structure). A real term-level e-graph is the proper fix and is explicitly NOT
//       attempted here. `RefirePolicy::NoFreshCrossRule` is the narrow remedy: see its own comment
//       below for exactly what it gives up.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "epykos/ir/annotate.hpp"
#include "epykos/ir/program.hpp"
#include "epykos/rewrite/rule.hpp"

namespace epykos::optimise {

using ClassId = std::int64_t;
inline constexpr ClassId invalid_class = -1;

// Which rules may fire on a program node that a STRUCTURAL rewrite produced (D62).
enum class RefirePolicy : std::uint8_t {
  // D49's original shape: every rule is matched against every node, every round. COMPLETE with
  // respect to the rule set (every composition reachable within `max_iterations` is enumerated)
  // and unbounded in practice — kept so the D54/D62 "before" numbers stay reproducible and so the
  // equivalence gate (tests/optimise/egraph_saturation_memo_verify_test.cpp) has something to
  // compare the memo against.
  AllRules = 0,
  // The default. A rule KNOWN to be structural (one this graph has already seen return a
  // `Proposal::program`) is matched against a program node that a structural rewrite produced
  // ONLY when it is that node's OWN producing rule. The root program is always matched against
  // everything, and ANNOTATION rules are never restricted (a fresh program node must still get a
  // plan, or extraction could only ever price its empty one).
  //
  // "KNOWN to be structural" is deliberate and has a visible consequence: a rule is unrestricted
  // until the first time it actually proposes a Program, so a rule that does not match the ROOT
  // at all still gets one free, unrestricted pass over whatever some other rule first made
  // applicable. That is why D52's own predicted R2-then-R1 mechanism (R2's bucketing gives R1
  // something to fold; R1 matches nothing on the unrewritten tape) is still found on Stage A
  // under this policy — measured, not assumed. What is cut is every LATER cross-rule step.
  //
  // WHAT THIS GIVES UP, stated plainly rather than glossed (CLAUDE.md, HARD RULE 10): this is a
  // COMPLETENESS tradeoff in the SEARCH. Past that first pass, structural derivations become
  // homogeneous chains — R5 -> R5 -> R5 is still reachable (so is the one structurally-novel
  // candidate this framework has ever found, `r5.group_formation` twice then
  // `planner.reduction_fusion`, D54 experiment 2), but a long MIXED structural chain is not, and
  // any genuine optimum that needs two different structural rewrites composed after both have
  // already fired once is no longer reachable at all. That is a real loss of reachable
  // candidates, not merely a slower path to them. What it does NOT give up is CORRECTNESS: every
  // candidate that IS enumerated is built by the same `Rule::propose` calls, carries the same
  // accumulated `Exactness`, and is extracted and verified at its declared class exactly as
  // before — a narrower search, never a wronger answer. Callers that need the complete search and
  // can afford it (small programs; egraph_test.cpp's synthetic rules) pass `AllRules`.
  NoFreshCrossRule = 1,
  // `NoFreshCrossRule`, PLUS the same idea in the PLAN tier: a rule at slot i of the caller's own
  // `rules` vector may merge its delta onto a plan node only when every rule already in that plan
  // node's derivation sits at a STRICTLY LOWER slot. Plan derivations therefore run in the
  // caller's rule order, each rule contributing at most once.
  //
  // WHY IT EXISTS, measured rather than assumed: with `NoFreshCrossRule` alone the program tier
  // reaches a real fixpoint on a 60-trade Stage A recording (39 nodes, 22 classes) and the PLAN
  // tier becomes the binding constraint instead — it is a pure 2^k subset lattice over the k ≈ 12
  // INDEPENDENT per-domain deltas available on one program (r6.materialise_boundaries' own
  // per-domain sites plus the five planner rules), so saturating it to a fixpoint costs 105,977
  // plan nodes and 10.5 GiB on 60 trades. That is cause (B) again, in the other tier: k
  // independent choices represented as 2^k whole-`PlanAnnotations` clones instead of one shared
  // choice point. Under this policy the same tier is Π_rule (1 + sites) nodes instead — linear in
  // each rule's site count, not exponential.
  //
  // WHAT IT GIVES UP, again plainly: every plan that needs one rule applied TWICE (two of
  // r6.materialise_boundaries' own boundary decisions together, say) or two rules applied OUT of
  // the caller's declared order. The M1 planner's own five-rule pipeline is in that declared
  // order by construction (planner_rules.hpp's `default_planner_rules()` fixed dependency order,
  // D47), so the default plan itself stays reachable and extraction still cannot do worse than
  // it; combinations off that spine no longer are. Correctness of what IS extracted is untouched,
  // exactly as for `NoFreshCrossRule`.
  PipelineOrderedPlans = 2,
};

struct SaturationLimits {
  int max_iterations = 16;          // rounds of "match every node, queue every proposal, rebuild"
  std::size_t max_plan_nodes = 20000;    // total PLAN nodes ever inserted, across every program
  std::size_t max_program_nodes = 2000;  // total PROGRAM nodes ever inserted
  RefirePolicy refire = RefirePolicy::NoFreshCrossRule;  // (B) above
  // (A) above: BOTH the (program node, rule, site) application memo and the "match only a program
  // CLASS's representative" skip, which are the same idea applied to a rule's own repeated work
  // and to congruent duplicate nodes. Off reproduces D49's pay-first-check-later behaviour
  // exactly, which is how D62's own "before" column was measured in this same binary; nothing
  // else should turn it off. Both halves are lossless, so this flag can only change how much work
  // saturation does, never which content it reaches — `egraph_saturation_memo_verify_test.cpp`
  // gates exactly that.
  bool dedup_before_construction = true;
};

// One rule application recorded during saturate(), win or lose: a full account of what ran, so a
// bound being hit is a logged event, never a silent truncation (PROBLEM.md §7's own words).
struct SaturationLogEntry {
  int iteration = 0;
  std::string rule;
  // Sites this rule's `match` returned THIS round. With the application memo on (the default) a
  // node's site list is computed exactly once, so this counts sites of nodes newly matched this
  // round, not the same sites over and over — that is the point of the memo, and a rule whose
  // whole frontier is already memoised legitimately reports 0 here after the round it fired in.
  std::size_t sites_matched = 0;
  std::size_t nodes_added = 0;       // new (pre-rebuild) nodes queued from this rule this round
  std::size_t proposals_memoised = 0;  // (node, rule, site) applications skipped: already applied
  std::size_t nodes_blocked = 0;       // (node, rule) pairs skipped by SaturationLimits::refire
  bool cut = false;                  // true: stopped partway (or skipped entirely) by a bound
  std::string note;                  // "" normally; the bound name, or a rejected-proposal reason
};

struct SaturationReport {
  int iterations_run = 0;
  std::size_t total_program_nodes = 1;  // starts at 1: the input program is always node 0
  std::size_t total_plan_nodes = 1;     // starts at 1: the empty PlanAnnotations{} of program 0
  bool bound_hit = false;
  std::string bound_reason;             // "" unless bound_hit
  std::vector<SaturationLogEntry> log;
};

// One alternative in the PROGRAM tier.
struct ProgramNode {
  ir::Program program;
  int parent = -1;                      // -1: the root (input) program
  std::string rule;                     // "" for the root
  rewrite::Exactness exactness = rewrite::Exactness::E0;
  std::vector<std::string> history;     // rule names from the root to this node, in order
};

// One alternative in a PLAN tier (always scoped to one program node).
struct PlanNode {
  ir::PlanAnnotations content;
  int parent = -1;                      // -1: the root (empty PlanAnnotations{}) of its program
  std::string rule;                     // "" for the root
  rewrite::Exactness exactness = rewrite::Exactness::E0;
  std::vector<std::string> history;
  // The highest slot, in the `rules` vector of the `saturate` call that built this node, of any
  // rule in this node's own derivation; -1 on a tier's root. Read only by
  // RefirePolicy::PipelineOrderedPlans; reset to -1 (i.e. "unrestricted", the conservative
  // direction) if a later `saturate` call arrives with a different rule set, since a slot index
  // means nothing against a vector it was not taken from.
  int max_rule_slot = -1;
};

class EGraph {
 public:
  // Seeds program node 0 = `root` (unannotated: `root.plan` is ignored, exactly as
  // rewrite::verify_annotations treats an incoming program's own `.plan`) and, under it, plan
  // node 0 = ir::PlanAnnotations{} (empty: "derive your own default", ir/annotate.hpp point 3).
  explicit EGraph(ir::Program root);

  // Runs `rules` to a fixpoint (a round that queues no content outside what is already known —
  // `plan_known` / `program_known` below skip re-queueing a rule's proposal when it merely
  // reproduces an existing node, so an always-matching rule like R0's five planner rules does not
  // grow the graph forever) or until `limits` is hit. Safe to call more than once (a later call
  // resumes from the current state and keeps adding); returns this call's own report.
  SaturationReport saturate(const std::vector<const rewrite::Rule*>& rules, const SaturationLimits& limits = {});

  // ---- PROGRAM tier ---------------------------------------------------------------------------

  int num_programs() const noexcept { return static_cast<int>(programs_.size()); }
  const ProgramNode& program_node(int id) const { return programs_.at(static_cast<std::size_t>(id)); }
  // The e-class a program node currently belongs to (path-compressing; two ids with the same
  // class id are congruent — identical ir::Program, plan excluded).
  ClassId program_class(int id) const;
  // One id per distinct PROGRAM e-class, the lowest-numbered member of each (deterministic).
  std::vector<int> program_class_representatives() const;

  // ---- PLAN tier (per program node) -----------------------------------------------------------

  int num_plan_nodes(int program_id) const;
  const PlanNode& plan_node(int program_id, int id) const;
  ClassId plan_class(int program_id, int id) const;
  std::vector<int> plan_class_representatives(int program_id) const;

  // ---- invariants (egraph_test.cpp) -----------------------------------------------------------

  // True iff, in every tier, every pair of nodes sharing a class has EQUAL content — the program
  // tier's own `ir::Program::operator==` (meaningful: it already ignores `.plan`, see
  // ir/annotate.hpp), the plan tier's `plan_content_equal` below (never
  // `ir::PlanAnnotations::operator==`, which is unconditionally `true` by that type's own design)
  // — and no two nodes with equal content sit in different classes. Calls rebuild() first so a
  // caller need not do so manually.
  bool congruent();

  // This file's own structural equality/hash for ir::PlanAnnotations (never
  // PlanAnnotations::operator==, which is unconditionally true by ir/annotate.hpp's own design).
  static bool plan_content_equal(const ir::PlanAnnotations& a, const ir::PlanAnnotations& b);
  static std::size_t plan_content_hash(const ir::PlanAnnotations& a);

 private:
  // Union-find, path compression + union by rank; `rebuild()` is the only place that unions.
  struct UnionFind {
    std::vector<int> parent;
    std::vector<int> rank;
    int make();                 // adds one new singleton, returns its id
    int find(int x);
    bool same(int a, int b) { return find(a) == find(b); }
    void unite(int a, int b);
  };

  struct PendingProgram {
    int parent;
    ir::Program program;
    std::string rule;
    rewrite::Exactness exactness;
  };
  struct PendingPlan {
    int program_id;      // the program NODE (not class) this plan tier belongs to; see the file
                          // header's "Known limitation" — plan tiers are per node, not per class
    int plan_parent;
    ir::PlanAnnotations content;
    std::string rule;
    rewrite::Exactness exactness;
    int rule_slot;       // the proposing rule's slot; becomes the new node's max_rule_slot
  };

  std::vector<ProgramNode> programs_;
  mutable UnionFind program_uf_;
  // ir::serialize(program) -> representative program node id. Exact identity (serialize's own
  // round-trip contract: deserialize(serialize(p)) == p), not a hash liable to collide.
  std::unordered_map<std::string, int> program_key_to_rep_;

  std::vector<std::vector<PlanNode>> plans_;  // plans_[program_id][plan_node_id]
  mutable std::vector<UnionFind> plan_uf_;    // one per program id
  // plan_key_to_rep_[program_id][content_hash] -> a representative node id with that hash (a
  // second node with the same hash but different content, a real collision, is checked with
  // plan_content_equal and kept as its own key via linear probing in the vector of candidates).
  std::vector<std::unordered_map<std::size_t, std::vector<int>>> plan_key_to_rep_;

  std::vector<PendingProgram> pending_programs_;
  std::vector<PendingPlan> pending_plans_;

  // ---- the application memo and the re-firing policy's rule table (D62) -------------------------
  //
  // What a rule has been observed to propose. Learned, never declared: rewrite::Rule has no
  // "which payload do you return" accessor and rule.hpp does not promise one payload per rule, so
  // this is filled in from proposals actually made and stays `Unknown` (hence never restricted by
  // RefirePolicy::NoFreshCrossRule) for a rule that has not proposed anything yet.
  enum class ProposalKind : std::uint8_t { Unknown = 0, Structural = 1, Annotation = 2, Mixed = 3 };

  // One (program node, rule) pair's memo. `Rule::match` / `Rule::propose` are pure functions of
  // (program, plan, site) by rule.hpp's contract, and both inputs are fixed for a program node's
  // whole life (its `program` is never mutated; the plan context a match sees is always that
  // node's plan-tier root, the empty PlanAnnotations{}), so each entry is computed once and reused.
  struct SiteMemo {
    bool matched = false;                        // `sites` has been computed
    std::vector<rewrite::MatchSite> sites;
    std::vector<std::uint8_t> kind;              // ProposalKind, per site, learned on first propose
    std::vector<std::int32_t> plan_parents_done; // annotation sites: plan nodes already merged onto
  };
  std::vector<std::vector<SiteMemo>> memo_;      // memo_[program_id][rule slot]
  // The rule vector the memo and `rule_kind_` were built against. A memo entry is only meaningful
  // for the rule that produced it, so both are discarded whenever `saturate` is called with a
  // different rule set (different length, different pointers, or different names at the same
  // slot). PRECONDITION, stated rather than assumed: a caller that destroys its rule objects and
  // constructs DIFFERENT ones at the same addresses, with the same names, between two `saturate`
  // calls on the SAME EGraph would defeat that check; every caller in this repository keeps one
  // rule set alive for the whole life of its graph.
  std::vector<const rewrite::Rule*> memo_rules_;
  std::vector<std::string> memo_rule_names_;
  std::vector<ProposalKind> rule_kind_;          // per rule slot

  // Discards the memo unless it was built against exactly `rules`; sizes it for the current node
  // count either way. Returns true when the memo survived (only ever informational).
  bool sync_memo(const std::vector<const rewrite::Rule*>& rules);
  void note_rule_kind(std::size_t slot, ProposalKind kind);

  // Resolves a hash-consing lookup, inserting `id` as a new representative if `content` (of
  // program `program_id`) collides with nothing already known; returns the representative id
  // `id` was unioned onto (== id itself when it is the new representative).
  int hash_cons_plan(int program_id, int id, const ir::PlanAnnotations& content);
  int hash_cons_program(int id, const ir::Program& program);

  // Read-only lookups against the PERSISTENT hash-cons tables (last round's rebuild(), never
  // this round's own not-yet-rebuilt pending queue): true when `content` is already some
  // existing node's content. `saturate()` checks these BEFORE queueing a proposal so re-matching
  // an already-fully-reflected rule (every one of R0's five rules matches unconditionally, so
  // this happens every round) costs a hash lookup, not a new row every time — real hash-consing,
  // not "insert first, discover the duplicate later" on every round forever. A genuinely NEW
  // convergence discovered by two DIFFERENT proposals in the SAME round is unaffected: neither
  // is in the persistent tables yet, so both queue, and `rebuild()` is what unions them.
  bool plan_known(int program_id, const ir::PlanAnnotations& content) const;
  bool program_known(const ir::Program& program) const;

  void rebuild();
};

}  // namespace epykos::optimise
