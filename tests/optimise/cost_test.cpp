// M4/CM: unit tests for the cost model (include/epykos/optimise/cost.hpp).
//
//   * synthetic, hand-built ir::Program objects (no fixture / recording dependency) for the
//     formula-level properties the package spec names: monotone in rows and in lanes, and a
//     materialised intermediate costing more than a fused one once its region exceeds L1;
//   * the M1 book's real program (ir::infer over fixtures::record_m1) for infer_plan's own
//     rules (the M1 book has no scan, no fused/inlined domain expected at its small sizes) and
//     for the tile-sweep ordering gate, which only means something against a FITTED model for
//     THIS machine's fingerprint (D9: never compared across fingerprints) — it skips when
//     tools/costmodel/ has not written bench/results/<this fingerprint>/cost_model.json yet.
#include "epykos/optimise/cost.hpp"

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "epykos/fixtures/m1_book.hpp"
#include "epykos/fixtures/record_m1.hpp"
#include "epykos/ir/program.hpp"
#include "epykos/ir/signature.hpp"

namespace {

namespace ir = epykos::ir;
namespace optimise = epykos::optimise;
namespace fs = std::filesystem;
using epykos::Op;

// ---- a tiny hand-built Program: one Input domain (rows = n), one elementwise domain computing
// exp(x) over it (rows = n). No recording, no signature pass: just enough structure for the cost
// formulas, built directly against ir::Program's own contract (plain data, program.hpp). A
// Group's steps are the op sequence for ONE row, evaluated once per row of its domain (the
// per-row variation comes from columns / gathers / segments, never from repeating steps) — see
// program.hpp's own header comment; every hand-built Program here follows it, one step per group.
ir::Program make_exp_program(int n) {
  ir::Program p;
  ir::Domain input_dom;
  input_dom.name = "in";
  input_dom.rows = n;
  input_dom.value_base = 0;
  p.domains.push_back(input_dom);
  ir::Group input_grp;
  input_grp.domain = 0;
  {
    ir::Step st;
    st.op = Op::Input;
    st.a.kind = ir::SlotKind::Input;  // the row's own ordinal (Program::inputs), index unused
    input_grp.steps.push_back(st);
  }
  p.groups.push_back(input_grp);
  p.inputs.resize(static_cast<std::size_t>(n));
  p.input_values.assign(static_cast<std::size_t>(n), 0.0);
  for (int i = 0; i < n; ++i) p.inputs[static_cast<std::size_t>(i)] = i;

  ir::Domain exp_dom;
  exp_dom.name = "exp(gather(in))";
  exp_dom.rows = n;
  exp_dom.value_base = n;
  exp_dom.reads = {0};
  p.domains.push_back(exp_dom);
  ir::Gather g;
  g.domain = 1;
  g.index.resize(static_cast<std::size_t>(n));
  for (int i = 0; i < n; ++i) g.index[static_cast<std::size_t>(i)] = i;  // gathers domain 0's rows 1:1
  const std::int32_t gather_idx = static_cast<std::int32_t>(p.gathers.size());
  p.gathers.push_back(g);
  ir::Group exp_grp;
  exp_grp.domain = 1;
  {
    ir::Step st;
    st.op = Op::Exp;
    st.a.kind = ir::SlotKind::Gather;
    st.a.index = gather_idx;
    exp_grp.steps.push_back(st);
  }
  p.groups.push_back(exp_grp);
  p.outputs.push_back(n);  // row 0 of the exp domain is an output (keeps it from looking dead)
  return p;
}

optimise::CostModel flat_model() {
  optimise::CostModel m;
  m.coeffs = optimise::CostCoefficients::defaults();
  m.fingerprint = "test";
  return m;
}

TEST(Cost, MonotoneInRows) {
  const optimise::CostModel model = flat_model();
  const ir::Program small = make_exp_program(64);
  const ir::Program large = make_exp_program(6400);
  const optimise::Plan sp = optimise::infer_plan(small, /*tile=*/256, /*lane_tile=*/8);
  const optimise::Plan lp = optimise::infer_plan(large, /*tile=*/256, /*lane_tile=*/8);
  const optimise::ProgramCost sc = optimise::estimate_program(small, sp, /*B=*/1, model);
  const optimise::ProgramCost lc = optimise::estimate_program(large, lp, /*B=*/1, model);
  EXPECT_GT(lc.total_ns, sc.total_ns);
}

TEST(Cost, MonotoneInLanes) {
  const optimise::CostModel model = flat_model();
  const ir::Program p = make_exp_program(4096);
  const optimise::Plan plan = optimise::infer_plan(p, /*tile=*/256, /*lane_tile=*/8);
  const optimise::ProgramCost b1 = optimise::estimate_program(p, plan, /*B=*/1, model);
  const optimise::ProgramCost b8 = optimise::estimate_program(p, plan, /*B=*/8, model);
  const optimise::ProgramCost b64 = optimise::estimate_program(p, plan, /*B=*/64, model);
  EXPECT_GT(b8.total_ns, b1.total_ns);
  EXPECT_GT(b64.total_ns, b8.total_ns);
}

// A domain fed only into one Sum reduction, large enough that its region (rows x 8 bytes) clears
// the default L1 size: FusedIntoReduction (never materialised) must cost less than pricing the
// same domain as Materialized (its rows written to, and read back once by, the value buffer).
TEST(Cost, MaterialisedCostsMoreThanFusedPastL1) {
  const optimise::CostModel model = flat_model();
  ASSERT_GT(model.coeffs.l1_bytes, 0u);
  const int n = static_cast<int>(model.coeffs.l1_bytes / 8) * 4;  // 4x the L1 byte budget, in rows

  ir::Program p;
  ir::Domain producer;
  producer.rows = n;
  producer.value_base = 0;
  p.domains.push_back(producer);
  ir::Group producer_grp;
  producer_grp.domain = 0;
  {
    ir::Step st;
    st.op = Op::Const;
    st.konst.kind = ir::SlotKind::Literal;  // shared by every row (program.hpp: "literal")
    st.konst.index = 0;
    producer_grp.steps.push_back(st);
  }
  p.literals.push_back(1.0);
  p.groups.push_back(producer_grp);

  ir::Domain reducer;
  reducer.rows = 1;
  reducer.value_base = n;
  reducer.reads = {0};
  p.domains.push_back(reducer);
  ir::Segment seg;
  seg.domain = 1;
  seg.offsets = {0, n};
  for (int i = 0; i < n; ++i) seg.members.push_back(i);
  const std::int32_t seg_idx = static_cast<std::int32_t>(p.segments.size());
  p.segments.push_back(seg);
  ir::Group reducer_grp;
  reducer_grp.domain = 1;
  ir::Step sum_step;
  sum_step.op = Op::Sum;
  sum_step.a.kind = ir::SlotKind::Segment;
  sum_step.a.index = seg_idx;
  reducer_grp.steps.push_back(sum_step);
  p.groups.push_back(reducer_grp);
  p.outputs.push_back(n);  // the sum itself

  optimise::Plan materialized_plan;
  materialized_plan.tile = 256;
  materialized_plan.lane_tile = 8;
  materialized_plan.domains.resize(2);
  materialized_plan.domains[0].domain = 0;
  materialized_plan.domains[0].treatment = optimise::Treatment::Materialized;
  materialized_plan.domains[1].domain = 1;
  materialized_plan.domains[1].treatment = optimise::Treatment::Materialized;

  optimise::Plan fused_plan = optimise::infer_plan(p, /*tile=*/256, /*lane_tile=*/8);
  ASSERT_EQ(fused_plan.of(0).treatment, optimise::Treatment::FusedIntoReduction) << "infer_plan should fold the producer into the reduction";

  const optimise::ProgramCost materialized = optimise::estimate_program(p, materialized_plan, /*B=*/1, model);
  const optimise::ProgramCost fused = optimise::estimate_program(p, fused_plan, /*B=*/1, model);
  EXPECT_GT(materialized.total_ns, fused.total_ns);
}

// Regression: a whole-domain Affine producer with a UNIFORM member count per row (an
// interpolation: the same two knots read at every row) is still eligible to be inlined into its
// one consumer (exec/interpreter.cpp's decide_inline, DESIGN.md §6 R4a/R7) — it is not
// disqualified just because its own group is a reduction shape. Caught on the Stage A tape's
// interpolated-DF domain during calibration: treating it as unconditionally Materialized
// overpriced it by three orders of magnitude (a 16,917-row domain the real interpreter never
// separately dispatches at all).
TEST(Cost, UniformAffineProducerCanBeInlined) {
  const int n = 200;
  ir::Program p;
  ir::Domain input_dom;
  input_dom.rows = n;
  input_dom.value_base = 0;
  p.domains.push_back(input_dom);
  ir::Group input_grp;
  input_grp.domain = 0;
  {
    ir::Step st;
    st.op = Op::Input;
    st.a.kind = ir::SlotKind::Input;
    input_grp.steps.push_back(st);
  }
  p.groups.push_back(input_grp);
  p.inputs.resize(static_cast<std::size_t>(n));
  p.input_values.assign(static_cast<std::size_t>(n), 0.0);
  for (int i = 0; i < n; ++i) p.inputs[static_cast<std::size_t>(i)] = i;

  // Domain 1: an affine producer, exactly 2 members per row (uniform), both from domain 0.
  ir::Domain affine_dom;
  affine_dom.rows = n;
  affine_dom.value_base = n;
  affine_dom.reads = {0};
  p.domains.push_back(affine_dom);
  ir::Segment seg;
  seg.domain = 1;
  seg.coefs.assign(static_cast<std::size_t>(2 * n), 1.0);
  for (int i = 0; i < n; ++i) {
    seg.offsets.push_back(2 * i);
    seg.members.push_back(i);
    seg.members.push_back((i + 1) % n);
  }
  seg.offsets.push_back(2 * n);
  const std::int32_t seg_idx = static_cast<std::int32_t>(p.segments.size());
  p.segments.push_back(seg);
  ir::Group affine_grp;
  affine_grp.domain = 1;
  ir::Step affine_step;
  affine_step.op = Op::Affine;
  affine_step.a.kind = ir::SlotKind::Segment;
  affine_step.a.index = seg_idx;
  affine_step.konst.kind = ir::SlotKind::Literal;  // c_0 = 0.0 (program.hpp: "Affine's c_0 is konst")
  affine_step.konst.index = static_cast<std::int32_t>(p.literals.size());
  p.literals.push_back(0.0);
  affine_grp.steps.push_back(affine_step);
  p.groups.push_back(affine_grp);

  // Domain 2: gathers domain 1 1:1 and applies Exp; its row 0 is the only output.
  ir::Domain consumer_dom;
  consumer_dom.rows = n;
  consumer_dom.value_base = 2 * n;
  consumer_dom.reads = {1};
  p.domains.push_back(consumer_dom);
  ir::Gather g;
  g.domain = 2;
  for (int i = 0; i < n; ++i) g.index.push_back(n + i);  // domain 1's rows, 1:1
  const std::int32_t gather_idx = static_cast<std::int32_t>(p.gathers.size());
  p.gathers.push_back(g);
  ir::Group consumer_grp;
  consumer_grp.domain = 2;
  {
    ir::Step st;
    st.op = Op::Exp;
    st.a.kind = ir::SlotKind::Gather;
    st.a.index = gather_idx;
    consumer_grp.steps.push_back(st);
  }
  p.groups.push_back(consumer_grp);
  p.outputs.push_back(2 * n);

  // lane_tile 32: row fusion pays only at lane_tile == 1 or >= 16 (exec/interpreter.cpp's
  // row_fusion_pays; docs/RESUME.md §5 "M3 result" fusion-coverage note) — 8 (the Stage A grid's
  // own default) would NOT inline here, which is not this test's point.
  const optimise::Plan plan = optimise::infer_plan(p, /*tile=*/256, /*lane_tile=*/32);
  EXPECT_EQ(plan.of(1).treatment, optimise::Treatment::Inlined) << "a uniform-arity affine producer with one consumer should inline";
  EXPECT_EQ(plan.of(2).treatment, optimise::Treatment::Materialized);

  const optimise::CostModel model = flat_model();
  const optimise::ProgramCost pc = optimise::estimate_program(p, plan, /*B=*/1, model);
  EXPECT_DOUBLE_EQ(pc.per_domain_ns[1], 0.0) << "an inlined domain's own slot never gets a dispatch/write/read cost";
}

TEST(Cost, InferPlanOnM1BookHasNoFusionOrInlining) {
  // The M1 book (D16, DESIGN.md §5) has no scan and, at its size, no domain the planner folds
  // away either (docs/RESUME.md §5 "M1 result": its own hand-fused-kernel comparison prices
  // every domain of the generic path). infer_plan should agree: every domain Materialized.
  const epykos::fixtures::Book book = epykos::fixtures::make_m1_book();
  const epykos::Tape tape = epykos::fixtures::record_m1(book);
  const ir::Program program = ir::infer(tape);
  const optimise::Plan plan = optimise::infer_plan(program, /*tile=*/256, /*lane_tile=*/8);
  for (const optimise::DomainPlan& dp : plan.domains) {
    EXPECT_EQ(dp.treatment, optimise::Treatment::Materialized) << "domain " << dp.domain;
  }
}

TEST(Cost, JacobianModeScalesWithInputsOrOutputs) {
  const optimise::CostModel model = flat_model();
  const double one_pass = 1000.0;
  const double fwd_10 = optimise::estimate_jacobian_ns(one_pass, 10, 5, optimise::ADMode::Forward, model);
  const double fwd_20 = optimise::estimate_jacobian_ns(one_pass, 20, 5, optimise::ADMode::Forward, model);
  EXPECT_DOUBLE_EQ(fwd_20, 2.0 * fwd_10);
  const double rev_5 = optimise::estimate_jacobian_ns(one_pass, 10, 5, optimise::ADMode::Reverse, model);
  const double rev_10 = optimise::estimate_jacobian_ns(one_pass, 10, 10, optimise::ADMode::Reverse, model);
  EXPECT_DOUBLE_EQ(rev_10, 2.0 * rev_5);
  const double affine = optimise::estimate_jacobian_ns(one_pass, 10, 5, optimise::ADMode::ClosedFormAffine, model);
  EXPECT_GT(affine, 0.0);
  EXPECT_LT(affine, rev_5);  // a constant Jacobian's matrix product is cheap next to a re-evaluated one
}

TEST(Cost, CoefficientsRoundTripThroughJson) {
  const fs::path dir = fs::temp_directory_path() / "epykos_cost_model_test";
  std::error_code ec;
  fs::remove_all(dir, ec);
  optimise::CostModel written;
  written.fingerprint = "unittest000a";
  written.coeffs = optimise::CostCoefficients::defaults();
  written.coeffs.gather_ns = 3.5;
  written.coeffs.dispatch_ns = 17.25;
  written.coeffs.op_ns[0][static_cast<std::size_t>(Op::Exp)] = 4.5;
  written.save(dir.string());

  const optimise::CostModel loaded = optimise::CostModel::load("unittest000a", dir.string());
  EXPECT_TRUE(loaded.loaded_from_file);
  EXPECT_DOUBLE_EQ(loaded.coeffs.gather_ns, 3.5);
  EXPECT_DOUBLE_EQ(loaded.coeffs.dispatch_ns, 17.25);
  EXPECT_DOUBLE_EQ(loaded.coeffs.op_ns[0][static_cast<std::size_t>(Op::Exp)], 4.5);
  fs::remove_all(dir, ec);
}

TEST(Cost, LoadOrDefaultFallsBackSilentlyEnoughToWarn) {
  const optimise::CostModel m = optimise::CostModel::load_or_default("no-such-fingerprint-ever", "/nonexistent/dir/for/sure");
  EXPECT_FALSE(m.loaded_from_file);
  EXPECT_DOUBLE_EQ(m.coeffs.dispatch_ns, optimise::CostCoefficients::defaults().dispatch_ns);
}

// scripts/fingerprint.sh --id, or "" if it cannot be run (mirrors tests/scripts/perf_gate_test.cpp's
// popen pattern). The tile-sweep and lane-width gates below only mean something for THIS
// fingerprint's own fitted coefficients (D9): they skip when tools/costmodel/ has not written one.
fs::path repo_root() {
  if (const char* e = std::getenv("EPYKOS_SOURCE_DIR")) return fs::path(e);
  return fs::path(__FILE__).parent_path().parent_path().parent_path();
}

std::string shell_quote(const std::string& s) {  // single-quote for the shell (paths may contain spaces)
  std::string r = "'";
  for (char c : s) {
    if (c == '\'') r += "'\\''";
    else r += c;
  }
  return r + "'";
}

std::string current_fingerprint_id() {
  const fs::path script = repo_root() / "scripts" / "fingerprint.sh";
  if (!fs::exists(script)) return "";
  const std::string cmd = shell_quote(script.string()) + " --id 2>/dev/null";
  FILE* p = popen(cmd.c_str(), "r");
  if (p == nullptr) return "";
  std::string out;
  char buf[256];
  while (std::size_t n = std::fread(buf, 1, sizeof buf, p)) out.append(buf, n);
  pclose(p);
  while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ')) out.pop_back();
  return out;
}

TEST(Cost, FittedModelReproducesM1TileSweepOrdering) {
  const std::string fp = current_fingerprint_id();
  if (fp.empty()) GTEST_SKIP() << "could not determine this machine's fingerprint";
  optimise::CostModel model;
  try {
    model = optimise::CostModel::load(fp, (repo_root() / "bench" / "results").string());
  } catch (const std::exception&) {
    GTEST_SKIP() << "no fitted bench/results/" << fp << "/cost_model.json yet (run tools/costmodel/)";
  }
  const epykos::fixtures::Book book = epykos::fixtures::make_m1_book();
  const epykos::Tape tape = epykos::fixtures::record_m1(book);
  const ir::Program program = ir::infer(tape);

  const optimise::ProgramCost t128 = optimise::estimate_program(program, /*B=*/1, /*tile=*/128, /*lane_tile=*/8, model);
  const optimise::ProgramCost t256 = optimise::estimate_program(program, /*B=*/1, /*tile=*/256, /*lane_tile=*/8, model);
  const optimise::ProgramCost t512 = optimise::estimate_program(program, /*B=*/1, /*tile=*/512, /*lane_tile=*/8, model);
  EXPECT_GT(t128.total_ns, t256.total_ns) << "M3 result: tile 128 < 256 < 512 (faster as tile grows) at B=1";
  EXPECT_GT(t256.total_ns, t512.total_ns);

  double best = -1.0;
  int best_lane = -1;
  for (int lane_tile : {1, 8, 16, 32, 64}) {
    const optimise::ProgramCost c = optimise::estimate_program(program, /*B=*/64, /*tile=*/256, lane_tile, model);
    if (best < 0.0 || c.total_ns < best) {
      best = c.total_ns;
      best_lane = lane_tile;
    }
  }
  EXPECT_EQ(best_lane, 32) << "M3 result: lane tile 32 best at B=64";
}

}  // namespace
