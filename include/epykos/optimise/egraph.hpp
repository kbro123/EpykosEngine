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

struct SaturationLimits {
  int max_iterations = 16;          // rounds of "match every node, queue every proposal, rebuild"
  std::size_t max_plan_nodes = 20000;    // total PLAN nodes ever inserted, across every program
  std::size_t max_program_nodes = 2000;  // total PROGRAM nodes ever inserted
};

// One rule application recorded during saturate(), win or lose: a full account of what ran, so a
// bound being hit is a logged event, never a silent truncation (PROBLEM.md §7's own words).
struct SaturationLogEntry {
  int iteration = 0;
  std::string rule;
  std::size_t sites_matched = 0;
  std::size_t nodes_added = 0;       // new (pre-rebuild) nodes queued from this rule this round
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
