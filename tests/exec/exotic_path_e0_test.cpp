// The exotics fixture's correctness gate (`docs/EXOTICS_KILL_TEST.md`, D1's revisit clause).
//
// `include/epykos/fixtures/exotic_path.hpp` is the first recorded PATH-DEPENDENT payoff in the
// repository: a GBM path grid carrying an arithmetic Asian, a geometric Asian and an up-and-out
// barrier call. `tools/exotics/exoticsprobe` measures it; this gate is what makes those
// measurements worth reading, because a ratio against a wrong answer is worthless.
//
// WHICH COMPARISONS ARE WHICH SIDE OF THE PIN (PRINCIPLES.md §5.2a), decided per test and not in
// one breath:
//
//   * interpreter against the tape REPLAY, catalogue on against off, one (tile, lane_tile) against
//     another, a B = 64 lane against its own B = 1 run: both sides evaluate the SAME pinned tape,
//     so case 1, and every one of them is asserted BITWISE. A failure here is a defect.
//   * interpreter against `exotic_path_evaluate<double>`: case 2. It CROSSES the pin -- the
//     reference is the recording as written, the engine side is the collapsed tape -- so it is a
//     relative tolerance, and the measured worst divergence is printed so drift stays visible.
//   * the geometric Asian against its exact CLOSED FORM: neither side of the pin, because it is
//     not the engine against itself. It is external maths (Black-Scholes, standard library only --
//     no dependency, so no D12 entry), and the only defensible gate for a Monte Carlo estimate is
//     its own sampling error. See kGeomSeeds below for why it is asserted over eight seeds and not
//     on the one that WORKLOADS.md fixes.
//
// This TU is compiled with -ffp-contract=off in every preset (the _e0_test.cpp convention), which
// is what lets the case-1 comparisons be bitwise under the release preset as well as the reference
// one (D25: the interpreter's kernels are pinned the same way inside libepykos).
#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

#include "epykos/exec/interpreter.hpp"
#include "epykos/fixtures/exotic_path.hpp"
#include "epykos/ir/program.hpp"
#include "epykos/ir/signature.hpp"
#include "epykos/tape/replay.hpp"
#include "epykos/tape/tape.hpp"

#ifndef EPYKOS_FP_CONTRACT_OFF
#error "an _e0_test.cpp TU must see EPYKOS_FP_CONTRACT_OFF"
#endif

namespace fixtures = epykos::fixtures;
namespace ir = epykos::ir;
namespace exec = epykos::exec;
using epykos::Replayer;
using epykos::Tape;

namespace {

bool same_bits(double a, double b) { return std::memcmp(&a, &b, sizeof(double)) == 0; }

std::size_t uz(int v) { return static_cast<std::size_t>(v); }

// MEASURED, not guessed (PRINCIPLES.md §5's error contract). The worst relative divergence seen
// across every (paths, steps, legs) case below between the pinned tape and the templated `double`
// recording is printed by the test; this gate is about an order of magnitude looser, which §5.1
// says is what a bug-detection tolerance is for.
constexpr double kCrossPin = 1e-13;

// The geometric Asian's agreement with its closed form is asserted over EIGHT INDEPENDENT SEEDS,
// not on the default one, and this is the reason rather than a convenience.
//
// A 3-sigma band on a single Monte Carlo estimate has a 0.3% false-alarm rate BY CONSTRUCTION, and
// measured, seed 20260922 -- the one docs/WORKLOADS.md fixes -- is one of the 0.3%: its 851,968
// Gaussian draws have a sample mean 2.0 to 3.0 standard errors BELOW zero (variance correct at
// 0.995-0.999), so the estimate lands z = -3.2 from the closed form at paths 16,384. The seed is
// NOT changed to get a prettier number -- the fixture discipline fixes it -- and the single-seed
// z is reported by the probe. What is GATED is the sampling distribution, which is the thing the
// closed form can actually certify: the mean z over independent seeds, whose own standard error is
// 1/sqrt(8) = 0.354. A path-machinery defect moves that mean; an unlucky stream does not.
constexpr int kGeomSeeds = 8;
constexpr double kGeomMeanZ = 1.5;  // 4.2 sigma on the mean of 8 -- loose, per §5.1

struct Case {
  int paths;
  int steps;
  unsigned legs;
  const char* what;
};

const Case kCases[] = {
    {16, 8, fixtures::exotic_legs_all, "all three legs"},
    {64, 12, fixtures::exotic_legs_all, "all three legs, wider"},
    {32, 16, fixtures::exotic_leg_spot, "spot only -- one recurrence, the scanning control"},
    {32, 16, fixtures::exotic_leg_arith, "arithmetic Asian alone"},
    {32, 16, fixtures::exotic_leg_geom, "geometric Asian alone"},
    {32, 16, fixtures::exotic_leg_barrier, "barrier alone -- the Select inside a recurrence"},
    {32, 16, fixtures::exotic_leg_arith | fixtures::exotic_leg_geom, "the two Asians"},
};

}  // namespace

// The recording discipline itself, asserted rather than trusted: the payoff's value branches are
// `select`, and the barrier's knock-out test depends on an INPUT, so `structural_if` on it must
// throw. If a future edit replaced the select with a structural_if, this is what catches it.
TEST(ExoticPath, StructuralIfOnTheBarrierTestThrows) {
  const fixtures::ExoticPathFixture f = fixtures::make_exotic_path(4, 4);
  Tape tape;
  Tape::Scope scope(tape);
  const std::vector<double> z0 = f.record_state();
  std::vector<epykos::Rec> in;
  for (double v : z0) in.push_back(epykos::make_input(tape, v));
  // The barrier predicate the payoff forms: run_max < barrier, with `barrier` an input.
  const epykos::RecBool c = in[fixtures::exotic_s0] < in[fixtures::exotic_barrier];
  EXPECT_THROW(epykos::structural_if(c), epykos::RecordError)
      << "the knock-out test depends on an input; the discipline must forbid structural_if on it";
  // And `select` on the same predicate is fine, which is what the payoff does.
  EXPECT_NO_THROW({
    const epykos::Rec r = epykos::select(c, in[fixtures::exotic_s0], epykos::Rec(0.0));
    (void)r;
  });
}

// Case 1: the interpreter reproduces the pinned tape's own replay BITWISE, at B = 1 and at every
// tile / lane-tile / catalogue configuration. Both sides evaluate the same pinned tape.
TEST(ExoticPath, InterpreterIsBitwiseTheReplay) {
  for (const Case& c : kCases) {
    const fixtures::ExoticPathFixture f = fixtures::make_exotic_path(c.paths, c.steps, 20260922, c.legs);
    Tape tape = fixtures::record_exotic_path(f, /*output_paths=*/true);
    const ir::Program prog = ir::infer(tape);
    ir::validate(prog);

    const std::vector<double> z = f.record_state();
    Replayer replay(tape);
    std::vector<double> want(uz(f.n_outputs(true)));
    replay.run(z.data(), want.data());

    for (int tile : {1, 7, 256, 4096}) {
      for (int lane_tile : {1, 3, 8}) {
        for (bool cat : {true, false}) {
          exec::Options o;
          o.tile = tile;
          o.lane_tile = lane_tile;
          o.use_catalogue = cat;
          o.max_batch = 8;
          exec::Interpreter in(prog, o);
          std::vector<double> got(uz(in.n_outputs()));
          in.run(z.data(), 1, got.data());
          for (std::size_t i = 0; i < want.size(); ++i) {
            ASSERT_TRUE(same_bits(want[i], got[i]))
                << c.what << ": output " << i << " tile " << tile << " lane_tile " << lane_tile << " catalogue "
                << cat << ": replay " << want[i] << " interpreter " << got[i];
          }
        }
      }
    }
  }
}

// Case 1: a batched run, lane for lane, is bitwise its own single-state runs. The eight lanes carry
// EIGHT DIFFERENT STATES -- a batch of one repeated state could not tell a lane mix-up from a
// correct run, which is the mistake D85's first ladder check made in another form.
TEST(ExoticPath, BatchedLanesAreBitwiseTheirSingleRuns) {
  for (const Case& c : kCases) {
    const fixtures::ExoticPathFixture f = fixtures::make_exotic_path(c.paths, c.steps, 20260922, c.legs);
    Tape tape = fixtures::record_exotic_path(f, /*output_paths=*/false);
    const ir::Program prog = ir::infer(tape);
    const int B = 8;
    const std::vector<std::vector<double>> states = fixtures::exotic_path_states(f, B);

    exec::Options o;
    o.max_batch = B;
    exec::Interpreter in(prog, o);
    const int nout = in.n_outputs();

    std::vector<double> soa(uz(fixtures::exotic_n_inputs) * uz(B));
    for (int k = 0; k < fixtures::exotic_n_inputs; ++k) {
      for (int b = 0; b < B; ++b) soa[uz(k) * uz(B) + uz(b)] = states[uz(b)][uz(k)];
    }
    std::vector<double> batched(uz(nout) * uz(B));
    in.run(soa.data(), B, batched.data());

    for (int b = 0; b < B; ++b) {
      std::vector<double> single(uz(nout));
      in.run(states[uz(b)].data(), 1, single.data());
      for (int o2 = 0; o2 < nout; ++o2) {
        ASSERT_TRUE(same_bits(single[uz(o2)], batched[uz(o2) * uz(B) + uz(b)]))
            << c.what << ": lane " << b << " output " << o2;
      }
    }
  }
}

// Case 2: the engine against the templated `double` recording. This CROSSES the pin, so it is a
// tolerance and the measured worst divergence is printed.
TEST(ExoticPath, AgreesWithTheTemplatedDoubleAcrossThePin) {
  double worst = 0.0;
  const char* worst_where = "";
  for (const Case& c : kCases) {
    const fixtures::ExoticPathFixture f = fixtures::make_exotic_path(c.paths, c.steps, 20260922, c.legs);
    Tape tape = fixtures::record_exotic_path(f, /*output_paths=*/true);
    const ir::Program prog = ir::infer(tape);
    exec::Options o;
    o.max_batch = 4;
    exec::Interpreter in(prog, o);

    // Over a ball of states, not only the record point: a check whose control case is the only
    // case it runs is not a check (PRINCIPLES.md §4a's warning, in its general form).
    for (const std::vector<double>& z : fixtures::exotic_path_states(f, 6)) {
      const std::vector<double> want = fixtures::exotic_path_oracle(f, z, /*output_paths=*/true);
      std::vector<double> got(uz(in.n_outputs()));
      in.run(z.data(), 1, got.data());
      for (std::size_t i = 0; i < want.size(); ++i) {
        const double scale = std::max(std::abs(want[i]), std::abs(got[i]));
        const double rel = scale > 1e-300 ? std::abs(want[i] - got[i]) / scale : std::abs(want[i] - got[i]);
        if (rel > worst) {
          worst = rel;
          worst_where = c.what;
        }
        EXPECT_LT(rel, kCrossPin) << c.what << ": output " << i;
      }
    }
  }
  std::cout << "[ exotic ] worst cross-pin relative divergence: " << worst << " (gate " << kCrossPin << "), at "
            << worst_where << "\n";
}

// The external maths. See kGeomSeeds for why the gate is on the mean over independent seeds and
// not on the single seed WORKLOADS.md fixes.
TEST(ExoticPath, GeometricAsianAgreesWithItsClosedForm) {
  const int paths = 4096;
  const int steps = 26;
  double zsum = 0.0;
  for (int i = 0; i < kGeomSeeds; ++i) {
    const std::uint64_t seed = 20260922 + static_cast<std::uint64_t>(i) * 7919;
    const fixtures::ExoticPathFixture f = fixtures::make_exotic_path(paths, steps, seed);
    const std::vector<double> out = fixtures::exotic_path_oracle(f, f.record_state(), /*output_paths=*/false);
    const double closed = fixtures::geometric_asian_call_closed_form(f);
    const double se = fixtures::geometric_asian_std_error(f);
    ASSERT_GT(se, 0.0);
    ASSERT_GT(closed, 0.0);
    const double z = (out[1] - closed) / se;  // leg order spot/arith/geom/barrier; default omits spot
    std::cout << "[ exotic ] seed " << seed << ": MC " << out[1] << " closed " << closed << " z " << z << "\n";
    zsum += z;
  }
  const double mean_z = zsum / kGeomSeeds;
  std::cout << "[ exotic ] mean z over " << kGeomSeeds << " seeds: " << mean_z << " (gate |z| < " << kGeomMeanZ
            << "; the mean's own standard error is " << 1.0 / std::sqrt(static_cast<double>(kGeomSeeds)) << ")\n";
  EXPECT_LT(std::abs(mean_z), kGeomMeanZ)
      << "the geometric Asian disagrees with its closed form beyond sampling error: the path "
         "machinery, the payoff or the discretisation is wrong";
}

// Case 3 in spirit (PRINCIPLES.md §5.2a): an assertion about STRUCTURE, stated as what is true now
// and saying what it records. This is the kill test's central structural finding, pinned so that a
// future change to ir::infer's scan layout shows up here rather than in a tool nobody reran.
TEST(ExoticPath, ScanLayoutSurvivesEveryLegExceptTheRunningExtremum) {
  struct Expect {
    unsigned legs;
    bool scans;
    const char* what;
  };
  const Expect cases[] = {
      {fixtures::exotic_leg_spot, true, "spot only"},
      {fixtures::exotic_leg_arith, true, "arithmetic Asian"},
      {fixtures::exotic_leg_geom, true, "geometric Asian"},
      {fixtures::exotic_leg_arith | fixtures::exotic_leg_geom, true, "both Asians"},
      // The running extremum is `M_k = select(S_k > M_{k-1}, S_k, M_{k-1})`: a Select INSIDE a
      // recurrence. ir::infer detects the class and refuses to lay it out -- "scan class N
      // (select) reads itself through other classes" -- and that refusal takes the spot's own
      // `mul` chain down with it, so the program gets NO scan domain at all.
      {fixtures::exotic_leg_barrier, false, "barrier alone -- a Select in the recurrence"},
      {fixtures::exotic_legs_all, false, "all three -- the barrier poisons the others' scan"},
  };
  // The consequence is asserted as a SCALING fact in the step count, not as a threshold at one
  // step count. An earlier version of this test asserted `domains < steps` for the scanning cases
  // and failed on "both Asians" at 20 domains against 16 steps -- because the domain count has a
  // constant part (the payoffs and the reductions) that dominates at a small step count. The
  // structural claim is not "few domains", it is "the domain count DOES NOT GROW with the step
  // count": with a scan the time axis is rows, without one it becomes domains.
  for (const Expect& e : cases) {
    std::size_t doms[2] = {0, 0};
    const int step_counts[2] = {16, 48};
    for (int i = 0; i < 2; ++i) {
      const fixtures::ExoticPathFixture f = fixtures::make_exotic_path(32, step_counts[i], 20260922, e.legs);
      Tape tape = fixtures::record_exotic_path(f, /*output_paths=*/false);
      ir::InferStats stats;
      const ir::Program prog = ir::infer(tape, &stats);
      std::size_t scan_rows = 0;
      for (const ir::Domain& d : prog.domains) {
        if (d.scan >= 0) scan_rows += static_cast<std::size_t>(d.rows);
      }
      doms[i] = prog.domains.size();
      EXPECT_EQ(scan_rows > 0, e.scans) << e.what << " at " << step_counts[i] << " steps: " << doms[i]
                                        << " domains, " << scan_rows << " scan rows, retries:" << stats.scan_retries;
    }
    std::cout << "[ exotic ] " << e.what << ": " << doms[0] << " domains at 16 steps, " << doms[1] << " at 48"
              << (e.scans ? "  (scans)" : "  (NO scan)") << "\n";
    if (e.scans) {
      EXPECT_EQ(doms[0], doms[1]) << e.what << ": with a scan the time axis is rows, so tripling the step count "
                                             "must not change the domain count";
    } else {
      EXPECT_GT(doms[1], doms[0] * 2) << e.what << ": without a scan the time axis becomes domains, so the domain "
                                                   "count must grow with the step count -- that is the cost";
    }
  }
}
