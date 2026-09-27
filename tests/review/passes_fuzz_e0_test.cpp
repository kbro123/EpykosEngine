// M1/P7 review: adversarial E0 fuzz of the tape passes, the signature pass round trip, the IR
// evaluator and the interpreter on random expression DAGs (not the M1 book). Every pass must
// replay bit-identically before and after at the record point and at random states; the
// inferred program must round-trip and evaluate bit-identically; the interpreter must agree.
// Compiled with -ffp-contract=off (an _e0_test.cpp TU).
#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "epykos/exec/interpreter.hpp"
#include "epykos/ir/evaluate.hpp"
#include "epykos/ir/expand.hpp"
#include "epykos/ir/program.hpp"
#include "epykos/ir/signature.hpp"
#include "epykos/scalar/rec.hpp"
#include "epykos/tape/passes.hpp"
#include "epykos/tape/replay.hpp"
#include "epykos/tape/tape.hpp"
#include "tape/tape_test_helpers.hpp"

#ifndef EPYKOS_FP_CONTRACT_OFF
#error "an _e0_test.cpp TU must see EPYKOS_FP_CONTRACT_OFF"
#endif

namespace ir = epykos::ir;
namespace exec = epykos::exec;
using epykos::Op;
using epykos::Rec;
using epykos::RecBool;
using epykos::Tape;
using epykos::test::bits;
using epykos::test::SplitMix64;

namespace {

struct Dag {
  Tape tape;
  int n_in = 0;
};

// A random DAG over Rec: inputs, constants (including signed zeros, tiny and huge), every
// arithmetic op, select / max / min / abs, fma, left-deep sums with scaled addends and leading
// constants, Sub chains, repeated sub-expressions (for cse), dead nodes (for dce).
Dag random_dag(std::uint64_t seed, int n_in, int n_ops, bool allow_nan) {
  SplitMix64 rng(seed);
  const double consts[] = {0.0, -0.0, 1.0, -1.0, 2.0, 0.5, 0.25, 3.0, 1e-300, 1e300, 1e-16, 7.0 / 3.0, -2.5, 1e-8};
  auto pick_const = [&]() { return consts[rng.next() % (sizeof consts / sizeof consts[0])]; };
  Dag d;
  d.n_in = n_in;
  {
    Tape::Scope scope(d.tape);
    std::vector<Rec> pool;
    for (int k = 0; k < n_in; ++k) pool.push_back(epykos::make_input(d.tape, rng.uniform(0.5, 2.0)));
    auto pick = [&]() -> const Rec& { return pool[rng.next() % pool.size()]; };
    auto finite = [&](const Rec& r) { return std::isfinite(r.unchecked_value()) && std::fabs(r.unchecked_value()) < 1e100; };
    for (int i = 0; i < n_ops; ++i) {
      const int kind = static_cast<int>(rng.next() % 18);
      const Rec a = pick();
      const Rec b = pick();
      Rec r;
      switch (kind) {
        case 0: r = a + b; break;
        case 1: r = a - b; break;
        case 2: r = a * b; break;
        case 3: r = (allow_nan || std::fabs(b.unchecked_value()) > 1e-6) ? a / b : a * b; break;
        case 4: r = -a; break;
        case 5: r = (allow_nan || std::fabs(a.unchecked_value()) < 30.0) ? exp(a) : a + 1.0; break;
        case 6: r = select(a < b, a, b); break;
        case 7: r = max(a, b) + min(a, pick_const()); break;
        case 8: r = abs(a - b); break;
        case 9: r = fma(a, b, pick()); break;
        case 10: r = a + pick_const(); break;
        case 11: r = pick_const() * a; break;
        case 12: r = a - pick_const(); break;
        case 13: r = pick_const() - a; break;
        case 14: {
          // Left-deep chain with scaled / negated / constant addends, sometimes a leading constant.
          const int n = 2 + static_cast<int>(rng.next() % 5);
          Rec acc = (rng.next() & 1) ? Rec(pick_const()) : pick();
          for (int k = 0; k < n; ++k) {
            const Rec& x = pick();
            const int form = static_cast<int>(rng.next() % 5);
            Rec term;
            if (form == 0) term = x;
            else if (form == 1) term = pick_const() * x;
            else if (form == 2) term = x * pick_const();
            else if (form == 3) term = -x;
            else term = Rec(pick_const());
            acc = (rng.next() % 4 == 0) ? acc - term : acc + term;
          }
          r = acc;
          break;
        }
        case 15: {
          // Repeat an existing binary expression exactly (CSE fodder), in either operand order.
          r = (rng.next() & 1) ? a * b : b * a;
          if (rng.next() & 1) r = r + (a + b);
          break;
        }
        case 16: r = (allow_nan || a.unchecked_value() > -0.99) ? sqrt(abs(a) + 1.0) + log(abs(b) + 1.0) : a + b; break;
        default: r = recip(finite(a) && (allow_nan || std::fabs(a.unchecked_value()) > 1e-6) ? a : Rec(2.0)) + a; break;
      }
      if (!allow_nan && !finite(r)) r = a + b;  // keep the graph finite-valued in strict mode
      pool.push_back(r);
      // Occasionally a dead node.
      if (rng.next() % 7 == 0) { const Rec dead = a * 3.0 - b; (void)dead; }
    }
    // Outputs: the last few, plus a few random ones (some registered twice), plus an input.
    for (std::size_t k = pool.size() > 6 ? pool.size() - 6 : 0; k < pool.size(); ++k) epykos::register_output(d.tape, pool[k]);
    for (int k = 0; k < 4; ++k) epykos::register_output(d.tape, pick());
    epykos::register_output(d.tape, pool[0]);
    epykos::register_output(d.tape, Rec(pick_const()));  // a constant output
  }
  d.tape.validate();
  return d;
}

std::vector<std::vector<double>> states_for(const Tape& t, int n, std::uint64_t seed) {
  SplitMix64 rng(seed);
  std::vector<std::vector<double>> s;
  s.push_back(t.input_values());
  for (int b = 1; b < n; ++b) {
    std::vector<double> z(t.num_inputs());
    for (double& v : z) v = rng.uniform(0.5, 2.0);
    s.push_back(z);
  }
  return s;
}

std::vector<std::vector<double>> replay_all(const Tape& t, const std::vector<std::vector<double>>& states) {
  std::vector<std::vector<double>> out;
  epykos::Replayer rp(t);
  for (const auto& z : states) {
    std::vector<double> o(t.num_outputs());
    rp.run(z.data(), o.data());
    out.push_back(o);
  }
  return out;
}

// Counts bitwise mismatches, ignoring pairs that are both NaN (payload differences are not E0
// violations we care about, but they are counted separately).
std::size_t mismatches(const std::vector<std::vector<double>>& a, const std::vector<std::vector<double>>& b, std::size_t* nan_pairs) {
  std::size_t bad = 0;
  for (std::size_t s = 0; s < a.size(); ++s) {
    for (std::size_t k = 0; k < a[s].size(); ++k) {
      if (bits(a[s][k]) == bits(b[s][k])) continue;
      if (std::isnan(a[s][k]) && std::isnan(b[s][k])) { if (nan_pairs) ++*nan_pairs; continue; }
      ++bad;
    }
  }
  return bad;
}

}  // namespace

TEST(ReviewPassesFuzz, EveryPassReplaysBitIdenticallyOnRandomDags) {
  std::size_t total_bad = 0, dags = 0, nodes_before = 0, nodes_after = 0;
  for (std::uint64_t seed = 1; seed <= 300; ++seed) {
    Dag d = random_dag(seed, 3 + static_cast<int>(seed % 5), 40 + static_cast<int>(seed % 60), false);
    const auto states = states_for(d.tape, 6, seed * 77);
    const auto before = replay_all(d.tape, states);
    nodes_before += d.tape.size();
    const char* names[] = {"cse", "dce", "fold_sum", "affine_collapse", "dce2"};
    for (int p = 0; p < 5; ++p) {
      switch (p) {
        case 0: epykos::cse(d.tape); break;
        case 1: case 4: epykos::dce(d.tape); break;
        case 2: epykos::fold_sum(d.tape); break;
        case 3: epykos::affine_collapse(d.tape); break;
      }
      ASSERT_NO_THROW(d.tape.validate()) << "seed " << seed << " after " << names[p];
      const auto after = replay_all(d.tape, states);
      const std::size_t bad = mismatches(before, after, nullptr);
      if (bad != 0 && total_bad < 10) {
        ADD_FAILURE() << "seed " << seed << ": " << bad << " mismatches after " << names[p] << "\n" << epykos::to_string(d.tape);
      }
      total_bad += bad;
    }
    nodes_after += d.tape.size();
    ++dags;
  }
  EXPECT_EQ(total_bad, 0u);
  std::cout << "[  fuzz    ] " << dags << " random DAGs, " << nodes_before << " -> " << nodes_after << " nodes, mismatches " << total_bad << '\n';
}

TEST(ReviewPassesFuzz, SignatureRoundTripEvaluatorAndInterpreterOnRandomDags) {
  std::size_t roundtrip_fail = 0, eval_bad = 0, interp_bad = 0, recurrent = 0, dags = 0, interp_runs = 0;
  for (std::uint64_t seed = 1; seed <= 200; ++seed) {
    Dag d = random_dag(seed, 3 + static_cast<int>(seed % 5), 30 + static_cast<int>(seed % 50), false);
    epykos::standard_passes(d.tape);
    const auto states = states_for(d.tape, 5, seed * 31);
    const auto expect = replay_all(d.tape, states);
    ir::Program program;
    ASSERT_NO_THROW(program = ir::infer(d.tape)) << "seed " << seed;
    std::string diff;
    const Tape expanded = ir::expand(program);
    if (!ir::roundtrip_identical(d.tape, expanded, &diff)) {
      if (roundtrip_fail < 5) ADD_FAILURE() << "seed " << seed << " round trip differs:\n" << diff;
      ++roundtrip_fail;
    }
    // The expanded tape replays bit-identically too.
    eval_bad += mismatches(expect, replay_all(expanded, states), nullptr);
    // The evaluator.
    {
      ir::Evaluator ev(program);
      std::vector<std::vector<double>> got;
      for (const auto& z : states) {
        std::vector<double> o(program.outputs.size());
        ev.run(z.data(), o.data());
        got.push_back(o);
      }
      const std::size_t bad = mismatches(expect, got, nullptr);
      if (bad && eval_bad < 5) ADD_FAILURE() << "seed " << seed << ": evaluator mismatches " << bad;
      eval_bad += bad;
    }
    // The interpreter (skips recurrent programs, which it rejects by contract).
    if (!ir::recurrent_domains(program).empty()) { ++recurrent; ++dags; continue; }
    for (int lane_tile : {1, 3, 8}) {
      for (int tile : {1, 3, 256}) {
        exec::Options o;
        o.tile = tile;
        o.lane_tile = lane_tile;
        o.max_batch = 5;
        exec::Interpreter in(program, o);
        const int B = 5;
        std::vector<double> state(static_cast<std::size_t>(in.n_inputs()) * B), out(static_cast<std::size_t>(in.n_outputs()) * B);
        for (int k = 0; k < in.n_inputs(); ++k) for (int b = 0; b < B; ++b) state[static_cast<std::size_t>(k * B + b)] = states[static_cast<std::size_t>(b)][static_cast<std::size_t>(k)];
        in.run(state.data(), B, out.data());
        std::vector<std::vector<double>> got(B, std::vector<double>(program.outputs.size()));
        for (int b = 0; b < B; ++b) for (std::size_t k = 0; k < program.outputs.size(); ++k) got[static_cast<std::size_t>(b)][k] = out[k * B + static_cast<std::size_t>(b)];
        const std::size_t bad = mismatches(expect, got, nullptr);
        if (bad && interp_bad < 5) ADD_FAILURE() << "seed " << seed << " tile " << tile << " lane_tile " << lane_tile << ": interpreter mismatches " << bad;
        interp_bad += bad;
        ++interp_runs;
      }
    }
    ++dags;
  }
  EXPECT_EQ(roundtrip_fail, 0u);
  EXPECT_EQ(eval_bad, 0u);
  EXPECT_EQ(interp_bad, 0u);
  std::cout << "[  fuzz    ] " << dags << " DAGs through infer/expand/evaluate, " << recurrent
            << " rejected by the interpreter as recurrent, " << interp_runs << " interpreter runs\n";
}

// With NaN-valued nodes allowed, the passes are still E0 on every non-NaN output; NaN payload
// differences (commuted operands under cse, -1*x vs -x under affine collapse) are reported.
TEST(ReviewPassesFuzz, NanPayloadsAreTheOnlyDifferenceUnderNanInputs) {
  std::size_t bad = 0, nan_payload_diffs = 0;
  for (std::uint64_t seed = 1; seed <= 100; ++seed) {
    Dag d = random_dag(seed, 4, 60, true);
    auto states = states_for(d.tape, 4, seed);
    for (auto& z : states) { z[0] = std::numeric_limits<double>::quiet_NaN(); if (z.size() > 1) z[1] = std::numeric_limits<double>::infinity(); }
    const auto before = replay_all(d.tape, states);
    epykos::standard_passes(d.tape);
    const auto after = replay_all(d.tape, states);
    bad += mismatches(before, after, nullptr);
    for (std::size_t s = 0; s < before.size(); ++s) for (std::size_t k = 0; k < before[s].size(); ++k) {
      if (std::isnan(before[s][k]) && std::isnan(after[s][k]) && bits(before[s][k]) != bits(after[s][k])) ++nan_payload_diffs;
    }
  }
  EXPECT_EQ(bad, 0u);
  std::cout << "[  fuzz    ] NaN mode: non-NaN mismatches " << bad << ", NaN pairs with differing payload/sign " << nan_payload_diffs << '\n';
}

// cse merges a + b with b + a. IEEE addition commutes for every non-NaN operand pair; with two
// NaN operands x86 returns the first operand's payload, so the merged node changes bits.
TEST(ReviewCse, CommutedNanOperandsChangePayloadAfterMerge) {
  Tape t;
  {
    Tape::Scope scope(t);
    const Rec x = epykos::make_input(t, 1.0);
    const Rec y = epykos::make_input(t, 2.0);
    epykos::register_output(t, x + y);
    epykos::register_output(t, y + x);
  }
  double nan1, nan2;
  std::uint64_t b1 = 0x7ff8000000000001ull, b2 = 0x7ff8000000000002ull;
  std::memcpy(&nan1, &b1, 8);
  std::memcpy(&nan2, &b2, 8);
  const std::vector<double> in = {nan1, nan2};
  const std::vector<double> before = epykos::replay(t, in);
  epykos::cse(t);
  const std::vector<double> after = epykos::replay(t, in);
  std::cout << "[  cse nan ] before: " << std::hex << bits(before[0]) << " " << bits(before[1]) << "; after: " << bits(after[0]) << " " << bits(after[1]) << std::dec << '\n';
  // Informational: the payloads may differ (low severity, NaN only). No assertion on bits.
  EXPECT_TRUE(std::isnan(after[0]) && std::isnan(after[1]));
}

// Signed zeros: -0.0 and +0.0 constants are distinct leaves and are not merged, and the affine
// c_0 = -0.0 identity holds when the first term is a signed zero.
TEST(ReviewPasses, SignedZeroConstantsAndAffineIdentity) {
  Tape t;
  {
    Tape::Scope scope(t);
    const Rec x = epykos::make_input(t, 1.0);
    const Rec y = epykos::make_input(t, -1.0);
    epykos::register_output(t, x * 0.0 + y * 0.0);    // +0 + -0 = +0
    epykos::register_output(t, x * -0.0 + y * -0.0);  // -0 + +0 = +0
    epykos::register_output(t, y * 0.0 + y * 0.0);    // -0 + -0 = -0
    epykos::register_output(t, 0.0 + x * 0.0);        // leading +0 const
    epykos::register_output(t, -0.0 + y * 0.0);       // leading -0 const: -0 + -0 = -0
    epykos::register_output(t, x * 0.0 - y * 0.0);    // +0 - -0 = +0
    epykos::register_output(t, y * 0.0 - y * 0.0);    // -0 - -0 = +0
    epykos::register_output(t, (y * 0.0) + (-(x * 0.0)));  // -0 + -0 = -0
  }
  const std::vector<double> in = t.input_values();
  const std::vector<double> before = epykos::replay(t, in);
  epykos::standard_passes(t);
  const std::vector<double> after = epykos::replay(t, in);
  for (std::size_t k = 0; k < before.size(); ++k) EXPECT_EQ(bits(before[k]), bits(after[k])) << "output " << k << ": " << before[k] << " vs " << after[k];
  EXPECT_GE(epykos::test::count_op(t, Op::Affine), 1u) << epykos::to_string(t);
}

// A chain of two same-shaped ops through a shared node is an acyclic DAG, yet the signature
// pass flags the class recurrent (D22 rule 4 only level-splits across classes) and the
// interpreter refuses the program.
TEST(ReviewSignature, SameClassChainThroughASharedNodeIsRejectedAsRecurrent) {
  Tape t;
  {
    Tape::Scope s(t);
    const Rec x = epykos::make_input(t, 0.5);
    const Rec p = exp(x);
    const Rec q = exp(p);
    epykos::register_output(t, p);
    epykos::register_output(t, q);
  }
  epykos::standard_passes(t);
  const ir::Program program = ir::infer(t);
  std::cout << ir::to_string(program);
  EXPECT_FALSE(ir::recurrent_domains(program).empty()) << "documented limitation; this test records it";
  EXPECT_THROW(exec::Interpreter{program}, std::invalid_argument);
  // The evaluator handles it (rows read earlier rows of the same domain).
  const std::vector<double> got = ir::evaluate(program, {0.5});
  const std::vector<double> want = epykos::replay(t, {0.5});
  EXPECT_EQ(bits(got[0]), bits(want[0]));
  EXPECT_EQ(bits(got[1]), bits(want[1]));
}

// shape_string() prints "op[N steps]" (with a space) for a group of more than 24 steps; that
// string is the domain name, which serialize() writes as one whitespace-delimited token.
TEST(ReviewSerialize, LongGroupNameBreaksDeserialize) {
  Tape t;
  {
    Tape::Scope s(t);
    Rec y = epykos::make_input(t, 0.5);
    for (int k = 0; k < 15; ++k) y = sqrt(y * 1.5);  // 30 inner steps (Mul is not folded), one boundary
    epykos::register_output(t, y);
  }
  epykos::standard_passes(t);
  const ir::Program program = ir::infer(t);
  bool long_group = false;
  for (const ir::Group& g : program.groups) long_group |= g.steps.size() > 24;
  ASSERT_TRUE(long_group);
  const std::string text = ir::serialize(program);
  ir::Program back;
  EXPECT_NO_THROW(back = ir::deserialize(text)) << "deserialize(serialize(p)) must not throw";
  EXPECT_TRUE(back == program);
}
