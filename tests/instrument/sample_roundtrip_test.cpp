// M3/G2 gate: recording the instrument sample (five seeded trades of every Stage A blueprint,
// fixtures/instrument_sample.hpp) with Rec — the round-trip identity of the domain IR on the raw
// recording and after the E0 passes, the compounded coupons laid out as scan domains (D41)
// without hints, the averaged coupons as reductions (no scan), the evaluator bitwise the replay.
//
// A gate of scripts/mutation_test.sh (the name matches "roundtrip").
#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "epykos/fixtures/instrument_sample.hpp"
#include "epykos/ir/evaluate.hpp"
#include "epykos/ir/expand.hpp"
#include "epykos/ir/program.hpp"
#include "epykos/ir/signature.hpp"
#include "epykos/tape/replay.hpp"
#include "epykos/tape/tape.hpp"

namespace fixtures = epykos::fixtures;
namespace ir = epykos::ir;
namespace instrument = epykos::instrument;
using epykos::Tape;

namespace {

std::uint64_t bits(double v) {
  std::uint64_t u;
  std::memcpy(&u, &v, sizeof u);
  return u;
}

const fixtures::InstrumentSample& sample() {
  static const fixtures::InstrumentSample s = fixtures::make_instrument_sample();
  return s;
}

struct ScanSummary {
  int domains = 0, chains = 0, rows = 0, min_steps = 0, max_steps = 0;
  std::vector<std::string> names;
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
  }
  return s;
}

ir::Program expect_roundtrip(const Tape& tape, const std::string& what, ir::InferStats* stats_out = nullptr) {
  ir::InferStats stats;
  const ir::Program program = ir::infer(tape, &stats);
  EXPECT_NO_THROW(ir::validate(program)) << what;
  std::cout << "[  infer   ] " << what << ": nodes " << stats.nodes << ", rows " << stats.boundaries << ", classes " << stats.classes
            << ", domains " << stats.domains << ", chains " << stats.chains << " (" << stats.chain_nodes << " steps), scan classes "
            << stats.scan_classes << ", rounds " << stats.scan_rounds << (stats.scan_retries.empty() ? "" : "; retries: " + stats.scan_retries) << '\n';
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
  for (std::size_t k = 0; k < a.size() && k < b.size() && k < c.size(); ++k) {
    EXPECT_EQ(bits(a[k]), bits(b[k])) << what << ": evaluator output " << k;
    EXPECT_EQ(bits(a[k]), bits(c[k])) << what << ": expanded replay output " << k;
  }
  if (stats_out != nullptr) *stats_out = stats;
  return program;
}

// Compounded coupons with at least three projected days (the ones that can be a chain), and
// the projected days of every averaged coupon.
struct Counts {
  int compounded_chains = 0;   // compounded coupons with >= 3 projected days
  int compounded_days = 0;     // their projected days
  int averaged_days = 0;
  int term_coupons = 0;
};

Counts count(const fixtures::InstrumentSample& s) {
  Counts n;
  for (const instrument::Instrument& in : s.instruments) {
    for (const instrument::Leg& leg : in.legs) {
      for (const instrument::Coupon& c : leg.coupons) {
        const int days = c.obs_end - c.obs_begin;
        if (c.kind == instrument::CouponKind::RfrCompounded && days >= 3) {
          ++n.compounded_chains;
          n.compounded_days += days;
        }
        if (c.kind == instrument::CouponKind::RfrAveraged) n.averaged_days += days;
        if (c.kind == instrument::CouponKind::TermRate) ++n.term_coupons;
      }
    }
  }
  return n;
}

}  // namespace

TEST(SampleRoundtrip, TheSampleHasEveryMechanism) {
  const fixtures::InstrumentSample& s = sample();
  EXPECT_EQ(s.n_trades(), 17 * fixtures::sample_per_blueprint);   // M3-fix added the lookback and payment-lag variants
  EXPECT_EQ(s.n_inputs(), 44);
  const Counts n = count(s);
  EXPECT_GT(n.compounded_chains, 40);
  EXPECT_GT(n.compounded_days, 10000);
  EXPECT_GT(n.averaged_days, 1000);
  EXPECT_GT(n.term_coupons, 50);
  int seasoned = 0, realised_term = 0, futures = 0, deposits = 0;
  for (const instrument::Instrument& in : s.instruments) {
    if (in.kind == instrument::Kind::Future) ++futures;
    if (in.kind == instrument::Kind::Deposit) ++deposits;
    for (const instrument::Leg& leg : in.legs) {
      for (const instrument::Coupon& c : leg.coupons) {
        if (c.current) ++seasoned;
        if (c.realised) ++realised_term;
      }
    }
  }
  EXPECT_EQ(futures, 15);
  EXPECT_EQ(deposits, 20);   // four deposit blueprints
  EXPECT_GE(seasoned, 2 * 10 * 2);   // two seasoned trades per swap blueprint, both legs current (10 swap blueprints since M3-fix)
  EXPECT_GE(realised_term, 2 * 3);  // the seasoned EURIBOR swaps' and basis swaps' current coupons
  std::cout << "[ sample   ] " << s.n_trades() << " trades, " << n.compounded_chains << " compounded coupons with >= 3 projected days ("
            << n.compounded_days << " days), " << n.averaged_days << " averaged days, " << n.term_coupons << " term coupons, " << seasoned
            << " current coupons, " << realised_term << " realised term fixings\n";
}

TEST(SampleRoundtrip, RecordingAfterPassesIsAScanOverTheCompoundedCoupons) {
  const fixtures::InstrumentSample& s = sample();
  ir::InferStats stats;
  const Tape tape = fixtures::record_sample(s);
  const ir::Program p = expect_roundtrip(tape, "instrument sample (passes)", &stats);
  const ScanSummary sc = summarise(p);
  const Counts n = count(s);
  // The compounded coupons are scan domains: after CSE two coupons over the same days on the
  // same curve are one chain (the spot-starting trades of one blueprint share their periods),
  // so there are at most as many chains as chain-worthy coupons, and every projected day of a
  // seasoned coupon is its own step; the averaged coupons never form a chain (a running sum is
  // fold_sum's reduction, D41).
  EXPECT_GE(sc.domains, 1) << ir::to_string(p);
  EXPECT_GE(sc.chains, 20);
  EXPECT_LE(sc.chains, n.compounded_chains);
  EXPECT_LE(sc.rows, n.compounded_days);
  EXPECT_GE(sc.min_steps, 3);
  EXPECT_LE(sc.max_steps, 265);   // an annual coupon has about 250 business days
  bool product_step = false;
  for (const std::string& name : sc.names) product_step |= name.rfind("mul(^,", 0) == 0;
  EXPECT_TRUE(product_step) << ir::to_string(p);
  // Usually settles in one round; the M3-fix blueprints occasionally make a greedy chain read
  // another chain's later row, which ir::infer retries as straight-line steps for just that class
  // (InferStats::scan_rounds = "1 + retries after a scan class could not be laid out") — a
  // documented, self-correcting contingency, not a defect: expect_roundtrip above already proves
  // the resulting program is bitwise identical to the recording either way.
  EXPECT_LE(stats.scan_rounds, 2u) << stats.scan_retries;
  EXPECT_EQ(static_cast<int>(stats.chains), sc.chains);
  std::cout << "[  scan    ] " << sc.domains << " scan domains, " << sc.chains << " chains, " << sc.rows << " steps, " << sc.min_steps << ".."
            << sc.max_steps << " steps per chain; " << p.domains.size() << " domains, " << p.num_values() << " values\n";
  for (const std::string& name : sc.names) std::cout << "[  scan    ]   " << name << '\n';
}

TEST(SampleRoundtrip, RawRecordingRoundTripsToo) {
  const fixtures::InstrumentSample& s = sample();
  ir::InferStats stats;
  const Tape tape = fixtures::record_sample(s, false);
  const ir::Program p = expect_roundtrip(tape, "instrument sample (raw)", &stats);
  const ScanSummary sc = summarise(p);
  const Counts n = count(s);
  EXPECT_GE(sc.domains, 1) << ir::to_string(p);
  EXPECT_GE(sc.chains, n.compounded_chains) << "no CSE: every compounded coupon is a chain of its own (or more)";
  EXPECT_EQ(stats.scan_rounds, 1u) << stats.scan_retries;
  std::cout << "[  scan    ] raw: " << sc.domains << " scan domains, " << sc.chains << " chains, " << sc.rows << " steps\n";
}

TEST(SampleRoundtrip, SerialisedProgramCarriesTheScans) {
  const fixtures::InstrumentSample& s = sample();
  const ir::Program p = ir::infer(fixtures::record_sample(s));
  const std::string text = ir::serialize(p);
  const ir::Program q = ir::deserialize(text);
  EXPECT_TRUE(q == p);
  EXPECT_EQ(q.scans, p.scans);
}
