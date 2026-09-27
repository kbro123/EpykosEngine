// M3/G2 E0 gate: the tiled interpreter on the instrument sample (fixtures/instrument_sample.hpp)
// bitwise the tape replay, and within a measured tolerance of the templated double maths
// (price_sample<double>, the oracle), at the record point and at 16 ball states at B = 1, and a
// batched run (B = 17) lane for lane the same, over tiles {1, 256, 4096} x lane tiles {1, 8, 17}.
// The averaged coupons run as reductions, the term coupons and deposits as elementwise rows; two
// scan domains survive the algebra phase and are asserted below.
//
// RE-ANCHORED for the algebra phase, PRINCIPLES.md §5.2a. `record_sample` now runs
// `epykos::compile`, which telescopes the compounded coupons. The comparisons against
// price_sample<double> are case 2: they cross the pin and are now a measured relative tolerance
// (kCrossPin below). They were the file's ONLY comparisons, because before the pin the replay
// and the oracle were the same bits; the interpreter is therefore now compared against the
// replay as well, which is case 1 and stays bitwise, so the below-the-pin gate this file exists
// for is kept rather than lost with the tightening it used to get for free.
//
// This TU is compiled with -ffp-contract=off in every preset (the _e0_test.cpp convention); the
// interpreter's kernels are pinned the same way (D25). A gate of scripts/mutation_test.sh.
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

#include "epykos/exec/interpreter.hpp"
#include "epykos/fixtures/instrument_sample.hpp"
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

std::uint64_t bits(double v) {
  std::uint64_t u;
  std::memcpy(&u, &v, sizeof u);
  return u;
}

// The §5.2a case 2 gate, on |a − b| / max(|a|, |b|). MEASURED, release preset, this sample's 17
// states x 86 outputs: worst 7.885e-10, at state 4 output 40 — replay 53.446094651706517 against
// oracle 53.446094693848863, i.e. 4.2e-8 ABSOLUTE on a PV of 53.4 while this sample's notionals
// are log-uniform in [1e6, 1e8]. The measure is inflated because a PV is a difference of legs:
// D26/D30's long-standing observation about this quantity, which is why the project's own E1
// class scales by |fixed| + |float| rather than by |pv|. Gate set an order of magnitude above
// the measurement; every test prints its own worst so drift is visible (§5.1: a bug-detection
// tolerance need not be tight).
constexpr double kCrossPin = 1e-8;

// |a − b| / max(|a|, |b|), 0 when both are 0: the cross-pin measure of §5.2a case 2.
double relative(double a, double b) {
  const double scale = std::max(std::fabs(a), std::fabs(b));
  return scale > 0.0 ? std::fabs(a - b) / scale : 0.0;
}

struct Case {
  const fixtures::InstrumentSample* sample = nullptr;
  Tape tape;
  ir::Program program;
  std::vector<std::vector<double>> states;   // record point, then 16 draws
  std::vector<std::vector<double>> replay;
  std::vector<std::vector<double>> oracle;
};

const Case& the_case() {
  static const Case c = [] {
    static const fixtures::InstrumentSample s = fixtures::make_instrument_sample();
    Case k;
    k.sample = &s;
    k.tape = fixtures::record_sample(s);
    k.program = ir::infer(k.tape);
    k.states.push_back(k.tape.input_values());
    for (const std::vector<double>& z : fixtures::sample_states(s, 16)) k.states.push_back(z);
    Replayer rp(k.tape);
    for (const std::vector<double>& z : k.states) {
      std::vector<double> out(k.tape.num_outputs());
      rp.run(z.data(), out.data());
      k.replay.push_back(out);
      std::vector<double> o(static_cast<std::size_t>(s.n_outputs()));
      fixtures::price_sample<double>(s, z.data(), o.data());
      k.oracle.push_back(o);
    }
    return k;
  }();
  return c;
}

}  // namespace

// §5.2a case 2 — was OracleAndReplayAgreeBitwise. The replay evaluates the collapsed tape and
// the oracle the recording as written: two roundings of the same real number.
TEST(SampleInterpE0, OracleAndReplayAgreeAcrossThePin) {
  const Case& c = the_case();
  ASSERT_EQ(c.tape.num_outputs(), static_cast<std::size_t>(c.sample->n_outputs()));
  std::size_t mismatches = 0;
  double worst = 0.0;
  for (std::size_t s = 0; s < c.states.size(); ++s) {
    for (std::size_t o = 0; o < c.replay[s].size(); ++o) {
      const double rel = relative(c.replay[s][o], c.oracle[s][o]);
      worst = std::max(worst, rel);
      if (!(rel <= kCrossPin)) {
        if (mismatches < 5) ADD_FAILURE() << "state " << s << ", output " << o << ": replay " << c.replay[s][o] << " vs oracle "
                                          << c.oracle[s][o] << " (relative " << rel << ")";
        ++mismatches;
      }
    }
  }
  EXPECT_EQ(mismatches, 0u);
  std::cout << "[ cross-pin ] replay vs price_sample<double>, worst relative " << worst << " (gate " << kCrossPin << ")\n";
  ASSERT_FALSE(ir::scan_domains(c.program).empty());
}

// §5.2a — was InterpreterIsBitwiseTheOracleAtEveryState. Bitwise against the replay (case 1),
// within the measured tolerance of the oracle (case 2).
TEST(SampleInterpE0, InterpreterIsBitwiseTheReplayAndMatchesTheOracleAtEveryState) {
  const Case& c = the_case();
  const int n_in = static_cast<int>(c.tape.num_inputs()), n_out = static_cast<int>(c.tape.num_outputs());
  double worst = 0.0;
  for (const int tile : {1, 256, 4096}) {
    for (const int lane_tile : {1, 8, 17}) {
      exec::Options opt;
      opt.tile = tile;
      opt.lane_tile = lane_tile;
      opt.max_batch = 17;
      const exec::Interpreter in(c.program, opt);
      ASSERT_EQ(in.n_inputs(), n_in);
      ASSERT_EQ(in.n_outputs(), n_out);
      std::size_t mismatches = 0;
      // B = 1 at every state
      std::vector<double> out(static_cast<std::size_t>(n_out));
      for (std::size_t s = 0; s < c.states.size(); ++s) {
        in.run(c.states[s].data(), 1, out.data());
        for (int o = 0; o < n_out; ++o) {
          const double v = out[static_cast<std::size_t>(o)];
          if (bits(v) != bits(c.replay[s][static_cast<std::size_t>(o)])) {
            if (mismatches < 5) {
              ADD_FAILURE() << "tile " << tile << " lane_tile " << lane_tile << ", state " << s << ", output " << o << ": interpreter "
                            << v << " vs replay " << c.replay[s][static_cast<std::size_t>(o)];
            }
            ++mismatches;
          }
          const double rel = relative(v, c.oracle[s][static_cast<std::size_t>(o)]);
          worst = std::max(worst, rel);
          if (!(rel <= kCrossPin)) {
            if (mismatches < 5) {
              ADD_FAILURE() << "tile " << tile << " lane_tile " << lane_tile << ", state " << s << ", output " << o << ": interpreter "
                            << v << " vs oracle " << c.oracle[s][static_cast<std::size_t>(o)] << " (relative " << rel << ")";
            }
            ++mismatches;
          }
        }
      }
      // B = 17: lane b is state b, bitwise
      const int B = static_cast<int>(c.states.size());
      std::vector<double> st(static_cast<std::size_t>(n_in) * static_cast<std::size_t>(B));
      std::vector<double> ob(static_cast<std::size_t>(n_out) * static_cast<std::size_t>(B));
      for (int k = 0; k < n_in; ++k) {
        for (int b = 0; b < B; ++b) st[static_cast<std::size_t>(k) * static_cast<std::size_t>(B) + static_cast<std::size_t>(b)] = c.states[static_cast<std::size_t>(b)][static_cast<std::size_t>(k)];
      }
      in.run(st.data(), B, ob.data());
      for (int o = 0; o < n_out; ++o) {
        for (int b = 0; b < B; ++b) {
          const double v = ob[static_cast<std::size_t>(o) * static_cast<std::size_t>(B) + static_cast<std::size_t>(b)];
          if (bits(v) != bits(c.replay[static_cast<std::size_t>(b)][static_cast<std::size_t>(o)])) {
            if (mismatches < 5) ADD_FAILURE() << "tile " << tile << " lane_tile " << lane_tile << ", batched lane " << b << ", output " << o
                                              << ": vs replay";
            ++mismatches;
          }
          const double rel = relative(v, c.oracle[static_cast<std::size_t>(b)][static_cast<std::size_t>(o)]);
          worst = std::max(worst, rel);
          if (!(rel <= kCrossPin)) {
            if (mismatches < 5) ADD_FAILURE() << "tile " << tile << " lane_tile " << lane_tile << ", batched lane " << b << ", output " << o
                                              << ": vs oracle, relative " << rel;
            ++mismatches;
          }
        }
      }
      EXPECT_EQ(mismatches, 0u) << "tile " << tile << " lane_tile " << lane_tile;
    }
  }
  std::cout << "[  e0      ] " << c.states.size() << " states, " << n_out << " outputs, " << c.program.num_values() << " values, "
            << ir::scan_domains(c.program).size() << " scan domains: bitwise vs replay, worst relative vs double " << worst << '\n';
}
