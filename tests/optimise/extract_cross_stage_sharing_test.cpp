// M4/EG "integration" gate: rewrite::cross_stage_sharing_guard (rewrite/cross_stage_sharing.hpp)
// wired onto optimise::ExtractOptions::reject (D53) — PROBLEM.md §7's "a rewrite that would split
// [the residual's and the book's shared DF domain] is rejected or costed": this file proves the
// "rejected" half end to end, extract.hpp included, not just ir::sharing in isolation (already
// covered by tests/ir/sharing_test.cpp).
//
// Two small hand-built programs (the tests/optimise/egraph_test.cpp "just enough for
// ir::validate" convention): `shared_program` has ONE Exp domain ("the DF domain") read directly
// by output 0 ("the residual") and indirectly, through an Add domain, by output 1 ("the book") —
// sharing intact. `split_program` duplicates that Exp domain so output 0 and output 1 each read
// their OWN copy — sharing broken. A test-only Rule proposes the split (unconditionally, so the
// test is about extract()'s own guard wiring, not about when a rule fires) and the two output-
// ordinal groups {0}, {1} are defined ONCE, against `shared_program`, and reused unchanged
// against the split candidate — exactly this package's own point: a group defined by output
// ordinal stays meaningful across a rewrite that renumbers domains and values.
#include <gtest/gtest.h>

#include <vector>

#include "epykos/ir/program.hpp"
#include "epykos/optimise/cost.hpp"
#include "epykos/optimise/egraph.hpp"
#include "epykos/optimise/extract.hpp"
#include "epykos/rewrite/cross_stage_sharing.hpp"
#include "epykos/rewrite/rule.hpp"

namespace ir = epykos::ir;
namespace rewrite = epykos::rewrite;
namespace optimise = epykos::optimise;
using epykos::Op;

namespace {

ir::Program shared_program() {
  ir::Program p;
  p.domains.push_back(ir::Domain{"input", 2, 0, 0, false, {}, false, -1});
  p.groups.push_back(ir::Group{0, {ir::Step{Op::Input, ir::Slot{ir::SlotKind::Input, -1}, {}, {}, {}}}});
  p.inputs = {0, 1};
  p.input_values = {0.1, 0.2};

  p.gathers.push_back(ir::Gather{1, {0}});
  p.domains.push_back(ir::Domain{"exp(@0)", 1, 2, 0, false, {0}, false, -1});
  p.groups.push_back(ir::Group{1, {ir::Step{Op::Exp, ir::Slot{ir::SlotKind::Gather, 0}, {}, {}, {}}}});

  p.gathers.push_back(ir::Gather{2, {2}});
  p.gathers.push_back(ir::Gather{2, {1}});
  p.domains.push_back(ir::Domain{"add(@1,@2)", 1, 3, 0, false, {0, 1}, false, -1});
  p.groups.push_back(ir::Group{2, {ir::Step{Op::Add, ir::Slot{ir::SlotKind::Gather, 1}, ir::Slot{ir::SlotKind::Gather, 2}, {}, {}}}});

  p.outputs = {2, 3};  // output 0: the DF domain directly ("the residual"); output 1: through Add ("the book")
  ir::validate(p);
  return p;
}

// The same problem, with the DF domain duplicated so output 0 and output 1 read disjoint copies.
ir::Program split_program() {
  ir::Program p;
  p.domains.push_back(ir::Domain{"input", 2, 0, 0, false, {}, false, -1});
  p.groups.push_back(ir::Group{0, {ir::Step{Op::Input, ir::Slot{ir::SlotKind::Input, -1}, {}, {}, {}}}});
  p.inputs = {0, 1};
  p.input_values = {0.1, 0.2};

  p.gathers.push_back(ir::Gather{1, {0}});
  p.domains.push_back(ir::Domain{"exp(@0)", 1, 2, 0, false, {0}, false, -1});
  p.groups.push_back(ir::Group{1, {ir::Step{Op::Exp, ir::Slot{ir::SlotKind::Gather, 0}, {}, {}, {}}}});

  p.gathers.push_back(ir::Gather{2, {0}});
  p.domains.push_back(ir::Domain{"exp(@0)_dup", 1, 3, 0, false, {0}, false, -1});
  p.groups.push_back(ir::Group{2, {ir::Step{Op::Exp, ir::Slot{ir::SlotKind::Gather, 1}, {}, {}, {}}}});

  p.gathers.push_back(ir::Gather{3, {3}});
  p.gathers.push_back(ir::Gather{3, {1}});
  p.domains.push_back(ir::Domain{"add(@2,@3)", 1, 4, 0, false, {0, 2}, false, -1});
  p.groups.push_back(ir::Group{3, {ir::Step{Op::Add, ir::Slot{ir::SlotKind::Gather, 2}, ir::Slot{ir::SlotKind::Gather, 3}, {}, {}}}});

  p.outputs = {2, 4};
  ir::validate(p);
  return p;
}

std::vector<std::vector<int>> groups() { return {{0}, {1}}; }  // {residual output}, {book output}

class UnconditionallySplitRule final : public rewrite::Rule {
 public:
  UnconditionallySplitRule(ir::Program from, ir::Program to) : from_(std::move(from)), to_(std::move(to)) {}
  const std::string& name() const noexcept override {
    static const std::string n = "test.unconditionally_split";
    return n;
  }
  rewrite::Exactness exactness_class() const noexcept override { return rewrite::Exactness::E0; }
  std::vector<rewrite::MatchSite> match(const ir::Program& program, const ir::PlanAnnotations&) const override {
    if (program == from_) return {rewrite::MatchSite::whole_program()};
    return {};
  }
  rewrite::Proposal propose(const ir::Program&, const ir::PlanAnnotations&, const rewrite::MatchSite&) const override {
    rewrite::Proposal p;
    p.program = to_;
    return p;
  }

 private:
  ir::Program from_, to_;
};

optimise::CostModel test_model() { return optimise::CostModel{optimise::CostCoefficients::defaults(), "test", false}; }

}  // namespace

TEST(CrossStageSharingGuard, AcceptsTheSharedProgramAndRejectsTheSplitOne) {
  const auto reject = rewrite::cross_stage_sharing_guard(groups());
  EXPECT_FALSE(reject(shared_program()));
  EXPECT_TRUE(reject(split_program()));
}

TEST(ExtractWithSharingGuard, RejectsTheSplitCandidateAndStillExtractsTheSharedOne) {
  const ir::Program shared = shared_program();
  const ir::Program split = split_program();
  UnconditionallySplitRule rule(shared, split);

  optimise::EGraph graph(shared);
  const optimise::SaturationReport report = graph.saturate({&rule});
  ASSERT_FALSE(report.bound_hit) << report.bound_reason;
  ASSERT_EQ(graph.num_programs(), 2);

  const optimise::CostModel model = test_model();

  optimise::ExtractOptions unguarded;
  const optimise::ExtractResult without_guard = optimise::extract(graph, model, unguarded);
  ASSERT_TRUE(without_guard.found);
  EXPECT_EQ(without_guard.candidates_considered, 2u) << "both the shared and the split program are real candidates without a guard";
  EXPECT_EQ(without_guard.candidates_rejected_guard, 0u);

  optimise::ExtractOptions guarded;
  guarded.reject = rewrite::cross_stage_sharing_guard(groups());
  const optimise::ExtractResult with_guard = optimise::extract(graph, model, guarded);
  ASSERT_TRUE(with_guard.found);
  EXPECT_EQ(with_guard.program, shared) << "the split candidate must never be extractable once the guard is set";
  EXPECT_EQ(with_guard.candidates_considered, 1u);
  EXPECT_EQ(with_guard.candidates_rejected_guard, 1u);
}
