// M3/G1 gate: the M4 composite fixture (docs/WORKLOADS.md §M4: [0, 1Y) Linear on zero, [1Y, 10Y)
// MonotoneCubic on zero, [10Y, ∞) Linear on logdf, on the M1 knots) recorded on the 10-swap
// M1-style book with DF outputs on a time grid: round-trip identity raw and after the passes;
// the replay, the expanded tape, the IR evaluator and exec::Interpreter bitwise the double
// instantiation at the record point and on a state ball; select buckets appear only for the
// monotone region — every Select-bearing domain reads the region's knots (and its 10Y anchor)
// only, and a DF output depends on a Select exactly when its time lies strictly inside the
// monotone region off a knot; the linear regions' DFs read Affine rows only.
//
// An E0 TU (-ffp-contract=off).
#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <set>
#include <string>
#include <vector>

#include "epykos/exec/interpreter.hpp"
#include "epykos/ir/evaluate.hpp"
#include "epykos/ir/expand.hpp"
#include "epykos/ir/program.hpp"
#include "epykos/ir/signature.hpp"
#include "epykos/tape/passes.hpp"
#include "epykos/tape/replay.hpp"
#include "epykos/verify/differential.hpp"
#include "curve/curve_test_helpers.hpp"
#include "tape/tape_test_helpers.hpp"

namespace curve = epykos::curve;
namespace ir = epykos::ir;
namespace fixtures = epykos::fixtures;
using epykos::Op;
using epykos::Tape;
using epykos::test::bits;
using epykos::test::count_op;

namespace {

// Times: before the first knot, in region 0, on the 1Y boundary, inside the monotone region (off
// and on knots), just before 10Y, on 10Y, in region 2, beyond the last knot.
const std::vector<double> df_times = {0.01, 0.3, 0.75, 1.0, 1.5, 2.0, 2.5, 4.0, 5.0, 6.0, 8.5, 9.99, 10.0, 15.0, 20.0, 25.0, 30.0, 30.5};
bool in_monotone_region_off_knot(double t) {
  if (!(t > 1.0 && t < 10.0)) return false;
  for (double k : fixtures::knot_times) {
    if (t == k) return false;
  }
  return true;
}

struct Fixture {
  curve::Composite c;
  std::vector<double> v0;  // the record point in the composite's variables (log DF in region 2)
  std::vector<std::vector<double>> states;
  epykos::test::Recording raw, passed;
  ir::Program program;
};

const Fixture& fixture() {
  static const Fixture f = [] {
    Fixture x;
    x.c = epykos::test::m4_composite();
    const fixtures::Book& book = epykos::test::curve_book();
    x.v0.assign(book.z0.begin(), book.z0.end());
    for (int k = 9; k < fixtures::n_knots; ++k) x.v0[static_cast<std::size_t>(k)] = -book.z0[static_cast<std::size_t>(k)] * fixtures::knot_times[static_cast<std::size_t>(k)];
    // The recording's record point is the book's z0 (the inputs' values); region 2's knots are
    // then read as log DF values — the maths does not care what the numbers mean. The states are
    // the ball around z0 (E0 TU draws).
    epykos::verify::BallOptions opt;
    opt.draws = 12;
    const epykos::verify::StateBall ball = epykos::verify::make_state_ball(book.z0.data(), fixtures::n_knots, opt);
    x.states.emplace_back(book.z0.begin(), book.z0.end());
    for (int r = 0; r < ball.n_draws; ++r) x.states.emplace_back(ball.state(r), ball.state(r) + fixtures::n_knots);
    x.raw = epykos::test::record_on(x.c, book, df_times, false, false);
    x.passed = epykos::test::record_on(x.c, book, df_times, false, true);
    x.program = ir::infer(x.passed.tape);
    return x;
  }();
  return f;
}

std::size_t mismatches(const std::vector<double>& a, const std::vector<double>& b) {
  std::size_t n = 0;
  for (std::size_t i = 0; i < a.size(); ++i) n += (bits(a[i]) != bits(b[i]));
  return n;
}

}  // namespace

TEST(CurveCompositeE0, RoundTripsRawAndAfterThePasses) {
  const Fixture& f = fixture();
  for (const auto* rec : {&f.raw, &f.passed}) {
    ir::InferStats stats;
    const ir::Program program = ir::infer(rec->tape, &stats);
    ASSERT_NO_THROW(ir::validate(program));
    const Tape expanded = ir::expand(program);
    std::string diff;
    EXPECT_TRUE(ir::roundtrip_identical(rec->tape, expanded, &diff)) << diff;
    EXPECT_EQ(expanded.op_histogram(), rec->tape.op_histogram());
    std::cout << "[  infer   ] nodes " << stats.nodes << ", rows " << stats.boundaries << ", classes " << stats.classes << ", domains " << stats.domains << '\n';
  }
  std::cout << "[ program  ]\n" << ir::to_string(f.program);
}

TEST(CurveCompositeE0, ReplayEvaluatorAndInterpreterAreBitwiseTheDoubleMaths) {
  const Fixture& f = fixture();
  const fixtures::Book& book = epykos::test::curve_book();
  const int n_out = f.passed.n_outputs;
  const Tape expanded = ir::expand(f.program);
  epykos::Replayer rp(f.passed.tape), rr(f.raw.tape), re(expanded);
  ir::Evaluator ev(f.program);
  epykos::exec::Options opt;
  opt.max_batch = 16;
  opt.lane_tile = 8;
  const epykos::exec::Interpreter in(f.program, opt);
  std::vector<double> out(static_cast<std::size_t>(n_out));
  std::size_t bad = 0;
  for (const std::vector<double>& z : f.states) {
    const std::vector<double> oracle = epykos::test::oracle_on(f.c, book, z.data(), df_times);
    rr.run(z.data(), out.data());
    bad += mismatches(out, oracle);
    rp.run(z.data(), out.data());
    bad += mismatches(out, oracle);
    re.run(z.data(), out.data());
    bad += mismatches(out, oracle);
    ev.run(z.data(), out.data());
    bad += mismatches(out, oracle);
    in.run(z.data(), 1, out.data());
    bad += mismatches(out, oracle);
  }
  EXPECT_EQ(bad, 0u);
  const int B = static_cast<int>(f.states.size());
  std::vector<double> state(static_cast<std::size_t>(fixtures::n_knots) * static_cast<std::size_t>(B));
  std::vector<double> outb(static_cast<std::size_t>(n_out) * static_cast<std::size_t>(B));
  for (int k = 0; k < fixtures::n_knots; ++k) {
    for (int b = 0; b < B; ++b) state[static_cast<std::size_t>(k) * static_cast<std::size_t>(B) + static_cast<std::size_t>(b)] = f.states[static_cast<std::size_t>(b)][static_cast<std::size_t>(k)];
  }
  in.run(state.data(), B, outb.data());
  std::size_t badb = 0;
  for (int b = 0; b < B; ++b) {
    const std::vector<double> oracle = epykos::test::oracle_on(f.c, book, f.states[static_cast<std::size_t>(b)].data(), df_times);
    for (int o = 0; o < n_out; ++o) badb += (bits(outb[static_cast<std::size_t>(o) * static_cast<std::size_t>(B) + static_cast<std::size_t>(b)]) != bits(oracle[static_cast<std::size_t>(o)]));
  }
  EXPECT_EQ(badb, 0u) << "B = " << B;
}

TEST(CurveCompositeE0, SelectBucketsOnlyInTheMonotoneRegion) {
  const Fixture& f = fixture();
  const Tape& t = f.passed.tape;
  const ir::Program& p = f.program;
  EXPECT_GT(count_op(t, Op::Select), 0u);

  // (1) In the IR: every ROW of a domain with a Select step reads, transitively, Input rows 4..9
  // only — the 1Y..7Y knots of the monotone region and the 10Y knot its right anchor is converted
  // from. Per row, not per domain: the signature pass puts region 1's secants and the linear
  // regions' interpolations into one affine(#0;%0) domain, whose rows read different knots.
  std::vector<std::set<int>> reads(p.num_values());  // per value: the input ordinals it depends on
  for (std::size_t d = 0; d < p.domains.size(); ++d) {
    const ir::Domain& dom = p.domains[d];
    const ir::Group& g = p.groups[d];
    for (ir::row_id r = 0; r < dom.rows; ++r) {
      std::set<int>& mine = reads[static_cast<std::size_t>(dom.value_base + r)];
      for (const ir::Step& s : g.steps) {
        if (s.op == Op::Input) {
          for (std::size_t k = 0; k < p.inputs.size(); ++k) {
            if (p.inputs[k] == dom.value_base + r) mine.insert(static_cast<int>(k));
          }
        }
        for (const ir::Slot* sl : {&s.a, &s.b, &s.c}) {
          if (sl->kind == ir::SlotKind::Gather) {
            const ir::value_id v = p.gathers[static_cast<std::size_t>(sl->index)].index[static_cast<std::size_t>(r)];
            mine.insert(reads[static_cast<std::size_t>(v)].begin(), reads[static_cast<std::size_t>(v)].end());
          } else if (sl->kind == ir::SlotKind::Segment) {
            const ir::Segment& seg = p.segments[static_cast<std::size_t>(sl->index)];
            for (std::int32_t m = seg.offsets[static_cast<std::size_t>(r)]; m < seg.offsets[static_cast<std::size_t>(r) + 1]; ++m) {
              const ir::value_id v = seg.members[static_cast<std::size_t>(m)];
              mine.insert(reads[static_cast<std::size_t>(v)].begin(), reads[static_cast<std::size_t>(v)].end());
            }
          }
        }
      }
    }
  }
  int select_domains = 0, select_rows = 0;
  for (std::size_t d = 0; d < p.domains.size(); ++d) {
    bool has_select = false;
    for (const ir::Step& s : p.groups[d].steps) has_select = has_select || s.op == Op::Select;
    if (!has_select) continue;
    ++select_domains;
    const ir::Domain& dom = p.domains[d];
    select_rows += dom.rows;
    for (ir::row_id r = 0; r < dom.rows; ++r) {
      const std::set<int>& mine = reads[static_cast<std::size_t>(dom.value_base + r)];
      EXPECT_FALSE(mine.empty());
      for (int k : mine) {
        EXPECT_GE(k, 4) << "domain " << ir::shape_string(p, static_cast<ir::domain_id>(d)) << " row " << r << " reads knot " << k;
        EXPECT_LE(k, 9) << "domain " << ir::shape_string(p, static_cast<ir::domain_id>(d)) << " row " << r << " reads knot " << k;
      }
    }
  }
  EXPECT_GT(select_domains, 0);
  std::cout << "[ selects  ] " << count_op(t, Op::Select) << " Select nodes in " << select_domains << " domains, " << select_rows << " rows\n";

  // (2) On the tape: a DF output depends on a Select iff its time is inside (1Y, 10Y) off a knot.
  const std::size_t first_df = static_cast<std::size_t>(epykos::test::curve_book().n_swaps) + 1;
  for (std::size_t i = 0; i < df_times.size(); ++i) {
    const epykos::node_id out = t.outputs()[first_df + i];
    const std::vector<char> up = epykos::test::upstream_of(t, out);
    bool depends = false;
    for (std::size_t j = 0; j < up.size(); ++j) depends = depends || (up[j] && t[static_cast<epykos::node_id>(j)].op == Op::Select);
    EXPECT_EQ(depends, in_monotone_region_off_knot(df_times[i])) << "DF(" << df_times[i] << ")";
    if (depends) continue;
    // A linear region's DF: its log-DF subgraph is Inputs, Affines and the −z·t product / negation.
    const epykos::Node& e = t[out];
    ASSERT_EQ(e.op, Op::Exp);
    const std::vector<char> arg = epykos::test::upstream_of(t, e.a);
    for (std::size_t j = 0; j < arg.size(); ++j) {
      if (!arg[j] || !t[static_cast<epykos::node_id>(j)].tainted) continue;
      const Op o = t[static_cast<epykos::node_id>(j)].op;
      EXPECT_TRUE(o == Op::Input || o == Op::Affine || o == Op::Neg || o == Op::Mul) << "DF(" << df_times[i] << "): " << epykos::to_string(o);
    }
  }
  // (3) Every swap PV depends on a Select (every swap has a coupon inside the monotone region).
  for (int i = 0; i < epykos::test::curve_book().n_swaps; ++i) {
    const std::vector<char> up = epykos::test::upstream_of(t, t.outputs()[static_cast<std::size_t>(i)]);
    bool depends = false;
    for (std::size_t j = 0; j < up.size(); ++j) depends = depends || (up[j] && t[static_cast<epykos::node_id>(j)].op == Op::Select);
    const fixtures::Book& b = epykos::test::curve_book();
    bool has_coupon_inside = false;
    for (int r = b.row_begin[static_cast<std::size_t>(i)]; r < b.row_begin[static_cast<std::size_t>(i) + 1]; ++r) {
      has_coupon_inside = has_coupon_inside || in_monotone_region_off_knot(b.row_t_end[static_cast<std::size_t>(r)]);
    }
    EXPECT_EQ(depends, has_coupon_inside) << "swap " << i;
  }
}
