// SEEDING THE PULL'S ACCUMULATOR (`adjoint::Options::seed_pull`, D102 §4 item 1): the two arms
// must be BITWISE equal, and this file is that gate.
//
// What the change is. `pull()` in src/adjoint/adjoint_e0.cpp used to zero a value's adjoint and
// then add every contribution to it:
//
//     for (l) dst[l] = 0.0;  then  for (reader) for (l) dst[l] += ...
//
// With `adjoint::Options::seed_pull` it instead takes the FIRST contribution in plan order as a
// store and `+=` the rest. D102 measured the zero fill at 165,245 of Stage A's 627,968
// reverse-only operations (26%), and 79.3% of Stage A's pull `Affine`s are `+0.0 + 1.0·x` — one
// reader at unit coefficient, for which the whole pull collapses to a single copy pass.
//
// The option is **OFF by default because it measured SLOWER** at the shipped `lane_tile = 8`
// (+10% to +13% of the whole call at B = 1 on both `stage_a` and `compare_ois`; the cost is per
// row and the saving per lane, so it only wins at `lane_tile` 32 — `Options::seed_pull` in
// adjoint.hpp carries the table). That changes nothing about this file: whether or not it is the
// default, the two forms must agree BITWISE, and a caller may set it.
//
// Why this is PRINCIPLES.md §5.2a CASE 1 and therefore stays bitwise. Both arms are the same
// pinned tape evaluated below the pin — the same program, the same plan, the same four CSR reader
// lists in the same order, the same contributions landing in the same positions of the same
// running sum. §5.2's below-the-pin column reads "may change roundings: no", so a divergence here
// is a DEFECT and never a tolerance to widen. The comparison is `memcmp`, because the one way the
// two forms can differ is invisible to every tolerance:
//
//     `0.0 + x == x` bitwise for every finite non-zero x, every infinity and (payload aside) every
//     quiet NaN. The forms differ in exactly one case — an accumulated ZERO one of whose
//     contributions is `-0.0`. Zero-then-add gives `+0.0`; seeding gives `-0.0`.
//
// That case is REACHABLE, and the gate is built around where it becomes observable rather than
// around a hope that it does not. `affine_coefs` is negative wherever the forward had a `Sub` or
// a negative weight, and `-1.0 · (+0.0)` is `-0.0`; under a zero seed every adjoint in the reverse
// pass is a zero, so the sign bit is the whole of the answer. The engine's carve-out is that
// `Op::Input`'s rule writes `state_bar[ordinal] = ybar` by COPY, so an Input domain's pull is an
// OUTPUT of the engine and keeps its zero start; every other consumer of a pulled v̄ accumulates
// into a step / gather / segment adjoint that `reverse()` has just memset to `+0.0`, where
// `(+0.0) + (-0.0)` is `+0.0` again.
//
// The seeds here therefore include the two that make a signed zero visible at all — the all-`+0.0`
// seed and D102 §1's mixed `-0.0` / `1.0` seed — which tests/adjoint/i1_gate_e0_test.cpp does NOT
// have: its "mixed" seed is `(o % 3) - 1`, i.e. {-1, +0.0, +1}, with no `-0.0` anywhere. Nothing
// else in the tree seeds `out_bar` with `-0.0`.
//
//
// Where the carve-out is observable, counted rather than argued
// ---------------------------------------------------------------------------------------------
// `census()` below walks every fixture's plan and counts the Input rows whose SEEDED pull could
// fold to `-0.0` where the zero start gives `+0.0` — the sites `adjoint.seed_input_pull` perturbs.
// The condition is narrow, because a single `+0.0` contribution anywhere in the fold washes the
// sign back: every reader of the row must be either an `Affine` at a negative coefficient (live
// under the all-`+0.0` seed) or an output reader (live under a `-0.0` seed), and one gather or
// `Sum` reader rules the row out. `census()`'s own comment derives that; the counts are printed
// per fixture, not assumed.
//
// Measured, and this corrects a conclusion this file first reached: `compare_ois` DOES have live
// rows, through the OUTPUT route — two of `compare_ois(short)`'s ten inputs are also outputs, and
// under the `-0.0` seeds their state adjoints come back `-0.0` with the carve-out removed. An
// earlier draft counted only the `Affine` route, found 0 everywhere, and concluded no recorded
// fixture could see the mutant. That was the wrong census, not a wrong fixture set.
//
// `affine_neg_program()` is kept anyway, and it is the other route as data: one input, read once,
// by an `Affine` of one member with coefficient `-1.0`, so it is live under the all-`+0.0` seed,
// which no recorded fixture is. It is here for the reason `div_xx_mix` and `neg` are in the i1
// gate — the engine has to be right for every shape `ir::validate` accepts — and so that the two
// routes are each covered by something. The `neg` program sits beside it as the NEGATIVE control:
// `Op::Neg`'s rule is `acc_minus`, which reaches `+0.0` from either sign, so that is the same
// maths through a route which cannot expose the sign, and its census count must be 0.
//
// The file name's two requirements are the ones adjoint_to_program_e0_test.cpp and
// i1_gate_e0_test.cpp record: `_e0_test` compiles this TU with -ffp-contract=off in every preset
// (D25), and `_e0_test$` matches scripts/mutation_test.sh's GATE_REGEX, so this file is selected
// as a gate at all.
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

#include "epykos/adjoint/adjoint.hpp"
#include "epykos/fixtures/affine_scan.hpp"
#include "epykos/fixtures/compare_ois.hpp"
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

// ---- the programs no fixture records ---------------------------------------------------------

// A one-row Input domain, then one row of `steps` (the i1 gate's helper, kept local so neither
// file depends on the other's table).
Program one_input_program(const std::string& name, std::vector<ir::Step> steps, std::vector<double> literals,
                          double input_value) {
  Program p;
  {
    ir::Domain d;
    d.name = "in";
    d.rows = 1;
    d.value_base = 0;
    p.domains.push_back(d);
    ir::Group g;
    g.domain = 0;
    g.steps.push_back(ir::Step{Op::Input, ir::Slot{ir::SlotKind::Input, -1}, {}, {}, {}});
    p.groups.push_back(g);
  }
  {
    ir::Domain d;
    d.name = name;
    d.rows = 1;
    d.value_base = 1;
    d.reads = {0};
    p.domains.push_back(d);
    ir::Group g;
    g.domain = 1;
    g.steps = std::move(steps);
    p.groups.push_back(g);
  }
  p.literals = std::move(literals);
  p.inputs = {0};
  p.input_values = {input_value};
  p.outputs = {1};
  return p;
}

// y = 0.0 + (-1.0)·x as an Affine of one member. THE shape the Input carve-out exists for: the
// input's only reader is an Affine coefficient of -1, so under `out_bar = +0.0` the seeded pull
// computes `-1.0 · (+0.0)` = `-0.0` and nothing follows it to wash the sign. The engine must
// answer `+0.0` for `d(-x)/dx · 0`, and only a memcmp can tell the two apart.
Program affine_neg_program() {
  Program p = one_input_program("affine_neg",
                               {ir::Step{Op::Affine, ir::Slot{ir::SlotKind::Segment, 0}, {}, {},
                                         ir::Slot{ir::SlotKind::Literal, 0}}},
                               {0.0}, 1.5);
  ir::Segment seg;
  seg.domain = 1;
  seg.offsets = {0, 1};
  seg.members = {0};
  seg.coefs = {-1.0};
  p.segments.push_back(seg);
  return p;
}

// y = -x through `Op::Neg`, whose rule is `acc_minus` into a freshly zeroed target: `0.0 - (+0.0)`
// and `0.0 - (-0.0)` are both `+0.0`, so this route CANNOT expose the sign. The negative control
// for the program above.
Program negate_program() {
  Program p = one_input_program("neg", {ir::Step{Op::Neg, ir::Slot{ir::SlotKind::Gather, 0}, {}, {}, {}}}, {}, 1.5);
  ir::Gather gather;
  gather.domain = 1;
  gather.index = {0};
  p.gathers.push_back(gather);
  return p;
}

// ---- states and seeds ------------------------------------------------------------------------

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

// Six seeds. The first two are the ones that can expose a signed zero at all; the `-0.0` / `1.0`
// alternation is D102 §1's, and the all-`-0.0` seed is the strongest form of it.
std::vector<std::vector<double>> out_bar_seeds(int n_out) {
  const std::size_t n = static_cast<std::size_t>(n_out);
  std::vector<std::vector<double>> seeds;
  seeds.emplace_back(n, 0.0);   // all +0.0: every adjoint is a zero and the sign bit is the answer
  seeds.emplace_back(n, -0.0);  // all -0.0
  {
    std::vector<double> m(n, 0.0);  // D102 §1's mixed seed
    for (std::size_t o = 0; o < n; ++o) m[o] = (o % 2 == 0) ? -0.0 : 1.0;
    seeds.push_back(m);
  }
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
  return seeds;
}

// ---- the fixture table -------------------------------------------------------------------------

struct Entry {
  const char* name;
  std::function<Program()> build;
  std::function<std::vector<std::vector<double>>(const Program&)> states;
  std::vector<int> batches;  // 5 and 7 are the split-lane widths (exec/lanes.hpp)
};

const std::vector<Entry>& fixtures_under_gate() {
  static const std::vector<Entry> kFixtures = {
      {"nearmiss(raw)", [] { return ir::infer(fixtures::record_nearmiss()); },
       [](const Program&) { return fixtures::nearmiss_states(3); }, {1, 4, 5}},
      {"nearmiss(passed)",
       [] {
         Tape t = fixtures::record_nearmiss();
         epykos::standard_passes(t);
         return ir::infer(t);
       },
       [](const Program&) { return fixtures::nearmiss_states(3); }, {1, 4, 5}},
      {"m1_book", [] { return ir::infer(fixtures::record_m1(fixtures::make_m1_book())); },
       [](const Program& p) { return ball_states(p.input_values, 2, 1e-4); }, {1, 4, 7}},
      {"compare_ois(short)",
       [] {
         fixtures::CompareOisOptions o;
         o.trades = 2;
         o.tenors = {"1Y", "2Y", "5Y", "10Y"};
         return ir::infer(fixtures::record_compare_ois(fixtures::make_compare_ois(o)).tape);
       },
       [](const Program& p) { return ball_states(p.input_values, 2, 1e-5); }, {1, 4, 5}},
      {"compare_ois(default)",
       [] {
         fixtures::CompareOisOptions o;
         o.trades = 2;
         return ir::infer(fixtures::record_compare_ois(fixtures::make_compare_ois(o)).tape);
       },
       [](const Program& p) { return ball_states(p.input_values, 2, 1e-5); }, {1, 4, 5}},
      {"affine_scan(final)", [] { return ir::infer(fixtures::record_affine_scan(fixtures::make_affine_scan(), false)); },
       [](const Program& p) { return ball_states(p.input_values, 2, 1e-3); }, {1, 4, 5}},
      {"affine_scan(path)", [] { return ir::infer(fixtures::record_affine_scan(fixtures::make_affine_scan(), true)); },
       [](const Program& p) { return ball_states(p.input_values, 1, 1e-3); }, {1, 4}},
      {"instrument_sample", [] { return ir::infer(fixtures::record_sample(fixtures::make_instrument_sample())); },
       [](const Program& p) { return ball_states(p.input_values, 2, 1e-4); }, {1, 4, 5}},
      {"rfr_book", [] { return ir::infer(fixtures::record_rfr(fixtures::make_rfr_book())); },
       [](const Program& p) { return ball_states(p.input_values, 1, 1e-4); }, {1, 4}},
      // The desk problem (PROBLEM.md Stage A): 26% of ITS reverse arithmetic is what this change
      // removes, so it is the one fixture the gate cannot be run without.
      {"stage_a(default)", [] { return ir::infer(fixtures::record_stage_a(fixtures::make_stage_a()).tape); },
       [](const Program& p) { return ball_states(p.input_values, 1, 1e-6); }, {1, 4}},
      {"affine_neg", [] { return affine_neg_program(); },
       [](const Program& p) { return std::vector<std::vector<double>>{p.input_values}; }, {1, 4, 5}},
      {"neg", [] { return negate_program(); },
       [](const Program& p) { return std::vector<std::vector<double>>{p.input_values}; }, {1, 4, 5}},
  };
  return kFixtures;
}

// Which Input rows of `p` could carry a `-0.0` out of a SEEDED pull — that is, where
// `adjoint.seed_input_pull` is observable in `state_bar`. Seeding stores the first contribution
// and adds the rest, so the fold lands on `-0.0` only if EVERY contribution is one: a single
// `+0.0` anywhere in it washes the sign back (`-0.0 + (+0.0)` is `+0.0`). The four reader kinds:
//
//   * a GATHER or Sum reader can never contribute `-0.0`. It reads an edge slot that `reverse()`
//     has memset to `+0.0` and that is only ever `+=` / `-=` accumulated, and neither operation
//     reaches `-0.0` from a `+0.0` base — `t += x` needs `t` already `-0.0`, and `t -= x` at
//     `t == x` gives `+0.0`. One such reader rules the row out;
//   * an OUTPUT reader contributes `out_bar[o]`, which is `-0.0` exactly when the caller's seed
//     is. So an input that is also an output is live, under a `-0.0` seed and not otherwise;
//   * an AFFINE reader at coef < 0 contributes `coef * (+0.0)` = `-0.0`. So it is live under the
//     all-`+0.0` seed, where every adjoint in the reverse pass is a zero.
//
// `live` is the structural condition (non-empty, every reader of one of those two kinds); the two
// sub-counts say which seed reaches it. BOTH are printed, because an earlier draft of this file
// counted only the Affine route, found 0 on every recorded fixture, and concluded the cover had to
// be hand-built — and then the mutant died on `compare_ois(short)` through the OUTPUT route. The
// narrow count was not wrong about what it measured; it was the wrong measurement.
struct Census {
  long long input_rows = 0;
  long long no_readers = 0;        // an input nothing reads: zero whichever form runs
  long long live = 0;              // the structural condition
  long long live_at_zero_seed = 0; // live and every reader an Affine at coef < 0
  long long live_at_minus_zero = 0;// live and at least one output reader: needs a -0.0 seed
};

Census census(const adjoint::AdjointPlan& plan, const Program& p) {
  Census c;
  for (std::size_t d = 0; d < p.domains.size(); ++d) {
    if (!plan.domains[d].is_input) continue;
    const ir::Domain& dom = p.domains[d];
    for (std::int32_t r = 0; r < dom.rows; ++r) {
      const std::size_t v = static_cast<std::size_t>(dom.value_base + r);
      ++c.input_rows;
      const std::int32_t n_out = plan.output_offsets[v + 1] - plan.output_offsets[v];
      const std::int32_t n_gat = plan.gather_offsets[v + 1] - plan.gather_offsets[v];
      const std::int32_t n_sum = plan.sum_offsets[v + 1] - plan.sum_offsets[v];
      const std::int32_t a0 = plan.affine_offsets[v], a1 = plan.affine_offsets[v + 1];
      if (n_out + n_gat + n_sum + (a1 - a0) == 0) {
        ++c.no_readers;
        continue;
      }
      if (n_gat + n_sum != 0) continue;  // one gather / Sum reader washes the sign
      bool all_negative = true;
      for (std::int32_t e = a0; e < a1; ++e) all_negative &= plan.affine_coefs[static_cast<std::size_t>(e)] < 0.0;
      if (!all_negative) continue;
      ++c.live;
      if (n_out == 0) ++c.live_at_zero_seed;
      else ++c.live_at_minus_zero;
    }
  }
  return c;
}

struct Row {
  std::string name;
  long long comparisons = 0;
  long long mismatches = 0;
  long long out_mismatches = 0;
  long long state_bar_mismatches = 0;
  Census cen;
};

}  // namespace

// ================================================================================================
// The gate: seed_pull on is bitwise seed_pull off, on every fixture the adjoint is gated on.
// ================================================================================================

TEST(SeedPull, SeedingTheAccumulatorIsBitwiseTheZeroStart) {
  std::vector<Row> rows;
  long long total_comparisons = 0, total_mismatches = 0, total_live = 0, total_live_zero = 0, total_live_minus = 0;

  for (const Entry& e : fixtures_under_gate()) {
    SCOPED_TRACE(e.name);
    const Program p = e.build();
    const int n_in = static_cast<int>(p.inputs.size());
    const int n_out = static_cast<int>(p.outputs.size());
    ASSERT_GT(n_in, 0) << e.name;
    ASSERT_GT(n_out, 0) << e.name;

    int max_batch = 1;
    for (const int B : e.batches) max_batch = std::max(max_batch, B);

    adjoint::Options ref_opt;
    ref_opt.max_batch = max_batch;
    ref_opt.seed_pull = false;  // THE REFERENCE ARM: the zero-then-accumulate this replaces
    adjoint::Options new_opt = ref_opt;
    new_opt.seed_pull = true;
    const adjoint::Adjoint ref(p, ref_opt), got(p, new_opt);

    Row row;
    row.name = e.name;
    row.cen = census(ref.plan(), p);
    total_live += row.cen.live;
    total_live_zero += row.cen.live_at_zero_seed;
    total_live_minus += row.cen.live_at_minus_zero;

    const std::vector<std::vector<double>> states = e.states(p);
    const std::vector<std::vector<double>> seeds = out_bar_seeds(n_out);
    for (const int B : e.batches) {
      const std::size_t Bs = static_cast<std::size_t>(B);
      for (std::size_t si = 0; si < states.size(); ++si) {
        std::vector<double> state(static_cast<std::size_t>(n_in) * Bs, 0.0);
        std::vector<double> out_bar(static_cast<std::size_t>(n_out) * Bs, 0.0);
        for (std::size_t se = 0; se < seeds.size(); ++se) {
          // Lane b takes a different state and a different seed, so the two arms are compared
          // over mixtures of seeds within one call as well as across calls.
          for (std::size_t b = 0; b < Bs; ++b) {
            const std::vector<double>& z = states[(si + b) % states.size()];
            const std::vector<double>& o = seeds[(se + b) % seeds.size()];
            for (int k = 0; k < n_in; ++k) state[static_cast<std::size_t>(k) * Bs + b] = z[static_cast<std::size_t>(k)];
            for (int k = 0; k < n_out; ++k) out_bar[static_cast<std::size_t>(k) * Bs + b] = o[static_cast<std::size_t>(k)];
          }
          std::vector<double> out_a(static_cast<std::size_t>(n_out) * Bs, 1.0), sb_a(static_cast<std::size_t>(n_in) * Bs, 1.0);
          std::vector<double> out_b(static_cast<std::size_t>(n_out) * Bs, 2.0), sb_b(static_cast<std::size_t>(n_in) * Bs, 2.0);
          ref.run(state.data(), B, out_bar.data(), out_a.data(), sb_a.data());
          got.run(state.data(), B, out_bar.data(), out_b.data(), sb_b.data());

          int shown = 0;
          auto compare = [&](const std::vector<double>& a, const std::vector<double>& b, const char* what,
                             long long* counter) {
            for (std::size_t i = 0; i < a.size(); ++i) {
              ++row.comparisons;
              if (same_bits(a[i], b[i])) continue;
              ++row.mismatches;
              ++*counter;
              if (shown++ < 4) {
                ADD_FAILURE() << e.name << ": " << what << " element " << i << " (B " << B << ", state " << si
                              << ", seed " << se << ") is " << bits_of(b[i]) << " with seed_pull on, "
                              << bits_of(a[i]) << " with it off. This gate is PRINCIPLES.md §5.2a case 1 "
                              << "(both arms are the same pinned tape below the pin): BITWISE, never a "
                              << "tolerance. Fix pull() in src/adjoint/adjoint_e0.cpp.";
              }
            }
          };
          compare(out_a, out_b, "out", &row.out_mismatches);
          compare(sb_a, sb_b, "state_bar", &row.state_bar_mismatches);
        }
      }
    }
    total_comparisons += row.comparisons;
    total_mismatches += row.mismatches;
    rows.push_back(row);
  }

  std::printf("\n[ seed_pull ] %-22s %12s %8s %7s %12s %7s %6s %8s %8s\n", "fixture", "comparisons", "mismatch",
              "in out", "in state_bar", "in rows", "live", "at +0.0", "at -0.0");
  for (const Row& r : rows) {
    std::printf("[ seed_pull ] %-22s %12lld %8lld %7lld %12lld %7lld %6lld %8lld %8lld\n", r.name.c_str(),
                r.comparisons, r.mismatches, r.out_mismatches, r.state_bar_mismatches, r.cen.input_rows, r.cen.live,
                r.cen.live_at_zero_seed, r.cen.live_at_minus_zero);
  }
  std::printf("[ seed_pull ] TOTAL %lld comparisons, %lld mismatches; %lld Input rows whose SEEDED pull could\n",
              total_comparisons, total_mismatches, total_live);
  std::printf("[ seed_pull ] fold to -0.0 where the zero start gives +0.0 -- the sites `adjoint.seed_input_pull`\n");
  std::printf("[ seed_pull ] perturbs. \"at +0.0\" fires under the all-+0.0 seed (every reader an Affine at\n");
  std::printf("[ seed_pull ] coef < 0); \"at -0.0\" needs a -0.0 seed (the input is also an output). 0 in the\n");
  std::printf("[ seed_pull ] total column would mean this gate cannot see its own mutant.\n");
  std::cout.flush();

  EXPECT_EQ(total_mismatches, 0);
  EXPECT_GT(total_comparisons, 0);
  // The gate must be able to observe the one case the two forms differ in. If this is ever zero
  // the mutant below is unobservable and the carve-out is being asserted by nothing.
  EXPECT_GT(total_live, 0) << "no fixture has an Input row whose seeded pull could carry -0.0, so this gate "
                              "cannot see adjoint.seed_input_pull. Add a program that does, as affine_neg is.";
  // Both routes must be covered, not just whichever one a fixture happens to have: the Affine
  // route fires under the all-+0.0 seed and the output route needs a -0.0 seed, and a gate that
  // held only one of them would go blind the day that fixture's recording changed.
  EXPECT_GT(total_live_zero, 0) << "no live row reachable from the all-+0.0 seed (an Input row every reader of "
                                   "which is an Affine at coef < 0): affine_neg is supposed to be one";
  EXPECT_GT(total_live_minus, 0) << "no live row reachable from a -0.0 seed (an input that is also an output)";
}

// ================================================================================================
// The carve-out, stated as its own assertion on the one program that exhibits it.
// ================================================================================================

TEST(SeedPull, TheInputPullKeepsItsZeroStartSoAStateAdjointIsNeverMinusZero) {
  struct Case {
    const char* name;
    Program p;
    long long live;  // the Affine route can expose the sign; the Neg route cannot
  };
  std::vector<Case> cases;
  cases.push_back({"affine_neg", affine_neg_program(), 1});
  cases.push_back({"neg", negate_program(), 0});

  for (const Case& cse : cases) {
    SCOPED_TRACE(cse.name);
    adjoint::Options o;
    o.max_batch = 1;
    // EXPLICIT, and it has to be: `seed_pull` is false by default (it measured slower at the
    // shipped lane_tile), and with it false there is no elision to carve out — the mutant would
    // have nothing to do and this test would pass against a broken carve-out.
    o.seed_pull = true;
    const adjoint::Adjoint ad(cse.p, o);
    ASSERT_EQ(ad.n_inputs(), 1);
    ASSERT_EQ(ad.n_outputs(), 1);
    const Census cen = census(ad.plan(), cse.p);
    EXPECT_EQ(cen.live, cse.live) << cse.name;
    EXPECT_EQ(cen.live_at_zero_seed, cse.live) << cse.name << ": and it must be the all-+0.0-seed route";

    const double state = cse.p.input_values[0];
    for (const double seed : {0.0, -0.0}) {
      double out = 1.0, sb = 1.0;
      ad.run(&state, 1, &seed, &out, &sb);
      // d(-x)/dx · (±0.0) is a zero, and the engine's answer is `+0.0`: the Input pull starts at
      // +0.0 and `0.0 + (-0.0)` is `+0.0`. `-0.0` here means the carve-out is gone.
      EXPECT_TRUE(same_bits(sb, 0.0)) << cse.name << ": state_bar is " << bits_of(sb) << " for out_bar "
                                      << bits_of(seed) << ", must be " << bits_of(0.0)
                                      << ". The Input domain's pull must keep its zero start "
                                      << "(src/adjoint/adjoint_e0.cpp, Options::seed_pull's carve-out).";
      EXPECT_TRUE(same_bits(out, -state)) << cse.name << ": forward output moved";
    }
  }
}
