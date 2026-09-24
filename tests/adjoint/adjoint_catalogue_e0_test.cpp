// M4/C1 gate: adjoint::Adjoint's catalogue dispatch (include/epykos/catalogue/) inside its
// forward pass (adjoint_e0.cpp's forward()) is E0 — bit-identical, in BOTH the forward outputs
// and the reverse state adjoints, to the generic per-step materialisation it replaces — on both
// reference workloads. Options::use_catalogue is the "registry on/off" toggle. Adjoint applies
// none of exec::Interpreter's fuse/inline optimisations (DESIGN.md §7: "the reverse of a fused
// group is rewrite / catalogue work (M4)"), so every catalogue-eligible domain is a candidate
// here, scan domains included — unlike the Interpreter side (see
// tests/exec/interpreter_catalogue_e0_test.cpp).
//
// An _e0_test.cpp TU (-ffp-contract=off in every preset): adjoint_e0.cpp and the generated
// catalogue kernels (src/catalogue/generated/kernels_e0.cpp) are both E0 TUs of libepykos.
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "epykos/adjoint/adjoint.hpp"
#include "epykos/fixtures/m1_book.hpp"
#include "epykos/fixtures/record_m1.hpp"
#include "epykos/fixtures/stage_a.hpp"
#include "epykos/ir/program.hpp"
#include "epykos/ir/signature.hpp"
#include "epykos/tape/tape.hpp"

#ifndef EPYKOS_FP_CONTRACT_OFF
#error "an _e0_test.cpp TU must see EPYKOS_FP_CONTRACT_OFF"
#endif

namespace adjoint = epykos::adjoint;
namespace fixtures = epykos::fixtures;
namespace ir = epykos::ir;

namespace {

std::uint64_t bits(double v) {
  std::uint64_t u;
  std::memcpy(&u, &v, sizeof u);
  return u;
}

std::size_t count_mismatches(const std::vector<double>& a, const std::vector<double>& b, const std::string& where) {
  EXPECT_EQ(a.size(), b.size()) << where;
  std::size_t bad = 0;
  for (std::size_t k = 0; k < a.size() && k < b.size(); ++k) {
    if (bits(a[k]) != bits(b[k])) {
      if (bad < 5) ADD_FAILURE() << where << ", index " << k << ": " << a[k] << " vs " << b[k];
      ++bad;
    }
  }
  return bad;
}

// Runs `program` at B = states.size() with the catalogue on and off, over a small tile sweep
// (lane_tile fixed: Adjoint's own scan / tile machinery is exercised by the pre-existing M2/G3
// gates; this test's own job is the catalogue hook only), comparing forward outputs AND state
// adjoints (seeded out_bar = 1 on every output) bitwise. Asserts the catalogue fired at least
// once. Returns the catalogued-on coverage() at the first configuration.
epykos::catalogue::Coverage check_catalogue_e0(const ir::Program& program, const std::vector<double>& state, int B,
                                               const std::string& label) {
  const int tiles[] = {7, 256};
  std::size_t mismatches = 0;
  epykos::catalogue::Coverage default_coverage;
  bool first = true;
  for (int tile : tiles) {
    adjoint::Options on;
    on.tile = tile;
    on.use_catalogue = true;
    adjoint::Options off = on;
    off.use_catalogue = false;
    adjoint::Adjoint ad_on(program, on);
    adjoint::Adjoint ad_off(program, off);
    if (first) {
      default_coverage = ad_on.coverage();
      first = false;
    }

    const std::size_t n_out = static_cast<std::size_t>(ad_on.n_outputs());
    const std::size_t n_in = static_cast<std::size_t>(ad_on.n_inputs());
    std::vector<double> out_bar(n_out * static_cast<std::size_t>(B), 1.0);
    std::vector<double> out_on(n_out * static_cast<std::size_t>(B)), out_off(n_out * static_cast<std::size_t>(B));
    std::vector<double> sb_on(n_in * static_cast<std::size_t>(B)), sb_off(n_in * static_cast<std::size_t>(B));
    ad_on.run(state.data(), B, out_bar.data(), out_on.data(), sb_on.data());
    ad_off.run(state.data(), B, out_bar.data(), out_off.data(), sb_off.data());
    mismatches += count_mismatches(out_on, out_off, label + " forward outputs, tile " + std::to_string(tile));
    mismatches += count_mismatches(sb_on, sb_off, label + " state adjoints, tile " + std::to_string(tile));
  }
  EXPECT_EQ(mismatches, 0u) << label;
  std::cout << "[ adj catalogue ] " << label << ": mismatches " << mismatches << "; coverage "
            << default_coverage.groups_catalogued << "/" << default_coverage.groups_total << " groups ("
            << (100.0 * default_coverage.group_fraction()) << "%), " << default_coverage.rows_catalogued << "/"
            << default_coverage.rows_total << " rows (" << (100.0 * default_coverage.row_fraction()) << "%)\n";
  EXPECT_GT(default_coverage.groups_catalogued, 0u) << label << ": the catalogue never fired -- this test would pass vacuously";
  return default_coverage;
}

}  // namespace

TEST(AdjointCatalogueE0, M1Book) {
  const fixtures::Book book = fixtures::make_m1_book();
  const epykos::Tape tape = fixtures::record_m1(book);
  const ir::Program program = ir::infer(tape);
  check_catalogue_e0(program, tape.input_values(), 1, "M1 book");
}

// The exact default Stage A (StageAOptions{}, no overrides): the same instance
// scripts/catalogue_regen.sh built the registry from, so every candidate domain MUST match. The
// full ~2,000-trade book (~8s to record); see interpreter_catalogue_e0_test.cpp's own
// DefaultStageAIsFullyCatalogued for why this is a guarantee here but not on a smaller draw.
TEST(AdjointCatalogueE0, DefaultStageAIsFullyCatalogued) {
  const fixtures::StageA s = fixtures::make_stage_a();
  const fixtures::StageATape tape = fixtures::record_stage_a(s);
  const ir::Program program = ir::infer(tape.tape);
  ASSERT_FALSE(epykos::ir::scan_domains(program).empty()) << "Stage A's compounding scan is the whole point of this gate";
  const epykos::catalogue::Coverage cov = check_catalogue_e0(program, program.input_values, 1, "Stage A (default)");
  EXPECT_EQ(cov.groups_catalogued, cov.groups_total)
      << "the registry was generated from exactly this instance (scripts/catalogue_regen.sh) -- every candidate must match";
}

// A smaller, differently-noised draw: signature independence holds (same catalogue, bitwise
// correct wherever it dispatches) but full coverage does not -- see
// interpreter_catalogue_e0_test.cpp's DifferentStageAInstanceHitsTheSameCatalogueAndIsMostlyCovered
// for the (structural, not a defect) reason a few small per-netting-set Sum shapes vary by draw.
TEST(AdjointCatalogueE0, SmallStageAIncludingScanDomains) {
  fixtures::StageAOptions opt;
  opt.trades = 200;
  opt.scenarios = 0;
  const fixtures::StageA s = fixtures::make_stage_a(opt);
  const fixtures::StageATape tape = fixtures::record_stage_a(s);
  const ir::Program program = ir::infer(tape.tape);
  ASSERT_FALSE(epykos::ir::scan_domains(program).empty()) << "Stage A's compounding scan is the whole point of this gate";
  const epykos::catalogue::Coverage cov = check_catalogue_e0(program, program.input_values, 1, "Stage A (200 trades)");
  EXPECT_GT(cov.group_fraction(), 0.75) << "coverage dropped well below what was measured at authoring time";
}
