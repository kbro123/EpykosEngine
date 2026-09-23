// M3/G1: select exports (tape/select_export.hpp) through the interpreter, and flip classification
// (DESIGN.md §6). On hand-made selects: mask, margin and arm gap come out as defined, a select
// whose arms differ at the boundary is a significant flip, a max whose arms cross is a degenerate
// one. On the MonotoneCubic curve: a flip of the Hyman limiter found by bisection along a state
// path has a vanishing arm gap and leaves the DF continuous (the limiter's selects are ties), the
// classifier says degenerate, and the pricing outputs and adjoint of a recording with exports are
// those of the recording without.
#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <iostream>
#include <vector>

#include "epykos/adjoint/adjoint.hpp"
#include "epykos/exec/interpreter.hpp"
#include "epykos/fixtures/m1_book.hpp"
#include "epykos/ir/program.hpp"
#include "epykos/ir/signature.hpp"
#include "epykos/maths/curve/curve.hpp"
#include "epykos/scalar/rec.hpp"
#include "epykos/tape/passes.hpp"
#include "epykos/tape/select_export.hpp"
#include "epykos/tape/tape.hpp"
#include "curve/curve_test_helpers.hpp"

namespace curve = epykos::curve;
namespace ir = epykos::ir;
namespace fixtures = epykos::fixtures;
using epykos::Op;
using epykos::Rec;
using epykos::Tape;

namespace {

struct Evaluation {
  ir::Program program;
  std::vector<double> out;
  int n_out = 0;
  Evaluation(const Tape& t) : program(ir::infer(t)), n_out(static_cast<int>(t.num_outputs())) {}
  const std::vector<double>& at(const std::vector<double>& z) {
    epykos::exec::Interpreter in(program);
    out.assign(static_cast<std::size_t>(n_out), 0.0);
    in.run(z.data(), 1, out.data());
    return out;
  }
};

}  // namespace

TEST(SelectExport, MaskMarginAndGapOfHandMadeSelects) {
  Tape t;
  {
    Tape::Scope scope(t);
    Rec x = epykos::make_input(t, 0.5), y = epykos::make_input(t, 0.7);
    epykos::register_output(t, select(x < 1.0, x, 2.0 * x));  // a jump of −x at x = 1
    epykos::register_output(t, max(x, y));                     // a tie: gap = margin
    epykos::register_output(t, abs(x));                        // x < 0 ? −x : x
    epykos::register_output(t, select(y >= x, 3.0, 4.0));
  }
  const epykos::SelectExports ex = epykos::export_selects(t);
  ASSERT_EQ(ex.n(), 4);
  EXPECT_EQ(ex.first_output, 4);
  EXPECT_EQ(t.num_outputs(), 16u);
  EXPECT_EQ(ex.selects[0].predicate_op, Op::CmpLt);
  EXPECT_EQ(ex.selects[3].predicate_op, Op::CmpGe);
  epykos::standard_passes(t);
  Evaluation run(t);
  {
    const std::vector<double>& o = run.at({0.5, 0.7});
    // select(x < 1, x, 2x): mask 1, margin 1 − x, gap x − 2x.
    EXPECT_EQ(ex.mask(0, o.data(), 1, 0), 1.0);
    EXPECT_DOUBLE_EQ(ex.margin(0, o.data(), 1, 0), 0.5);
    EXPECT_DOUBLE_EQ(ex.gap(0, o.data(), 1, 0), -0.5);
    // max(x, y) = select(x < y, y, x): mask 1, margin y − x, gap y − x.
    EXPECT_EQ(ex.mask(1, o.data(), 1, 0), 1.0);
    EXPECT_DOUBLE_EQ(ex.margin(1, o.data(), 1, 0), 0.2);
    EXPECT_DOUBLE_EQ(ex.gap(1, o.data(), 1, 0), 0.2);
    // abs(x) = select(x < 0, −x, x): mask 0, margin 0 − x, gap −2x.
    EXPECT_EQ(ex.mask(2, o.data(), 1, 0), 0.0);
    EXPECT_DOUBLE_EQ(ex.margin(2, o.data(), 1, 0), -0.5);
    EXPECT_DOUBLE_EQ(ex.gap(2, o.data(), 1, 0), -1.0);
    // select(y >= x, 3, 4): mask 1, margin y − x, gap −1.
    EXPECT_EQ(ex.mask(3, o.data(), 1, 0), 1.0);
    EXPECT_DOUBLE_EQ(ex.margin(3, o.data(), 1, 0), 0.2);
    EXPECT_DOUBLE_EQ(ex.gap(3, o.data(), 1, 0), -1.0);
    EXPECT_DOUBLE_EQ(o[0], 0.5);
    EXPECT_DOUBLE_EQ(o[1], 0.7);
  }
  // Across x = 1 the first select flips with a gap of about 1: significant. max(x, y) flips
  // between (0.69, 0.7) and (0.71, 0.7) with a gap of 0.01: degenerate at a threshold of 0.05.
  const std::vector<double> a = run.at({0.99, 0.7});
  const std::vector<double> b = run.at({1.01, 0.7});
  {
    const epykos::Flip f = epykos::classify_flip(ex.mask(0, a.data(), 1, 0), ex.margin(0, a.data(), 1, 0), ex.gap(0, a.data(), 1, 0),
                                                 ex.mask(0, b.data(), 1, 0), ex.margin(0, b.data(), 1, 0), ex.gap(0, b.data(), 1, 0), 0.05);
    EXPECT_TRUE(f.flipped);
    EXPECT_TRUE(f.significant);
    EXPECT_GT(f.margin_a, 0.0);
    EXPECT_LT(f.margin_b, 0.0);
  }
  const std::vector<double> c = run.at({0.69, 0.7});
  const std::vector<double> d = run.at({0.71, 0.7});
  {
    const epykos::Flip f = epykos::classify_flip(ex.mask(1, c.data(), 1, 0), ex.margin(1, c.data(), 1, 0), ex.gap(1, c.data(), 1, 0),
                                                 ex.mask(1, d.data(), 1, 0), ex.margin(1, d.data(), 1, 0), ex.gap(1, d.data(), 1, 0), 0.05);
    EXPECT_TRUE(f.flipped);
    EXPECT_FALSE(f.significant);
    const epykos::Flip g = epykos::classify_flip(ex.mask(2, c.data(), 1, 0), ex.margin(2, c.data(), 1, 0), ex.gap(2, c.data(), 1, 0),
                                                 ex.mask(2, d.data(), 1, 0), ex.margin(2, d.data(), 1, 0), ex.gap(2, d.data(), 1, 0), 0.05);
    EXPECT_FALSE(g.flipped);
    EXPECT_FALSE(g.significant);
  }
  // A tape without selects exports nothing.
  Tape none;
  {
    Tape::Scope scope(none);
    Rec x = epykos::make_input(none, 0.5);
    epykos::register_output(none, exp(x));
  }
  EXPECT_EQ(epykos::export_selects(none).n(), 0);
  EXPECT_EQ(none.num_outputs(), 1u);
}

TEST(SelectExport, MonotoneCubicFlipsAreDegenerateAndTheDFStaysContinuous) {
  // DF at a grid of times off a MonotoneCubic zero curve on the M1 knots, with exports.
  const std::vector<double> ts = {0.3, 1.5, 2.5, 4.0, 6.0, 8.5, 12.0, 25.0};
  const curve::Curve<curve::MonotoneCubic, curve::Variable::zero> c(fixtures::knot_times.data(), fixtures::n_knots);
  Tape t;
  {
    Tape::Scope scope(t);
    std::vector<Rec> z(static_cast<std::size_t>(fixtures::n_knots));
    for (int k = 0; k < fixtures::n_knots; ++k) z[static_cast<std::size_t>(k)] = epykos::make_input(t, fixtures::record_state[static_cast<std::size_t>(k)]);
    const auto st = c.prepare(z.data());
    for (double x : ts) epykos::register_output(t, c.df(st, x));
  }
  const epykos::SelectExports ex = epykos::export_selects(t);
  ASSERT_GT(ex.n(), 0);
  epykos::standard_passes(t);
  Evaluation run(t);
  std::cout << "[ exports  ] " << ex.n() << " selects, " << t.num_outputs() << " outputs\n";

  // A state path: knot 5 moves by up to 20 bp (the record point sits on a tie of |d_4| and
  // |d_5|, so a flip is certain). Bisect the first select that flips between the ends.
  auto state_at = [&](double lambda) {
    std::vector<double> z(fixtures::record_state.begin(), fixtures::record_state.end());
    z[5] += 0.002 * lambda;
    return z;
  };
  auto masks = [&](const std::vector<double>& o) {
    std::vector<double> m(static_cast<std::size_t>(ex.n()));
    for (int s = 0; s < ex.n(); ++s) m[static_cast<std::size_t>(s)] = ex.mask(s, o.data(), 1, 0);
    return m;
  };
  double lo = 0.0, hi = 1.0;
  std::vector<double> mlo = masks(run.at(state_at(lo)));
  std::vector<double> mhi = masks(run.at(state_at(hi)));
  ASSERT_NE(mlo, mhi) << "no select flips along the path";
  for (int it = 0; it < 60 && hi - lo > 1e-13; ++it) {
    const double mid = 0.5 * (lo + hi);
    const std::vector<double> mm = masks(run.at(state_at(mid)));
    if (mm == mlo) {
      lo = mid;
      mlo = mm;
    } else {
      hi = mid;
      mhi = mm;
    }
  }
  const std::vector<double> a = run.at(state_at(lo));
  const std::vector<double> b = run.at(state_at(hi));
  int flips = 0, significant = 0;
  double worst_gap = 0.0, worst_margin = 0.0;
  for (int s = 0; s < ex.n(); ++s) {
    const epykos::Flip f = epykos::classify_flip(ex.mask(s, a.data(), 1, 0), ex.margin(s, a.data(), 1, 0), ex.gap(s, a.data(), 1, 0),
                                                 ex.mask(s, b.data(), 1, 0), ex.margin(s, b.data(), 1, 0), ex.gap(s, b.data(), 1, 0), 1e-9);
    if (!f.flipped) continue;
    ++flips;
    significant += f.significant;
    worst_gap = std::fmax(worst_gap, std::fmax(std::fabs(f.gap_a), std::fabs(f.gap_b)));
    worst_margin = std::fmax(worst_margin, std::fmax(std::fabs(f.margin_a), std::fabs(f.margin_b)));
    // The margin crosses zero and the gap vanishes with it: a tie.
    EXPECT_LE(f.margin_a * f.margin_b, 0.0) << "select " << s;
    EXPECT_LT(std::fmax(std::fabs(f.gap_a), std::fabs(f.gap_b)), 1e-9) << "select " << s;
  }
  EXPECT_GT(flips, 0);
  EXPECT_EQ(significant, 0) << "every flip of the Hyman limiter is a tie";
  // The DFs are continuous across the flip.
  double worst_df = 0.0;
  for (std::size_t i = 0; i < ts.size(); ++i) worst_df = std::fmax(worst_df, std::fabs(a[i] - b[i]) / a[i]);
  EXPECT_LT(worst_df, 1e-10);
  std::cout << "[  flip    ] lambda in [" << lo << ", " << hi << "]: " << flips << " selects flip, worst |gap| " << worst_gap
            << ", worst |margin| " << worst_margin << ", worst relative DF jump " << worst_df << '\n';
}

TEST(SelectExport, ExportsDoNotChangeThePricingOutputsOrTheAdjoint) {
  const curve::Composite c = epykos::test::m4_composite();
  const fixtures::Book& book = epykos::test::curve_book();
  const std::vector<double> df_times = {1.5, 4.0, 8.5};
  const epykos::test::Recording plain = epykos::test::record_on(c, book, df_times, false, true);
  const epykos::test::Recording with = epykos::test::record_on(c, book, df_times, true, true);
  ASSERT_GT(with.exports.n(), 0);
  EXPECT_EQ(with.tape.num_outputs(), plain.tape.num_outputs() + static_cast<std::size_t>(with.exports.n_outputs()));
  const ir::Program pp = ir::infer(plain.tape);
  const ir::Program pw = ir::infer(with.tape);
  epykos::adjoint::Adjoint ap(pp), aw(pw);
  const std::vector<double> z(book.z0.begin(), book.z0.end());
  const int n_plain = plain.n_outputs;
  std::vector<double> ob_p(static_cast<std::size_t>(n_plain), 0.0), ob_w(static_cast<std::size_t>(aw.n_outputs()), 0.0);
  ob_p[static_cast<std::size_t>(book.n_swaps)] = 1.0;  // the book PV
  ob_w[static_cast<std::size_t>(book.n_swaps)] = 1.0;
  std::vector<double> out_p(static_cast<std::size_t>(n_plain)), out_w(static_cast<std::size_t>(aw.n_outputs()));
  std::vector<double> sb_p(static_cast<std::size_t>(fixtures::n_knots)), sb_w(static_cast<std::size_t>(fixtures::n_knots));
  ap.run(z.data(), 1, ob_p.data(), out_p.data(), sb_p.data());
  aw.run(z.data(), 1, ob_w.data(), out_w.data(), sb_w.data());
  for (int o = 0; o < n_plain; ++o) EXPECT_EQ(out_p[static_cast<std::size_t>(o)], out_w[static_cast<std::size_t>(o)]) << o;
  for (int k = 0; k < fixtures::n_knots; ++k) {
    EXPECT_NEAR(sb_p[static_cast<std::size_t>(k)], sb_w[static_cast<std::size_t>(k)], 1e-12 * std::fabs(sb_p[static_cast<std::size_t>(k)]) + 1e-9) << k;
  }
  // The exported masks are 0/1 and the margins finite.
  for (int s = 0; s < with.exports.n(); ++s) {
    const double m = with.exports.mask(s, out_w.data(), 1, 0);
    EXPECT_TRUE(m == 0.0 || m == 1.0) << s;
    EXPECT_TRUE(std::isfinite(with.exports.margin(s, out_w.data(), 1, 0))) << s;
    EXPECT_TRUE(std::isfinite(with.exports.gap(s, out_w.data(), 1, 0))) << s;
  }
}
