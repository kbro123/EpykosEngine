// M2/Q1 E0 gate: the differential tester on the M1 book (docs/WORKLOADS.md §M2 "State ball",
// DESIGN.md §11). The compiled program — exec::Interpreter over infer(record_m1(book)) — must
// reproduce price_book<double> bit for bit at all 256 draws of the ball z + 0.005·u, at B = 1
// and at B = 64 (four batched calls, each lane against its own scalar reference), plus the tape
// replay as a second reference. A harness that cannot fail is not a gate, so the last test
// perturbs one interpreter output by one ulp at one draw and checks the report names it.
//
// Why the reference is evaluated in this TU. The interpreter's arithmetic lives in libepykos,
// whose TUs are compiled with the preset's flags: under `release` the compiler may contract
// a·b + c into one fused multiply-add, which changes bits. The kernels that do the interpreter's
// arithmetic are pinned to -ffp-contract=off by name (src/exec/kernels_l*_e0.cpp, D25), so
// their bits are the same under every preset — but the reference they are compared with has to
// be contraction-free too, or the gate compares an E0 kernel against an E1 oracle and fails at
// the rounding level for the wrong reason. price_book<double> is a header template
// (fixtures/m1_price.hpp over maths/curve/linear.hpp and maths/swap/ois.hpp): it is
// instantiated in whichever TU calls it and takes that TU's flags. So it is instantiated and
// called HERE, in a *_e0_test.cpp TU, which the build compiles with -ffp-contract=off in every
// preset (tests/CMakeLists.txt) — the same for the record-point values Rec carries (record_m1
// is header-only for the same reason), the replay, and the ball's draws (src/verify/
// state_ball_e0.cpp, pinned). The result is a gate that holds under `release` as well as
// `reference`; had any of the arithmetic leaked into a non-pinned library TU, it would hold
// under the reference preset only.
#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

#include "epykos/exec/interpreter.hpp"
#include "epykos/fixtures/m1_book.hpp"
#include "epykos/fixtures/m1_differential.hpp"
#include "epykos/fixtures/record_m1.hpp"
#include "epykos/ir/program.hpp"
#include "epykos/ir/signature.hpp"
#include "epykos/rng/philox.hpp"
#include "epykos/tape/replay.hpp"
#include "epykos/tape/tape.hpp"
#include "epykos/verify/differential.hpp"

#ifndef EPYKOS_FP_CONTRACT_OFF
#error "an _e0_test.cpp TU must see EPYKOS_FP_CONTRACT_OFF"
#endif

namespace fixtures = epykos::fixtures;
namespace ir = epykos::ir;
namespace exec = epykos::exec;
namespace verify = epykos::verify;
using epykos::Replayer;
using epykos::Tape;
using verify::DifferentialOptions;
using verify::Report;
using verify::Tolerance;

namespace {

std::uint64_t bits(double v) {
  std::uint64_t u;
  std::memcpy(&u, &v, sizeof u);
  return u;
}

struct Fixture {
  fixtures::Book book;
  Tape tape;             // record_m1 in this TU: contraction-free record-point values
  ir::Program program;
  verify::StateBall ball;  // the WORKLOADS §M2 ball: 256 draws, rho 0.005, sub-stream 200000 + r
  verify::ScalarFn reference;  // price_book<double>, instantiated in this TU
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
    x.n_outputs = static_cast<int>(x.tape.num_outputs());
    return x;
  }();
  return f;
}

void print(const char* what, const Report& rep) { std::cout << "[  " << what << "  ] " << rep.summary() << '\n'; }

}  // namespace

TEST(M1DifferentialE0, TheBallIsTheWorkloadsBall) {
  const Fixture& f = fixture();
  const verify::StateBall& ball = f.ball;
  ASSERT_EQ(ball.n_inputs, fixtures::n_knots);
  ASSERT_EQ(ball.n_draws, 256);
  EXPECT_EQ(ball.options.rho, 0.0050);
  EXPECT_EQ(ball.options.seed, fixtures::default_seed);
  EXPECT_EQ(ball.options.substream_base, 200000u);
  for (int k = 0; k < fixtures::n_knots; ++k) EXPECT_EQ(bits(ball.centre[static_cast<std::size_t>(k)]), bits(f.book.z0[static_cast<std::size_t>(k)]));
  // Every draw is z + rho·u with u in [−1, 1), recomputed here (an E0 TU, like the generator)
  // and compared bitwise; no draw is the record point; all 256 are distinct.
  std::size_t distinct = 0;
  double max_dev = 0.0;
  for (int r = 0; r < ball.n_draws; ++r) {
    epykos::rng::Philox g(fixtures::default_seed, 200000u + static_cast<std::uint64_t>(r));
    bool at_centre = true;
    bool same_as_previous = r > 0;
    for (int k = 0; k < fixtures::n_knots; ++k) {
      const double u = g.uniform_range(-1.0, 1.0);
      const double expect = f.book.z0[static_cast<std::size_t>(k)] + 0.0050 * u;
      const double z = ball.state(r)[k];
      EXPECT_EQ(bits(z), bits(expect)) << "draw " << r << " knot " << k;
      const double dev = std::fabs(z - f.book.z0[static_cast<std::size_t>(k)]);
      EXPECT_LE(dev, 0.0050);
      if (dev > max_dev) max_dev = dev;
      if (dev != 0.0) at_centre = false;
      if (r > 0 && bits(z) != bits(ball.state(r - 1)[k])) same_as_previous = false;
    }
    EXPECT_FALSE(at_centre) << "draw " << r;
    if (!same_as_previous) ++distinct;
  }
  EXPECT_EQ(distinct, 256u);
  std::cout << "[  ball  ] 256 draws in [z - 0.005, z + 0.005]^12, max |z_k - z0_k| = " << max_dev << '\n';
}

TEST(M1DifferentialE0, InterpreterMatchesPriceBookBitwiseAtB1) {
  const Fixture& f = fixture();
  exec::Interpreter in(f.program);
  ASSERT_EQ(in.n_inputs(), fixtures::n_knots);
  ASSERT_EQ(in.n_outputs(), f.n_outputs);
  DifferentialOptions o;
  o.tolerance = Tolerance::e0();
  o.batch = 1;
  const Report rep = verify::differential(f.ball, f.n_outputs, f.reference, verify::interpreter_fn(in), o);
  print("B=1 ", rep);
  EXPECT_TRUE(rep.passed) << rep.summary();
  EXPECT_TRUE(rep.bitwise_equal);
  EXPECT_EQ(rep.mismatches, 0u);
  EXPECT_EQ(rep.violations, 0u);
  EXPECT_EQ(rep.n_draws, 256);
  EXPECT_EQ(rep.n_outputs, fixtures::n_swaps + 1);
  for (const verify::OutputStats& s : rep.outputs) EXPECT_TRUE(s.bitwise());
}

TEST(M1DifferentialE0, BatchedInterpreterMatches64ScalarReferencesBitwise) {
  // B = 64: four compiled calls of 64 lanes, each lane against the scalar reference of its draw.
  const Fixture& f = fixture();
  exec::Options opt;
  opt.max_batch = 64;
  opt.lane_tile = 32;  // the M1 best point at B = 64 (RESUME.md §5)
  opt.tile = 256;
  exec::Interpreter in(f.program, opt);
  DifferentialOptions o;
  o.tolerance = Tolerance::e0();
  o.batch = 64;
  const Report rep = verify::differential(f.ball, f.n_outputs, f.reference, verify::interpreter_fn(in), o);
  print("B=64", rep);
  EXPECT_TRUE(rep.passed) << rep.summary();
  EXPECT_TRUE(rep.bitwise_equal);
  EXPECT_EQ(rep.batch, 64);
}

TEST(M1DifferentialE0, ReplayMatchesPriceBookBitwise) {
  // The tape itself (after the E0 passes), replayed in this TU, against the same reference.
  const Fixture& f = fixture();
  Replayer rp(f.tape);
  DifferentialOptions o;
  o.tolerance = Tolerance::e0();
  const Report rep = verify::differential(f.ball, f.n_outputs, f.reference,
                                          verify::batch_of(verify::replayer_fn(rp), fixtures::n_knots, f.n_outputs), o);
  print("replay", rep);
  EXPECT_TRUE(rep.passed) << rep.summary();
}

TEST(M1DifferentialE0, AOneUlpChangeAtOneDrawIsCaughtAndLocated) {
  // Mutant: output 17 (swap 17) of draw 3 moved one ulp up on the compiled side. E0 must fail
  // with exactly that (output, draw) and that state; E1 at 4 ulps must accept it.
  const Fixture& f = fixture();
  exec::Interpreter in(f.program);
  const int mutant_output = 17;
  const int mutant_draw = 3;
  const double* target = f.ball.state(mutant_draw);
  const verify::BatchFn mutant = [&in, target](const double* state, int B, double* out) {
    in.run(state, B, out);
    for (int b = 0; b < B; ++b) {
      bool hit = true;
      for (int k = 0; k < fixtures::n_knots; ++k) {
        if (bits(state[static_cast<std::size_t>(k * B + b)]) != bits(target[k])) hit = false;
      }
      if (hit) {
        double& v = out[static_cast<std::size_t>(mutant_output * B + b)];
        v = std::nextafter(v, std::numeric_limits<double>::infinity());
      }
    }
  };
  for (int batch : {1, 64}) {
    DifferentialOptions o;
    o.tolerance = Tolerance::e0();
    o.batch = batch;
    const Report rep = verify::differential(f.ball, f.n_outputs, f.reference, mutant, o);
    EXPECT_FALSE(rep.passed) << rep.summary();
    EXPECT_FALSE(rep.bitwise_equal);
    EXPECT_EQ(rep.mismatches, 1u);
    EXPECT_EQ(rep.violations, 1u);
    EXPECT_EQ(rep.worst_output, mutant_output);
    EXPECT_EQ(rep.worst_draw, mutant_draw);
    EXPECT_EQ(rep.outputs[static_cast<std::size_t>(mutant_output)].mismatches, 1);
    EXPECT_LE(rep.max_ulps, 1.0);
    EXPECT_GT(rep.max_ulps, 0.0);
    ASSERT_EQ(rep.worst_state.size(), static_cast<std::size_t>(fixtures::n_knots));
    for (int k = 0; k < fixtures::n_knots; ++k) EXPECT_EQ(bits(rep.worst_state[static_cast<std::size_t>(k)]), bits(target[k]));
    o.tolerance = Tolerance::e1(4.0);
    const Report e1 = verify::differential(f.ball, f.n_outputs, f.reference, mutant, o);
    EXPECT_TRUE(e1.passed) << e1.summary();
    EXPECT_EQ(e1.mismatches, 1u);
    EXPECT_EQ(e1.violations, 0u);
  }
}
