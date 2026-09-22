// M1/P2 E0 gates: replay is bit-identical to the double evaluation, and every pass preserves the
// replay bitwise. This TU is compiled with -ffp-contract=off in every preset (the _e0_test.cpp
// convention), so the double instantiation, the Rec record-point values and the header-only
// replay interpreter all run without fused multiply-adds.
#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <new>
#include <vector>

#include "epykos/scalar/rec.hpp"
#include "epykos/scalar/select.hpp"
#include "epykos/tape/passes.hpp"
#include "epykos/tape/replay.hpp"
#include "epykos/tape/tape.hpp"
#include "tape/mini_book.hpp"
#include "tape/tape_test_helpers.hpp"

#ifndef EPYKOS_FP_CONTRACT_OFF
#error "an _e0_test.cpp TU must see EPYKOS_FP_CONTRACT_OFF"
#endif

using epykos::Op;
using epykos::PassResult;
using epykos::Rec;
using epykos::Replayer;
using epykos::Tape;
using epykos::test::bits;
using epykos::test::count_op;
using epykos::test::eval_mini_book_double;
using epykos::test::kMiniKnots;
using epykos::test::mini_book;
using epykos::test::mini_states;
using epykos::test::MiniSwap;
using epykos::test::record_mini_book;
using epykos::test::ops_string;
using epykos::test::SplitMix64;

// ---- allocation counter (replaceable global allocation functions) --------------------------------

namespace {
std::atomic<std::size_t> g_allocations{0};
}  // namespace

void* operator new(std::size_t n) {
  ++g_allocations;
  if (void* p = std::malloc(n == 0 ? 1 : n)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) {
  ++g_allocations;
  if (void* p = std::malloc(n == 0 ? 1 : n)) return p;
  throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {

// ---- a hand-written function using every recorded op ----------------------------------------------

template <class Scalar>
Scalar hand_function(Scalar x, Scalar y, Scalar z) {
  using epykos::select;
  Scalar a = x * y + z;              // mul, add
  Scalar b = (a - x) / (y + 1.5);    // sub, add(const), div
  Scalar c = exp(-b * 0.25);         // neg, mul(const), exp
  Scalar d = log(x * x + 1.0);       // mul, add, log
  Scalar e = sqrt(y * y + z * z);    // sqrt
  Scalar f = fma(a, c, d);           // fma
  Scalar g = epykos::recip(e + 2.0);
  Scalar m = epykos::max(f, g);      // select via cmp_lt
  Scalar n = epykos::min(m, 100.0 * z);
  Scalar p = epykos::abs(n - 3.0);
  Scalar q = select(x < y, p, -p);
  Scalar r = select(x <= y, q * 2.0, q);
  Scalar s = select(x > y, r, r + 1.0);
  Scalar t = select(x >= y, s, s - 1.0);
  Scalar u = select(x == y, t, t * 0.5);
  return (((u + a) + b) + c) + d;    // a left-deep add chain
}

void expect_bits_equal(const std::vector<double>& a, const std::vector<double>& b, const char* what) {
  ASSERT_EQ(a.size(), b.size()) << what;
  std::size_t mismatches = 0;
  for (std::size_t k = 0; k < a.size(); ++k) {
    if (bits(a[k]) != bits(b[k])) {
      if (mismatches < 5) {
        ADD_FAILURE() << what << ": output " << k << " differs: " << a[k] << " vs " << b[k];
      }
      ++mismatches;
    }
  }
  EXPECT_EQ(mismatches, 0u) << what;
}

}  // namespace

// ---- replay vs double ------------------------------------------------------------------------------

TEST(ReplayE0, HandWrittenFunctionMatchesDoubleBitwise) {
  Tape t;
  Rec rec_out;
  {
    Tape::Scope scope(t);
    rec_out = hand_function(epykos::make_input(1.3), epykos::make_input(0.7), epykos::make_input(2.1));
    epykos::register_output(rec_out);
  }
  EXPECT_NO_THROW(t.validate());
  // Every op family is on the tape.
  for (Op op : {Op::Add, Op::Sub, Op::Mul, Op::Div, Op::Neg, Op::Exp, Op::Log, Op::Sqrt, Op::Recip,
                Op::Fma, Op::Select, Op::CmpLt, Op::CmpLe, Op::CmpGt, Op::CmpGe, Op::CmpEq}) {
    EXPECT_GE(count_op(t, op), 1u) << epykos::to_string(op) << " missing: " << ops_string(t);
  }
  Replayer rp(t);
  double out = 0.0;
  const double record_in[3] = {1.3, 0.7, 2.1};
  rp.run(record_in, &out);
  EXPECT_EQ(bits(out), bits(rec_out.unchecked_value())) << "replay at the record point == Rec::v";
  EXPECT_EQ(bits(out), bits(hand_function(1.3, 0.7, 2.1)));
  // Every node's replay value equals the double evaluation of that node: check via random states,
  // including ones that flip each comparison.
  SplitMix64 rng(7);
  const double probes[][3] = {{0.7, 1.3, 2.1}, {1.0, 1.0, 0.5}, {2.0, -1.0, 0.25}};
  for (const auto& p : probes) {
    rp.run(p, &out);
    EXPECT_EQ(bits(out), bits(hand_function(p[0], p[1], p[2]))) << p[0] << ' ' << p[1] << ' ' << p[2];
  }
  for (int i = 0; i < 1000; ++i) {
    const double in[3] = {rng.uniform(0.1, 3.0), rng.uniform(0.1, 3.0), rng.uniform(0.1, 3.0)};
    rp.run(in, &out);
    ASSERT_EQ(bits(out), bits(hand_function(in[0], in[1], in[2]))) << in[0] << ' ' << in[1] << ' ' << in[2];
  }
}

TEST(ReplayE0, RunDoesNotAllocate) {
  Tape t;
  {
    Tape::Scope scope(t);
    epykos::register_output(hand_function(epykos::make_input(1.3), epykos::make_input(0.7),
                                          epykos::make_input(2.1)));
  }
  epykos::standard_passes(t);
  Replayer rp(t);
  double out = 0.0;
  const double in[3] = {1.0, 2.0, 3.0};
  rp.run(in, &out);  // warm
  const std::size_t before = g_allocations.load();
  for (int i = 0; i < 100; ++i) rp.run(in, &out);
  EXPECT_EQ(g_allocations.load(), before) << "Replayer::run allocated";
}

TEST(ReplayE0, RecordPointNodeValuesEqualReplayValues) {
  Tape t;
  std::vector<Rec> recs;
  {
    Tape::Scope scope(t);
    const Rec x = epykos::make_input(0.3);
    const Rec y = epykos::make_input(1.7);
    recs = {x, y, x * y, exp(x) - y, sqrt(y) / x, fma(x, y, x), epykos::max(x, y), log(y) * 0.5};
    for (const Rec& r : recs) epykos::register_output(r);
  }
  Replayer rp(t);
  std::vector<double> out(t.num_outputs());
  const std::vector<double> in = t.input_values();
  rp.run(in.data(), out.data());
  for (std::size_t k = 0; k < recs.size(); ++k) {
    EXPECT_EQ(bits(out[k]), bits(recs[k].unchecked_value())) << k;
    EXPECT_EQ(bits(rp.values()[recs[k].id]), bits(recs[k].unchecked_value())) << k;
  }
}

// ---- passes preserve the replay bitwise ------------------------------------------------------------

TEST(FoldSumE0, ReplayIsBitIdenticalBeforeAndAfter) {
  // Long left-deep chains of terms of varying magnitude and sign, where any change of the
  // association order would change bits.
  Tape t;
  const int n_terms = 64;
  {
    Tape::Scope scope(t);
    std::vector<Rec> xs;
    for (int i = 0; i < n_terms; ++i) xs.push_back(epykos::make_input(1.0));
    SplitMix64 rng(11);
    Rec acc = xs[0] * rng.uniform(-1e6, 1e6);
    for (int i = 1; i < n_terms; ++i) acc = acc + xs[i] * rng.uniform(-1e6, 1e6);
    epykos::register_output(acc);
    Rec acc2 = 0.0;
    for (int i = 0; i < n_terms; ++i) acc2 = acc2 + exp(xs[i]) * rng.uniform(1e-3, 1e3);
    epykos::register_output(acc2 * 3.0);
    epykos::register_output(acc + acc2);
  }
  std::vector<std::vector<double>> states;
  SplitMix64 rng(12);
  for (int b = 0; b < 64; ++b) {
    std::vector<double> s(n_terms);
    for (double& v : s) v = rng.uniform(-2.0, 2.0);
    states.push_back(s);
  }
  std::vector<std::vector<double>> before;
  for (const auto& s : states) before.push_back(epykos::replay(t, s));
  const PassResult r = epykos::fold_sum(t);
  EXPECT_NO_THROW(t.validate());
  EXPECT_GE(r.changed, static_cast<std::size_t>(2 * (n_terms - 1)));
  EXPECT_EQ(count_op(t, Op::Add), 0u) << ops_string(t);
  EXPECT_EQ(count_op(t, Op::Sum), 3u) << ops_string(t);
  for (std::size_t b = 0; b < states.size(); ++b) {
    expect_bits_equal(epykos::replay(t, states[b]), before[b], "fold_sum");
  }
}

TEST(AffineE0, ReplayIsBitIdenticalBeforeAndAfter) {
  Tape t;
  {
    Tape::Scope scope(t);
    const Rec a = epykos::make_input(1.0);
    const Rec b = epykos::make_input(2.0);
    const Rec c = epykos::make_input(3.0);
    const Rec d = epykos::make_input(4.0);
    SplitMix64 rng(21);
    // Interpolation-like weighted sums, differences, negations, leading and trailing constants,
    // nested affine through a scale, and a long chain.
    epykos::register_output(a * 0.3 + b * 0.7);
    epykos::register_output(a + (b - a) * rng.uniform(0.0, 1.0));
    epykos::register_output(1.0 + 0.5 * a);
    epykos::register_output((a + 3.0) + b);
    epykos::register_output((2.0 - a) - 4.0 * b);
    epykos::register_output(-a + b);
    epykos::register_output(a - (-b));
    epykos::register_output(a - b);
    epykos::register_output(a + b);
    epykos::register_output(a * 1e30 + b * 1e-30);
    epykos::register_output((a * 1e16 + b) - a * 1e16);
    Rec acc = a * rng.uniform(-1.0, 1.0);
    for (int i = 0; i < 40; ++i) {
      const Rec& x = (i % 4 == 0) ? a : (i % 4 == 1) ? b : (i % 4 == 2) ? c : d;
      const double coef = rng.uniform(-1e3, 1e3);
      acc = (i % 3 == 0) ? acc - x * coef : acc + coef * x;
    }
    epykos::register_output(acc);
    epykos::register_output(exp(-(a * 0.3 + b * 0.7) * 2.5) * c);
    // An affine node feeding a non-affine consumer and another affine node.
    const Rec z = 0.25 * c + 0.75 * d;
    epykos::register_output(z * z);
    epykos::register_output(z + 0.5 * a);
  }
  std::vector<std::vector<double>> states;
  SplitMix64 rng(22);
  for (int b = 0; b < 200; ++b) {
    std::vector<double> s(4);
    for (double& v : s) v = rng.uniform(-3.0, 3.0);
    states.push_back(s);
  }
  // Signed zeros and exact cancellations.
  states.push_back({-0.0, -0.0, -0.0, -0.0});
  states.push_back({0.0, -0.0, 1.0, -1.0});
  states.push_back({1.0, -1.0, 0.0, 0.0});
  states.push_back({0.0, 0.0, 0.0, 0.0});
  std::vector<std::vector<double>> before;
  for (const auto& s : states) before.push_back(epykos::replay(t, s));

  const PassResult r = epykos::affine_collapse(t);
  EXPECT_NO_THROW(t.validate());
  EXPECT_GT(r.changed, 0u);
  EXPECT_GE(count_op(t, Op::Affine), 12u) << ops_string(t);
  for (std::size_t b = 0; b < states.size(); ++b) {
    expect_bits_equal(epykos::replay(t, states[b]), before[b], "affine_collapse");
  }
  // And again after fold_sum first (Sum inputs to the collapse).
  Tape t2;
  {
    Tape::Scope scope(t2);
    const Rec a = epykos::make_input(1.0);
    const Rec b = epykos::make_input(2.0);
    const Rec c = epykos::make_input(3.0);
    const Rec d = epykos::make_input(4.0);
    epykos::register_output(((a * 0.1 + b * 0.2) + c * 0.3) + d * 0.4);
    epykos::register_output(((a - b) + c) + d);
    epykos::register_output(((a + b) + c) + a * b);  // prefix (a + b) + c is affine, a*b is not
  }
  std::vector<std::vector<double>> before2;
  for (const auto& s : states) before2.push_back(epykos::replay(t2, s));
  epykos::fold_sum(t2);
  epykos::affine_collapse(t2);
  EXPECT_NO_THROW(t2.validate());
  EXPECT_GE(count_op(t2, Op::Affine), 2u) << ops_string(t2);
  for (std::size_t b = 0; b < states.size(); ++b) {
    expect_bits_equal(epykos::replay(t2, states[b]), before2[b], "fold_sum + affine_collapse");
  }
}

TEST(CseDceE0, ReplayIsBitIdenticalBeforeAndAfter) {
  Tape t;
  {
    Tape::Scope scope(t);
    const Rec x = epykos::make_input(1.3);
    const Rec y = epykos::make_input(0.7);
    const Rec z = epykos::make_input(2.1);
    const Rec f1 = hand_function(x, y, z);
    const Rec f2 = hand_function(x, y, z);  // duplicate work, merged by cse
    const Rec dead = hand_function(z, y, x);
    (void)dead;
    epykos::register_output(f1 + f2);
    epykos::register_output(f2);
  }
  const std::size_t n0 = t.size();
  SplitMix64 rng(31);
  std::vector<std::vector<double>> states;
  for (int b = 0; b < 100; ++b) states.push_back({rng.uniform(0.1, 3.0), rng.uniform(0.1, 3.0), rng.uniform(0.1, 3.0)});
  std::vector<std::vector<double>> before;
  for (const auto& s : states) before.push_back(epykos::replay(t, s));
  const PassResult rc = epykos::cse(t);
  const PassResult rd = epykos::dce(t);
  EXPECT_NO_THROW(t.validate());
  EXPECT_GT(rc.changed, 0u);
  EXPECT_GT(rd.changed, 0u);
  EXPECT_LT(t.size(), n0 / 2);
  for (std::size_t b = 0; b < states.size(); ++b) {
    expect_bits_equal(epykos::replay(t, states[b]), before[b], "cse + dce");
  }
}

// ---- the mini book end to end -----------------------------------------------------------------------

TEST(MiniBookE0, RecordingReplaysBitIdenticalToDoubleAndSurvivesAllPasses) {
  const std::vector<MiniSwap> book = mini_book(50);
  Tape t = record_mini_book(book);
  EXPECT_NO_THROW(t.validate());
  ASSERT_EQ(t.num_inputs(), static_cast<std::size_t>(kMiniKnots));
  ASSERT_EQ(t.num_outputs(), book.size() + 1);
  EXPECT_GE(count_op(t, Op::Select), book.size());
  EXPECT_GE(count_op(t, Op::Exp), 1u);

  const std::vector<std::array<double, kMiniKnots>> states = mini_states(64);
  std::vector<std::vector<double>> oracle;
  for (const auto& s : states) oracle.push_back(eval_mini_book_double(s, book));

  // Before any pass: replay == double, at the record point and in the state ball.
  {
    Replayer rp(t);
    std::vector<double> out(t.num_outputs());
    for (std::size_t b = 0; b < states.size(); ++b) {
      rp.run(states[b].data(), out.data());
      expect_bits_equal(out, oracle[b], "raw tape vs double");
    }
  }
  const std::size_t n_raw = t.size();

  // Each pass separately, then the standard pipeline.
  struct Step {
    const char* name;
    PassResult (*run)(Tape&);
  };
  const Step steps[] = {
      {"cse", [](Tape& x) { return epykos::cse(x); }},
      {"dce", [](Tape& x) { return epykos::dce(x); }},
      {"fold_sum", [](Tape& x) { return epykos::fold_sum(x, {}); }},
      {"affine_collapse", [](Tape& x) { return epykos::affine_collapse(x); }},
      {"dce", [](Tape& x) { return epykos::dce(x); }},
  };
  for (const Step& step : steps) {
    const PassResult r = step.run(t);
    EXPECT_NO_THROW(t.validate()) << step.name;
    EXPECT_EQ(r.nodes_after, t.size()) << step.name;
    Replayer rp(t);
    std::vector<double> out(t.num_outputs());
    for (std::size_t b = 0; b < states.size(); ++b) {
      rp.run(states[b].data(), out.data());
      expect_bits_equal(out, oracle[b], step.name);
    }
  }
  EXPECT_LT(t.size(), n_raw);
  // The interpolation collapsed into Affine nodes and the legs folded into Sums.
  EXPECT_GT(count_op(t, Op::Affine), 0u) << ops_string(t);
  EXPECT_GT(count_op(t, Op::Sum), 0u);
  EXPECT_EQ(count_op(t, Op::Add), 0u);

  // The one-shot pipeline on a fresh recording gives the same tape.
  Tape t2 = record_mini_book(book);
  epykos::standard_passes(t2);
  EXPECT_EQ(epykos::to_string(t2), epykos::to_string(t));
}
