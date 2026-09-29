// M4/EG "core": the e-graph's own invariants (include/epykos/optimise/egraph.hpp), independent of
// any real rewrite::Rule (see egraph_m1_extract_test.cpp for that) — congruence after rebuild in
// both tiers, "produce the alternative without discarding the original", a saturation bound
// reported rather than silently hit, and determinism across two independent runs. Every rule here
// is a small synthetic test-only rewrite::Rule, in the style tests/rewrite/rule_framework_test.cpp
// already established (MarkEverythingFusedRule) for exercising the framework without depending on
// planner semantics.
#include "epykos/optimise/egraph.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "epykos/ir/program.hpp"
#include "epykos/rewrite/rule.hpp"

namespace ir = epykos::ir;
namespace rewrite = epykos::rewrite;
namespace optimise = epykos::optimise;
using epykos::Op;

namespace {

// The same two-domain shape tests/rewrite/rule_framework_test.cpp uses: domain 0 is two Input
// rows, domain 1 gathers both into one Add — just enough for ir::validate to accept it.
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

ir::PlanAnnotations all_domains(ir::Materialise choice, std::size_t n) {
  ir::PlanAnnotations a;
  a.domain.assign(n, ir::DomainPlan{});
  for (ir::DomainPlan& d : a.domain) d.choice = choice;
  return a;
}

// Proposes FuseIntoReduction for EVERY domain, unconditionally: rule_framework_test.cpp's own
// MarkEverythingFusedRule, reused here to check "the original stays a sibling".
class FuseAllRule : public rewrite::Rule {
 public:
  const std::string& name() const noexcept override {
    static const std::string n = "test.fuse_all";
    return n;
  }
  rewrite::Exactness exactness_class() const noexcept override { return rewrite::Exactness::E0; }
  std::vector<rewrite::MatchSite> match(const ir::Program&, const ir::PlanAnnotations&) const override {
    return {rewrite::MatchSite::whole_program()};
  }
  rewrite::Proposal propose(const ir::Program& program, const ir::PlanAnnotations&, const rewrite::MatchSite&) const override {
    rewrite::Proposal p;
    p.annotations = all_domains(ir::Materialise::FuseIntoReduction, program.domains.size());
    return p;
  }
};

// Touches ONLY domain 0 (leaves domain 1's entry default, so merge_annotations never overwrites
// whatever another rule already decided for it — see rewrite/greedy.hpp's own doc).
class MarkDomain0Rule : public rewrite::Rule {
 public:
  const std::string& name() const noexcept override {
    static const std::string n = "test.mark_domain0";
    return n;
  }
  rewrite::Exactness exactness_class() const noexcept override { return rewrite::Exactness::E0; }
  std::vector<rewrite::MatchSite> match(const ir::Program&, const ir::PlanAnnotations&) const override {
    return {rewrite::MatchSite::whole_program()};
  }
  rewrite::Proposal propose(const ir::Program& program, const ir::PlanAnnotations&, const rewrite::MatchSite&) const override {
    rewrite::Proposal p;
    ir::PlanAnnotations a;
    a.domain.assign(program.domains.size(), ir::DomainPlan{});
    a.domain[0].choice = ir::Materialise::FuseIntoReduction;
    p.annotations = a;
    return p;
  }
};

// Touches ONLY domain 1.
class MarkDomain1Rule : public rewrite::Rule {
 public:
  const std::string& name() const noexcept override {
    static const std::string n = "test.mark_domain1";
    return n;
  }
  rewrite::Exactness exactness_class() const noexcept override { return rewrite::Exactness::E0; }
  std::vector<rewrite::MatchSite> match(const ir::Program&, const ir::PlanAnnotations&) const override {
    return {rewrite::MatchSite::whole_program()};
  }
  rewrite::Proposal propose(const ir::Program& program, const ir::PlanAnnotations&, const rewrite::MatchSite&) const override {
    rewrite::Proposal p;
    ir::PlanAnnotations a;
    a.domain.assign(program.domains.size(), ir::DomainPlan{});
    a.domain[1].choice = ir::Materialise::InlineIntoConsumer;
    a.domain[1].inline_consumer = 0;
    p.annotations = a;
    return p;
  }
};

// A structural rule matching TWO independent sites of the SAME program, both proposing the SAME
// genuinely-new content (an unread literal appended) — the program tier's hash-consing must fold
// the two resulting nodes into ONE class, not two.
class DuplicatePerturbRule : public rewrite::Rule {
 public:
  const std::string& name() const noexcept override {
    static const std::string n = "test.duplicate_perturb";
    return n;
  }
  rewrite::Exactness exactness_class() const noexcept override { return rewrite::Exactness::E0; }
  std::vector<rewrite::MatchSite> match(const ir::Program&, const ir::PlanAnnotations&) const override {
    return {rewrite::MatchSite::of_domain(0), rewrite::MatchSite::of_domain(1)};
  }
  rewrite::Proposal propose(const ir::Program& program, const ir::PlanAnnotations&, const rewrite::MatchSite&) const override {
    rewrite::Proposal p;
    ir::Program np = program;
    np.literals.push_back(99.0);  // identical regardless of which site proposed it
    p.program = np;
    return p;
  }
};

// A structural rule that genuinely changes the program (adds an unread literal): a real new
// PROGRAM e-class, distinct from the root.
class PerturbRule : public rewrite::Rule {
 public:
  const std::string& name() const noexcept override {
    static const std::string n = "test.perturb";
    return n;
  }
  rewrite::Exactness exactness_class() const noexcept override { return rewrite::Exactness::E0; }
  std::vector<rewrite::MatchSite> match(const ir::Program&, const ir::PlanAnnotations&) const override {
    return {rewrite::MatchSite::whole_program()};
  }
  rewrite::Proposal propose(const ir::Program& program, const ir::PlanAnnotations&, const rewrite::MatchSite&) const override {
    rewrite::Proposal p;
    ir::Program np = program;
    np.literals.push_back(42.0);  // unread: still a valid, distinct Program
    p.program = np;
    return p;
  }
};

}  // namespace

TEST(EGraph, ConstructionSeedsRootProgramAndEmptyPlan) {
  optimise::EGraph g(tiny_program());
  EXPECT_EQ(g.num_programs(), 1);
  EXPECT_EQ(g.num_plan_nodes(0), 1);
  EXPECT_TRUE(optimise::EGraph::plan_content_equal(g.plan_node(0, 0).content, ir::PlanAnnotations{}));
  EXPECT_TRUE(g.congruent());
}

TEST(EGraph, SaturateWithNoRulesIsAFixpointImmediately) {
  optimise::EGraph g(tiny_program());
  const optimise::SaturationReport report = g.saturate({});
  EXPECT_EQ(report.iterations_run, 1);
  EXPECT_FALSE(report.bound_hit);
  EXPECT_EQ(g.num_programs(), 1);
  EXPECT_EQ(g.num_plan_nodes(0), 1);
}

TEST(EGraph, SaturateAddsASiblingWithoutDiscardingTheOriginal) {
  optimise::EGraph g(tiny_program());
  const FuseAllRule rule;
  const optimise::SaturationReport report = g.saturate({&rule});
  EXPECT_FALSE(report.bound_hit);
  ASSERT_EQ(g.num_plan_nodes(0), 2);
  // The original (empty) plan is still there, unmodified.
  EXPECT_TRUE(optimise::EGraph::plan_content_equal(g.plan_node(0, 0).content, ir::PlanAnnotations{}));
  // The new sibling really is FuseAllRule's proposal.
  const ir::PlanAnnotations expected = all_domains(ir::Materialise::FuseIntoReduction, 2);
  EXPECT_TRUE(optimise::EGraph::plan_content_equal(g.plan_node(0, 1).content, expected));
  EXPECT_EQ(g.plan_node(0, 1).history, std::vector<std::string>{rule.name()});
}

TEST(EGraph, SaturateReachesAFixpointAndStopsGrowing) {
  optimise::EGraph g(tiny_program());
  const FuseAllRule rule;
  g.saturate({&rule}, optimise::SaturationLimits{/*max_iterations=*/1});
  const int after_one_round = g.num_plan_nodes(0);
  g.saturate({&rule}, optimise::SaturationLimits{/*max_iterations=*/10});
  // A second (idempotent) application of the same whole-program rule proposes nothing new: the
  // fixpoint was already reached in round 1 (FuseAllRule doesn't depend on the running plan).
  EXPECT_EQ(g.num_plan_nodes(0), after_one_round);
}

TEST(EGraph, CongruenceAfterRebuildUnifiesPathIndependentDuplicates) {
  optimise::EGraph g(tiny_program());
  const MarkDomain0Rule rule_a;
  const MarkDomain1Rule rule_b;
  const optimise::SaturationReport report = g.saturate({&rule_a, &rule_b}, optimise::SaturationLimits{/*max_iterations=*/4});
  EXPECT_FALSE(report.bound_hit);
  ASSERT_TRUE(g.congruent());

  // The two-step result (domain 0 fused, domain 1 inlined) is reachable via A-then-B AND
  // B-then-A -- two different derivations queued in the SAME round (round 2, since each is one
  // node's whole history away from a DIFFERENT round-1 node), so before rebuild() they are
  // distinct pending entries. Find both and check they landed in one class.
  ir::PlanAnnotations expected;
  expected.domain.assign(2, ir::DomainPlan{});
  expected.domain[0].choice = ir::Materialise::FuseIntoReduction;
  expected.domain[1].choice = ir::Materialise::InlineIntoConsumer;
  expected.domain[1].inline_consumer = 0;

  std::vector<int> matches;
  for (int i = 0; i < g.num_plan_nodes(0); ++i) {
    if (optimise::EGraph::plan_content_equal(g.plan_node(0, i).content, expected)) matches.push_back(i);
  }
  ASSERT_GE(matches.size(), 2u) << "expected at least two distinct derivations of the same content";
  for (std::size_t i = 1; i < matches.size(); ++i) {
    EXPECT_EQ(g.plan_class(0, matches[0]), g.plan_class(0, matches[i]));
  }
}

TEST(EGraph, StructuralHashConsingFoldsIdenticalProposalsIntoOneClass) {
  optimise::EGraph g(tiny_program());
  const DuplicatePerturbRule dup;
  // One round only: DuplicatePerturbRule's match() is unconditional, so a second round would
  // apply it AGAIN to the round-1 result (a real, different, further-perturbed program) rather
  // than reproducing a duplicate — a real structural rule's `match` would stop firing once its
  // target pattern is gone; this synthetic one does not need to, for what this test checks.
  const optimise::SaturationReport report = g.saturate({&dup}, optimise::SaturationLimits{/*max_iterations=*/1});
  EXPECT_FALSE(report.bound_hit);
  // Two sites in the SAME round, both proposing the same new content BEFORE rebuild() has had a
  // chance to hash-cons either one (matching only ever reads the PERSISTENT tables, updated by
  // rebuild() alone -- see program_known's own doc): both are queued and both get a real row
  // (raw node count grows by 2, exactly egraph.hpp's "duplicate insertion, then union" rebuild
  // pattern -- the same one CongruenceAfterRebuildUnifiesPathIndependentDuplicates exercises on
  // the plan tier), but rebuild() must fold them into exactly ONE new e-CLASS.
  ASSERT_EQ(g.num_programs(), 3);
  ir::Program expected = tiny_program();
  expected.literals.push_back(99.0);
  EXPECT_TRUE(g.program_node(1).program == expected);
  EXPECT_TRUE(g.program_node(2).program == expected);
  EXPECT_FALSE(g.program_node(1).program == g.program_node(0).program);  // genuinely different
  EXPECT_TRUE(g.program_node(0).program == tiny_program());              // root itself untouched
  EXPECT_EQ(g.program_class(1), g.program_class(2));                     // folded into one class
  EXPECT_NE(g.program_class(0), g.program_class(1));
  EXPECT_EQ(g.program_class_representatives().size(), 2u);
  EXPECT_TRUE(g.congruent());
}

TEST(EGraph, StructuralRewritesGetTheirOwnClassAndFreshPlanTier) {
  optimise::EGraph g(tiny_program());
  const PerturbRule perturb;
  const optimise::SaturationReport report = g.saturate({&perturb}, optimise::SaturationLimits{/*max_iterations=*/1});
  EXPECT_FALSE(report.bound_hit);
  ASSERT_EQ(g.num_programs(), 2);
  EXPECT_NE(g.program_class(0), g.program_class(1));
  EXPECT_FALSE(g.program_node(0).program == g.program_node(1).program);
  // The new program's plan tier starts over at the empty annotation, independent of program 0's.
  EXPECT_EQ(g.num_plan_nodes(1), 1);
  EXPECT_TRUE(optimise::EGraph::plan_content_equal(g.plan_node(1, 0).content, ir::PlanAnnotations{}));
  EXPECT_TRUE(g.congruent());
}

TEST(EGraph, SaturationBoundIsReportedNotSilentlyHit) {
  optimise::EGraph g(tiny_program());
  const MarkDomain0Rule rule_a;
  const MarkDomain1Rule rule_b;
  optimise::SaturationLimits limits;
  limits.max_iterations = 10;
  limits.max_plan_nodes = 2;  // room for exactly one more node past the root
  const optimise::SaturationReport report = g.saturate({&rule_a, &rule_b}, limits);
  EXPECT_TRUE(report.bound_hit);
  EXPECT_EQ(report.bound_reason, "max_plan_nodes");
  bool logged_cut = false;
  for (const optimise::SaturationLogEntry& entry : report.log) {
    if (entry.cut) logged_cut = true;
  }
  EXPECT_TRUE(logged_cut) << "a bound hit must be a logged event, not a silent truncation";
  EXPECT_LE(g.num_plan_nodes(0), 3);  // did not blow past the bound before stopping
}

TEST(EGraph, DeterminismTwoIndependentRunsAgree) {
  const MarkDomain0Rule rule_a;
  const MarkDomain1Rule rule_b;
  const FuseAllRule rule_c;
  std::vector<const rewrite::Rule*> rules = {&rule_a, &rule_b, &rule_c};

  optimise::EGraph g1(tiny_program());
  optimise::EGraph g2(tiny_program());
  const optimise::SaturationReport r1 = g1.saturate(rules, optimise::SaturationLimits{/*max_iterations=*/4});
  const optimise::SaturationReport r2 = g2.saturate(rules, optimise::SaturationLimits{/*max_iterations=*/4});

  EXPECT_EQ(r1.total_plan_nodes, r2.total_plan_nodes);
  EXPECT_EQ(r1.total_program_nodes, r2.total_program_nodes);
  ASSERT_EQ(g1.num_plan_nodes(0), g2.num_plan_nodes(0));
  const std::vector<int> reps1 = g1.plan_class_representatives(0);
  const std::vector<int> reps2 = g2.plan_class_representatives(0);
  ASSERT_EQ(reps1.size(), reps2.size());
  for (std::size_t i = 0; i < reps1.size(); ++i) {
    EXPECT_TRUE(optimise::EGraph::plan_content_equal(g1.plan_node(0, reps1[i]).content, g2.plan_node(0, reps2[i]).content));
  }
}
