// M3 review probe (m3-review-one-tape): NOT a gate, NOT landed. Checks the one-tape claims the
// review lens names: how many Tape objects a recording constructs (D24 serials), tape length and
// IR independence from the solver's start (three starts), CSE completeness over every op
// (duplicates), the DF domains' readers by output group on the full book.
#include <gtest/gtest.h>

#include <cstddef>
#include <iostream>
#include <string>
#include <vector>

#include "epykos/ir/expand.hpp"
#include "epykos/ir/program.hpp"
#include "epykos/ir/sharing.hpp"
#include "epykos/ir/signature.hpp"
#include "epykos/tape/op.hpp"
#include "stage_a/stage_a_test_helpers.hpp"

namespace fixtures = epykos::fixtures;
namespace ir = epykos::ir;
namespace solver = epykos::solver;
using epykos::Tape;

namespace {

fixtures::StageA reduced(double flat_start) {
  fixtures::StageAOptions o;
  o.trades = 150;
  o.scenarios = 2;
  o.flat_start = flat_start;
  return fixtures::make_stage_a(o);
}

}  // namespace

TEST(ReviewProbe, TapeSerialsConsumedByOneRecording) {
  const fixtures::StageA s = reduced(0.03);
  const epykos::tape_serial before = Tape().serial();
  const fixtures::StageATape t = fixtures::record_stage_a(s, /*passes=*/false);
  const epykos::tape_serial after_raw = Tape().serial();
  std::cout << "[ probe ] raw recording (no passes): serials consumed = " << (after_raw - before - 1) << " (1 result tape + "
            << s.n_quotes() << " scratch tapes + " << s.n_quotes() << " scratch slices for dependency discovery + per block: 1 slice + its passes)\n";
  const epykos::tape_serial b2 = Tape().serial();
  const fixtures::StageATape tp = fixtures::record_stage_a(s, /*passes=*/true);
  const epykos::tape_serial a2 = Tape().serial();
  std::cout << "[ probe ] recording with passes: serials consumed = " << (a2 - b2 - 1) << '\n';
  EXPECT_EQ(t.tape.num_outputs(), tp.tape.num_outputs());
}

TEST(ReviewProbe, TapeLengthAndIrIndependentOfTheSolverStart) {
  std::vector<double> starts = {0.005, 0.03, 0.08};
  std::vector<std::size_t> raw, passed;
  std::vector<ir::Program> programs;
  std::vector<std::string> iterations;
  std::vector<std::vector<std::size_t>> hist;
  for (double z0 : starts) {
    const fixtures::StageA s = reduced(z0);
    const fixtures::StageATape t = fixtures::record_stage_a(s, /*passes=*/true);
    raw.push_back(t.stats.nodes_raw);
    passed.push_back(t.stats.nodes_after_passes);
    hist.push_back(t.tape.op_histogram());
    std::string it;
    for (const solver::SolveReport& r : t.record_reports) it += std::to_string(r.iterations) + "/" + std::to_string(r.jacobians) + " ";
    iterations.push_back(it);
    programs.push_back(ir::infer(t.tape));
    std::cout << "[ probe ] start " << z0 << ": raw " << t.stats.nodes_raw << " nodes, after passes " << t.stats.nodes_after_passes << ", iterations/jacobians per block " << it
              << ", domains " << programs.back().domains.size() << ", values " << programs.back().num_values() << '\n';
  }
  for (std::size_t i = 1; i < starts.size(); ++i) {
    EXPECT_EQ(raw[i], raw[0]);
    EXPECT_EQ(passed[i], passed[0]);
    EXPECT_EQ(hist[i], hist[0]);
    ir::Program a = programs[0], b = programs[i];
    b.input_values = a.input_values;   // the record-point values differ; the structure must not
    EXPECT_TRUE(a == b) << "the IR differs between starts " << starts[0] << " and " << starts[i];
  }
  EXPECT_NE(iterations[0], iterations[1]);
}

TEST(ReviewProbe, DuplicatesOverEveryOpOnTheReducedProgram) {
  const fixtures::StageA s = reduced(0.03);
  const fixtures::StageATape t = fixtures::record_stage_a(s, /*passes=*/true);
  const ir::Program p = ir::infer(t.tape);
  std::size_t total = 0;
  for (int o = 0; o < static_cast<int>(epykos::Op::Count_); ++o) {
    const epykos::Op op = static_cast<epykos::Op>(o);
    std::size_t d = 0;
    try {
      d = ir::duplicates(p, op);
    } catch (const std::exception& e) {
      std::cout << "[ probe ] duplicates(" << epykos::to_string(op) << ") threw: " << e.what() << '\n';
      continue;
    }
    if (d > 0) std::cout << "[ probe ] duplicates(" << epykos::to_string(op) << ") = " << d << '\n';
    total += d;
  }
  std::cout << "[ probe ] duplicates over every op: " << total << '\n';
  EXPECT_EQ(total, 0u);
}

TEST(ReviewProbe, DfDomainsReadersOnTheFullBook) {
  const fixtures::StageA& s = epykos::test::stage_a();
  const fixtures::StageATape& t = epykos::test::stage_a_tape();
  const ir::Program p = ir::infer(t.tape);
  const std::vector<std::vector<int>> groups = t.output_groups();
  const ir::SharingReport rep = ir::sharing(p, t.sharing_groups(), epykos::Op::Exp);
  for (ir::domain_id d : rep.matching) {
    const ir::SharingReport::Row& row = rep.rows[static_cast<std::size_t>(d)];
    std::cout << "[ probe ] DF domain " << d << ' ' << row.name << ": rows " << row.rows << ", reader domains " << row.n_readers << ", groups mask " << row.groups << '\n';
  }
  // Every domain: which output groups it reaches (0 block outputs, 1 knots, 2 book, 3 exports).
  const std::vector<std::uint32_t> mask = ir::reach(p, groups);
  std::size_t residual_only = 0, book_only = 0, both = 0;
  for (std::size_t d = 0; d < p.domains.size(); ++d) {
    const bool r = mask[d] & 1u, b = mask[d] & 4u;
    if (r && b) ++both; else if (r) ++residual_only; else if (b) ++book_only;
  }
  std::cout << "[ probe ] domains reaching residuals only " << residual_only << ", book only " << book_only << ", both " << both << " of " << p.domains.size() << '\n';
  for (std::size_t d = 0; d < p.domains.size(); ++d) {
    const bool r = mask[d] & 1u, b = mask[d] & 4u;
    if (r && !b) std::cout << "[ probe ]   residual-only domain " << d << ' ' << ir::shape_string(p, static_cast<ir::domain_id>(d)) << " rows " << p.domains[d].rows << '\n';
  }
  EXPECT_GT(both, 0u);
  (void)s;
}
