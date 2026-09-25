// M2/Q1 E1 differential test on the M1 book (docs/WORKLOADS.md §M2 "State ball", DESIGN.md
// §11): exec::Interpreter over infer(record_m1(book)) against price_book<double> at all 256
// draws of the ball z + 0.005·u, at B = 1 and B = 64.
//
// This TU is compiled with the preset's own flags (it is not an *_e0_test.cpp), so
// price_book<double> instantiated here is what the compiler makes of the templated maths under
// that preset: under `reference` (-ffp-contract=off everywhere) it is bit for bit the E0
// reference and the comparison comes out bitwise (asserted); under `release` the compiler may
// contract a·b + c into fused multiply-adds (the interpolation's (1 − w)·z_k + w·z_{k+1}; on
// GCC the leg folds acc + N·τ·K·DF too), which is E1: a rounding-level difference in each DF
// and coupon. The interpreter's kernels are pinned E0 (src/exec/kernels_l*_e0.cpp, D25), so
// what is measured here is exactly the contraction the preset applies to the reference.
//
// Tolerance: D26 (the project's E1 class for a difference of legs, D30):
//   |Δpv_i| <= 1e-12·(|fixed_i| + |float_i|),  |Δbook| <= 1e-12·Σ_i |pv_i|            (asserted)
//   |Δ| <= 1e-12·|value| wherever |value| >= 1e-2 × that scale                        (asserted)
// The harness default of 4 ulps is reported alongside, scaled and unscaled, with its violation
// counts, and is NOT asserted: on Apple clang 21 release the contraction of the reference alone
// reaches 13 ulps of the leg scale (879 of 256,256 values beyond 4 ulps), because the float
// coupon's fwd = (DF(s)/DF(e) − 1)/τ amplifies a 1-ulp DF difference by ~1/(z·τ) ≈ 25 before the
// leg sums it (D30). 4 ulps is a bound for a kernel that shares the reference's operations
// (the E0 gate holds bitwise), not for a reference the compiler was free to contract.
//
// Ad hoc runs (the "gtest flag" route): the Custom test is skipped unless at least one of the
// environment variables EPYKOS_DIFF_DRAWS, EPYKOS_DIFF_RHO, EPYKOS_DIFF_BATCH, EPYKOS_DIFF_ULPS,
// EPYKOS_DIFF_REL, EPYKOS_DIFF_TILE, EPYKOS_DIFF_LANE_TILE is set; e.g. 1,024 draws at B = 64:
//   EPYKOS_DIFF_DRAWS=1024 EPYKOS_DIFF_BATCH=64 build/release/tests/verify_m1_differential_test \
//       --gtest_filter='*Custom*'
// EPYKOS_DIFF_ULPS / EPYKOS_DIFF_REL set the asserted tolerance of that run (default D26's).
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "epykos/exec/interpreter.hpp"
#include "epykos/fixtures/m1_book.hpp"
#include "epykos/fixtures/m1_differential.hpp"
#include "epykos/fixtures/record_m1.hpp"
#include "epykos/ir/program.hpp"
#include "epykos/ir/signature.hpp"
#include "epykos/tape/tape.hpp"
#include "epykos/verify/differential.hpp"
#include "epykos/version.hpp"

namespace fixtures = epykos::fixtures;
namespace ir = epykos::ir;
namespace exec = epykos::exec;
namespace verify = epykos::verify;
using epykos::Tape;
using verify::DifferentialOptions;
using verify::Report;
using verify::Tolerance;

namespace {

constexpr double kD26 = 1e-12;

struct Fixture {
  fixtures::Book book;
  Tape tape;
  ir::Program program;
  verify::StateBall ball;      // docs/WORKLOADS.md §M2 defaults
  verify::ScalarFn reference;  // price_book<double> in this TU (the preset's flags)
  verify::ScaleFn leg_scale;   // D26 scales in this TU
  int n_outputs = 0;
};

const Fixture& fixture() {
  static const Fixture f = [] {
    Fixture x;
    x.book = fixtures::make_m1_book();
    x.tape = fixtures::record_m1(x.book);
    x.program = ir::infer(x.tape);
    x.ball = fixtures::m1_state_ball(x.book);
    x.reference = fixtures::m1_reference_fn(x.book);
    x.leg_scale = fixtures::m1_leg_scale_fn(x.book);
    x.n_outputs = static_cast<int>(x.tape.num_outputs());
    return x;
  }();
  return f;
}

// The D26 gate (asserted) and the 4-ulp picture (reported) at the given batch.
void run_e1(const Fixture& f, const verify::StateBall& ball, const exec::Interpreter& in, int batch, Tolerance gate) {
  const verify::BatchFn compiled = verify::interpreter_fn(in);
  DifferentialOptions o;
  o.batch = batch;

  // (1) D26 scaled: |Δ| <= 1e-12 · (|fixed| + |float|), |Δbook| <= 1e-12 · Σ|pv|.
  o.tolerance = gate;
  o.scale = f.leg_scale;
  const Report scaled = verify::differential(ball, f.n_outputs, f.reference, compiled, o);
  // (2) D26 literal where well posed: |Δ| <= 1e-12 · |value| for |value| >= 1e-2 × scale.
  int exempted = 0;
  o.scale = fixtures::m1_literal_mask_fn(f.book, 1e-2, &exempted);
  const Report literal = verify::differential(ball, f.n_outputs, f.reference, compiled, o);
  // (3) The harness default, 4 ulps, scaled and unscaled: reported only.
  o.tolerance = Tolerance::e1(4.0);
  o.scale = f.leg_scale;
  const Report ulps_scaled = verify::differential(ball, f.n_outputs, f.reference, compiled, o);
  o.scale = nullptr;
  const Report ulps_plain = verify::differential(ball, f.n_outputs, f.reference, compiled, o);

  std::cout << "[  flags    ] " << epykos::build_flags() << '\n'
            << "[  D26 scale] " << scaled.summary() << '\n'
            << "[  D26 lit. ] " << literal.summary() << " (" << exempted << " of "
            << static_cast<long>(ball.n_draws) * f.n_outputs << " values ill-conditioned, exempt)\n"
            << "[  4 ulps sc] " << ulps_scaled.summary() << '\n'
            << "[  4 ulps   ] " << ulps_plain.summary() << '\n';
  if (!ulps_plain.bitwise_equal) {
    int outputs_differing = 0;
    for (const verify::OutputStats& s : ulps_plain.outputs) outputs_differing += s.bitwise() ? 0 : 1;
    std::cout << "[  detail   ] outputs differing at some draw: " << outputs_differing << " of " << ulps_plain.n_outputs
              << "; book pv: max " << ulps_plain.outputs.back().max_ulps << " ulps of itself, "
              << ulps_scaled.outputs.back().max_ulps << " ulps of sum|pv|, rel " << scaled.outputs.back().max_rel
              << " of sum|pv|; worst swap rel to leg scale " << scaled.max_rel << " (output " << scaled.max_rel_output
              << ", draw " << scaled.max_rel_draw << ")\n";
  }

  EXPECT_TRUE(scaled.passed) << scaled.summary();
  EXPECT_EQ(scaled.violations, 0u);
  EXPECT_TRUE(literal.passed) << literal.summary();
  EXPECT_EQ(literal.violations, 0u);
  EXPECT_EQ(scaled.n_draws, ball.n_draws);
  EXPECT_EQ(scaled.batch, batch);
  // The four reports see the same values.
  EXPECT_EQ(scaled.mismatches, ulps_plain.mismatches);
  EXPECT_EQ(literal.mismatches, ulps_plain.mismatches);
  EXPECT_EQ(ulps_scaled.mismatches, ulps_plain.mismatches);
  EXPECT_LE(ulps_scaled.max_ulps, ulps_plain.max_ulps);  // a scale can only shrink an ulp count
#ifdef EPYKOS_FP_CONTRACT_OFF
  // The reference preset: this TU is contraction-free too, so the comparison is bitwise.
  EXPECT_TRUE(ulps_plain.bitwise_equal) << ulps_plain.summary();
#endif
}

bool env_set(const char* name) { return std::getenv(name) != nullptr; }
int env_int(const char* name, int dflt) { return env_set(name) ? std::atoi(std::getenv(name)) : dflt; }
double env_double(const char* name, double dflt) { return env_set(name) ? std::atof(std::getenv(name)) : dflt; }

}  // namespace

TEST(M1Differential, InterpreterVsPriceBookE1AtB1) {
  const Fixture& f = fixture();
  exec::Options opt;
  opt.tile = 512;  // the M1 best point at B = 1
  exec::Interpreter in(f.program, opt);
  ASSERT_EQ(in.n_outputs(), f.n_outputs);
  run_e1(f, f.ball, in, 1, Tolerance::e1_relative(kD26));
}

TEST(M1Differential, InterpreterVsPriceBookE1AtB64) {
  const Fixture& f = fixture();
  exec::Options opt;
  opt.max_batch = 64;
  opt.tile = 256;
  opt.lane_tile = 32;  // the M1 best point at B = 64
  exec::Interpreter in(f.program, opt);
  run_e1(f, f.ball, in, 64, Tolerance::e1_relative(kD26));
}

TEST(M1Differential, Custom) {
  const char* vars[] = {"EPYKOS_DIFF_DRAWS", "EPYKOS_DIFF_RHO",  "EPYKOS_DIFF_BATCH",    "EPYKOS_DIFF_ULPS",
                        "EPYKOS_DIFF_REL",   "EPYKOS_DIFF_TILE", "EPYKOS_DIFF_LANE_TILE"};
  bool any = false;
  for (const char* v : vars) any = any || env_set(v);
  if (!any) GTEST_SKIP() << "set EPYKOS_DIFF_{DRAWS,RHO,BATCH,ULPS,REL,TILE,LANE_TILE} to run an ad hoc differential";
  const Fixture& f = fixture();
  verify::BallOptions bo;
  bo.draws = env_int("EPYKOS_DIFF_DRAWS", bo.draws);
  bo.rho = env_double("EPYKOS_DIFF_RHO", bo.rho);
  const int batch = env_int("EPYKOS_DIFF_BATCH", 1);
  Tolerance gate = Tolerance::e1_relative(kD26);
  if (env_set("EPYKOS_DIFF_ULPS") || env_set("EPYKOS_DIFF_REL")) {
    gate = Tolerance::e1_relative(env_double("EPYKOS_DIFF_REL", 0.0), env_double("EPYKOS_DIFF_ULPS", 0.0));
  }
  exec::Options opt;
  opt.max_batch = batch;
  opt.tile = env_int("EPYKOS_DIFF_TILE", batch == 1 ? 512 : 256);
  opt.lane_tile = env_int("EPYKOS_DIFF_LANE_TILE", batch == 1 ? 1 : 32);
  std::cout << "[  custom   ] draws " << bo.draws << ", rho " << bo.rho << ", batch " << batch << ", gate " << gate.ulps
            << " ulps or " << gate.rel << " relative, tile " << opt.tile << ", lane_tile " << opt.lane_tile << '\n';
  const verify::StateBall ball = fixtures::m1_state_ball(f.book, bo);
  exec::Interpreter in(f.program, opt);
  run_e1(f, ball, in, batch, gate);
}
