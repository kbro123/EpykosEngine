// M4/R0: the Rule / Proposal / MatchSite contract (rewrite/rule.hpp), the greedy driver
// (apply_greedy, run_pipeline, merge_annotations, rewrite/greedy.hpp) and the R1-R7 identity
// stubs (rewrite/stub_rule.hpp and rewrite/r*.hpp) -- framework tests independent of any real
// planner decision (see planner_shapes_test.cpp / planner_rules_m1_test.cpp for those).
#include <gtest/gtest.h>

#include "epykos/rewrite/greedy.hpp"
#include "epykos/rewrite/r1_fold_uniform_columns.hpp"
#include "epykos/rewrite/r2_bucket_rows.hpp"
#include "epykos/rewrite/r3_elide_trivial_maps.hpp"
#include "epykos/rewrite/r4a_push_unary_through_gathers.hpp"
#include "epykos/rewrite/r4b_shared_reciprocal.hpp"
#include "epykos/rewrite/r5_group_formation.hpp"
#include "epykos/rewrite/r6_materialise_boundaries.hpp"
#include "epykos/rewrite/r7_block_linmap.hpp"
#include "epykos/rewrite/rule.hpp"

namespace ir = epykos::ir;
namespace rewrite = epykos::rewrite;

namespace {

// A tiny two-domain Program: just enough for `ir::validate` to accept it (framework tests do not
// need it to mean anything as maths).
ir::Program tiny_program() {
  ir::Program p;
  p.domains.push_back(ir::Domain{"input", 2, 0, 0, false, {}, false, -1});
  p.groups.push_back(ir::Group{0, {ir::Step{epykos::Op::Input, ir::Slot{ir::SlotKind::Input, -1}, {}, {}, {}}}});
  p.inputs = {0, 1};
  p.input_values = {1.0, 2.0};
  p.domains.push_back(ir::Domain{"add(@0,@1)", 1, 2, 0, false, {0}, false, -1});
  p.gathers.push_back(ir::Gather{1, {0}});  // gather 0: row 0 reads value 0
  p.gathers.push_back(ir::Gather{1, {1}});  // gather 1: row 0 reads value 1
  p.groups.push_back(ir::Group{1, {ir::Step{epykos::Op::Add, ir::Slot{ir::SlotKind::Gather, 0},
                                            ir::Slot{ir::SlotKind::Gather, 1}, {}, {}}}});
  p.outputs = {2};
  return p;
}

// A rule that always matches the whole program and marks every domain FuseIntoReduction with no
// kept rows -- deliberately wrong for `tiny_program()` (domain 1's row IS an output and is not a
// whole-domain member, so it would never be written anywhere): used to exercise the framework's
// plumbing (match/propose/apply_greedy), not to be run through a real Interpreter.
class MarkEverythingFusedRule : public rewrite::Rule {
 public:
  const std::string& name() const noexcept override {
    static const std::string n = "test.mark_everything_fused";
    return n;
  }
  rewrite::Exactness exactness_class() const noexcept override { return rewrite::Exactness::E0; }
  std::vector<rewrite::MatchSite> match(const ir::Program&, const ir::PlanAnnotations&) const override {
    return {rewrite::MatchSite::whole_program()};
  }
  rewrite::Proposal propose(const ir::Program& program, const ir::PlanAnnotations&,
                            const rewrite::MatchSite&) const override {
    rewrite::Proposal p;
    ir::PlanAnnotations a;
    a.domain.assign(program.domains.size(), ir::DomainPlan{});
    for (ir::DomainPlan& dp : a.domain) dp.choice = ir::Materialise::FuseIntoReduction;
    p.annotations = a;
    return p;
  }
};

// A structural rule that hands back the input Program unchanged (Proposal::program set, exactly
// like the R1-R7 stubs) -- exercises apply_greedy's structural path.
class IdentityStructuralRule : public rewrite::Rule {
 public:
  const std::string& name() const noexcept override {
    static const std::string n = "test.identity_structural";
    return n;
  }
  rewrite::Exactness exactness_class() const noexcept override { return rewrite::Exactness::E0; }
  std::vector<rewrite::MatchSite> match(const ir::Program&, const ir::PlanAnnotations&) const override {
    return {rewrite::MatchSite::whole_program()};
  }
  rewrite::Proposal propose(const ir::Program& program, const ir::PlanAnnotations&,
                            const rewrite::MatchSite&) const override {
    rewrite::Proposal p;
    p.program = program;
    return p;
  }
};

}  // namespace

TEST(RuleFramework, MatchSiteFactoriesSetTheExpectedFields) {
  const rewrite::MatchSite d = rewrite::MatchSite::of_domain(3);
  EXPECT_EQ(d.kind, rewrite::SiteKind::Domain);
  EXPECT_EQ(d.domain, 3);
  const rewrite::MatchSite s = rewrite::MatchSite::of_step(3, 7);
  EXPECT_EQ(s.kind, rewrite::SiteKind::Step);
  EXPECT_EQ(s.domain, 3);
  EXPECT_EQ(s.step, 7);
  const rewrite::MatchSite g = rewrite::MatchSite::of_gather(4);
  EXPECT_EQ(g.kind, rewrite::SiteKind::Gather);
  EXPECT_EQ(g.index, 4);
  EXPECT_EQ(rewrite::MatchSite::whole_program().kind, rewrite::SiteKind::Program);
}

TEST(RuleFramework, ProposalClassifiesStructuralVsAnnotationVsEmpty) {
  rewrite::Proposal empty;
  EXPECT_TRUE(empty.empty());
  EXPECT_FALSE(empty.is_structural());
  EXPECT_FALSE(empty.is_annotation());

  rewrite::Proposal structural;
  structural.program = tiny_program();
  EXPECT_TRUE(structural.is_structural());
  EXPECT_FALSE(structural.is_annotation());

  rewrite::Proposal annotation;
  annotation.annotations = ir::PlanAnnotations{};
  EXPECT_TRUE(annotation.is_annotation());
  EXPECT_FALSE(annotation.is_structural());
}

TEST(RuleFramework, ApplyGreedyMergesAnAnnotationProposalOntoThePlan) {
  ir::Program program = tiny_program();
  ir::PlanAnnotations plan;
  const MarkEverythingFusedRule rule;
  const std::size_t applied = rewrite::apply_greedy(rule, program, plan);
  EXPECT_EQ(applied, 1u);
  ASSERT_EQ(plan.domain.size(), 2u);
  EXPECT_EQ(plan.domain[0].choice, ir::Materialise::FuseIntoReduction);
  EXPECT_EQ(plan.domain[1].choice, ir::Materialise::FuseIntoReduction);
}

TEST(RuleFramework, ApplyGreedyReplacesTheProgramOnAStructuralProposal) {
  ir::Program program = tiny_program();
  const ir::Program original = program;
  ir::PlanAnnotations plan;
  const IdentityStructuralRule rule;
  const std::size_t applied = rewrite::apply_greedy(rule, program, plan);
  EXPECT_EQ(applied, 1u);
  EXPECT_TRUE(program == original);
}

TEST(RuleFramework, RunPipelineThreadsAnnotationsThroughInOrder) {
  const ir::Program program = tiny_program();
  ir::PlanAnnotations plan;
  const MarkEverythingFusedRule fuse_all;
  std::vector<const rewrite::Rule*> rules = {&fuse_all};
  ir::Program working = program;
  rewrite::run_pipeline(rules, working, plan);
  ASSERT_EQ(plan.domain.size(), 2u);
  EXPECT_EQ(plan.domain[0].choice, ir::Materialise::FuseIntoReduction);
}

TEST(RuleFramework, MergeAnnotationsOrsEmittedAndOverwritesNamedDomains) {
  ir::PlanAnnotations dst;
  dst.domain.assign(2, ir::DomainPlan{});
  dst.domain[0].choice = ir::Materialise::InlineIntoConsumer;
  dst.domain[0].inline_consumer = 5;
  dst.emitted = {1, 0, 0};

  ir::PlanAnnotations src;
  src.domain.assign(2, ir::DomainPlan{});
  src.domain[1].choice = ir::Materialise::FuseIntoReduction;  // domain 0 left default: must not overwrite dst[0]
  src.emitted = {0, 1, 1};

  rewrite::merge_annotations(dst, src);
  ASSERT_EQ(dst.domain.size(), 2u);
  EXPECT_EQ(dst.domain[0].choice, ir::Materialise::InlineIntoConsumer);  // unchanged
  EXPECT_EQ(dst.domain[0].inline_consumer, 5);
  EXPECT_EQ(dst.domain[1].choice, ir::Materialise::FuseIntoReduction);   // set by src
  ASSERT_EQ(dst.emitted.size(), 3u);
  EXPECT_EQ(dst.emitted[0], 1);  // dst's own bit kept
  EXPECT_EQ(dst.emitted[1], 1);  // OR'd in from src
  EXPECT_EQ(dst.emitted[2], 1);
}

TEST(RuleFramework, MergeAnnotationsInsertsAndOverwritesJacobianBlocksByKey) {
  ir::PlanAnnotations dst;
  dst.jacobian.mode["sofr"] = ir::AdMode::Forward;
  ir::PlanAnnotations src;
  src.jacobian.mode["sofr"] = ir::AdMode::Reverse;
  src.jacobian.mode["estr"] = ir::AdMode::ClosedFormAffine;
  rewrite::merge_annotations(dst, src);
  EXPECT_EQ(dst.jacobian.mode.at("sofr"), ir::AdMode::Reverse);
  EXPECT_EQ(dst.jacobian.mode.at("estr"), ir::AdMode::ClosedFormAffine);
}

TEST(RuleFramework, PlanAnnotationsEqualityIgnoresContentByDesign) {
  // ir/annotate.hpp: PlanAnnotations is excluded from a Program's identity (round-trip, D10) --
  // its own operator== is unconditionally true, so two Programs differing only in `.plan` compare
  // equal under Program::operator== (= default).
  ir::Program a = tiny_program();
  ir::Program b = tiny_program();
  ir::PlanAnnotations plan;
  plan.domain.assign(a.domains.size(), ir::DomainPlan{});
  plan.domain[0].choice = ir::Materialise::FuseIntoReduction;
  b.plan = plan;
  EXPECT_TRUE(a == b);
  // PlanAnnotations::operator== is unconditionally true (ir/annotate.hpp) -- even two annotations
  // that plainly differ (one has a domain entry, one is default) compare equal, by design.
  EXPECT_TRUE(plan == ir::PlanAnnotations{});
}

// The identity stubs never fire (R-a/R-b/R-c fill them in with a real match/propose): match()
// returns no sites on a program that would exercise every real rule's target shape were it real.
TEST(RuleFramework, R1ThroughR7StubsNeverMatchAndReportTheirDeclaredExactness) {
  const ir::Program program = tiny_program();
  const ir::PlanAnnotations plan;
  const rewrite::R1FoldUniformColumns r1;
  const rewrite::R2BucketRows r2;
  const rewrite::R3ElideTrivialMaps r3;
  const rewrite::R4aPushUnaryThroughGathers r4a;
  const rewrite::R4bSharedReciprocal r4b;
  const rewrite::R5GroupFormation r5;
  const rewrite::R6MaterialiseBoundaries r6;
  const rewrite::R7BlockLinmap r7;
  const rewrite::Rule* stubs[] = {&r1, &r2, &r3, &r4a, &r4b, &r5, &r6, &r7};
  for (const rewrite::Rule* stub : stubs) {
    EXPECT_TRUE(stub->match(program, plan).empty()) << stub->name();
  }
  EXPECT_EQ(r1.name(), "r1.fold_uniform_columns");
  EXPECT_EQ(r7.name(), "r7.block_linmap");
  EXPECT_EQ(r1.exactness_class(), rewrite::Exactness::E0);
  EXPECT_EQ(r4b.exactness_class(), rewrite::Exactness::E1);  // the one E1 rewrite in DESIGN.md §6
}

TEST(RuleFramework, AStubsProposeIsIdentityIfEverCalledDirectly) {
  const ir::Program program = tiny_program();
  const rewrite::R3ElideTrivialMaps r3;
  rewrite::Proposal p = r3.propose(program, ir::PlanAnnotations{}, rewrite::MatchSite::whole_program());
  ASSERT_TRUE(p.is_structural());
  EXPECT_TRUE(*p.program == program);
}
