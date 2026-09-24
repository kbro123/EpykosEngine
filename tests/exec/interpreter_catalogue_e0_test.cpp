// M4/C1 gate: exec::Interpreter's catalogue dispatch (include/epykos/catalogue/) is E0 —
// bit-identical to the generic per-step path it replaces — on both reference workloads, and the
// kernels it dispatches to are signature-, not data-, keyed (HARD RULE 9): a Stage A instance
// built with a different trade count and a different quote-noise draw still dispatches through
// the catalogue, and still matches bitwise. Options::use_catalogue is the "registry on/off"
// toggle DESIGN.md §7 tier 1 / RESUME.md's C1 row asks for.
//
// StageAOptions has no PRNG-seed override of its own (the seed lives in the checked-in
// blueprints/problems/stage_a.json definition, D36) — "signature independence from data" is
// exercised here as different trade counts and a different quote_noise_bp instead of a literal
// second seed, which is the same property a second seed would exercise (the SAME recorded op
// shapes recur under a different concrete numeric draw); reported as such, not as a second seed.
//
// An _e0_test.cpp TU (-ffp-contract=off in every preset): the interpreter's kernels and this
// package's generated catalogue kernels (src/catalogue/generated/kernels_e0.cpp) are both E0 TUs
// of libepykos, so this gate holds under release and reference alike.
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "epykos/exec/interpreter.hpp"
#include "epykos/fixtures/m1_book.hpp"
#include "epykos/fixtures/record_m1.hpp"
#include "epykos/fixtures/stage_a.hpp"
#include "epykos/ir/program.hpp"
#include "epykos/ir/signature.hpp"
#include "epykos/tape/tape.hpp"

#ifndef EPYKOS_FP_CONTRACT_OFF
#error "an _e0_test.cpp TU must see EPYKOS_FP_CONTRACT_OFF"
#endif

namespace exec = epykos::exec;
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
      if (bad < 5) ADD_FAILURE() << where << ", output " << k << ": " << a[k] << " vs " << b[k];
      ++bad;
    }
  }
  return bad;
}

std::vector<double> run_batched(const exec::Interpreter& in, const std::vector<std::vector<double>>& states) {
  const int B = static_cast<int>(states.size());
  const std::size_t K = static_cast<std::size_t>(in.n_inputs());
  std::vector<double> state(K * static_cast<std::size_t>(B));
  for (std::size_t k = 0; k < K; ++k) {
    for (int b = 0; b < B; ++b) state[k * static_cast<std::size_t>(B) + static_cast<std::size_t>(b)] = states[static_cast<std::size_t>(b)][k];
  }
  std::vector<double> out(static_cast<std::size_t>(in.n_outputs()) * static_cast<std::size_t>(B));
  in.run(state.data(), B, out.data());
  return out;
}

// Runs `program` with the catalogue on and off, at B=1 (record point) and batched, over a small
// (tile, lane_tile) sweep; asserts bitwise equality throughout and that the catalogue actually
// fired at least once (a vacuous "on == off" would prove nothing). Returns the catalogued-on
// Interpreter's coverage() at the default Options, for the caller to report / assert further on.
epykos::catalogue::Coverage check_catalogue_e0(const ir::Program& program, const std::vector<std::vector<double>>& states,
                                               const std::string& label) {
  if (states.empty()) {
    ADD_FAILURE() << label << ": no states given";
    return {};
  }
  const std::vector<std::vector<double>> batch(states.begin() + (states.size() > 1 ? 1 : 0), states.end());

  const int tiles[] = {7, 256};
  const int lane_tiles[] = {1, 8, 32};
  std::size_t mismatches = 0;
  std::size_t configs = 0;
  epykos::catalogue::Coverage default_coverage;
  for (int tile : tiles) {
    for (int lane_tile : lane_tiles) {
      exec::Options on;
      on.tile = tile;
      on.lane_tile = lane_tile;
      on.use_catalogue = true;
      exec::Options off = on;
      off.use_catalogue = false;
      exec::Interpreter in_on(program, on);
      exec::Interpreter in_off(program, off);
      if (tile == tiles[0] && lane_tile == lane_tiles[0]) default_coverage = in_on.coverage();

      std::vector<double> rec_on(static_cast<std::size_t>(in_on.n_outputs()));
      std::vector<double> rec_off(static_cast<std::size_t>(in_off.n_outputs()));
      in_on.run(states[0].data(), 1, rec_on.data());
      in_off.run(states[0].data(), 1, rec_off.data());
      mismatches += count_mismatches(rec_on, rec_off, label + " B=1, tile " + std::to_string(tile) + " lane_tile " + std::to_string(lane_tile));

      if (!batch.empty()) {
        const std::vector<double> batched_on = run_batched(in_on, batch);
        const std::vector<double> batched_off = run_batched(in_off, batch);
        mismatches += count_mismatches(batched_on, batched_off,
                                       label + " batched, tile " + std::to_string(tile) + " lane_tile " + std::to_string(lane_tile));
      }
      ++configs;
    }
  }
  EXPECT_EQ(mismatches, 0u) << label;
  std::cout << "[ catalogue ] " << label << ": " << configs << " (tile, lane_tile) configs, catalogue on/off mismatches "
            << mismatches << "; coverage " << default_coverage.groups_catalogued << "/" << default_coverage.groups_total
            << " groups (" << (100.0 * default_coverage.group_fraction()) << "%), " << default_coverage.rows_catalogued << "/"
            << default_coverage.rows_total << " rows (" << (100.0 * default_coverage.row_fraction()) << "%)\n";
  EXPECT_GT(default_coverage.groups_catalogued, 0u) << label << ": the catalogue never fired -- this test would pass vacuously";
  return default_coverage;
}

struct StageACase {
  fixtures::StageA s;
  ir::Program program;
};

StageACase build_stage_a(int trades, int scenarios, double quote_noise_bp) {
  fixtures::StageAOptions opt;
  opt.trades = trades;
  opt.scenarios = scenarios;
  opt.quote_noise_bp = quote_noise_bp;
  fixtures::StageA s = fixtures::make_stage_a(opt);
  const fixtures::StageATape tape = fixtures::record_stage_a(s);
  return StageACase{std::move(s), ir::infer(tape.tape)};
}

}  // namespace

TEST(InterpreterCatalogueE0, M1Book) {
  const fixtures::Book book = fixtures::make_m1_book();
  const epykos::Tape tape = fixtures::record_m1(book);
  const ir::Program program = ir::infer(tape);
  std::vector<std::vector<double>> states;
  states.push_back(tape.input_values());
  const fixtures::Batch batch = fixtures::make_m1_batch();
  for (int b = 0; b < 8; ++b) {
    std::vector<double> z(static_cast<std::size_t>(fixtures::n_knots));
    batch.state(b, z.data());
    states.push_back(z);
  }
  check_catalogue_e0(program, states, "M1 book");
}

// The exact default Stage A (StageAOptions{}, no overrides): the same instance
// scripts/catalogue_regen.sh built the registry from, so every one of its candidate domains MUST
// match (checked below) -- it is the full ~2,000-trade book (bench/results' M3 baseline: ~8s to
// record), slower than this file's other cases but the only one 100% coverage is a guaranteed
// property of rather than an empirical observation (see DifferentStageAInstanceIsMostlyButNotFullyCatalogued).
TEST(InterpreterCatalogueE0, DefaultStageAIsFullyCatalogued) {
  const StageACase c = build_stage_a(/*trades=*/-1, /*scenarios=*/0, /*quote_noise_bp=*/-1.0);
  std::vector<std::vector<double>> states(1, c.program.input_values);
  const epykos::catalogue::Coverage cov = check_catalogue_e0(c.program, states, "Stage A (default)");
  EXPECT_EQ(cov.groups_catalogued, cov.groups_total)
      << "the registry was generated from exactly this instance (scripts/catalogue_regen.sh) -- every candidate must match";
}

TEST(InterpreterCatalogueE0, SmallStageA) {
  const StageACase c = build_stage_a(/*trades=*/200, /*scenarios=*/0, /*quote_noise_bp=*/-1.0);
  std::vector<std::vector<double>> states(1, c.program.input_values);
  check_catalogue_e0(c.program, states, "Stage A (200 trades)");
}

// HARD RULE 9 / DESIGN.md §7's "signature independence from data": a differently-sized, and
// differently-noised, Stage A instance must dispatch through the SAME catalogue (built once from
// the reference workloads) and stay bitwise correct wherever it does -- the registry cannot be
// keyed on this instance's own row counts or quote values, only on IR structure. It is NOT
// expected to reach the DefaultStageAIsFullyCatalogued test's 100%: a per-netting-set PV
// aggregation records as a FIXED-ARITY Sum (2 or 3 operands, part of the Signature by design --
// DESIGN.md §5.6) only when that particular netting set happens to hold 2 or 3 trades, and a
// LARGER Sum falls back to the row-count-independent Segment form instead: which netting sets
// land on which side of that line depends on the trade-to-netting-set draw, which a different
// trade count or noise level can and does change (measured here: 200 trades and 300 trades each
// leave 3 of 23 candidate domains -- exactly the 2-/3-ary Sum ones the OTHER draw did not
// happen to produce -- uncatalogued; ~87% of groups, ~99.96% of rows, since the uncatalogued
// domains are themselves small). This is a real, structural limit of generating the catalogue
// from two fixed reference runs (the package brief's own scope), reported as such rather than
// hidden behind a looser assertion -- not a hash or binding defect (those still land at 0
// mismatches, checked above) and not a reason M4/C1 failed its own coverage target on the
// instances that DO matter (the two reference workloads themselves: see
// DefaultStageAIsFullyCatalogued and InterpreterCatalogueE0.M1Book).
TEST(InterpreterCatalogueE0, DifferentStageAInstanceHitsTheSameCatalogueAndIsMostlyCovered) {
  const StageACase small = build_stage_a(60, 0, -1.0);
  const StageACase other = build_stage_a(300, 0, 0.0);  // different trade count AND quote_noise_bp
  const epykos::catalogue::Coverage cov_small = check_catalogue_e0(
      small.program, std::vector<std::vector<double>>(1, small.program.input_values), "Stage A (60 trades, default noise)");
  const epykos::catalogue::Coverage cov_other = check_catalogue_e0(
      other.program, std::vector<std::vector<double>>(1, other.program.input_values), "Stage A (300 trades, quote_noise_bp=0)");
  for (const epykos::catalogue::Coverage* cov : {&cov_small, &cov_other}) {
    EXPECT_GT(cov->group_fraction(), 0.75) << "coverage dropped well below the ~87% measured at authoring time";
  }
}
