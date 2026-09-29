// D63's gate: STEP PAIRING HAS A PRICE, AND EXTRACTION PAYS IT.
//
// The defect this file exists to make impossible to reintroduce (M4's single binding constraint,
// D59/D62): `optimise::Plan` priced per-domain MATERIALISATION only, so `ir::PlanAnnotations::
// group` — exec::Interpreter's fused pairs and chain tails, the interpreter's own core fusion
// mechanism and the thing R1-R7 and the five planner rules exist to decide — was worth exactly
// zero to the cost model. Every fusion candidate therefore tied EXACTLY with its unfused
// counterpart, and extraction's deterministic ascending-id tie-break then kept whichever came
// first, which on the M1 book was the plan with NO pairings at all (measured 1.081x-1.104x slower
// in wall clock while the cost model reported a ratio of exactly 1.0). The search was hunting for
// fusion wins with a model that could not see fusion.
//
// Everything here runs against `CostCoefficients::defaults()`, never a fitted file: the property
// gated is an ORDERING that must hold for any non-degenerate coefficients, not a number for one
// machine (D9 — a fingerprint's fitted model is never the subject of a ctest gate).
//
// A `*_verify_test` so scripts/mutation_test.sh's own gate regex selects it (D62's
// egraph_saturation_memo_verify_test set the precedent for a search/estimation-level gate). Its
// two registered mutants, `cost.pairing_unpriced` and `plan_bridge.discards_group`, are the
// defect itself in each of the two places it lived.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "epykos/ir/annotate.hpp"
#include "epykos/ir/program.hpp"
#include "epykos/optimise/cost.hpp"
#include "epykos/optimise/egraph.hpp"
#include "epykos/optimise/extract.hpp"
#include "epykos/optimise/plan_bridge.hpp"
#include "epykos/rewrite/planner.hpp"
#include "epykos/rewrite/planner_rules.hpp"
#include "epykos/rewrite/rule.hpp"

namespace ir = epykos::ir;
namespace optimise = epykos::optimise;
namespace rewrite = epykos::rewrite;
using epykos::Op;

namespace {

constexpr int kRows = 4096;   // several tiles at the default tile of 256, so dispatch counts matter
constexpr int kTile = 256;
constexpr int kLaneTile = 8;

// A program with BOTH halves of the decision in it:
//   d0  an Input domain (kRows rows);
//   d1  elementwise over a 1:1 gather of d0, TWO chained steps — `mul(gather, literal)` then
//       `add(step0, literal)` — which is exactly rewrite::planner::match_pair's shape (step k is
//       a fixed-arity arithmetic op with at least one gather; step k+1 reads step k and one
//       scalar; nothing else reads step k). Every row is an output, so no rule folds the domain
//       away and its pairing actually reaches the price.
//   d2  elementwise `const` rows feeding, and read ONLY by,
//   d3  a whole-domain Sum over them (one row, an output) — so planner.reduction_fusion has
//       something to decide and the e-graph's plan tier contains a NON-EMPTY annotation that
//       says nothing about `group`. That candidate is the one the old code tied with the paired
//       plan, and the one exec::Interpreter really would run one-kernel-per-step.
ir::Program paired_chain_program() {
  ir::Program p;
  p.literals = {1.5, 0.25, 2.0};

  ir::Domain in;
  in.name = "in";
  in.rows = kRows;
  in.value_base = 0;
  p.domains.push_back(in);
  ir::Group in_grp;
  in_grp.domain = 0;
  {
    ir::Step st;
    st.op = Op::Input;
    st.a.kind = ir::SlotKind::Input;
    in_grp.steps.push_back(st);
  }
  p.groups.push_back(in_grp);
  p.inputs.resize(static_cast<std::size_t>(kRows));
  p.input_values.assign(static_cast<std::size_t>(kRows), 1.0);
  for (int i = 0; i < kRows; ++i) p.inputs[static_cast<std::size_t>(i)] = i;

  ir::Domain chain;
  chain.name = "add(mul(gat,lit),lit)";
  chain.rows = kRows;
  chain.value_base = kRows;
  chain.reads = {0};
  p.domains.push_back(chain);
  ir::Gather g;
  g.domain = 1;
  g.index.resize(static_cast<std::size_t>(kRows));
  for (int i = 0; i < kRows; ++i) g.index[static_cast<std::size_t>(i)] = i;
  const std::int32_t gather_idx = static_cast<std::int32_t>(p.gathers.size());
  p.gathers.push_back(g);
  ir::Group chain_grp;
  chain_grp.domain = 1;
  {
    ir::Step mul;
    mul.op = Op::Mul;
    mul.a.kind = ir::SlotKind::Gather;
    mul.a.index = gather_idx;
    mul.b.kind = ir::SlotKind::Literal;
    mul.b.index = 0;
    chain_grp.steps.push_back(mul);
    ir::Step add;
    add.op = Op::Add;
    add.a.kind = ir::SlotKind::Step;
    add.a.index = 0;
    add.b.kind = ir::SlotKind::Literal;
    add.b.index = 1;
    chain_grp.steps.push_back(add);
  }
  p.groups.push_back(chain_grp);
  for (int i = 0; i < kRows; ++i) p.outputs.push_back(kRows + i);

  ir::Domain members;
  members.name = "const";
  members.rows = kRows;
  members.value_base = 2 * kRows;
  p.domains.push_back(members);
  ir::Group members_grp;
  members_grp.domain = 2;
  {
    ir::Step st;
    st.op = Op::Const;
    st.konst.kind = ir::SlotKind::Literal;
    st.konst.index = 2;
    members_grp.steps.push_back(st);
  }
  p.groups.push_back(members_grp);

  ir::Domain reducer;
  reducer.name = "sum";
  reducer.rows = 1;
  reducer.value_base = 3 * kRows;
  reducer.reads = {2};
  p.domains.push_back(reducer);
  ir::Segment seg;
  seg.domain = 3;
  seg.offsets = {0, kRows};
  for (int i = 0; i < kRows; ++i) seg.members.push_back(2 * kRows + i);
  const std::int32_t seg_idx = static_cast<std::int32_t>(p.segments.size());
  p.segments.push_back(seg);
  ir::Group reducer_grp;
  reducer_grp.domain = 3;
  {
    ir::Step st;
    st.op = Op::Sum;
    st.a.kind = ir::SlotKind::Segment;
    st.a.index = seg_idx;
    reducer_grp.steps.push_back(st);
  }
  p.groups.push_back(reducer_grp);
  p.outputs.push_back(3 * kRows);
  return p;
}

optimise::CostModel default_model() { return optimise::CostModel{optimise::CostCoefficients::defaults(), "test", false}; }

std::size_t total_pairings(const optimise::Plan& plan) {
  std::size_t n = 0;
  for (const optimise::DomainPlan& dp : plan.domains) n += dp.pairings.size();
  return n;
}

std::size_t total_pairings(const ir::PlanAnnotations& plan) {
  std::size_t n = 0;
  for (const ir::GroupPlan& g : plan.group) n += g.pairings.size();
  return n;
}

double cost_of(const ir::Program& program, const optimise::Plan& plan) {
  return optimise::estimate_program(program, plan, /*B=*/1, default_model()).total_ns;
}

}  // namespace

// ---- the structural facts the price is built on ------------------------------------------------

TEST(CostStepPairingVerify, InferPlanFindsThePlannersOwnPairingAndCollapsesItsKernelCalls) {
  const ir::Program program = paired_chain_program();
  const optimise::Plan plan = optimise::infer_plan(program, kTile, kLaneTile);

  // infer_plan does not reproduce the interpreter's pairing rule, it CALLS it
  // (rewrite::planner::fused_pairs_plan) — so this must agree with the planner exactly.
  const ir::PlanAnnotations planner_pairs = rewrite::planner::fused_pairs_plan(program);
  ASSERT_EQ(total_pairings(planner_pairs), 1u) << "fixture no longer matches rewrite::planner::match_pair's shape";
  EXPECT_EQ(total_pairings(plan), 1u);
  EXPECT_EQ(plan.of(1).pairings.front().first, 0);
  EXPECT_EQ(plan.of(1).pairings.front().second, 1);

  // Two IR steps, one kernel call; step 0's value never leaves a register.
  EXPECT_EQ(optimise::kernel_calls(program, plan, 1), 1u);
  const std::vector<std::uint8_t> internal = optimise::internal_steps(program.groups[1], plan.of(1).pairings);
  ASSERT_EQ(internal.size(), 2u);
  EXPECT_EQ(internal[0], 1);
  EXPECT_EQ(internal[1], 0);

  optimise::Plan unpaired = plan;
  for (optimise::DomainPlan& dp : unpaired.domains) dp.pairings.clear();
  EXPECT_EQ(optimise::kernel_calls(program, unpaired, 1), 2u);
}

// ---- THE bug: a program differing ONLY in step pairing must price differently, and cheaper ----

TEST(CostStepPairingVerify, PairingIsStrictlyCheaperThanTheIdenticalUnpairedPlan) {
  const ir::Program program = paired_chain_program();
  const optimise::Plan paired = optimise::infer_plan(program, kTile, kLaneTile);
  optimise::Plan unpaired = paired;
  for (optimise::DomainPlan& dp : unpaired.domains) dp.pairings.clear();
  ASSERT_EQ(total_pairings(paired), 1u);
  ASSERT_EQ(total_pairings(unpaired), 0u);

  const double paired_ns = cost_of(program, paired);
  const double unpaired_ns = cost_of(program, unpaired);
  EXPECT_LT(paired_ns, unpaired_ns) << "two plans differing ONLY in ir::StepPairing priced the same: " << paired_ns << " vs "
                                    << unpaired_ns << " ns. That is the D63 defect (pairing free to extraction).";

  // And the saving is in the two places the interpreter really makes it: one kernel dispatch per
  // tile, and the store plus reload of the intermediate.
  const std::vector<optimise::DomainFacts> facts = optimise::analyze(program);
  const optimise::DomainCost paired_d = optimise::estimate_domain(program, facts, paired, 1, /*L=*/1, default_model());
  const optimise::DomainCost unpaired_d = optimise::estimate_domain(program, facts, unpaired, 1, /*L=*/1, default_model());
  EXPECT_LT(paired_d.dispatch_ns, unpaired_d.dispatch_ns);
  EXPECT_LT(paired_d.intermediate_ns, unpaired_d.intermediate_ns);
  EXPECT_DOUBLE_EQ(paired_d.op_ns, unpaired_d.op_ns) << "pairing must not change the arithmetic priced";
  EXPECT_DOUBLE_EQ(paired_d.write_ns, unpaired_d.write_ns);
}

// ---- the bridge: ir::PlanAnnotations::group reaches the price ---------------------------------

TEST(CostStepPairingVerify, PlanBridgeTranslatesGroupAndModelsWhatTheInterpreterWouldRun) {
  const ir::Program program = paired_chain_program();
  const std::vector<optimise::DomainFacts> facts = optimise::analyze(program);

  // (a) an annotation that NAMES the pairings -> they are priced.
  const ir::PlanAnnotations with_group = rewrite::planner::fused_pairs_plan(program);
  const optimise::Plan bridged_group = optimise::plan_from_annotations(program, facts, with_group, kTile, kLaneTile);
  EXPECT_EQ(total_pairings(bridged_group), 1u);

  // (b) a NON-EMPTY annotation that does not mention `group` -> no pairings, because
  //     exec::Interpreter's build_group finds none in it and runs one kernel per step. This is
  //     experiment 1's extracted plan exactly (`planner.reduction_fusion` alone).
  ir::PlanAnnotations domain_only;
  domain_only.domain.assign(program.domains.size(), ir::DomainPlan{});
  domain_only.domain[2].choice = ir::Materialise::FuseIntoReduction;
  ASSERT_FALSE(domain_only.empty());
  const optimise::Plan bridged_domain_only = optimise::plan_from_annotations(program, facts, domain_only, kTile, kLaneTile);
  EXPECT_EQ(total_pairings(bridged_domain_only), 0u);

  // (c) the EMPTY annotation ("nothing decided") -> infer_plan's, because that is what
  //     Interpreter::Impl::build_plan derives for itself in that case.
  const optimise::Plan bridged_empty = optimise::plan_from_annotations(program, facts, ir::PlanAnnotations{}, kTile, kLaneTile);
  EXPECT_EQ(total_pairings(bridged_empty), 1u);

  // (d) and the direction, at the level the extractor actually compares: the same materialisation
  //     decision, plus the pairings, is strictly cheaper than the same decision without them.
  ir::PlanAnnotations domain_and_group = domain_only;
  domain_and_group.group = with_group.group;
  const optimise::Plan bridged_both = optimise::plan_from_annotations(program, facts, domain_and_group, kTile, kLaneTile);
  ASSERT_EQ(total_pairings(bridged_both), 1u);
  EXPECT_LT(cost_of(program, bridged_both), cost_of(program, bridged_domain_only));
}

// ---- extraction: the cheapest candidate is a paired one ---------------------------------------

TEST(CostStepPairingVerify, ExtractionPrefersThePairedCandidateOverEveryUnpairedOne) {
  const ir::Program program = paired_chain_program();

  // Just the two planner rules this gate is about, so the plan tier is small and the test runs in
  // milliseconds (it runs once per registered mutant): reduction_fusion produces the non-empty
  // annotation with no `group`, fused_pairs produces the one with it, and saturation composes
  // both.
  const rewrite::planner::ReductionFusionRule reduction_fusion(kLaneTile);
  const rewrite::planner::FusedPairsRule fused_pairs;
  const std::vector<const rewrite::Rule*> rules = {&reduction_fusion, &fused_pairs};

  optimise::EGraph graph(program);
  const optimise::SaturationReport report =
      graph.saturate(rules, optimise::SaturationLimits{/*max_iterations=*/8, /*max_plan_nodes=*/256, /*max_program_nodes=*/16});
  ASSERT_FALSE(report.bound_hit) << report.bound_reason;

  const optimise::CostModel model = default_model();
  optimise::ExtractOptions options;
  options.max_exactness = rewrite::Exactness::E0;
  options.B = 1;
  options.tile = kTile;
  options.lane_tile = kLaneTile;
  const optimise::ExtractResult result = optimise::extract(graph, model, options);
  ASSERT_TRUE(result.found);

  // What the interpreter would really run for the extracted annotation must be paired.
  const optimise::Plan extracted = optimise::plan_from_annotations(result.program, result.plan, options.tile, options.lane_tile);
  EXPECT_EQ(total_pairings(extracted), 1u) << "extraction chose a plan exec::Interpreter would run one-kernel-per-step";

  // Stronger, and independent of extraction's tie-break: EVERY unpaired candidate in the graph is
  // strictly more expensive than the cheapest paired one. Under the defect they all tie exactly,
  // so this is the assertion that cannot be satisfied by luck.
  double best_paired = std::numeric_limits<double>::infinity();
  double worst_unpaired = -1.0;
  std::size_t n_paired = 0, n_unpaired = 0;
  for (int program_id : graph.program_class_representatives()) {
    const optimise::ProgramNode& pnode = graph.program_node(program_id);
    const std::vector<optimise::DomainFacts> facts = optimise::analyze(pnode.program);
    for (int plan_id : graph.plan_class_representatives(program_id)) {
      const optimise::PlanNode& plnode = graph.plan_node(program_id, plan_id);
      const optimise::Plan plan = optimise::plan_from_annotations(pnode.program, facts, plnode.content, options.tile, options.lane_tile);
      const double ns = optimise::estimate_program(pnode.program, plan, options.B, model).total_ns;
      if (total_pairings(plan) > 0) {
        best_paired = std::min(best_paired, ns);
        ++n_paired;
      } else {
        worst_unpaired = std::max(worst_unpaired, ns);
        ++n_unpaired;
      }
    }
  }
  ASSERT_GT(n_paired, 0u);
  ASSERT_GT(n_unpaired, 0u) << "the fixture no longer produces an unpaired candidate: nothing is being gated";
  EXPECT_LT(best_paired, worst_unpaired) << "no unpaired candidate costs more than the cheapest paired one — pairing is still free";
  EXPECT_DOUBLE_EQ(result.estimated_ns, best_paired);
}
