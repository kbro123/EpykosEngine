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
#include "epykos/rewrite/stub_rule.hpp"

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

// A tiny three-domain Program with exactly R6's own target shape: domain 1 is read once, by one
// non-reduction consumer (domain 2) -- unlike tiny_program()'s domain 0, domain 1 here is NOT an
// Input (R6MaterialiseBoundaries excludes those: rewrite/r6_materialise_boundaries.cpp).
ir::Program program_with_an_inlinable_producer() {
  ir::Program p;
  p.domains.push_back(ir::Domain{"input", 1, 0, 0, false, {}, false, -1});
  p.groups.push_back(ir::Group{0, {ir::Step{epykos::Op::Input, ir::Slot{ir::SlotKind::Input, -1}, {}, {}, {}}}});
  p.inputs = {0};
  p.input_values = {1.0};
  p.gathers.push_back(ir::Gather{1, {0}});  // P's gather: row 0 reads Input's row 0
  p.domains.push_back(ir::Domain{"neg(@0)", 1, 1, 0, false, {0}, false, -1});
  p.groups.push_back(ir::Group{1, {ir::Step{epykos::Op::Neg, ir::Slot{ir::SlotKind::Gather, 0}, {}, {}, {}}}});
  p.gathers.push_back(ir::Gather{2, {1}});  // consumer's gather: reads P's row 0
  p.domains.push_back(ir::Domain{"neg(@0)", 1, 2, 0, false, {1}, false, -1});
  p.groups.push_back(ir::Group{2, {ir::Step{epykos::Op::Neg, ir::Slot{ir::SlotKind::Gather, 1}, {}, {}, {}}}});
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

// All seven of DESIGN.md §6's rewrites are real now: R1-R3 (M4/R-a), R4a/R4b/R5 (M4/R-b), R6/R7
// (M4/R-c) -- rewrite/stub_rule.hpp's IdentityStubRule has no remaining user among them. None of
// R1, R2 or R3 matches `tiny_program()`: R1/R2 need a column of their own to fold or bucket on
// (this program has none), R3's target is a length-1 Sum (its one non-Input domain is
// `Add(@0,@1)`) -- checked here; R4a/R4b/R5 are checked the same way just below
// (rewrite/r4a_push_unary_through_gathers.hpp etc.); R6 DOES match `tiny_program()` (domain 0's
// two rows are each read once, by one non-reduction consumer -- exactly R6's own target), so it
// and R7 are checked separately again further down (rewrite/r6_materialise_boundaries.hpp,
// r7_block_linmap.hpp) rather than folded into "never matches" here.
TEST(RuleFramework, R1R2R3DoNotMatchTheFrameworksTinyAddProgram) {
  const ir::Program program = tiny_program();
  const ir::PlanAnnotations plan;
  const rewrite::R1FoldUniformColumns r1;
  const rewrite::R2BucketRows r2;
  const rewrite::R3ElideTrivialMaps r3;
  const rewrite::Rule* rules[] = {&r1, &r2, &r3};
  for (const rewrite::Rule* rule : rules) {
    EXPECT_TRUE(rule->match(program, plan).empty()) << rule->name();
  }
  EXPECT_EQ(r1.name(), "r1.fold_uniform_columns");
  EXPECT_EQ(r1.exactness_class(), rewrite::Exactness::E0);
}

TEST(RuleFramework, R4aR4bR5DoNotMatchTheFrameworksTinyAddProgramEither) {
  const ir::Program program = tiny_program();
  const ir::PlanAnnotations plan;
  const rewrite::R4aPushUnaryThroughGathers r4a;
  const rewrite::R4bSharedReciprocal r4b;
  const rewrite::R5GroupFormation r5;
  EXPECT_TRUE(r4a.match(program, plan).empty());
  EXPECT_TRUE(r4b.match(program, plan).empty());
  EXPECT_TRUE(r5.match(program, plan).empty());
  EXPECT_EQ(r4a.name(), "r4a.push_unary_through_gathers");
  EXPECT_EQ(r4a.exactness_class(), rewrite::Exactness::E0);
  EXPECT_EQ(r4b.name(), "r4b.shared_reciprocal");
  EXPECT_EQ(r4b.exactness_class(), rewrite::Exactness::E1);  // the one E1 rewrite in DESIGN.md §6
  EXPECT_EQ(r5.name(), "r5.group_formation");
  EXPECT_EQ(r5.exactness_class(), rewrite::Exactness::E0);
}

// R6 / R7's own declared names / exactness, plus the shape of their (real, non-stub) behaviour:
// R6 fires on program_with_an_inlinable_producer() (domain 1 is read exactly once, by one
// non-reduction consumer); neither fires on tiny_program() (its domain 0 is an Input -- not "an
// intermediate" R6 will touch -- and its domain 1 is an Add, not an Affine, so R7 has no linmap
// to find). Their differential / mutation gates are tests/rewrite/r6_materialise_boundaries_e0_test.cpp
// and r7_block_linmap_e0_test.cpp.
TEST(RuleFramework, R6AndR7AreRealRulesNotStubs) {
  const ir::Program tiny = tiny_program();
  const ir::Program inlinable = program_with_an_inlinable_producer();
  const ir::PlanAnnotations plan;
  const rewrite::R6MaterialiseBoundaries r6;
  const rewrite::R7BlockLinmap r7;
  EXPECT_EQ(r6.name(), "r6.materialise_boundaries");
  EXPECT_EQ(r7.name(), "r7.block_linmap");
  EXPECT_EQ(r6.exactness_class(), rewrite::Exactness::E0);
  EXPECT_EQ(r7.exactness_class(), rewrite::Exactness::E0);
  EXPECT_TRUE(r6.match(tiny, plan).empty());
  EXPECT_FALSE(r6.match(inlinable, plan).empty());
  EXPECT_TRUE(r7.match(tiny, plan).empty());
  EXPECT_TRUE(r7.match(inlinable, plan).empty());
}


// All seven of DESIGN.md §6's rewrites are real now (R1-R3 M4/R-a, R4a/R4b/R5 M4/R-b, R6/R7
// M4/R-c): rewrite::IdentityStubRule<Tag> (stub_rule.hpp) has no rule file left using it, so this
// checks the TEMPLATE ITSELF against a throwaway Tag rather than a rule that has since been
// filled in. rule.hpp's own contract (point 2) says `propose` at a site `match` would not have
// returned is a CALLER error, not a condition `propose` is asked to handle -- a STUB's own
// identity-if-ever-called-directly guarantee (`IdentityStubRule::propose` returns the input
// Program unchanged regardless of the site) is what this pins.
namespace {
struct TestStubTag {
  static const char* rule_name() { return "test.stub_tag"; }
  static constexpr rewrite::Exactness exactness() { return rewrite::Exactness::E0; }
};
}  // namespace

TEST(RuleFramework, AStubsProposeIsIdentityIfEverCalledDirectly) {
  const ir::Program program = tiny_program();
  const rewrite::IdentityStubRule<TestStubTag> stub;
  EXPECT_TRUE(stub.match(program, ir::PlanAnnotations{}).empty());
  EXPECT_EQ(stub.name(), "test.stub_tag");
  EXPECT_EQ(stub.exactness_class(), rewrite::Exactness::E0);
  rewrite::Proposal p = stub.propose(program, ir::PlanAnnotations{}, rewrite::MatchSite::whole_program());
  ASSERT_TRUE(p.is_structural());
  EXPECT_TRUE(*p.program == program);
}
