// M3/G1 gate: every (scheme, variable) pair — six schemes on zero and logdf, Flat and Linear on
// forward — recorded on the 10-swap M1-style book (with DF outputs on a time grid): round-trip
// identity raw and after the standard passes; the replay after the passes, the expanded tape and
// exec::Interpreter (B = 1 and an odd batch) bitwise the double instantiation of the same maths
// at the record point and on a state ball; the linear-in-values schemes collapse to Affine rows
// with no Select (the log-DF subgraph holds Inputs, Affines and the −z·t / −∫f products only);
// MonotoneCubic records N > 0 Select nodes. Also: the single-region composite is bitwise the
// Curve<S, V> template, and Curve<Linear, zero> is bitwise the M1 curve of maths/curve/linear.hpp.
//
// An E0 TU (-ffp-contract=off): the double reference and the Rec record-point values are computed
// here; the interpreter's kernels are pinned by D25; the ball's draws come from an E0 TU.
#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "epykos/exec/interpreter.hpp"
#include "epykos/ir/evaluate.hpp"
#include "epykos/ir/expand.hpp"
#include "epykos/ir/program.hpp"
#include "epykos/ir/signature.hpp"
#include "epykos/maths/curve/linear.hpp"
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

const std::vector<double> df_times = {0.01, 0.3, 0.75, 1.0, 1.5, 2.5, 4.0, 6.0, 8.5, 9.99, 10.0, 15.0, 25.0, 30.0, 30.5};
constexpr int n_draws = 8;

// The states: the record point then the ball draws (a state per row).
std::vector<std::vector<double>> states(const fixtures::Book& book) {
  epykos::verify::BallOptions opt;
  opt.draws = n_draws;
  const epykos::verify::StateBall ball = epykos::verify::make_state_ball(book.z0.data(), fixtures::n_knots, opt);
  std::vector<std::vector<double>> out;
  out.emplace_back(book.z0.begin(), book.z0.end());
  for (int r = 0; r < ball.n_draws; ++r) out.emplace_back(ball.state(r), ball.state(r) + fixtures::n_knots);
  return out;
}

std::size_t mismatches(const std::vector<double>& a, const std::vector<double>& b) {
  std::size_t n = 0;
  for (std::size_t i = 0; i < a.size(); ++i) n += (bits(a[i]) != bits(b[i]));
  return n;
}

void expect_roundtrip(const Tape& tape, const std::string& what) {
  ir::InferStats stats;
  const ir::Program program = ir::infer(tape, &stats);
  ASSERT_NO_THROW(ir::validate(program)) << what;
  const Tape expanded = ir::expand(program);
  std::string diff;
  EXPECT_TRUE(ir::roundtrip_identical(tape, expanded, &diff)) << what << ":\n" << diff;
  EXPECT_EQ(expanded.op_histogram(), tape.op_histogram()) << what;
}

// The ops of the tainted nodes upstream of every Exp's argument (the log-DF subgraph).
std::vector<std::size_t> logdf_subgraph_histogram(const Tape& t, std::size_t* n_exps) {
  std::vector<char> mark(t.size(), 0);
  *n_exps = 0;
  for (std::size_t i = 0; i < t.size(); ++i) {
    const epykos::Node& n = t[static_cast<epykos::node_id>(i)];
    if (n.op != Op::Exp || !n.tainted) continue;
    ++*n_exps;
    const std::vector<char> up = epykos::test::upstream_of(t, n.a);
    for (std::size_t j = 0; j < up.size(); ++j) mark[j] |= up[j];
  }
  std::vector<std::size_t> h(static_cast<std::size_t>(epykos::op_count), 0);
  for (std::size_t i = 0; i < t.size(); ++i) {
    if (mark[i] && t[static_cast<epykos::node_id>(i)].tainted) ++h[static_cast<std::size_t>(t[static_cast<epykos::node_id>(i)].op)];
  }
  return h;
}

}  // namespace

TEST(CurveRecordE0, EverySchemeAndVariableRoundTripsAndReplaysBitwise) {
  const fixtures::Book& book = epykos::test::curve_book();
  ASSERT_EQ(book.n_swaps, 10);
  const std::vector<std::vector<double>> zs = states(book);
  for (const epykos::test::SchemeVariable& sv : epykos::test::all_scheme_variables()) {
    const curve::Composite c = epykos::test::single_composite(sv.scheme, sv.variable);
    const epykos::test::Recording raw = epykos::test::record_on(c, book, df_times, false, false);
    epykos::test::Recording passed = epykos::test::record_on(c, book, df_times, false, true);
    const int n_out = raw.n_outputs;
    ASSERT_EQ(static_cast<int>(raw.tape.num_outputs()), n_out) << sv.name();
    ASSERT_EQ(raw.tape.num_inputs(), static_cast<std::size_t>(fixtures::n_knots)) << sv.name();

    expect_roundtrip(raw.tape, sv.name() + " raw");
    expect_roundtrip(passed.tape, sv.name() + " passes");

    // The record-point values carried by Rec are the double instantiation's.
    const std::vector<double> oracle0 = epykos::test::oracle_on(c, book, zs[0].data(), df_times);
    {
      const std::vector<double> rp = epykos::replay(raw.tape, zs[0]);
      EXPECT_EQ(mismatches(rp, oracle0), 0u) << sv.name() << ": raw replay at the record point";
    }
    // Replay after the passes, the expanded tape, the IR evaluator and the interpreter at every state.
    const ir::Program program = ir::infer(passed.tape);
    const Tape expanded = ir::expand(program);
    epykos::Replayer rp(passed.tape), re(expanded);
    ir::Evaluator ev(program);
    epykos::exec::Options opt;
    opt.max_batch = 16;
    opt.lane_tile = 4;
    const epykos::exec::Interpreter in(program, opt);
    std::vector<double> out(static_cast<std::size_t>(n_out));
    std::size_t bad = 0;
    for (std::size_t s = 0; s < zs.size(); ++s) {
      const std::vector<double> oracle = epykos::test::oracle_on(c, book, zs[s].data(), df_times);
      rp.run(zs[s].data(), out.data());
      bad += mismatches(out, oracle);
      re.run(zs[s].data(), out.data());
      bad += mismatches(out, oracle);
      ev.run(zs[s].data(), out.data());
      bad += mismatches(out, oracle);
      in.run(zs[s].data(), 1, out.data());
      bad += mismatches(out, oracle);
    }
    EXPECT_EQ(bad, 0u) << sv.name() << ": replay / expanded / evaluator / interpreter B=1 vs double";
    // An odd batch: every lane its own state.
    {
      const int B = static_cast<int>(zs.size());
      std::vector<double> state(static_cast<std::size_t>(fixtures::n_knots) * static_cast<std::size_t>(B));
      std::vector<double> outb(static_cast<std::size_t>(n_out) * static_cast<std::size_t>(B));
      for (int k = 0; k < fixtures::n_knots; ++k) {
        for (int b = 0; b < B; ++b) state[static_cast<std::size_t>(k) * static_cast<std::size_t>(B) + static_cast<std::size_t>(b)] = zs[static_cast<std::size_t>(b)][static_cast<std::size_t>(k)];
      }
      in.run(state.data(), B, outb.data());
      std::size_t badb = 0;
      for (int b = 0; b < B; ++b) {
        const std::vector<double> oracle = epykos::test::oracle_on(c, book, zs[static_cast<std::size_t>(b)].data(), df_times);
        for (int o = 0; o < n_out; ++o) badb += (bits(outb[static_cast<std::size_t>(o) * static_cast<std::size_t>(B) + static_cast<std::size_t>(b)]) != bits(oracle[static_cast<std::size_t>(o)]));
      }
      EXPECT_EQ(badb, 0u) << sv.name() << ": interpreter B=" << B;
    }

    // Structure: the linear-in-values schemes collapse to Affine rows with no Select; the log-DF
    // subgraph holds nothing but Inputs, Affines, the −z·t / −∫f products and their negations.
    std::size_t n_exps = 0;
    const std::vector<std::size_t> h = logdf_subgraph_histogram(passed.tape, &n_exps);
    EXPECT_GT(n_exps, 0u);
    const std::size_t n_select = count_op(passed.tape, Op::Select);
    if (sv.scheme == curve::SchemeKind::monotone_cubic) {
      EXPECT_GT(n_select, 0u) << sv.name() << ": MonotoneCubic records Select nodes";
      EXPECT_GT(h[static_cast<std::size_t>(Op::Select)], 0u);
    } else {
      EXPECT_EQ(n_select, 0u) << sv.name() << ": no Select";
      for (int op = 0; op < epykos::op_count; ++op) {
        const Op o = static_cast<Op>(op);
        const bool allowed = o == Op::Input || o == Op::Affine || o == Op::Neg || o == Op::Mul;
        if (!allowed) EXPECT_EQ(h[static_cast<std::size_t>(op)], 0u) << sv.name() << ": " << epykos::to_string(o) << " in the log-DF subgraph";
      }
      if (sv.scheme != curve::SchemeKind::flat) EXPECT_GT(h[static_cast<std::size_t>(Op::Affine)], 0u) << sv.name() << ": affine rows";
    }
    std::cout << "[ " << sv.name() << " ] nodes raw " << raw.tape.size() << " -> passes " << passed.tape.size() << ", domains "
              << program.domains.size() << ", values " << program.num_values() << ", exps " << n_exps << ", selects " << n_select
              << ", affines " << count_op(passed.tape, Op::Affine) << '\n';
  }
}

TEST(CurveRecordE0, SingleRegionCompositeIsBitwiseTheTemplateAndLinearZeroIsTheM1Curve) {
  const double* kt = fixtures::knot_times.data();
  const double* z0 = fixtures::record_state.data();
  const std::vector<double> ts = {0.0, 0.01, 0.3, 0.5, 0.75, 1.0, 2.6, 4.0, 9.99, 10.0, 12.0, 29.0, 30.0, 30.02};
  auto same = [&](const auto& tc, curve::SchemeKind kind, curve::Variable var) {
    const curve::Composite cc = epykos::test::single_composite(kind, var);
    const auto st = tc.prepare(z0);
    const auto sc = cc.prepare(z0);
    for (double t : ts) {
      EXPECT_EQ(bits(tc.df(st, t)), bits(cc.df(sc, t))) << curve::to_string(kind) << "/" << curve::to_string(var) << " t=" << t;
      EXPECT_EQ(bits(tc.log_df(st, t)), bits(cc.log_df(sc, t))) << curve::to_string(kind) << "/" << curve::to_string(var) << " t=" << t;
    }
  };
  using curve::SchemeKind;
  using curve::Variable;
  same(curve::Curve<curve::Flat, Variable::zero>(kt, fixtures::n_knots), SchemeKind::flat, Variable::zero);
  same(curve::Curve<curve::Flat, Variable::logdf>(kt, fixtures::n_knots), SchemeKind::flat, Variable::logdf);
  same(curve::Curve<curve::Flat, Variable::forward>(kt, fixtures::n_knots), SchemeKind::flat, Variable::forward);
  same(curve::Curve<curve::Linear, Variable::zero>(kt, fixtures::n_knots), SchemeKind::linear, Variable::zero);
  same(curve::Curve<curve::Linear, Variable::logdf>(kt, fixtures::n_knots), SchemeKind::linear, Variable::logdf);
  same(curve::Curve<curve::Linear, Variable::forward>(kt, fixtures::n_knots), SchemeKind::linear, Variable::forward);
  same(curve::Curve<curve::Hermite, Variable::zero>(kt, fixtures::n_knots), SchemeKind::hermite, Variable::zero);
  same(curve::Curve<curve::Hermite, Variable::logdf>(kt, fixtures::n_knots), SchemeKind::hermite, Variable::logdf);
  same(curve::Curve<curve::NaturalCubic, Variable::zero>(kt, fixtures::n_knots), SchemeKind::natural_cubic, Variable::zero);
  same(curve::Curve<curve::NaturalCubic, Variable::logdf>(kt, fixtures::n_knots), SchemeKind::natural_cubic, Variable::logdf);
  same(curve::Curve<curve::MonotoneCubic, Variable::zero>(kt, fixtures::n_knots), SchemeKind::monotone_cubic, Variable::zero);
  same(curve::Curve<curve::MonotoneCubic, Variable::logdf>(kt, fixtures::n_knots), SchemeKind::monotone_cubic, Variable::logdf);
  same(curve::Curve<curve::BSpline, Variable::zero>(kt, fixtures::n_knots), SchemeKind::bspline, Variable::zero);
  same(curve::Curve<curve::BSpline, Variable::logdf>(kt, fixtures::n_knots), SchemeKind::bspline, Variable::logdf);

  // The M1 member: Curve<Linear, zero> is linear.hpp bitwise, on every M1 batch state and a dense grid.
  const curve::Curve<curve::Linear, Variable::zero> lz(kt, fixtures::n_knots);
  const curve::Composite lc = epykos::test::single_composite(SchemeKind::linear, Variable::zero);
  const fixtures::Batch batch = fixtures::make_m1_batch();
  std::size_t bad = 0, n = 0;
  for (int b = 0; b < batch.n_states; ++b) {
    double z[fixtures::n_knots];
    batch.state(b, z);
    const auto st = lz.prepare(z);
    const auto sc = lc.prepare(z);
    for (int i = 0; i <= 3100; ++i) {
      const double t = i / 100.0;
      const double ref = curve::linear::df(kt, z, fixtures::n_knots, t);
      bad += (bits(lz.df(st, t)) != bits(ref)) + (bits(lc.df(sc, t)) != bits(ref));
      n += 2;
    }
  }
  EXPECT_EQ(bad, 0u) << "of " << n;
}
