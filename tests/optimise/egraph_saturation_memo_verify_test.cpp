// M4/EG scaling (D62): the two properties `EGraph::saturate`'s application memo and its re-firing
// policy must hold, checked against SYNTHETIC rules only (the same style egraph_test.cpp
// established), so this file stays fast enough to be a mutation gate — scripts/mutation_test.sh
// runs every gate once per registered mutant, and Stage A recordings are far too slow for that.
//
// Two invariants, and they are deliberately different in kind:
//
//   1. DEDUPPING CHANGES NOTHING. `SaturationLimits::dedup_before_construction` is a pure
//      performance change: it exists because `Rule::match` / `Rule::propose` are pure functions of
//      (program, plan, site) (rule.hpp contract points 1 and 2) and a program node's inputs to
//      both are fixed for its whole life, so re-running them every round — and running them at all
//      on a node congruent to one already matched — can only ever re-derive content the graph
//      already holds. The gate is therefore an EQUIVALENCE: dedup on and dedup off must reach
//      exactly the same set of program contents and, per program, the same set of plan contents. `eg.memo_ignores_site` — a memo that retires a whole (node,
//      rule) pair after its first site — is caught here because it turns a dedup into a silent
//      narrowing of the search.
//
//   2. THE RE-FIRING POLICY KEEPS SELF-CHAINING. RefirePolicy::NoFreshCrossRule gives up mixed
//      structural chains on purpose (egraph.hpp says exactly what), but it must NOT give up a
//      rule chaining onto its OWN output: that is the shape of the only structurally-novel
//      candidate this framework has ever found on Stage A (`r5.group_formation` applied twice,
//      D54 experiment 2). `eg.refire_blocks_self_chain` is caught here.
//
// A third test pins RefirePolicy::PipelineOrderedPlans' own stated contract: a plan derivation
// that runs the rules in the caller's declared order is still reachable (which is what keeps the
// M1 planner's own five-rule default plan inside the search space, D47's fixed dependency order),
// and one that runs them backwards is not.
#include "epykos/optimise/egraph.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "epykos/ir/program.hpp"
#include "epykos/rewrite/rule.hpp"

namespace ir = epykos::ir;
namespace rewrite = epykos::rewrite;
namespace optimise = epykos::optimise;
using epykos::Op;

namespace {

// egraph_test.cpp's own two-domain shape, repeated rather than shared (that file's helpers are
// static to its own anonymous namespace, and a test fixture header for two structs is not worth
// the coupling).
ir::Program tiny_program() {
  ir::Program p;
  p.domains.push_back(ir::Domain{"input", 2, 0, 0, false, {}, false, -1});
  p.groups.push_back(ir::Group{0, {ir::Step{Op::Input, ir::Slot{ir::SlotKind::Input, -1}, {}, {}, {}}}});
  p.inputs = {0, 1};
  p.input_values = {1.0, 2.0};
  p.domains.push_back(ir::Domain{"add(@0,@1)", 1, 2, 0, false, {0}, false, -1});
  p.gathers.push_back(ir::Gather{1, {0}});
  p.gathers.push_back(ir::Gather{1, {1}});
  p.groups.push_back(ir::Group{1, {ir::Step{Op::Add, ir::Slot{ir::SlotKind::Gather, 0}, ir::Slot{ir::SlotKind::Gather, 1}, {}, {}}}});
  p.outputs = {2};
  return p;
}

// A structural rule with SEVERAL sites, each appending a DIFFERENT literal, and which keeps
// matching as long as the program has fewer than `depth_limit` of them. Two independent axes the
// memo must not collapse: several sites per node (site index -> literal value) and a chain of
// applications (the rule re-firing on its own output).
class MultiSiteAppendRule : public rewrite::Rule {
 public:
  MultiSiteAppendRule(std::string name, double base, int sites, std::size_t depth_limit)
      : name_(std::move(name)), base_(base), sites_(sites), depth_limit_(depth_limit) {}

  const std::string& name() const noexcept override { return name_; }
  rewrite::Exactness exactness_class() const noexcept override { return rewrite::Exactness::E0; }

  std::vector<rewrite::MatchSite> match(const ir::Program& program, const ir::PlanAnnotations&) const override {
    if (program.literals.size() >= depth_limit_) return {};
    std::vector<rewrite::MatchSite> sites;
    for (int i = 0; i < sites_; ++i) sites.push_back(rewrite::MatchSite::of_domain(i));
    return sites;
  }

  rewrite::Proposal propose(const ir::Program& program, const ir::PlanAnnotations&,
                            const rewrite::MatchSite& site) const override {
    rewrite::Proposal p;
    ir::Program np = program;
    np.literals.push_back(base_ + static_cast<double>(site.domain));  // unread: still valid
    p.program = np;
    return p;
  }

 private:
  std::string name_;
  double base_;
  int sites_;
  std::size_t depth_limit_;
};

// Sets ONE domain's choice; `slot_marker` makes two instances distinguishable in a plan's content
// (they write different `inline_consumer` values), so a test can tell A-then-B from B-then-A.
class MarkOneDomainRule : public rewrite::Rule {
 public:
  MarkOneDomainRule(std::string name, std::size_t domain, ir::Materialise choice, std::int32_t slot_marker)
      : name_(std::move(name)), domain_(domain), choice_(choice), marker_(slot_marker) {}

  const std::string& name() const noexcept override { return name_; }
  rewrite::Exactness exactness_class() const noexcept override { return rewrite::Exactness::E0; }
  std::vector<rewrite::MatchSite> match(const ir::Program&, const ir::PlanAnnotations&) const override {
    return {rewrite::MatchSite::whole_program()};
  }
  rewrite::Proposal propose(const ir::Program& program, const ir::PlanAnnotations&,
                            const rewrite::MatchSite&) const override {
    rewrite::Proposal p;
    ir::PlanAnnotations a;
    a.domain.assign(program.domains.size(), ir::DomainPlan{});
    a.domain[domain_].choice = choice_;
    a.domain[domain_].inline_consumer = marker_;
    p.annotations = a;
    return p;
  }

 private:
  std::string name_;
  std::size_t domain_;
  ir::Materialise choice_;
  std::int32_t marker_;
};

// Every program CONTENT in the graph, as ir::serialize keys, sorted: the observable result of a
// saturation, independent of how many duplicate nodes or which derivation reached it.
std::vector<std::string> program_contents(const optimise::EGraph& g) {
  std::vector<std::string> keys;
  for (int i = 0; i < g.num_programs(); ++i) keys.push_back(ir::serialize(g.program_node(i).program));
  std::sort(keys.begin(), keys.end());
  keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
  return keys;
}

// How many DISTINCT plan contents program node `pid` has (the plan tier has no serialize, so this
// counts e-classes, which is the same thing after rebuild()).
std::size_t distinct_plans(const optimise::EGraph& g, int pid) {
  return g.plan_class_representatives(pid).size();
}

}  // namespace

// ---- 1. the memo is an equivalence, not a narrowing ----------------------------------------------

TEST(EGraphSaturationMemo, MemoReachesExactlyTheSameProgramsAndPlansAsNoMemo) {
  const MultiSiteAppendRule append("test.append", 100.0, /*sites=*/3, /*depth_limit=*/3);
  const MarkOneDomainRule mark_a("test.mark_a", 0, ir::Materialise::FuseIntoReduction, 7);
  const MarkOneDomainRule mark_b("test.mark_b", 1, ir::Materialise::InlineIntoConsumer, 0);
  const std::vector<const rewrite::Rule*> rules = {&append, &mark_a, &mark_b};

  // AllRules on both sides: dedupping must be invisible under the COMPLETE search, which is the
  // only setting where "same content reached" is a statement about the memo alone.
  optimise::SaturationLimits with_memo;
  with_memo.max_iterations = 6;
  with_memo.refire = optimise::RefirePolicy::AllRules;
  with_memo.dedup_before_construction = true;
  optimise::SaturationLimits without_memo = with_memo;
  without_memo.dedup_before_construction = false;

  optimise::EGraph memo_graph(tiny_program());
  optimise::EGraph plain_graph(tiny_program());
  const optimise::SaturationReport memo_report = memo_graph.saturate(rules, with_memo);
  const optimise::SaturationReport plain_report = plain_graph.saturate(rules, without_memo);
  ASSERT_FALSE(memo_report.bound_hit);
  ASSERT_FALSE(plain_report.bound_hit);
  EXPECT_TRUE(memo_graph.congruent());
  EXPECT_TRUE(plain_graph.congruent());

  // The rule really does exercise several sites and several rounds, or the equivalence below
  // would be vacuous (D53: a gate must state whether its deliverable actually fires).
  ASSERT_GT(memo_graph.num_programs(), 4) << "the fixture must actually branch for this to mean anything";

  const std::vector<std::string> memo_keys = program_contents(memo_graph);
  const std::vector<std::string> plain_keys = program_contents(plain_graph);
  EXPECT_EQ(memo_keys, plain_keys) << "the application memo must skip only work whose result the "
                                      "graph already holds, never a reachable program";

  // Same for the plan tier, per program CLASS representative (the nodes extract() actually
  // visits). Class ids are comparable between the two graphs because both saturations queue in
  // the same rule / program / site order.
  const std::vector<int> memo_reps = memo_graph.program_class_representatives();
  const std::vector<int> plain_reps = plain_graph.program_class_representatives();
  ASSERT_EQ(memo_reps.size(), plain_reps.size());
  for (std::size_t i = 0; i < memo_reps.size(); ++i) {
    EXPECT_EQ(distinct_plans(memo_graph, memo_reps[i]), distinct_plans(plain_graph, plain_reps[i]))
        << "plan tier of representative #" << i << " differs with the memo on";
  }

  // And it really did save work, or the whole entry is pointless.
  std::size_t memoised = 0;
  for (const optimise::SaturationLogEntry& e : memo_report.log) memoised += e.proposals_memoised;
  EXPECT_GT(memoised, 0u) << "the memo reported skipping nothing at all";
}

// ---- 2. the re-firing policy keeps a rule's own chain --------------------------------------------

TEST(EGraphSaturationRefire, NoFreshCrossRuleStillLetsARuleChainOntoItsOwnOutput) {
  // One site each, so the only way past depth 1 is the rule firing on what it produced itself.
  const MultiSiteAppendRule a("test.append_a", 100.0, /*sites=*/1, /*depth_limit=*/3);
  const std::vector<const rewrite::Rule*> rules = {&a};

  optimise::SaturationLimits limits;
  limits.max_iterations = 6;
  limits.refire = optimise::RefirePolicy::NoFreshCrossRule;
  optimise::EGraph g(tiny_program());
  const optimise::SaturationReport report = g.saturate(rules, limits);
  EXPECT_FALSE(report.bound_hit);
  EXPECT_TRUE(g.congruent());

  // depth_limit 3 means: root (0 literals) -> 1 -> 2 -> 3, then match() stops. Four programs, in
  // one chain, each the previous one's own output. Anything less means the policy refused a rule
  // its own output — the r5.group_formation-applied-twice shape (D54) would be unreachable.
  EXPECT_EQ(g.num_programs(), 4) << "NoFreshCrossRule must not block a rule from re-firing on its "
                                    "OWN output: that is the r5-twice candidate's shape";
  EXPECT_EQ(g.program_node(3).history,
            (std::vector<std::string>{a.name(), a.name(), a.name()}));
}

TEST(EGraphSaturationRefire, NoFreshCrossRuleBlocksAnotherStructuralRuleOnAFreshRewrite) {
  // Two structural rules, each with one site, each able to keep firing. Under AllRules the two
  // interleave and the graph grows with every mixed chain; under NoFreshCrossRule each keeps to
  // its own chain. This is the completeness the policy gives up, pinned so it cannot change by
  // accident and go unnoticed.
  const MultiSiteAppendRule a("test.append_a", 100.0, /*sites=*/1, /*depth_limit=*/3);
  const MultiSiteAppendRule b("test.append_b", 200.0, /*sites=*/1, /*depth_limit=*/3);
  const std::vector<const rewrite::Rule*> rules = {&a, &b};

  optimise::SaturationLimits complete;
  complete.max_iterations = 6;
  complete.refire = optimise::RefirePolicy::AllRules;
  optimise::EGraph full(tiny_program());
  full.saturate(rules, complete);

  optimise::SaturationLimits narrowed = complete;
  narrowed.refire = optimise::RefirePolicy::NoFreshCrossRule;
  optimise::EGraph chained(tiny_program());
  chained.saturate(rules, narrowed);

  // Complete search: every word over {a, b} of length <= 3 that is distinct as CONTENT. Narrowed:
  // the root plus each rule's own chain (a, aa, aaa, b, bb, bbb) = 7 classes.
  EXPECT_GT(full.program_class_representatives().size(), chained.program_class_representatives().size())
      << "NoFreshCrossRule is supposed to be a strictly narrower search than AllRules";
  EXPECT_EQ(chained.program_class_representatives().size(), 7u);
  EXPECT_TRUE(full.congruent());
  EXPECT_TRUE(chained.congruent());
}

// ---- 3. PipelineOrderedPlans keeps the declared order reachable ----------------------------------

TEST(EGraphSaturationRefire, PipelineOrderedPlansKeepsTheDeclaredOrderAndDropsTheReverse) {
  // Two annotation rules on DIFFERENT domains, so their composition is a genuinely new plan
  // content rather than one of them overwriting the other (rewrite::merge_annotations is
  // copy-if-not-default, greedy.hpp) — and so both ORDERS reach that same content, which is what
  // makes "which orders are enumerated" the thing under test rather than "which contents exist".
  const MarkOneDomainRule first("test.plan_first", 0, ir::Materialise::FuseIntoReduction, -1);
  const MarkOneDomainRule second("test.plan_second", 1, ir::Materialise::InlineIntoConsumer, 0);
  const std::vector<const rewrite::Rule*> rules = {&first, &second};  // slots 0 and 1

  auto histories = [](const optimise::EGraph& g) {
    std::vector<std::vector<std::string>> out;
    for (int i = 0; i < g.num_plan_nodes(0); ++i) out.push_back(g.plan_node(0, i).history);
    return out;
  };
  const std::vector<std::string> declared = {first.name(), second.name()};
  const std::vector<std::string> reverse = {second.name(), first.name()};

  optimise::SaturationLimits limits;
  limits.max_iterations = 6;
  limits.refire = optimise::RefirePolicy::PipelineOrderedPlans;
  optimise::EGraph g(tiny_program());
  const optimise::SaturationReport report = g.saturate(rules, limits);
  EXPECT_FALSE(report.bound_hit);
  EXPECT_TRUE(g.congruent());

  // Reachable: the empty root, `first` alone, `second` alone, and `first`-then-`second` (slot 0
  // then slot 1, the declared order — this is what keeps the M1 planner's own fixed-order
  // five-rule default plan inside the search space). NOT reachable: `second`-then-`first`.
  const std::vector<std::vector<std::string>> got = histories(g);
  EXPECT_NE(std::find(got.begin(), got.end(), declared), got.end())
      << "the caller's own declared rule order must stay reachable";
  EXPECT_EQ(std::find(got.begin(), got.end(), reverse), got.end())
      << "PipelineOrderedPlans is documented to drop out-of-order plans";
  EXPECT_EQ(g.num_plan_nodes(0), 4);

  // And the complete search really does enumerate both orders — the duplicate work (same content,
  // two nodes) this policy declines to do.
  optimise::SaturationLimits complete = limits;
  complete.refire = optimise::RefirePolicy::AllRules;
  optimise::EGraph full(tiny_program());
  full.saturate(rules, complete);
  const std::vector<std::vector<std::string>> all = histories(full);
  EXPECT_NE(std::find(all.begin(), all.end(), declared), all.end());
  EXPECT_NE(std::find(all.begin(), all.end(), reverse), all.end());
  // Same reachable CONTENT either way here (both orders converge), one fewer node to hold it.
  EXPECT_EQ(full.plan_class_representatives(0).size(), g.plan_class_representatives(0).size());
  EXPECT_GT(full.num_plan_nodes(0), g.num_plan_nodes(0));
}
