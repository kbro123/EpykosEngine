// M2/Q6 review probes (evidence only; not landed): adjoint correctness at the edges.
#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "epykos/adjoint/adjoint.hpp"
#include "epykos/fixtures/m1_book.hpp"
#include "epykos/fixtures/m1_tangent.hpp"
#include "epykos/fixtures/record_m1.hpp"
#include "epykos/ir/program.hpp"
#include "epykos/ir/signature.hpp"
#include "epykos/maths/curve/linear.hpp"
#include "epykos/rng/philox.hpp"
#include "epykos/scalar/dual.hpp"
#include "epykos/scalar/rec.hpp"
#include "epykos/scalar/select.hpp"
#include "epykos/tape/passes.hpp"
#include "epykos/tape/tape.hpp"

namespace ir = epykos::ir;
namespace adjoint = epykos::adjoint;
namespace fixtures = epykos::fixtures;
using epykos::Dual;
using epykos::Rec;
using epykos::Tape;

namespace {

// Jacobian of every output by the adjoint, one lane per output (B <= max_batch), state broadcast.
std::vector<std::vector<double>> adj_jac(const adjoint::Adjoint& ad, const std::vector<double>& x, std::vector<double>* fwd = nullptr) {
  const int n_in = ad.n_inputs();
  const int n_out = ad.n_outputs();
  std::vector<std::vector<double>> J(static_cast<std::size_t>(n_out), std::vector<double>(static_cast<std::size_t>(n_in), 0.0));
  if (fwd) fwd->assign(static_cast<std::size_t>(n_out), 0.0);
  for (int o0 = 0; o0 < n_out; o0 += ad.max_batch()) {
    const int B = std::min(ad.max_batch(), n_out - o0);
    const std::size_t Bs = static_cast<std::size_t>(B);
    std::vector<double> st(static_cast<std::size_t>(n_in) * Bs), ob(static_cast<std::size_t>(n_out) * Bs, 0.0);
    std::vector<double> out(static_cast<std::size_t>(n_out) * Bs), sb(static_cast<std::size_t>(n_in) * Bs);
    for (std::size_t k = 0; k < static_cast<std::size_t>(n_in); ++k)
      for (std::size_t b = 0; b < Bs; ++b) st[k * Bs + b] = x[k];
    for (std::size_t b = 0; b < Bs; ++b) ob[(static_cast<std::size_t>(o0) + b) * Bs + b] = 1.0;
    ad.run(st.data(), B, ob.data(), out.data(), sb.data());
    for (std::size_t b = 0; b < Bs; ++b)
      for (std::size_t k = 0; k < static_cast<std::size_t>(n_in); ++k) J[static_cast<std::size_t>(o0) + b][k] = sb[k * Bs + b];
    if (fwd) for (std::size_t o = 0; o < static_cast<std::size_t>(n_out); ++o) (*fwd)[o] = out[o * Bs];
  }
  return J;
}

// ---- probe 1: an op whose two operands are the same value (div(g, g)): acc_div's two __restrict
// targets alias. Shapes: x/x (Input twice), (x+y)/(x+y) after CSE, x*x for comparison.
template <class S>
void same_operand_shapes(const S* in, S* out) {
  const S x = in[0], y = in[1], z = in[2];
  out[0] = x / x;                      // div(@0,@0)?  d/dx = 0
  out[1] = (x + y) / (x + y);          // after CSE: div(n, n)
  out[2] = x * x;                      // mul(@0,@0): d/dx = 2x
  out[3] = (x * y) / (x * y) * z;      // div then mul: d/dz = 1, d/dx = 0
  out[4] = (x + y) / (x + y) + z;      // Sum after passes
  out[5] = fma(x, x, y);               // fma(@0,@0,@1): d/dx = 2x
  out[6] = (x - y) / (x - y);          // d/dx = 0, d/dy = 0
}

constexpr int n_same_out = 7;

Tape record_same(const std::vector<double>& x0, bool passes) {
  Tape t;
  {
    Tape::Scope scope(t);
    Rec in[3];
    for (int k = 0; k < 3; ++k) in[k] = epykos::make_input(t, x0[static_cast<std::size_t>(k)]);
    Rec out[n_same_out];
    same_operand_shapes<Rec>(in, out);
    for (const Rec& r : out) epykos::register_output(t, r);
  }
  if (passes) epykos::standard_passes(t);
  t.validate();
  return t;
}

}  // namespace

TEST(ReviewProbe, SameOperandDivAliasing) {
  const std::vector<double> x0 = {0.7, 1.3, 2.1};
  for (bool passes : {false, true}) {
    const Tape t = record_same(x0, passes);
    const ir::Program p = ir::infer(t);
    std::cout << (passes ? "---- after passes\n" : "---- raw\n") << ir::to_string(p) << '\n';
    adjoint::Adjoint ad(p);
    std::vector<double> fwd;
    const auto J = adj_jac(ad, x0, &fwd);
    // Dual<3> reference.
    Dual<3> din[3], dout[n_same_out];
    for (int k = 0; k < 3; ++k) din[k] = Dual<3>::variable(x0[static_cast<std::size_t>(k)], k);
    same_operand_shapes<Dual<3>>(din, dout);
    for (int o = 0; o < n_same_out; ++o) {
      EXPECT_NEAR(fwd[static_cast<std::size_t>(o)], dout[o].v, 1e-12 * std::max(1.0, std::fabs(dout[o].v))) << "forward, output " << o;
      for (int k = 0; k < 3; ++k) {
        const double a = J[static_cast<std::size_t>(o)][static_cast<std::size_t>(k)];
        const double d = dout[o].d[static_cast<std::size_t>(k)];
        EXPECT_LE(std::fabs(a - d), 1e-12 * std::max(1.0, std::fabs(d)))
            << (passes ? "after passes" : "raw") << ", output " << o << ", input " << k << ": adjoint " << a << " vs dual " << d;
      }
    }
  }
}

// ---- probe 2: the curve at exact knot times, in both flat-extrapolation regions, and between
// knots (weights near 0 and 1): DF(t) = exp(-z(t) t) for many t, adjoint vs Dual<12>.
namespace {
constexpr int nk = fixtures::n_knots;

std::vector<double> probe_times() {
  std::vector<double> ts;
  const auto& kt = fixtures::knot_times;
  ts.push_back(0.0);
  ts.push_back(kt[0] * 0.5);           // before the first knot (flat)
  for (int k = 0; k < nk; ++k) ts.push_back(kt[k]);  // exactly at every knot
  for (int k = 0; k + 1 < nk; ++k) {
    ts.push_back(kt[k] + (kt[k + 1] - kt[k]) * 1e-9);   // w ~ 0
    ts.push_back(kt[k + 1] - (kt[k + 1] - kt[k]) * 1e-9);  // w ~ 1
    ts.push_back(0.5 * (kt[k] + kt[k + 1]));
  }
  ts.push_back(kt[nk - 1] * 1.5);      // beyond the last knot (flat)
  ts.push_back(std::nextafter(kt[nk - 1], 0.0));  // one ulp below the last knot: w rounds to 1?
  ts.push_back(std::nextafter(kt[4], 2.0));       // one ulp above knot 4 (t = 1)
  return ts;
}

template <class S>
std::vector<S> curve_shapes(const S* z) {
  std::vector<S> out;
  for (double t : probe_times()) out.push_back(epykos::curve::linear::df<S>(fixtures::knot_times.data(), z, nk, t));
  // a float coupon between two exact knots (t=1 and t=2) and one spanning the flat region
  {
    const S dfs = epykos::curve::linear::df<S>(fixtures::knot_times.data(), z, nk, 1.0);
    const S dfe = epykos::curve::linear::df<S>(fixtures::knot_times.data(), z, nk, 2.0);
    out.push_back(1e6 * 1.0 * ((dfs / dfe - 1.0) / 1.0) * dfe);
  }
  {
    const S dfs = epykos::curve::linear::df<S>(fixtures::knot_times.data(), z, nk, 29.5);
    const S dfe = epykos::curve::linear::df<S>(fixtures::knot_times.data(), z, nk, 31.0);
    out.push_back(1e6 * 1.5 * ((dfs / dfe - 1.0) / 1.5) * dfe);
  }
  {
    const S dfs = epykos::curve::linear::df<S>(fixtures::knot_times.data(), z, nk, 0.0);
    const S dfe = epykos::curve::linear::df<S>(fixtures::knot_times.data(), z, nk, 0.01);
    out.push_back(1e6 * 0.01 * ((dfs / dfe - 1.0) / 0.01) * dfe);
  }
  return out;
}

Tape record_curve(const std::vector<double>& z0, bool passes) {
  Tape t;
  {
    Tape::Scope scope(t);
    std::vector<Rec> in;
    for (double v : z0) in.push_back(epykos::make_input(t, v));
    for (const Rec& r : curve_shapes<Rec>(in.data())) epykos::register_output(t, r);
  }
  if (passes) epykos::standard_passes(t);
  t.validate();
  return t;
}
}  // namespace

TEST(ReviewProbe, CurveAtKnotsAndFlatRegions) {
  const fixtures::Book book = fixtures::make_m1_book();
  const std::vector<double> z0(book.z0.begin(), book.z0.end());
  for (bool passes : {false, true}) {
    const Tape t = record_curve(z0, passes);
    const ir::Program p = ir::infer(t);
    std::cout << (passes ? "---- curve after passes\n" : "---- curve raw\n") << ir::to_string(p) << '\n';
    adjoint::Adjoint ad(p);
    std::vector<double> fwd;
    const auto J = adj_jac(ad, z0, &fwd);
    std::vector<Dual<nk>> zd(nk);
    for (int k = 0; k < nk; ++k) zd[static_cast<std::size_t>(k)] = Dual<nk>::variable(z0[static_cast<std::size_t>(k)], k);
    const std::vector<Dual<nk>> dout = curve_shapes<Dual<nk>>(zd.data());
    ASSERT_EQ(dout.size(), J.size());
    std::size_t nonzero = 0;
    for (std::size_t o = 0; o < dout.size(); ++o) {
      EXPECT_EQ(std::memcmp(&fwd[o], &dout[o].v, sizeof(double)), 0) << "forward, output " << o << ": " << fwd[o] << " vs " << dout[o].v;
      for (int k = 0; k < nk; ++k) {
        const double a = J[o][static_cast<std::size_t>(k)];
        const double d = dout[o].d[static_cast<std::size_t>(k)];
        if (d != 0.0) ++nonzero;
        EXPECT_LE(std::fabs(a - d), 1e-12 * std::max(std::fabs(d), 1e-3))
            << (passes ? "after passes" : "raw") << ", output " << o << " (t = " << (o < probe_times().size() ? probe_times()[o] : -1.0)
            << "), knot " << k << ": adjoint " << a << " vs dual " << d;
      }
    }
    std::cout << "nonzero Jacobian entries: " << nonzero << " of " << dout.size() * nk << '\n';
  }
}

// ---- probe 3: multi-chunk partial batches on the M1 book (B = 17 / 3 / 13 at lane_tile 8 and 4),
// each lane bitwise the B = 1 run with that lane's state and seed.
TEST(ReviewProbe, MultiChunkPartialBatches) {
  const fixtures::Book book = fixtures::make_m1_book();
  const fixtures::Batch batch = fixtures::make_m1_batch();
  const Tape tape = fixtures::record_m1(book);
  const ir::Program p = ir::infer(tape);
  constexpr int n_out = fixtures::n_swaps + 1;
  for (int lane_tile : {8, 4, 5}) {
    adjoint::Options o;
    o.lane_tile = lane_tile;
    adjoint::Adjoint ad(p, o);
    for (int B : {3, 13, 17, 33}) {
      const std::size_t Bs = static_cast<std::size_t>(B);
      std::vector<double> state(12 * Bs), out_bar(static_cast<std::size_t>(n_out) * Bs), out(static_cast<std::size_t>(n_out) * Bs), sb(12 * Bs);
      epykos::rng::Philox g(fixtures::default_seed, 700000 + static_cast<std::uint64_t>(B));
      for (std::size_t b = 0; b < Bs; ++b) {
        std::vector<double> z(12);
        batch.state(static_cast<int>(b), z.data());
        for (std::size_t k = 0; k < 12; ++k) state[k * Bs + b] = z[k];
        for (std::size_t oo = 0; oo < static_cast<std::size_t>(n_out); ++oo) out_bar[oo * Bs + b] = g.uniform_range(-1.0, 1.0);
      }
      ad.run(state.data(), B, out_bar.data(), out.data(), sb.data());
      std::size_t bad = 0;
      for (std::size_t b = 0; b < Bs; ++b) {
        std::vector<double> z(12), ob1(static_cast<std::size_t>(n_out)), out1(static_cast<std::size_t>(n_out)), sb1(12);
        for (std::size_t k = 0; k < 12; ++k) z[k] = state[k * Bs + b];
        for (std::size_t oo = 0; oo < static_cast<std::size_t>(n_out); ++oo) ob1[oo] = out_bar[oo * Bs + b];
        ad.run(z.data(), 1, ob1.data(), out1.data(), sb1.data());
        for (std::size_t k = 0; k < 12; ++k) if (std::memcmp(&sb1[k], &sb[k * Bs + b], sizeof(double)) != 0) ++bad;
        for (std::size_t oo = 0; oo < static_cast<std::size_t>(n_out); ++oo) if (std::memcmp(&out1[oo], &out[oo * Bs + b], sizeof(double)) != 0) ++bad;
      }
      EXPECT_EQ(bad, 0u) << "lane_tile " << lane_tile << ", B " << B;
    }
  }
}

// ---- probe 4: coverage of the M1 Jacobian: which knots are exercised at all?
TEST(ReviewProbe, M1JacobianColumnCoverage) {
  const fixtures::Book book = fixtures::make_m1_book();
  const std::vector<double> z0(book.z0.begin(), book.z0.end());
  const fixtures::Jacobian j = fixtures::jacobian_forward_wide(book, z0.data());
  for (int k = 0; k < nk; ++k) {
    std::size_t nz = 0;
    double maxabs = 0.0;
    for (int o = 0; o < j.n_outputs; ++o) {
      if (j.at(o, k) != 0.0) ++nz;
      maxabs = std::max(maxabs, std::fabs(j.at(o, k)));
    }
    std::cout << "knot " << k << " (t=" << fixtures::knot_times[static_cast<std::size_t>(k)] << "): " << nz << " nonzero of " << j.n_outputs
              << ", max |J| " << maxabs << '\n';
  }
}

// ---- probe 5: a hand-built (validate-accepted) program in which both operands of a Div resolve
// to the same adjoint target: div(@0,@0) (one gather in both slots) and div(step0, step0). Not
// producible by ir::infer today (one gather per reference slot), but the IR is plain data and a
// rewrite / deserialised program may carry it. acc_div takes ta and tb as __restrict.
TEST(ReviewProbe, DivWithAliasedTargets) {
  const std::vector<double> x0 = {0.7, 1.3, 2.1};
  // (a) div(@0,@0): from the raw recording of x / x (domain 1: div(@0,@1)), point b at gather 0.
  {
    const Tape t = record_same(x0, false);
    ir::Program p = ir::infer(t);
    ir::Step& s = p.groups[1].steps[0];
    ASSERT_EQ(s.op, epykos::Op::Div);
    ASSERT_EQ(s.a.kind, ir::SlotKind::Gather);
    s.b = s.a;
    ASSERT_NO_THROW(ir::validate(p));
    std::cout << "---- div(@0,@0)\n" << ir::to_string(p) << '\n';
    for (int tile : {256, 1}) {
      adjoint::Options o;
      o.tile = tile;
      adjoint::Adjoint ad(p, o);
      std::vector<double> fwd;
      const auto J = adj_jac(ad, x0, &fwd);
      std::cout << "tile " << tile << ": x/x forward " << fwd[0] << ", d(x/x)/dx adjoint = " << J[0][0] << " (expected 0), d/dy " << J[0][1] << '\n';
      EXPECT_NEAR(fwd[0], 1.0, 0.0);
      EXPECT_LE(std::fabs(J[0][0]), 1e-12) << "tile " << tile << ": div(@0,@0) adjoint wrong (restrict aliasing in acc_div)";
    }
  }
  // (b) div(step0, step0): from the raw recording of (x-y)/(x-y) (domain 7: div(sub(@0,@1),sub(@2,@3))).
  {
    const Tape t = record_same(x0, false);
    ir::Program p = ir::infer(t);
    ir::Group& g = p.groups[7];
    ASSERT_EQ(g.steps.size(), 3u);
    ASSERT_EQ(g.steps[2].op, epykos::Op::Div);
    g.steps[2].b = ir::Slot{ir::SlotKind::Step, 0};
    ASSERT_NO_THROW(ir::validate(p));
    std::cout << "---- div(step0,step0)\n" << ir::to_string(p) << '\n';
    adjoint::Adjoint ad(p);
    std::vector<double> fwd;
    const auto J = adj_jac(ad, x0, &fwd);
    std::cout << "(x-y)/(x-y) forward " << fwd[6] << ", d/dx adjoint = " << J[6][0] << " (expected 0), d/dy " << J[6][1] << " (expected 0)\n";
    EXPECT_LE(std::fabs(J[6][0]), 1e-12) << "div(step0,step0): d/dx wrong (restrict aliasing in acc_div)";
    EXPECT_LE(std::fabs(J[6][1]), 1e-12) << "div(step0,step0): d/dy wrong (restrict aliasing in acc_div)";
  }
}

// ---- probe 5b: the same aliased Div with many lanes (the vectorised body of acc_div).
TEST(ReviewProbe, DivWithAliasedTargetsManyLanes) {
  const std::vector<double> x0 = {0.7, 1.3, 2.1};
  const Tape t = record_same(x0, false);
  ir::Program p = ir::infer(t);
  p.groups[1].steps[0].b = p.groups[1].steps[0].a;  // div(@0,@0)
  p.groups[7].steps[2].b = ir::Slot{ir::SlotKind::Step, 0};  // div(step0, step0)
  ir::validate(p);
  for (int lanes : {64, 32, 16, 8, 4, 3}) {
    adjoint::Options o;
    o.lane_tile = lanes;
    adjoint::Adjoint ad(p, o);
    const int B = lanes;
    const std::size_t Bs = static_cast<std::size_t>(B);
    std::vector<double> st(3 * Bs), ob(static_cast<std::size_t>(n_same_out) * Bs, 0.0), out(static_cast<std::size_t>(n_same_out) * Bs), sb(3 * Bs);
    for (std::size_t k = 0; k < 3; ++k) for (std::size_t b = 0; b < Bs; ++b) st[k * Bs + b] = x0[k];
    for (std::size_t b = 0; b < Bs; ++b) ob[0 * Bs + b] = 1.0;  // seed output 0 = x/x in every lane
    ad.run(st.data(), B, ob.data(), out.data(), sb.data());
    double worst = 0.0;
    for (std::size_t b = 0; b < Bs; ++b) worst = std::max(worst, std::fabs(sb[0 * Bs + b]));
    std::cout << "lanes " << lanes << ": d(x/x)/dx worst |adjoint| over lanes = " << worst << " (expected 0; -1/x = " << -1.0 / x0[0] << ")\n";
    EXPECT_LE(worst, 1e-12) << "lanes " << lanes << ": div(@0,@0) adjoint wrong";
    for (std::size_t b = 0; b < Bs; ++b) ob[0 * Bs + b] = 0.0;
    for (std::size_t b = 0; b < Bs; ++b) ob[6 * Bs + b] = 1.0;  // seed output 6 = (x-y)/(x-y)
    ad.run(st.data(), B, ob.data(), out.data(), sb.data());
    worst = 0.0;
    for (std::size_t b = 0; b < Bs; ++b) worst = std::max(worst, std::max(std::fabs(sb[0 * Bs + b]), std::fabs(sb[1 * Bs + b])));
    std::cout << "lanes " << lanes << ": d((x-y)/(x-y))/d{x,y} worst |adjoint| = " << worst << " (expected 0; 1/(x-y) = " << 1.0 / (x0[0] - x0[1]) << ")\n";
    EXPECT_LE(worst, 1e-12) << "lanes " << lanes << ": div(step0,step0) adjoint wrong";
  }
}
