// M3/G5 gate: the Stage A desk problem recorded as ONE tape (fixtures/stage_a.hpp; PROBLEM.md
// sections 4 Stage A and 5) — it records with no RecordError, every block's record-time solve
// converges (O1 ‖Jᵀr‖∞ < 1e-12), the domain IR round-trips to the recording node for node after
// the E0 passes (the evaluator bitwise the replay), the cross-stage sharing holds (every DF
// domain feeds both the residuals and the book, no DF computed twice; the domain table is
// reported), the compounding is laid out as scan domains, and the tape node count, IR domain
// count, scan domain count and record time are reported. A reduced book round-trips raw too and
// its program serialises. A gate of scripts/mutation_test.sh (the name matches "roundtrip").
#include <gtest/gtest.h>

#include <cstddef>
#include <iostream>
#include <string>
#include <vector>

#include "epykos/ir/evaluate.hpp"
#include "epykos/ir/expand.hpp"
#include "epykos/ir/program.hpp"
#include "epykos/ir/sharing.hpp"
#include "epykos/ir/signature.hpp"
#include "epykos/tape/replay.hpp"
#include "stage_a/stage_a_test_helpers.hpp"

namespace fixtures = epykos::fixtures;
namespace ir = epykos::ir;
namespace solver = epykos::solver;
using epykos::Tape;
using epykos::test::bits;
using epykos::test::stage_a;
using epykos::test::stage_a_tape;

namespace {

ir::Program expect_roundtrip(const Tape& tape, const std::string& what, ir::InferStats* stats_out = nullptr) {
  ir::InferStats stats;
  const epykos::test::clock_type::time_point t0 = epykos::test::clock_type::now();
  const ir::Program program = ir::infer(tape, &stats);
  const double t_infer = epykos::test::seconds_since(t0);
  EXPECT_NO_THROW(ir::validate(program)) << what;
  std::cout << "[  infer   ] " << what << ": " << t_infer << " s; nodes " << stats.nodes << ", rows " << stats.boundaries << ", classes " << stats.classes
            << ", domains " << stats.domains << ", chains " << stats.chains << " (" << stats.chain_nodes << " steps), scan classes " << stats.scan_classes
            << ", rounds " << stats.scan_rounds << (stats.scan_retries.empty() ? "" : "; retries:" + stats.scan_retries) << '\n';
  const Tape expanded = ir::expand(program);
  EXPECT_EQ(expanded.size(), tape.size()) << what;
  std::string diff;
  EXPECT_TRUE(ir::roundtrip_identical(tape, expanded, &diff)) << what << ": expanded tape differs from the recording:\n" << diff;
  EXPECT_EQ(expanded.op_histogram(), tape.op_histogram()) << what;
  const std::vector<double> z = tape.input_values();
  const std::vector<double> a = epykos::replay(tape, z);
  const std::vector<double> b = ir::evaluate(program, z);
  const std::vector<double> c = epykos::replay(expanded, z);
  EXPECT_EQ(a.size(), b.size());
  std::size_t mismatches = 0;
  for (std::size_t k = 0; k < a.size() && k < b.size() && k < c.size(); ++k) {
    if (bits(a[k]) != bits(b[k]) || bits(a[k]) != bits(c[k])) {
      if (mismatches < 5) ADD_FAILURE() << what << ": output " << k << ": replay " << a[k] << ", evaluator " << b[k] << ", expanded replay " << c[k];
      ++mismatches;
    }
  }
  EXPECT_EQ(mismatches, 0u) << what;
  if (stats_out != nullptr) *stats_out = stats;
  return program;
}

const ir::Program& program() {
  static const ir::Program p = expect_roundtrip(stage_a_tape().tape, "stage A (passes)");
  return p;
}

}  // namespace

TEST(StageARoundtrip, RecordsAsOneTapeAndEveryBlockConverges) {
  const fixtures::StageA& s = stage_a();
  const fixtures::StageATape& t = stage_a_tape();
  EXPECT_EQ(s.n_trades(), s.def.book.trades);
  EXPECT_EQ(s.n_scenarios(), s.def.scenarios.count);
  EXPECT_EQ(t.n_blocks(), s.n_curves()) << "sequential mode: one block per curve (no cycle among the Stage A curves)";
  for (std::size_t k = 0; k < t.record_reports.size(); ++k) {
    const solver::SolveReport& r = t.record_reports[k];
    EXPECT_TRUE(r.converged) << "block " << k;
    EXPECT_LT(r.jtr_inf, 1e-12) << "block " << k << " (PROBLEM.md section 6 O1)";
    EXPECT_LT(r.residual_inf, s.options.solve_tol) << "block " << k;
  }
  // The dependency order discovered from the maths: SOFR alone; €STR before the 6M curve before the 3M curve.
  const int sofr = s.slots.slot("USD-SOFR"), estr = s.slots.slot("EUR-ESTR"), e3 = s.slots.slot("EUR-EURIBOR-3M"), e6 = s.slots.slot("EUR-EURIBOR-6M");
  auto block_of = [&](int slot) {
    for (std::size_t k = 0; k < t.block_curves.size(); ++k) {
      for (int c : t.block_curves[k]) {
        if (c == slot) return static_cast<int>(k);
      }
    }
    return -1;
  };
  EXPECT_GE(block_of(sofr), 0);
  EXPECT_LT(block_of(estr), block_of(e6));
  EXPECT_LT(block_of(e6), block_of(e3));
  // Inputs: the quotes are the free inputs; the layout covers every output.
  EXPECT_EQ(t.quote_inputs.size(), static_cast<std::size_t>(s.n_quotes()));
  EXPECT_EQ(t.registry.free_inputs(static_cast<int>(t.tape.num_inputs())), t.quote_inputs);
  const std::size_t expected_outputs = static_cast<std::size_t>(s.n_quotes()) + 2u * t.registry.blocks.size() + static_cast<std::size_t>(s.n_quotes()) /* knots == instruments */ +
                                       4u * static_cast<std::size_t>(s.n_trades()) + static_cast<std::size_t>(s.n_currencies() + s.n_netting_sets() + 1) +
                                       static_cast<std::size_t>(t.layout.selects.n_outputs());
  EXPECT_EQ(t.tape.num_outputs(), expected_outputs);
  EXPECT_EQ(t.layout.n_outputs, static_cast<int>(t.tape.num_outputs()));
  EXPECT_EQ(t.layout.selects.n(), 0) << "the base problem (linear zero curves) records no Select";
  EXPECT_GT(t.stats.nodes_after_passes, 100000u);
  std::cout << "[  tape    ] nodes raw " << t.stats.nodes_raw << " -> " << t.stats.nodes_after_passes << " after the passes; record " << t.stats.seconds_total
            << " s (dependencies " << t.stats.seconds_dependencies << ", calibrate " << t.stats.seconds_calibrate << ", book " << t.stats.seconds_book
            << ", passes " << t.stats.seconds_passes << "); inputs " << t.tape.num_inputs() << " (" << s.n_quotes() << " quotes), outputs "
            << t.tape.num_outputs() << '\n';
  std::cout << "[  book    ] record point: book " << t.record_book.book_total << " (" << s.def.reporting_currency << "), currencies";
  for (int c = 0; c < s.n_currencies(); ++c) std::cout << ' ' << s.def.currencies[static_cast<std::size_t>(c)] << ' ' << t.record_book.currency_total[static_cast<std::size_t>(c)];
  std::cout << '\n';
}

TEST(StageARoundtrip, RoundTripIdentityAfterThePasses) {
  const ir::Program& p = program();
  const fixtures::StageAStructure st = fixtures::stage_a_structure(p);
  std::cout << "[  struct  ] " << st.to_string() << '\n';
  EXPECT_GE(st.scan_domains, 1u) << "the compounded coupons must be laid out as scan domains";
  EXPECT_GT(st.chains, 500u);
  EXPECT_GT(st.scan_rows, 100000u);
  EXPECT_EQ(st.df_domains, 2u) << "the interpolated and the knot-time DF buckets of D22 / D40";
  EXPECT_LT(st.domains, 200u) << "the scan class must not have been split by level";
  std::cout << ir::to_string(p);
}

TEST(StageARoundtrip, SharingGateOverResidualsAndBook) {
  const ir::Program& p = program();
  const fixtures::StageATape& t = stage_a_tape();
  const ir::SharingReport rep = ir::sharing(p, t.sharing_groups(), epykos::Op::Exp);
  std::cout << "[  sharing ] DF domains: " << rep.matching.size() << " (shared " << rep.shared.size() << ", partial " << rep.partial.size() << ", unshared "
            << rep.unshared.size() << ")\n";
  for (ir::domain_id d : rep.matching) {
    const ir::SharingReport::Row& row = rep.rows[static_cast<std::size_t>(d)];
    std::cout << "[  sharing ]   domain " << d << ' ' << row.name << ": " << row.rows << " rows, " << row.n_readers << " reader domains, groups mask " << row.groups << '\n';
  }
  std::string why;
  EXPECT_TRUE(ir::assert_all_shared(rep, &why)) << why;
  EXPECT_EQ(rep.matching.size(), 2u);
  EXPECT_EQ(ir::duplicates(p, epykos::Op::Exp), 0u) << "a discount factor computed twice";
  // Every output group is reached from the inputs domain; the reach masks over all four groups.
  const std::vector<std::vector<int>> groups = t.output_groups();
  const std::vector<std::uint32_t> mask = ir::reach(p, groups);
  const ir::domain_id input_domain = p.domain_of(p.inputs[0]);
  EXPECT_EQ(mask[static_cast<std::size_t>(input_domain)], (1u << groups.size()) - 1u);
}

TEST(StageARoundtrip, ReducedBookRoundTripsRawAndSerialises) {
  fixtures::StageAOptions o;
  o.trades = 120;
  o.scenarios = 4;
  const fixtures::StageA s = fixtures::make_stage_a(o);
  ir::InferStats raw_stats, pass_stats;
  const fixtures::StageATape raw = fixtures::record_stage_a(s, /*passes=*/false);
  const ir::Program pr = expect_roundtrip(raw.tape, "reduced stage A (raw)", &raw_stats);
  EXPECT_GE(ir::scan_domains(pr).size(), 1u);
  const fixtures::StageATape passed = fixtures::record_stage_a(s, /*passes=*/true);
  const ir::Program pp = expect_roundtrip(passed.tape, "reduced stage A (passes)", &pass_stats);
  EXPECT_GE(ir::scan_domains(pp).size(), 1u);
  EXPECT_LE(pass_stats.chains, raw_stats.chains) << "cse merges the coupons over the same days";
  const std::string text = ir::serialize(pp);
  const ir::Program q = ir::deserialize(text);
  EXPECT_TRUE(q == pp);
  EXPECT_EQ(q.scans, pp.scans);
  std::cout << "[  reduced ] " << s.n_trades() << " trades: raw " << raw.stats.nodes_raw << " nodes, " << raw_stats.chains << " chains; passes "
            << passed.stats.nodes_after_passes << " nodes, " << pass_stats.chains << " chains; serialised " << text.size() << " bytes\n";
}
