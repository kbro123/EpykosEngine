// M3/G3 gate: scan detection and the round-trip identity on the two scan fixtures (D41) — the
// RFR compounding book (fixtures/rfr_book.hpp: ten quarterly swaps whose float coupons compound
// daily as the natural product loop, three of them seasoned with realised fixings) and the
// affine scan (fixtures/affine_scan.hpp: x_{k+1} = a_k·x_k + b_k over 100 steps on 4 interleaved
// paths, with and without the path as outputs). The signature pass must find the scans WITHOUT
// hints on the raw recording and after the E0 passes, lay them out as recurrent scan domains,
// and the expander must unroll them back node for node. The M1 book must have no scan (its
// program is asserted unchanged in tests/ir/domain_chain_test.cpp; re-asserted here).
//
// A gate of scripts/mutation_test.sh (the name matches "roundtrip"): the expander's scan mutant
// is caught here.
//
// RE-ANCHORED 2026-09-27 (PRINCIPLES.md §5.2a case 3 — the algebra removed the structure two of
// these assertions were about). `epykos::compile` now runs `simplify` first, and the RFR book is
// exactly the fixture its telescope was written for: `acc <- acc·(1 + fwd·τ)` collapses to a
// ratio of endpoint discount factors, so the compounding scan the "(passes)" case was built to
// exhibit is mostly GONE. The round trip, the expander and the evaluator are untouched and still
// bitwise; only the shape of what they round-trip changed. The RFR book's RAW recording (156
// chains, 12,589 steps) and the affine scan on both its legs are unaffected — no rule matches
// `a_k·x_k + b_k` — so the expander's scan mutant is still exercised on plenty of chains. What
// each re-stated assertion used to say is at the test.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "epykos/fixtures/affine_scan.hpp"
#include "epykos/fixtures/m1_book.hpp"
#include "epykos/fixtures/record_m1.hpp"
#include "epykos/fixtures/rfr_book.hpp"
#include "epykos/fixtures/rfr_price.hpp"
#include "epykos/ir/evaluate.hpp"
#include "epykos/ir/expand.hpp"
#include "epykos/ir/program.hpp"
#include "epykos/ir/signature.hpp"
#include "epykos/tape/replay.hpp"
#include "epykos/tape/tape.hpp"

namespace fixtures = epykos::fixtures;
namespace ir = epykos::ir;
using epykos::Tape;

namespace {

std::uint64_t bits(double v) {
  std::uint64_t u;
  std::memcpy(&u, &v, sizeof u);
  return u;
}

struct ScanSummary {
  int domains = 0;      // scan domains
  int chains = 0;       // over all scan domains
  int rows = 0;         // over all scan domains
  int min_steps = 0, max_steps = 0;
  std::vector<std::string> names;
  std::vector<int> chains_of;  // per entry of `names`
  std::vector<int> rows_of;    // per entry of `names`

  // The index in `names` of the one scan domain called `name`, or -1.
  int find(const std::string& name) const {
    const auto it = std::find(names.begin(), names.end(), name);
    return it == names.end() ? -1 : static_cast<int>(it - names.begin());
  }
};

ScanSummary summarise(const ir::Program& p) {
  ScanSummary s;
  for (ir::domain_id d : ir::scan_domains(p)) {
    const ir::Domain& dom = p.domains[static_cast<std::size_t>(d)];
    const ir::Scan& sc = p.scans[static_cast<std::size_t>(dom.scan)];
    ++s.domains;
    s.chains += sc.chains();
    s.rows += dom.rows;
    for (std::int32_t c = 0; c < sc.chains(); ++c) {
      const int len = sc.chain_offsets[static_cast<std::size_t>(c) + 1] - sc.chain_offsets[static_cast<std::size_t>(c)];
      s.min_steps = (s.min_steps == 0) ? len : std::min(s.min_steps, len);
      s.max_steps = std::max(s.max_steps, len);
    }
    s.names.push_back(dom.name);
    s.chains_of.push_back(sc.chains());
    s.rows_of.push_back(dom.rows);
  }
  return s;
}

// infer, validate, round trip node for node, evaluator bitwise the replay at the record point.
ir::Program expect_roundtrip(const Tape& tape, const std::string& what, ir::InferStats* stats_out = nullptr) {
  ir::InferStats stats;
  const ir::Program program = ir::infer(tape, &stats);
  EXPECT_NO_THROW(ir::validate(program)) << what;
  std::cout << "[  infer   ] " << what << ": nodes " << stats.nodes << ", rows " << stats.boundaries << ", classes "
            << stats.classes << ", domains " << stats.domains << ", chains " << stats.chains << " (" << stats.chain_nodes
            << " steps), scan classes " << stats.scan_classes << ", rounds " << stats.scan_rounds << ", promoted "
            << stats.class_promoted << (stats.scan_retries.empty() ? "" : "; retries: " + stats.scan_retries) << '\n';
  const Tape expanded = ir::expand(program);
  EXPECT_EQ(expanded.size(), tape.size()) << what;
  std::string diff;
  EXPECT_TRUE(ir::roundtrip_identical(tape, expanded, &diff)) << what << ": expanded tape differs from the recording:\n" << diff;
  EXPECT_EQ(expanded.op_histogram(), tape.op_histogram()) << what;
  // The IR evaluator and the tape replay are header templates of this TU: bitwise.
  const std::vector<double> z = tape.input_values();
  const std::vector<double> a = epykos::replay(tape, z);
  const std::vector<double> b = ir::evaluate(program, z);
  const std::vector<double> c = epykos::replay(expanded, z);
  EXPECT_EQ(a.size(), b.size());
  for (std::size_t k = 0; k < a.size() && k < b.size() && k < c.size(); ++k) {
    EXPECT_EQ(bits(a[k]), bits(b[k])) << what << ": evaluator output " << k;
    EXPECT_EQ(bits(a[k]), bits(c[k])) << what << ": expanded replay output " << k;
  }
  if (stats_out != nullptr) *stats_out = stats;
  return program;
}

}  // namespace

// Used to assert that the compiled RFR book is a scan over EVERY float coupon: two scan domains,
// one chain per distinct accrual period plus one per seasoned swap's fixings, and one row per
// observation day of all of them — 51 chains over 4,382 rows on this fixture.
//
// The telescope took all but three of those chains. `acc <- acc·(1 + ((DF(s)/DF(e) − 1)/τ)·τ)`
// reduces to `acc·(DF(s)/DF(e))` step by step, and an UNSEASONED coupon starts at `acc = 1`, so
// `mul(1, div(d0,d1)) -> div(d0,d1)` makes the accumulator a Div and the telescope
// `mul(div(p,q), div(q,s)) -> div(p,s)` then runs the chain to a single endpoint ratio. A SEASONED
// coupon does not: its accumulator enters the projected days as the product of the realised
// fixings — a Mul, not a Div — so `mul(div(p,q), div(q,s))` never matches and the whole chain
// survives, together with the realised-fixings chain that feeds it.
//
// That is a real and reportable limit of the peephole rule set, not a property of the fixture:
// one reassociation, `mul(mul(x, div(p,q)), div(q,s)) -> mul(x, div(p,s))`, would collapse the
// seasoned coupons too. The gate below states it as the measurement it is — the number of
// surviving projected chains IS the number of seasoned coupons, checked, not asserted from the
// shape — so a rule set that later closes the gap fails here and gets re-measured.
TEST(ScanRoundtrip, RfrBookAfterPassesTelescopesEveryUnseasonedCoupon) {
  const fixtures::RfrBook book = fixtures::make_rfr_book();
  int float_coupons = 0, seasoned_coupons = 0;
  for (int i = 0; i < book.n_swaps; ++i) {
    float_coupons += fixtures::rfr_periods(book, i);
    seasoned_coupons += book.seasoned[static_cast<std::size_t>(i)] ? 1 : 0;
  }
  ir::InferStats stats;
  const Tape tape = fixtures::record_rfr(book);
  const ir::Program p = expect_roundtrip(tape, "RFR book (passes)", &stats);
  std::cout << "[ program  ] RFR book (passes)\n" << ir::to_string(p);
  const ScanSummary s = summarise(p);
  // The days of the seasoned coupons, which are the only ones left: their realised fixings
  // (start < 0) and the projected days that follow them in the same coupon.
  std::set<std::pair<int, int>> periods;
  int unseasoned_periods_days = 0, seasoned_projected_days = 0, realised_days = 0;
  for (int i = 0; i < book.n_swaps; ++i) {
    for (int j = 0; j < fixtures::rfr_periods(book, i); ++j) {
      const std::size_t r = static_cast<std::size_t>(fixtures::rfr_float_row_begin(book, i) + j);
      const int start = book.row_start_day[r], end = book.row_end_day[r];
      if (start < 0) {
        realised_days += -start;
        seasoned_projected_days += end;
      } else if (periods.insert({start, end}).second) {
        unseasoned_periods_days += end - start;
      }
    }
  }

  // Still two scan domains, and still the same two classes: the projected days (mul(^, @g)) and
  // the realised days of a seasoned swap's first coupon (mul(^, $c), a constant per step).
  EXPECT_EQ(s.domains, 2) << ir::to_string(p);
  const int projected = s.find("mul(^,@0)@scan");
  const int realised = s.find("mul(^,$0)@scan");
  ASSERT_GE(projected, 0) << ir::to_string(p);
  ASSERT_GE(realised, 0) << ir::to_string(p);

  // THE MEASUREMENT: one surviving projected chain per seasoned coupon and not one more, so the
  // telescope fired on every unseasoned coupon and on none of the seasoned ones.
  EXPECT_EQ(s.chains_of[static_cast<std::size_t>(projected)], seasoned_coupons) << ir::to_string(p);
  EXPECT_EQ(s.chains_of[static_cast<std::size_t>(realised)], seasoned_coupons) << ir::to_string(p);
  EXPECT_EQ(s.chains, 2 * seasoned_coupons) << ir::to_string(p);
  EXPECT_EQ(s.rows_of[static_cast<std::size_t>(projected)], seasoned_projected_days);
  // One row short of the book's fixings PER CHAIN: a realised chain also starts at `acc = 1`, so
  // its first step `mul(1, c_0)` is `c_0` and the chain begins at the second fixing. That is the
  // same `mul(x, 1) -> x`, and it is why the old `min_steps >= 10` is now 9.
  EXPECT_EQ(s.rows_of[static_cast<std::size_t>(realised)], realised_days - seasoned_coupons);
  EXPECT_EQ(s.rows, seasoned_projected_days + realised_days - seasoned_coupons);
  EXPECT_EQ(static_cast<int>(stats.chains), s.chains);
  EXPECT_EQ(stats.scan_rounds, 1u) << stats.scan_retries;
  // The surviving projected chains are still whole coupons: about ninety sub-periods each.
  EXPECT_GT(s.rows_of[static_cast<std::size_t>(projected)], 70 * seasoned_coupons)
      << "about ninety sub-periods per surviving coupon";
  EXPECT_GE(s.min_steps, 5);
  EXPECT_LE(s.max_steps, 93);
  // What the telescope removed: every coupon that is not a seasoned swap's first one.
  EXPECT_GT(float_coupons - seasoned_coupons, 0);
  EXPECT_GT(unseasoned_periods_days, 0) << "the unseasoned coupons really did have days to collapse";
  std::cout << "[  scan    ] " << s.domains << " scan domains, " << s.chains << " chains, " << s.rows << " steps, "
            << s.min_steps << ".." << s.max_steps << " steps per chain; the telescope removed "
            << (float_coupons - seasoned_coupons) << " of the " << float_coupons << " coupons ("
            << unseasoned_periods_days << " observation days over " << periods.size() << " distinct periods)\n";
}

TEST(ScanRoundtrip, RfrBookRawRecordingIsAScanToo) {
  const fixtures::RfrBook book = fixtures::make_rfr_book();
  ir::InferStats stats;
  const Tape tape = fixtures::record_rfr(book, false);
  const ir::Program p = expect_roundtrip(tape, "RFR book (raw)", &stats);
  const ScanSummary s = summarise(p);
  int float_coupons = 0;
  for (int i = 0; i < book.n_swaps; ++i) float_coupons += fixtures::rfr_periods(book, i);
  // No CSE: every coupon is its own chain (or two: a coupon starting at the valuation date
  // has a week of flat-extrapolated days whose step reads a knot directly, then the interpolated
  // days), the daily forward inlined into the step: mul(^, add(#, mul(div(sub(div(exp(...
  EXPECT_GE(s.domains, 2) << ir::to_string(p);
  EXPECT_GE(s.chains, float_coupons) << ir::to_string(p);
  EXPECT_EQ(stats.scan_rounds, 1u) << stats.scan_retries;
  bool inlined_step = false;
  for (const std::string& name : s.names) inlined_step |= name.rfind("mul(^,add(#0,mul(div(sub(div(exp(", 0) == 0;
  EXPECT_TRUE(inlined_step) << ir::to_string(p);
  std::cout << "[  scan    ] raw: " << s.domains << " scan domains, " << s.chains << " chains, " << s.rows << " steps, "
            << s.min_steps << ".." << s.max_steps << " steps per chain\n";
}

TEST(ScanRoundtrip, AffineScanIsOneDomainOfInterleavedChains) {
  const fixtures::AffineScanFixture f = fixtures::make_affine_scan();
  for (const bool output_path : {false, true}) {
    for (const bool passes : {false, true}) {
      const std::string what = std::string("affine scan (") + (output_path ? "path outputs" : "final outputs") + ", " +
                               (passes ? "passes" : "raw") + ")";
      ir::InferStats stats;
      const Tape tape = fixtures::record_affine_scan(f, output_path, passes);
      const ir::Program p = expect_roundtrip(tape, what, &stats);
      const ScanSummary s = summarise(p);
      EXPECT_EQ(s.domains, 1) << what << "\n" << ir::to_string(p);
      EXPECT_EQ(s.chains, f.n_paths) << what;
      EXPECT_EQ(s.rows, f.n_steps * f.n_paths) << what;
      EXPECT_EQ(s.min_steps, f.n_steps) << what;
      EXPECT_EQ(s.max_steps, f.n_steps) << what;
      EXPECT_EQ(stats.scan_rounds, 1u) << what;
      ASSERT_EQ(p.scans.size(), 1u);
      const ir::Scan& sc = p.scans[0];
      const ir::Domain& dom = p.domains[static_cast<std::size_t>(sc.domain)];
      // Chains are laid out chain-major although the recording interleaves the paths.
      for (std::int32_t c = 0; c < sc.chains(); ++c) {
        EXPECT_EQ(sc.chain_offsets[static_cast<std::size_t>(c)], c * f.n_steps);
      }
      const ir::Gather& carry = p.gathers[static_cast<std::size_t>(sc.carry_gather)];
      for (std::int32_t c = 0; c < sc.chains(); ++c) {
        EXPECT_EQ(carry.index[static_cast<std::size_t>(c * f.n_steps)], p.inputs[static_cast<std::size_t>(c)]) << "chain " << c << " starts at x_0 of path " << c;
      }
      if (passes) {
        // fold_sum turned a·x + drift + noise into a three-member Sum: a fixed-arity Sum step.
        EXPECT_EQ(dom.name, "sum(mul(@0,^),@1,@2)@scan") << what << "\n" << ir::to_string(p);
      } else {
        // On the raw recording the single-use noise term is an inner node of the step.
        EXPECT_EQ(dom.name, "add(add(mul(@1,^),@2),mul(@0,$0))@scan") << what << "\n" << ir::to_string(p);
      }
      if (passes && output_path) std::cout << "[ program  ] " << what << "\n" << ir::to_string(p);
    }
  }
}

TEST(ScanRoundtrip, M1BookHasNoScan) {
  const fixtures::Book book = fixtures::make_m1_book();
  for (const bool passes : {true, false}) {
    ir::InferStats stats;
    const Tape tape = passes ? fixtures::record_m1(book) : fixtures::record_m1_raw(book);
    const ir::Program p = ir::infer(tape, &stats);
    EXPECT_EQ(stats.chains, 0u) << (passes ? "passes" : "raw");
    EXPECT_TRUE(ir::scan_domains(p).empty());
    EXPECT_TRUE(ir::scan_class_domains(p).empty());
    EXPECT_TRUE(ir::recurrent_domains(p).empty());
    if (passes) {
      // Was 10 domains / 8 classes / 42,314 values. `simplify`'s `mul(x, 1) -> x` splits the DF
      // class (the t = 1 discount factor) and the swap class (side == +1 loses its multiply),
      // and the side == −1 half then merges with the constant-rate coupon class: 14 domains,
      // 9 classes, 42,799 values. tests/ir/domain_chain_test.cpp has the full account.
      EXPECT_EQ(p.domains.size(), 14u);
      EXPECT_EQ(stats.classes, 9u);
      EXPECT_EQ(p.num_values(), 42799u);
    }
  }
}

TEST(ScanRoundtrip, SerialisedProgramsCarryTheScan) {
  const fixtures::AffineScanFixture f = fixtures::make_affine_scan();
  const ir::Program p = ir::infer(fixtures::record_affine_scan(f, true));
  const std::string text = ir::serialize(p);
  EXPECT_EQ(text.substr(0, 12), "epykos-ir 3\n");
  const ir::Program q = ir::deserialize(text);
  EXPECT_TRUE(q == p);
  EXPECT_EQ(q.scans, p.scans);
  EXPECT_EQ(ir::serialize(q), text);
}
