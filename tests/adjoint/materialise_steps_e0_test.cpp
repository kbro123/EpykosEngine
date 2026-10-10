// MATERIALISING A GROUP'S INTERMEDIATE STEPS (`adjoint::Options::materialise_steps`): the two arms
// must be BITWISE equal, and this file is that gate.
//
// What the change is. D31 item 1 chose that `Adjoint`'s forward stores every ROW VALUE — the last
// step of every group — and that steps 0 .. last-1 of a group are RECOMPUTED per tile in the
// reverse, from operands the reverse loads anyway for the local rules. `Options::materialise_steps`
// is the other arm: the forward stores those intermediates too, and the reverse copies them back
// into the tile buffer instead of recomputing them. The option ships FALSE, so shipped behaviour
// is D31's, byte for byte; `tools/matprobe/` times the two arms in one process.
//
// WHY BITWISE IS A CONSTRUCTION HERE AND NOT AN ARGUMENT ABOUT FLOATING POINT. Both arms call the
// same `forward_step` on the same step with the same operands, and an intra-chain step's operands
// are:
//
//   * earlier steps of its own group, which both arms produce in the same order; and
//   * literals, columns, and gathers or segment members reading EARLIER domains — `ir::validate`
//     forbids a forward read, and a non-scan domain cannot read its own rows at all.
//
// So the value buffer holds the same bits at those addresses whether the step runs in the forward
// with the buffer partly filled or in the reverse with it complete. That is the same fact that
// makes TODAY'S recompute bitwise — D31's "recomputed with the forward's bits" — and materialising
// only moves when the identical call happens. A divergence is therefore a DEFECT in the plumbing
// (an offset, a tile boundary, a lane width, a missing store), never a rounding difference and
// never a tolerance to widen: PRINCIPLES.md §5.2's below-the-pin column reads "may change
// roundings: no", and §5.2a case 1 says a case-1 gate is not converted to a tolerance one.
//
// The comparison is `memcmp` for that reason, and because the failure modes this gate exists to
// catch are silent under every tolerance: a stale intermediate from the PREVIOUS tile differs from
// the right one by whatever the two rows happen to differ by, which on a smooth curve is small.
//
// WHAT IS SWEPT, and why each axis is here rather than assumed
// ---------------------------------------------------------------------------------------------
//   * `tile` — the materialise buffer is indexed by (step, row) over the WHOLE domain while the
//     recompute arm's `fs` is indexed by (step, row-within-tile). A tile that does not divide the
//     row count is where an offset error lands, so 1, 3, 256 and a value larger than any domain.
//   * `lane_tile` and B — the layout's innermost stride is L = min(lane_tile, max_batch), and the
//     split-lane widths (2, 5, 7; exec/lanes.hpp) run a single call as several chunks of
//     DIFFERENT L against ONE buffer, which is the stride bug's natural home.
//   * `use_catalogue` BOTH WAYS — a catalogued kernel writes the domain's value and nothing else,
//     so the materialise arm has to run its own sweep for the intermediates. With the catalogue
//     off that code never executes, so a gate run only one way would be blind to half the arm.
//     This is the `adjoint.materialise_skip_catalogue` mutant's only observable configuration.
//
// THE GATE COUNTS ITS OWN OBSERVABILITY RATHER THAN ARGUING IT (D105 §4, D111 §4: a gate binds
// only what it can observe, and citing a gate is not checking it). `census()` below counts, per
// fixture, the rows the arm actually moves — domains with more than one step — split into the two
// mutants' sites: rows of CATALOGUED multi-step domains, and intra-chain steps whose rule reads an
// operand out of `c.ops`. `TheGateCanObserveBothMutants` asserts BOTH totals are non-empty over
// the gate set, so this file cannot go quietly blind the way `i1_gate_e0_test.cpp` did for a
// signed zero (D105 §4) or `structural_gate_test.cpp` did for its own name (D111 §4).
//
// `exotic_path` is in the gate set deliberately and is the only fixture that matters for one of
// these counts: on the rates book every `exp` is a group's VALUE, so 0 of `stage_a`'s 11,087 and 0
// of `compare_ois`'s 81 transcendentals are ever recomputed, while on D106's path grid 50.1% of
// them are intra-chain. A gate set without it would assert this arm's correctness over a
// population from which the interesting op is structurally absent.
//
// The file name's two requirements are the ones adjoint_to_program_e0_test.cpp,
// i1_gate_e0_test.cpp and seed_pull_e0_test.cpp record: `_e0_test` compiles this TU with
// -ffp-contract=off in every preset (D25), and `_e0_test$` matches scripts/mutation_test.sh's
// GATE_REGEX, so this file is selected as a gate at all. D111 §4 found a correctly-written catcher
// silently excluded because its filename did not match, and the harness then reported its mutant
// as surviving; that was the second instance in this repository.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <iostream>
#include <string>
#include <vector>

#include "epykos/adjoint/adjoint.hpp"
#include "epykos/catalogue/registry.hpp"
#include "epykos/catalogue/signature.hpp"
#include "epykos/fixtures/affine_scan.hpp"
#include "epykos/fixtures/compare_ois.hpp"
#include "epykos/fixtures/exotic_path.hpp"
#include "epykos/fixtures/instrument_sample.hpp"
#include "epykos/fixtures/m1_book.hpp"
#include "epykos/fixtures/nearmiss_shapes.hpp"
#include "epykos/fixtures/record_m1.hpp"
#include "epykos/fixtures/rfr_book.hpp"
#include "epykos/fixtures/rfr_price.hpp"
#include "epykos/fixtures/stage_a.hpp"
#include "epykos/ir/program.hpp"
#include "epykos/ir/signature.hpp"
#include "epykos/tape/passes.hpp"
#include "epykos/tape/tape.hpp"

namespace {

using namespace epykos;
using ir::Program;

bool same_bits(double a, double b) { return std::memcmp(&a, &b, sizeof(double)) == 0; }

std::string bits_of(double v) {
  std::uint64_t b = 0;
  std::memcpy(&b, &v, sizeof b);
  char buf[32];
  std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(b));
  return std::string(buf) + " (" + std::to_string(v) + ")";
}

std::vector<std::vector<double>> ball_states(const std::vector<double>& x0, int n, double scale) {
  std::vector<std::vector<double>> states;
  for (int r = 0; r < n; ++r) {
    std::vector<double> z(x0);
    for (std::size_t k = 0; k < z.size(); ++k) {
      z[k] += scale * static_cast<double>((r + 1) * (static_cast<int>(k) % 5 - 2));
    }
    states.push_back(z);
  }
  return states;
}

// Four seeds. Unlike seed_pull_e0_test.cpp this change cannot move the sign of a zero — it moves
// no arithmetic at all — so the seeds are here to exercise every RULE (which operand a rule reads
// out of `c.ops` is what the second mutant perturbs), not to expose a signed zero.
std::vector<std::vector<double>> out_bar_seeds(int n_out) {
  const std::size_t n = static_cast<std::size_t>(n_out);
  std::vector<std::vector<double>> seeds;
  seeds.emplace_back(n, 1.0);
  {
    std::vector<double> m(n, 0.0);
    for (std::size_t o = 0; o < n; ++o) m[o] = static_cast<double>(static_cast<int>(o % 3) - 1);
    seeds.push_back(m);
  }
  {
    std::vector<double> e(n, 0.0);
    e[n - 1] = 1.0;
    seeds.push_back(e);
  }
  {
    std::vector<double> m(n, 0.0);
    for (std::size_t o = 0; o < n; ++o) m[o] = 1.0 + 0.5 * static_cast<double>(o % 7);
    seeds.push_back(m);
  }
  return seeds;
}

// ---- the fixture table -------------------------------------------------------------------------

struct Entry {
  const char* name;
  std::function<Program()> build;
  std::function<std::vector<std::vector<double>>(const Program&)> states;
  std::vector<int> batches;  // 2, 5 and 7 are the split-lane widths (exec/lanes.hpp)
  // A fixture with tens of thousands of intra-chain instances gets a REDUCED tile / lane_tile
  // sweep. The offsets and strides this arm can get wrong are properties of the layout, not of
  // the program, so they are covered on the cheap fixtures at full sweep; a heavy fixture is here
  // for its OPS and its catalogued domains. The whole set at full sweep is ~70 s, and
  // scripts/mutation_test.sh runs the gate set once per mutant that has no recorded catcher.
  bool heavy = false;
};

const std::vector<Entry>& fixtures_under_gate() {
  static const std::vector<Entry> kFixtures = {
      {"nearmiss(raw)", [] { return ir::infer(fixtures::record_nearmiss()); },
       [](const Program&) { return fixtures::nearmiss_states(2); }, {1, 4, 5}},
      {"nearmiss(passed)",
       [] {
         Tape t = fixtures::record_nearmiss();
         epykos::standard_passes(t);
         return ir::infer(t);
       },
       [](const Program&) { return fixtures::nearmiss_states(2); }, {1, 4, 7}},
      {"m1_book", [] { return ir::infer(fixtures::record_m1(fixtures::make_m1_book())); },
       [](const Program& p) { return ball_states(p.input_values, 2, 1e-4); }, {1, 4, 7}, true},
      {"compare_ois(short)",
       [] {
         fixtures::CompareOisOptions o;
         o.trades = 2;
         o.tenors = {"1Y", "2Y", "5Y", "10Y"};
         return ir::infer(fixtures::record_compare_ois(fixtures::make_compare_ois(o)).tape);
       },
       [](const Program& p) { return ball_states(p.input_values, 2, 1e-5); }, {1, 2, 5}},
      {"compare_ois(default)",
       [] {
         fixtures::CompareOisOptions o;
         o.trades = 2;
         return ir::infer(fixtures::record_compare_ois(fixtures::make_compare_ois(o)).tape);
       },
       [](const Program& p) { return ball_states(p.input_values, 2, 1e-5); }, {1, 4, 5}},
      // A scan: its forward runs one row per tile in ROW order while its reverse runs the rows
      // backwards, so it is the one shape where the two arms visit the materialise buffer in
      // opposite directions.
      {"affine_scan(final)", [] { return ir::infer(fixtures::record_affine_scan(fixtures::make_affine_scan(), false)); },
       [](const Program& p) { return ball_states(p.input_values, 2, 1e-3); }, {1, 4, 5}},
      {"affine_scan(path)", [] { return ir::infer(fixtures::record_affine_scan(fixtures::make_affine_scan(), true)); },
       [](const Program& p) { return ball_states(p.input_values, 1, 1e-3); }, {1, 4}},
      {"instrument_sample", [] { return ir::infer(fixtures::record_sample(fixtures::make_instrument_sample())); },
       [](const Program& p) { return ball_states(p.input_values, 2, 1e-4); }, {1, 4, 5}},
      {"rfr_book", [] { return ir::infer(fixtures::record_rfr(fixtures::make_rfr_book())); },
       [](const Program& p) { return ball_states(p.input_values, 1, 1e-4); }, {1, 4}},
      // D106's path grid: the ONLY fixture in the tree whose recomputed intermediates include
      // transcendentals (50.1% of them at scale), and the low-sharing end of the spectrum this
      // arm was built to measure. Small here — the gate is a bitwise check, not a benchmark.
      {"exotic_path(8x6)",
       [] { return ir::infer(fixtures::record_exotic_path(fixtures::make_exotic_path(8, 6), false)); },
       [](const Program& p) { return ball_states(p.input_values, 2, 1e-4); }, {1, 4, 5}},
      {"exotic_path(16x4,paths)",
       [] { return ir::infer(fixtures::record_exotic_path(fixtures::make_exotic_path(16, 4), true)); },
       [](const Program& p) { return ball_states(p.input_values, 1, 1e-4); }, {1, 4}},
      // The desk problem (PROBLEM.md Stage A). 8.3% of its forward arithmetic is what this arm
      // moves, and it is the fixture with the most catalogued domains.
      {"stage_a(default)", [] { return ir::infer(fixtures::record_stage_a(fixtures::make_stage_a()).tape); },
       [](const Program& p) { return ball_states(p.input_values, 1, 1e-6); }, {1, 4}, true},
  };
  return kFixtures;
}

// Recording and inferring `stage_a` is the dominant cost of this file, so each fixture's Program
// is built once and shared by the three tests below rather than three times. They only ever read
// it (`Adjoint` takes a reference and requires the program to outlive it), so a cache is sound.
const Program& program_for(const Entry& e) {
  static std::map<std::string, Program> cache;
  auto it = cache.find(e.name);
  if (it == cache.end()) it = cache.emplace(e.name, e.build()).first;
  return it->second;
}

// ---- the observability census ------------------------------------------------------------------
//
// What the arm MOVES, and where each mutant is visible. A domain with one step has no intra-chain
// step at all, so neither arm does anything with it and it contributes nothing to this gate; the
// counts below are over the rest.
struct Census {
  long long domains = 0;
  long long multi_step_domains = 0;    // last > 0: the arm has something to store
  long long intra_rows = 0;            // (steps - 1) * rows over those domains: the buffer's size
  long long cat_multi_step_rows = 0;   // ... of which in CATALOGUED domains: mutant 1's sites
  long long operand_reading_rows = 0;  // intra-chain steps whose rule reads a / b: mutant 2's sites
  long long trans_rows = 0;            // ... of which a transcendental: the expensive recompute
};

// Does `reverse_step` read this op's operands out of `c.ops`? Its `default:` branch calls
// `operand(..., load = false)` for a and b; Sum, Affine, Const, Input and the comparisons take
// that branch not at all. Kept in step with src/adjoint/adjoint_e0.cpp's `load_step_operands`,
// which is the function the second mutant removes.
bool rule_reads_operands(Op op) {
  switch (op) {
    case Op::Const:
    case Op::Input:
    case Op::Sum:
    case Op::Affine:
    case Op::CmpLt:
    case Op::CmpLe:
    case Op::CmpGt:
    case Op::CmpGe:
    case Op::CmpEq:
      return false;
    default:
      return true;
  }
}

bool is_transcendental(Op op) { return op == Op::Exp || op == Op::Log || op == Op::Sqrt; }

Census census(const Program& p, bool use_catalogue) {
  Census c;
  c.domains = static_cast<long long>(p.domains.size());
  for (std::size_t d = 0; d < p.domains.size(); ++d) {
    const int last = static_cast<int>(p.groups[d].steps.size()) - 1;
    if (last <= 0) continue;
    const long long rows = p.domains[d].rows;
    ++c.multi_step_domains;
    c.intra_rows += rows * static_cast<long long>(last);
    bool catalogued = false;
    if (use_catalogue) {
      const ir::domain_id did = static_cast<ir::domain_id>(d);
      if (catalogue::is_cataloguable(p, did) && catalogue::lookup(catalogue::signature_of(p, did)) != nullptr) {
        catalogued = true;
      }
    }
    if (catalogued) c.cat_multi_step_rows += rows * static_cast<long long>(last);
    for (int k = 0; k < last; ++k) {
      const Op op = p.groups[d].steps[static_cast<std::size_t>(k)].op;
      if (rule_reads_operands(op)) c.operand_reading_rows += rows;
      if (is_transcendental(op)) c.trans_rows += rows;
    }
  }
  return c;
}

struct Row {
  std::string name;
  long long comparisons = 0;
  long long mismatches = 0;
  Census cat_on;
  Census cat_off;
};

}  // namespace

// ================================================================================================
// The gate: materialise_steps on is bitwise materialise_steps off, over every fixture the adjoint
// is gated on, both catalogue settings, and a sweep of tile, lane_tile and B.
// ================================================================================================

TEST(MaterialiseSteps, MaterialisingTheIntermediatesIsBitwiseRecomputingThem) {
  // tile: 1 forces one row per tile; 3 divides no domain's row count; 256 is the default; 100000
  // puts every domain in one tile. lane_tile 1 / 3 / 8 against B including the split widths.
  const std::vector<int> tiles = {1, 3, 256, 100000};
  const std::vector<int> lane_tiles = {1, 3, 8};
  // Heavy fixtures: a multi-tile width and a whole-domain width, at the shipped lane_tile.
  const std::vector<int> heavy_tiles = {7, 100000};
  const std::vector<int> heavy_lane_tiles = {8};

  std::vector<Row> rows;
  long long total_comparisons = 0, total_mismatches = 0;

  for (const Entry& e : fixtures_under_gate()) {
    SCOPED_TRACE(e.name);
    const Program& p = program_for(e);
    const int n_in = static_cast<int>(p.inputs.size());
    const int n_out = static_cast<int>(p.outputs.size());
    ASSERT_GT(n_in, 0) << e.name;
    ASSERT_GT(n_out, 0) << e.name;

    Row row;
    row.name = e.name;
    row.cat_on = census(p, true);
    row.cat_off = census(p, false);

    const std::vector<std::vector<double>> states = e.states(p);
    const std::vector<std::vector<double>> seeds = out_bar_seeds(n_out);

    for (const bool use_cat : {true, false}) {
      for (const int B : e.batches) {
        const std::size_t Bs = static_cast<std::size_t>(B);
        for (const int tile : e.heavy ? heavy_tiles : tiles) {
          for (const int lt : e.heavy ? heavy_lane_tiles : lane_tiles) {
            adjoint::Options ref_opt;
            ref_opt.max_batch = B;
            ref_opt.tile = tile;
            ref_opt.lane_tile = lt;
            ref_opt.use_catalogue = use_cat;
            ref_opt.materialise_steps = false;  // THE REFERENCE ARM: D31's recompute
            adjoint::Options new_opt = ref_opt;
            new_opt.materialise_steps = true;
            const adjoint::Adjoint ref(p, ref_opt), got(p, new_opt);

            std::vector<double> state(static_cast<std::size_t>(n_in) * Bs, 0.0);
            std::vector<double> out_bar(static_cast<std::size_t>(n_out) * Bs, 0.0);
            for (std::size_t si = 0; si < states.size(); ++si) {
              for (std::size_t se = 0; se < seeds.size(); ++se) {
                for (std::size_t b = 0; b < Bs; ++b) {
                  const std::vector<double>& z = states[(si + b) % states.size()];
                  const std::vector<double>& o = seeds[(se + b) % seeds.size()];
                  for (int k = 0; k < n_in; ++k)
                    state[static_cast<std::size_t>(k) * Bs + b] = z[static_cast<std::size_t>(k)];
                  for (int k = 0; k < n_out; ++k)
                    out_bar[static_cast<std::size_t>(k) * Bs + b] = o[static_cast<std::size_t>(k)];
                }
                // Distinct fill values, so an arm that writes nothing at all is a mismatch rather
                // than an accidental agreement on a shared zero.
                std::vector<double> out_a(static_cast<std::size_t>(n_out) * Bs, 1.0),
                    sb_a(static_cast<std::size_t>(n_in) * Bs, 1.0);
                std::vector<double> out_b(static_cast<std::size_t>(n_out) * Bs, 2.0),
                    sb_b(static_cast<std::size_t>(n_in) * Bs, 2.0);
                ref.run(state.data(), B, out_bar.data(), out_a.data(), sb_a.data());
                got.run(state.data(), B, out_bar.data(), out_b.data(), sb_b.data());

                int shown = 0;
                auto compare = [&](const std::vector<double>& a, const std::vector<double>& b, const char* what) {
                  for (std::size_t i = 0; i < a.size(); ++i) {
                    ++row.comparisons;
                    if (same_bits(a[i], b[i])) continue;
                    ++row.mismatches;
                    if (shown++ < 4) {
                      ADD_FAILURE() << e.name << ": " << what << "[" << i << "] differs at B=" << B
                                    << " tile=" << tile << " lane_tile=" << lt
                                    << " catalogue=" << (use_cat ? "on" : "off") << " state " << si << " seed " << se
                                    << "\n  recompute   " << bits_of(a[i]) << "\n  materialise " << bits_of(b[i]);
                    }
                  }
                };
                compare(out_a, out_b, "out");
                compare(sb_a, sb_b, "state_bar");
              }
            }
          }
        }
      }
    }
    total_comparisons += row.comparisons;
    total_mismatches += row.mismatches;
    rows.push_back(row);
  }

  std::cout << "\n  materialise_steps: BITWISE comparisons against D31's recompute\n";
  std::cout << "  " << std::string(118, '-') << "\n";
  std::printf("  %-24s %12s %10s | %8s %10s %12s %12s %10s\n", "fixture", "memcmp", "mismatch", "domains",
              "multistep", "intra rows", "catalogued", "trans");
  for (const Row& r : rows) {
    std::printf("  %-24s %12lld %10lld | %8lld %10lld %12lld %12lld %10lld\n", r.name.c_str(), r.comparisons,
                r.mismatches, r.cat_on.domains, r.cat_on.multi_step_domains, r.cat_on.intra_rows,
                r.cat_on.cat_multi_step_rows, r.cat_on.trans_rows);
  }
  std::printf("  TOTAL %lld comparisons, %lld mismatches\n", total_comparisons, total_mismatches);

  EXPECT_EQ(total_mismatches, 0);
  // Not a formality: a gate whose comparisons are all over one-step domains would pass while
  // observing nothing about this arm.
  EXPECT_GT(total_comparisons, 100000);
}

// ================================================================================================
// The gate observes what it claims to (D105 §4, D111 §4). Both mutants have sites in this set,
// counted rather than argued, and one of them has sites ONLY with the catalogue on.
// ================================================================================================

TEST(MaterialiseSteps, TheGateCanObserveBothMutants) {
  long long intra = 0, cat_sites = 0, operand_sites = 0, trans_sites = 0;
  long long fixtures_with_cat_sites = 0, fixtures_with_trans = 0;

  std::cout << "\n  observability of the two mutants over the gate set\n";
  std::printf("  %-24s %12s %14s %14s %10s\n", "fixture", "intra rows", "cat sites", "operand sites", "trans");
  for (const Entry& e : fixtures_under_gate()) {
    const Program& p = program_for(e);
    const Census c = census(p, true);
    std::printf("  %-24s %12lld %14lld %14lld %10lld\n", e.name, c.intra_rows, c.cat_multi_step_rows,
                c.operand_reading_rows, c.trans_rows);
    intra += c.intra_rows;
    cat_sites += c.cat_multi_step_rows;
    operand_sites += c.operand_reading_rows;
    trans_sites += c.trans_rows;
    if (c.cat_multi_step_rows > 0) ++fixtures_with_cat_sites;
    if (c.trans_rows > 0) ++fixtures_with_trans;

    // With the catalogue OFF nothing is catalogued, which is the whole reason the bitwise test
    // above sweeps both settings rather than taking the default.
    EXPECT_EQ(census(p, false).cat_multi_step_rows, 0) << e.name;
  }
  std::printf("  TOTAL intra rows %lld | catalogued multi-step rows %lld (%lld fixtures) | operand-reading %lld | "
              "transcendental %lld (%lld fixtures)\n",
              intra, cat_sites, fixtures_with_cat_sites, operand_sites, trans_sites, fixtures_with_trans);

  // `adjoint.materialise_skip_operands`: the arm must have steps whose rule reads an operand.
  EXPECT_GT(operand_sites, 0) << "no intra-chain step in the gate set has a rule that reads its operands, so "
                                 "adjoint.materialise_skip_operands is unobservable here";
  // `adjoint.materialise_skip_catalogue`: the arm must have a CATALOGUED domain with more than one
  // step. If the generated registry goes stale this is the assertion that says so rather than the
  // mutant quietly surviving (D81 §6: a stale registry dropped coverage by a quarter unnoticed).
  EXPECT_GT(cat_sites, 0) << "no catalogued domain in the gate set has more than one step, so "
                             "adjoint.materialise_skip_catalogue is unobservable here -- check the generated "
                             "catalogue registry is not stale";
  // The path grid is the only source of recomputed transcendentals, and the reason it is in the
  // set. If this fails the gate has lost the op whose recompute cost actually differs.
  EXPECT_GT(trans_sites, 0) << "no intra-chain transcendental in the gate set: on the rates book every exp is a "
                               "group's VALUE, so exotic_path is what supplies these";
}

// ================================================================================================
// The materialise buffer is accounted for, and the recompute arm allocates none of it. A regression
// that allocated it unconditionally would be invisible to every bitwise check above.
// ================================================================================================

TEST(MaterialiseSteps, TheRecomputeArmAllocatesNoMaterialiseBuffer) {
  for (const Entry& e : fixtures_under_gate()) {
    const Program& p = program_for(e);
    adjoint::Options off;
    off.max_batch = 4;
    off.lane_tile = 4;
    adjoint::Options on = off;
    on.materialise_steps = true;
    const adjoint::Adjoint a_off(p, off), a_on(p, on);

    EXPECT_EQ(a_off.value_bytes(), a_on.value_bytes()) << e.name;
    EXPECT_EQ(a_off.edge_bytes(), a_on.edge_bytes()) << e.name;
    // The arm's whole footprint cost is in scratch_bytes(), and it is the sum over domains of
    // (steps - 1) * rows * L doubles.
    std::size_t want = 0;
    for (std::size_t d = 0; d < p.domains.size(); ++d) {
      const std::size_t last = p.groups[d].steps.size() - 1;
      want += last * static_cast<std::size_t>(p.domains[d].rows) * 4u * sizeof(double);
    }
    if (want == 0) continue;  // every group is one step: nothing to materialise
    EXPECT_EQ(a_on.scratch_bytes() - a_off.scratch_bytes(), want) << e.name;
    EXPECT_GT(a_on.scratch_bytes(), a_off.scratch_bytes()) << e.name;
    EXPECT_NE(a_on.describe().find("materialise_steps on"), std::string::npos) << e.name;
    EXPECT_NE(a_off.describe().find("materialise_steps off"), std::string::npos) << e.name;
  }
}
