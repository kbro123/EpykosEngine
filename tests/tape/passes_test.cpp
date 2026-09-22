// M1/P2: the tape passes rewrite the node table into the expected shapes.
//   cse merges duplicates and preserves outputs; dce removes dead nodes and keeps inputs;
//   fold_sum turns left-deep Add chains into one Sum with the fold order preserved;
//   affine_collapse turns affine-in-inputs chains into Affine nodes and leaves segment sums alone.
// Bit-identity of the replay before and after each pass is in replay_e0_test.cpp.
#include <gtest/gtest.h>

#include <vector>

#include "epykos/scalar/rec.hpp"
#include "epykos/tape/passes.hpp"
#include "epykos/tape/replay.hpp"
#include "epykos/tape/tape.hpp"
#include "tape/tape_test_helpers.hpp"

using epykos::invalid_node;
using epykos::node_id;
using epykos::Op;
using epykos::PassResult;
using epykos::Rec;
using epykos::Tape;
using epykos::test::bits;
using epykos::test::count_op;
using epykos::test::ops_of;
using epykos::test::ops_string;

namespace {

std::vector<node_id> args_vec(const Tape& t, node_id id) {
  const auto a = t.args(id);
  return std::vector<node_id>(a.begin(), a.end());
}
std::vector<double> coefs_vec(const Tape& t, node_id id) {
  const auto c = t.coefs(id);
  return std::vector<double>(c.begin(), c.end());
}

}  // namespace

// ---- cse ----------------------------------------------------------------------------------------

TEST(Cse, MergesDuplicatesAndPreservesOutputs) {
  Tape t;
  {
    Tape::Scope scope(t);
    const Rec x = epykos::make_input(1.5);
    const Rec y = epykos::make_input(2.5);
    const Rec p1 = x * y;
    const Rec p2 = x * y;  // duplicate
    const Rec p3 = y * x;  // commutative duplicate
    const Rec d1 = x - y;
    const Rec d2 = y - x;  // not a duplicate
    const Rec s1 = x * 0.5;
    const Rec s2 = x * 0.5;  // same constant leaf (deduplicated at record time)
    epykos::register_output(p1);
    epykos::register_output(p2);
    epykos::register_output(p3);
    epykos::register_output(d1);
    epykos::register_output(d2);
    epykos::register_output(s1);
    epykos::register_output(s2);
  }
  const std::vector<double> in = t.input_values();
  const std::vector<double> before = epykos::replay(t, in);
  const std::size_t n_before = t.size();
  const PassResult r = epykos::cse(t);
  EXPECT_NO_THROW(t.validate());
  EXPECT_EQ(r.nodes_before, n_before);
  EXPECT_EQ(r.nodes_after, t.size());
  EXPECT_EQ(r.changed, 3u) << ops_string(t);  // p2, p3, s2
  EXPECT_EQ(count_op(t, Op::Mul), 2u) << ops_string(t);
  EXPECT_EQ(count_op(t, Op::Sub), 2u);
  ASSERT_EQ(t.num_outputs(), 7u);
  EXPECT_EQ(t.outputs()[0], t.outputs()[1]);
  EXPECT_EQ(t.outputs()[0], t.outputs()[2]);
  EXPECT_NE(t.outputs()[3], t.outputs()[4]);
  EXPECT_EQ(t.outputs()[5], t.outputs()[6]);
  EXPECT_EQ(t.num_inputs(), 2u);
  const std::vector<double> after = epykos::replay(t, in);
  ASSERT_EQ(after.size(), before.size());
  for (std::size_t k = 0; k < after.size(); ++k) EXPECT_EQ(bits(after[k]), bits(before[k])) << k;
  // remap: every old node maps to a live node.
  ASSERT_EQ(r.remap.size(), n_before);
  for (node_id m : r.remap) EXPECT_NE(m, invalid_node);
}

TEST(Cse, NeverMergesInputsOrDifferentConstants) {
  Tape t;
  {
    Tape::Scope scope(t);
    const Rec x = epykos::make_input(1.0);
    const Rec y = epykos::make_input(1.0);  // same value, different input
    epykos::register_output(x + 1.0);
    epykos::register_output(y + 1.0);
    epykos::register_output(x + 2.0);
  }
  epykos::cse(t);
  EXPECT_EQ(t.num_inputs(), 2u);
  EXPECT_EQ(count_op(t, Op::Add), 3u);
  EXPECT_EQ(count_op(t, Op::Const), 2u);
}

TEST(Cse, MergesVariadicNodesByArgsCoefsAndKonst) {
  Tape t;
  const node_id x = t.input(1.0);
  const node_id y = t.input(2.0);
  const node_id a[] = {x, y};
  const double c1[] = {1.0, 2.0};
  const double c2[] = {1.0, 3.0};
  const node_id s1 = t.variadic(Op::Sum, a);
  const node_id s2 = t.variadic(Op::Sum, a);
  const node_id f1 = t.variadic(Op::Affine, a, c1, 0.5);
  const node_id f2 = t.variadic(Op::Affine, a, c1, 0.5);
  const node_id f3 = t.variadic(Op::Affine, a, c2, 0.5);
  const node_id f4 = t.variadic(Op::Affine, a, c1, -0.5);
  for (node_id o : {s1, s2, f1, f2, f3, f4}) t.output(o);
  const PassResult r = epykos::cse(t);
  EXPECT_EQ(r.changed, 2u);
  EXPECT_EQ(t.outputs()[0], t.outputs()[1]);
  EXPECT_EQ(t.outputs()[2], t.outputs()[3]);
  EXPECT_NE(t.outputs()[2], t.outputs()[4]);
  EXPECT_NE(t.outputs()[2], t.outputs()[5]);
  EXPECT_NO_THROW(t.validate());
}

// ---- dce ----------------------------------------------------------------------------------------

TEST(Dce, RemovesDeadNodesAndKeepsInputs) {
  Tape t;
  Rec x, y, z;
  {
    Tape::Scope scope(t);
    x = epykos::make_input(1.0);
    y = epykos::make_input(2.0);
    z = epykos::make_input(3.0);  // unused input
    const Rec live = x + y;
    const Rec dead1 = x * y;
    const Rec dead2 = exp(dead1) * 7.0;
    (void)dead2;
    epykos::register_output(live);
  }
  const std::size_t n_before = t.size();
  const PassResult r = epykos::dce(t);
  EXPECT_NO_THROW(t.validate());
  EXPECT_EQ(r.nodes_before, n_before);
  EXPECT_EQ(r.changed, 4u) << ops_string(t);  // Mul, Exp, Const 7, Mul
  EXPECT_EQ(ops_of(t), (std::vector<Op>{Op::Input, Op::Input, Op::Input, Op::Add})) << ops_string(t);
  EXPECT_EQ(t.num_inputs(), 3u) << "unused inputs are kept so ordinals stay stable";
  EXPECT_EQ(t[t.inputs()[2]].a, 2);
  EXPECT_EQ(t.num_outputs(), 1u);
  EXPECT_EQ(t.outputs()[0], 3);
  EXPECT_EQ(r.remap[x.id], 0);
  EXPECT_EQ(r.remap[y.id], 1);
  ASSERT_EQ(r.remap.size(), n_before);
  EXPECT_EQ(r.remap[n_before - 1], invalid_node);
  const std::vector<double> out = epykos::replay(t, t.input_values());
  EXPECT_EQ(out[0], 3.0);
}

TEST(Dce, KeepsEverythingReachableFromAnyOutput) {
  Tape t;
  {
    Tape::Scope scope(t);
    const Rec x = epykos::make_input(1.0);
    const Rec a = x * 2.0;
    const Rec b = a + 1.0;
    epykos::register_output(a);
    epykos::register_output(b);
  }
  const PassResult r = epykos::dce(t);
  EXPECT_EQ(r.changed, 0u);
  EXPECT_EQ(t.size(), 5u);
}

// ---- fold_sum -------------------------------------------------------------------------------------

TEST(FoldSum, LeftDeepChainBecomesOneSumInFoldOrder) {
  Tape t;
  Rec a, b, c, d;
  {
    Tape::Scope scope(t);
    a = epykos::make_input(1.0);
    b = epykos::make_input(2.0);
    c = epykos::make_input(3.0);
    d = epykos::make_input(4.0);
    const Rec s = ((a + b) + c) + d;
    epykos::register_output(s);
  }
  const PassResult r = epykos::fold_sum(t);
  EXPECT_NO_THROW(t.validate());
  EXPECT_EQ(r.changed, 3u);
  ASSERT_EQ(ops_of(t), (std::vector<Op>{Op::Input, Op::Input, Op::Input, Op::Input, Op::Sum}))
      << ops_string(t);
  const node_id s = t.outputs()[0];
  EXPECT_EQ(t[s].nargs, 4);
  EXPECT_EQ(args_vec(t, s), (std::vector<node_id>{0, 1, 2, 3}));
  EXPECT_TRUE(t[s].tainted);
  const std::vector<double> out = epykos::replay(t, t.input_values());
  EXPECT_EQ(out[0], 10.0);
}

TEST(FoldSum, AccumulatorLoopWithLeadingZeroKeepsTheZero) {
  Tape t;
  {
    Tape::Scope scope(t);
    Rec acc = 0.0;
    for (int i = 0; i < 5; ++i) acc = acc + epykos::make_input(static_cast<double>(i));
    epykos::register_output(acc);
  }
  epykos::fold_sum(t);
  const node_id s = t.outputs()[0];
  EXPECT_EQ(t[s].op, Op::Sum);
  EXPECT_EQ(t[s].nargs, 6);
  EXPECT_EQ(t[args_vec(t, s)[0]].op, Op::Const) << "0 + x is not folded away: -0.0 would flip sign";
  EXPECT_EQ(count_op(t, Op::Add), 0u);
}

TEST(FoldSum, RightNestedAddsAreSeparateSums) {
  Tape t;
  {
    Tape::Scope scope(t);
    const Rec a = epykos::make_input(1.0);
    const Rec b = epykos::make_input(2.0);
    const Rec c = epykos::make_input(3.0);
    const Rec d = epykos::make_input(4.0);
    epykos::register_output((a + b) + (c + d));
  }
  epykos::fold_sum(t);
  EXPECT_EQ(count_op(t, Op::Sum), 2u) << ops_string(t);
  const node_id outer = t.outputs()[0];
  ASSERT_EQ(t[outer].nargs, 3);
  const auto args = args_vec(t, outer);
  EXPECT_EQ(args[0], 0);
  EXPECT_EQ(args[1], 1);
  EXPECT_EQ(t[args[2]].op, Op::Sum) << "(c + d) is its own Sum, used as a term";
  EXPECT_EQ(args_vec(t, args[2]), (std::vector<node_id>{2, 3}));
}

TEST(FoldSum, MultiUseInnerAddStopsTheChain) {
  Tape t;
  {
    Tape::Scope scope(t);
    const Rec a = epykos::make_input(1.0);
    const Rec b = epykos::make_input(2.0);
    const Rec c = epykos::make_input(3.0);
    const Rec ab = a + b;
    const Rec abc = ab + c;
    epykos::register_output(abc);
    epykos::register_output(ab);  // second use of (a + b)
  }
  epykos::fold_sum(t);
  EXPECT_EQ(count_op(t, Op::Sum), 2u) << ops_string(t);
  const node_id abc = t.outputs()[0];
  const node_id ab = t.outputs()[1];
  EXPECT_EQ(args_vec(t, abc), (std::vector<node_id>{ab, 2}));
  EXPECT_EQ(args_vec(t, ab), (std::vector<node_id>{0, 1}));
}

TEST(FoldSum, MinTermsThreeKeepsLoneAdds) {
  Tape t;
  {
    Tape::Scope scope(t);
    const Rec a = epykos::make_input(1.0);
    const Rec b = epykos::make_input(2.0);
    const Rec c = epykos::make_input(3.0);
    epykos::register_output(a + b);
    epykos::register_output((a + b) + c);
  }
  epykos::FoldSumOptions opt;
  opt.min_terms = 3;
  epykos::fold_sum(t, opt);
  EXPECT_EQ(count_op(t, Op::Add), 1u) << ops_string(t);
  EXPECT_EQ(count_op(t, Op::Sum), 1u) << ops_string(t);
  EXPECT_EQ(t[t.outputs()[0]].op, Op::Add);
  EXPECT_EQ(t[t.outputs()[1]].op, Op::Sum);
  EXPECT_EQ(t[t.outputs()[1]].nargs, 3);
}

TEST(FoldSum, SingleUseSumInTheLeftSlotIsSplicedAndThePassIsIdempotent) {
  Tape t;
  const node_id a = t.input(1.0);
  const node_id b = t.input(2.0);
  const node_id c = t.input(3.0);
  const node_id d = t.input(4.0);
  const node_id ab[] = {a, b};
  const node_id inner = t.variadic(Op::Sum, ab);
  const node_id abc = t.binary(Op::Add, inner, c);   // Sum(a,b) + c
  const node_id abc_d[] = {abc, d};
  const node_id outer = t.variadic(Op::Sum, abc_d);  // Sum(Sum(a,b) + c, d)
  t.output(outer);
  const PassResult r1 = epykos::fold_sum(t);
  EXPECT_NO_THROW(t.validate());
  EXPECT_EQ(r1.changed, 3u);
  EXPECT_EQ(count_op(t, Op::Sum), 1u) << ops_string(t);
  EXPECT_EQ(count_op(t, Op::Add), 0u);
  EXPECT_EQ(args_vec(t, t.outputs()[0]), (std::vector<node_id>{0, 1, 2, 3}));
  const std::string once = epykos::to_string(t);
  const PassResult r2 = epykos::fold_sum(t);
  EXPECT_EQ(r2.changed, 0u);
  EXPECT_EQ(epykos::to_string(t), once);
  EXPECT_EQ(epykos::replay(t, t.input_values())[0], 10.0);
}

TEST(FoldSum, SubIsNotFolded) {
  Tape t;
  {
    Tape::Scope scope(t);
    const Rec a = epykos::make_input(1.0);
    const Rec b = epykos::make_input(2.0);
    const Rec c = epykos::make_input(3.0);
    epykos::register_output((a - b) + c);
  }
  epykos::fold_sum(t);
  EXPECT_EQ(count_op(t, Op::Sub), 1u);
  EXPECT_EQ(count_op(t, Op::Sum), 1u);
  EXPECT_EQ(t[t.outputs()[0]].nargs, 2);
}

// ---- affine_collapse ------------------------------------------------------------------------------

TEST(Affine, WeightedSumOfInputsBecomesOneAffineNode) {
  // z(t) = z_k * (1 - w) + z_{k+1} * w, the linear interpolation on the knots.
  Tape t;
  Rec zk, zk1;
  {
    Tape::Scope scope(t);
    zk = epykos::make_input(0.04);
    zk1 = epykos::make_input(0.041);
    const double w = 0.3;
    epykos::register_output(zk * (1.0 - w) + zk1 * w);
  }
  const PassResult r = epykos::affine_collapse(t);
  EXPECT_NO_THROW(t.validate());
  EXPECT_EQ(r.changed, 3u) << ops_string(t);  // two Muls absorbed + the Add collapsed
  ASSERT_EQ(count_op(t, Op::Affine), 1u) << ops_string(t);
  EXPECT_EQ(count_op(t, Op::Mul), 0u);
  EXPECT_EQ(count_op(t, Op::Add), 0u);
  const node_id f = t.outputs()[0];
  EXPECT_EQ(t[f].op, Op::Affine);
  EXPECT_EQ(bits(t[f].konst), bits(-0.0)) << "no leading constant: c_0 is the exact identity -0.0";
  EXPECT_EQ(args_vec(t, f), (std::vector<node_id>{0, 1}));
  EXPECT_EQ(coefs_vec(t, f), (std::vector<double>{1.0 - 0.3, 0.3}));
  EXPECT_TRUE(t[f].tainted);
}

TEST(Affine, WorksOnSumsProducedByFoldSum) {
  Tape t;
  {
    Tape::Scope scope(t);
    const Rec a = epykos::make_input(1.0);
    const Rec b = epykos::make_input(2.0);
    const Rec c = epykos::make_input(3.0);
    epykos::register_output(((2.0 * a) + b) + (-c));
  }
  epykos::fold_sum(t);
  ASSERT_EQ(count_op(t, Op::Sum), 1u);
  epykos::affine_collapse(t);
  EXPECT_EQ(count_op(t, Op::Sum), 0u) << ops_string(t);
  EXPECT_EQ(count_op(t, Op::Neg), 0u) << ops_string(t);
  const node_id f = t.outputs()[0];
  ASSERT_EQ(t[f].op, Op::Affine);
  EXPECT_EQ(args_vec(t, f), (std::vector<node_id>{0, 1, 2}));
  EXPECT_EQ(coefs_vec(t, f), (std::vector<double>{2.0, 1.0, -1.0}));
}

TEST(Affine, SubAndNegFoldIntoNegatedCoefficients) {
  Tape t;
  {
    Tape::Scope scope(t);
    const Rec x = epykos::make_input(1.0);
    const Rec y = epykos::make_input(2.0);
    epykos::register_output(x - 2.0 * y);  // 1*x + (-2)*y
    epykos::register_output(-x + y);       // (-1)*x + 1*y
    epykos::register_output(x - (-y));     // 1*x + 1*y
  }
  epykos::affine_collapse(t);
  ASSERT_EQ(count_op(t, Op::Affine), 3u) << ops_string(t);
  EXPECT_EQ(coefs_vec(t, t.outputs()[0]), (std::vector<double>{1.0, -2.0}));
  EXPECT_EQ(coefs_vec(t, t.outputs()[1]), (std::vector<double>{-1.0, 1.0}));
  EXPECT_EQ(coefs_vec(t, t.outputs()[2]), (std::vector<double>{1.0, 1.0}));
  for (node_id o : t.outputs()) EXPECT_EQ(args_vec(t, o), (std::vector<node_id>{0, 1}));
  EXPECT_EQ(count_op(t, Op::Sub), 0u);
  EXPECT_EQ(count_op(t, Op::Neg), 0u);
  EXPECT_EQ(count_op(t, Op::Mul), 0u);
}

TEST(Affine, LeadingConstantBecomesC0AndLaterConstantsStayInPlace) {
  Tape t;
  {
    Tape::Scope scope(t);
    const Rec x = epykos::make_input(1.0);
    const Rec y = epykos::make_input(2.0);
    epykos::register_output(1.0 + 0.5 * x);         // c0 = 1, 0.5*x
    epykos::register_output((x + 3.0) + y);         // c0 = -0.0; 1*x + 1*Const(3) + 1*y
    epykos::register_output((2.0 - x) - 4.0 * y);   // c0 = 2; -1*x + -4*y
  }
  epykos::affine_collapse(t);
  ASSERT_EQ(count_op(t, Op::Affine), 3u) << ops_string(t);
  const node_id f0 = t.outputs()[0];
  EXPECT_EQ(t[f0].konst, 1.0);
  EXPECT_EQ(args_vec(t, f0), (std::vector<node_id>{0}));
  EXPECT_EQ(coefs_vec(t, f0), (std::vector<double>{0.5}));
  const node_id f1 = t.outputs()[1];
  EXPECT_EQ(bits(t[f1].konst), bits(-0.0));
  ASSERT_EQ(t[f1].nargs, 3);
  EXPECT_EQ(t[args_vec(t, f1)[1]].op, Op::Const);
  EXPECT_EQ(t[args_vec(t, f1)[1]].konst, 3.0);
  EXPECT_EQ(coefs_vec(t, f1), (std::vector<double>{1.0, 1.0, 1.0}));
  const node_id f2 = t.outputs()[2];
  EXPECT_EQ(t[f2].konst, 2.0);
  EXPECT_EQ(coefs_vec(t, f2), (std::vector<double>{-1.0, -4.0}));
}

TEST(Affine, NestedAffineThroughAConstantScale) {
  // z_k + (z_{k+1} - z_k) * w: the inner difference is affine, the outer chain scales it.
  Tape t;
  {
    Tape::Scope scope(t);
    const Rec zk = epykos::make_input(0.04);
    const Rec zk1 = epykos::make_input(0.041);
    epykos::register_output(zk + (zk1 - zk) * 0.3);
  }
  epykos::affine_collapse(t);
  EXPECT_EQ(count_op(t, Op::Affine), 2u) << ops_string(t);
  EXPECT_EQ(count_op(t, Op::Sub), 0u);
  EXPECT_EQ(count_op(t, Op::Mul), 0u);
  EXPECT_EQ(count_op(t, Op::Add), 0u);
  const node_id outer = t.outputs()[0];
  ASSERT_EQ(t[outer].op, Op::Affine);
  const auto args = args_vec(t, outer);
  ASSERT_EQ(args.size(), 2u);
  EXPECT_EQ(args[0], 0);
  EXPECT_EQ(t[args[1]].op, Op::Affine);
  EXPECT_EQ(coefs_vec(t, outer), (std::vector<double>{1.0, 0.3}));
  EXPECT_EQ(coefs_vec(t, args[1]), (std::vector<double>{1.0, -1.0}));
  EXPECT_EQ(args_vec(t, args[1]), (std::vector<node_id>{1, 0}));
}

TEST(Affine, SegmentSumsOfProductsAreLeftAlone) {
  // A leg: sum of coupon values N*tau*K*DF, all tainted products. Not affine in inputs.
  Tape t;
  {
    Tape::Scope scope(t);
    const Rec k = epykos::make_input(0.04);
    Rec leg = 0.0;
    for (int j = 0; j < 4; ++j) {
      const Rec df = exp(-epykos::make_input(0.04) * (j + 1.0));
      leg = leg + 1e6 * 1.01 * k * df;
    }
    epykos::register_output(leg);
  }
  epykos::fold_sum(t);
  const std::string before = ops_string(t);
  const PassResult r = epykos::affine_collapse(t);
  EXPECT_EQ(r.changed, 0u);
  EXPECT_EQ(ops_string(t), before);
  EXPECT_EQ(count_op(t, Op::Affine), 0u);
  EXPECT_EQ(count_op(t, Op::Sum), 1u);
}

TEST(Affine, ChainStopsAtTheFirstNonAffineAddend) {
  Tape t;
  {
    Tape::Scope scope(t);
    const Rec a = epykos::make_input(1.0);
    const Rec b = epykos::make_input(2.0);
    const Rec pv = a * b;  // tainted, not affine
    epykos::register_output((a + b) + pv);  // (a + b) collapses; the outer Add stays
    epykos::register_output((pv + a) + b);  // nothing: the leftmost addend is not affine
  }
  epykos::affine_collapse(t);
  EXPECT_EQ(count_op(t, Op::Affine), 1u) << ops_string(t);
  const node_id o0 = t.outputs()[0];
  EXPECT_EQ(t[o0].op, Op::Add);
  EXPECT_EQ(t[t[o0].a].op, Op::Affine);
  EXPECT_EQ(args_vec(t, t[o0].a), (std::vector<node_id>{0, 1}));
  const node_id o1 = t.outputs()[1];
  EXPECT_EQ(t[o1].op, Op::Add);
  EXPECT_EQ(t[t[o1].a].op, Op::Add);
}

TEST(Affine, MultiUseScaledTermIsAnAtomNotAbsorbed) {
  Tape t;
  {
    Tape::Scope scope(t);
    const Rec a = epykos::make_input(1.0);
    const Rec b = epykos::make_input(2.0);
    const Rec sa = 2.0 * a;
    epykos::register_output(sa + b);
    epykos::register_output(sa);  // second use: the Mul must survive
  }
  epykos::affine_collapse(t);
  EXPECT_EQ(count_op(t, Op::Mul), 1u) << ops_string(t);
  EXPECT_EQ(count_op(t, Op::Affine), 0u) << "a tainted Mul atom is not affine in inputs";
  EXPECT_EQ(t[t.outputs()[0]].op, Op::Add);
}

TEST(Affine, UntaintedOnlyChainsAreNotCollapsed) {
  Tape t;
  {
    Tape::Scope scope(t);
    const Rec x = epykos::make_input(1.0);
    const Rec k = (Rec(1.0) + 2.0) + 3.0;  // constants only
    epykos::register_output(k * x);
  }
  epykos::affine_collapse(t);
  EXPECT_EQ(count_op(t, Op::Affine), 0u) << ops_string(t);
}

TEST(Affine, IdempotentAndComposable) {
  Tape t;
  {
    Tape::Scope scope(t);
    const Rec a = epykos::make_input(1.0);
    const Rec b = epykos::make_input(2.0);
    const Rec c = epykos::make_input(3.0);
    const Rec z = 0.25 * a + 0.75 * b;
    epykos::register_output(exp(-z * 2.0) * c);
    epykos::register_output(z + c);
  }
  const PassResult r1 = epykos::affine_collapse(t);
  EXPECT_GT(r1.changed, 0u);
  // z (multi-use) collapses first; z + c then sees the Affine z as an atom in the same run.
  EXPECT_EQ(count_op(t, Op::Affine), 2u) << ops_string(t);
  const std::string once = epykos::to_string(t);
  const PassResult r2 = epykos::affine_collapse(t);
  EXPECT_EQ(r2.changed, 0u) << "a second run has nothing left to do";
  EXPECT_EQ(once, epykos::to_string(t));
  EXPECT_NO_THROW(t.validate());
}

// ---- composition ----------------------------------------------------------------------------------

TEST(StandardPasses, ComposedRemapPointsAtLiveNodes) {
  Tape t;
  Rec out;
  {
    Tape::Scope scope(t);
    const Rec a = epykos::make_input(1.0);
    const Rec b = epykos::make_input(2.0);
    const Rec z = 0.25 * a + 0.75 * b;
    const Rec dead = a * b;
    (void)dead;
    const Rec dup = 0.25 * a + 0.75 * b;
    out = (z + dup) + exp(z);
    epykos::register_output(out);
  }
  const std::vector<double> in = t.input_values();
  const std::vector<double> before = epykos::replay(t, in);
  const std::size_t n_before = t.size();
  const PassResult r = epykos::standard_passes(t);
  EXPECT_NO_THROW(t.validate());
  ASSERT_EQ(r.remap.size(), n_before);
  EXPECT_EQ(r.nodes_before, n_before);
  EXPECT_EQ(r.nodes_after, t.size());
  EXPECT_LT(t.size(), n_before);
  EXPECT_EQ(r.remap[out.id], t.outputs()[0]);
  EXPECT_EQ(count_op(t, Op::Affine), 1u) << ops_string(t);  // z (dup merged by cse)
  EXPECT_EQ(count_op(t, Op::Sum), 1u) << ops_string(t);     // (z + z) + exp(z)
  const std::vector<double> after = epykos::replay(t, in);
  EXPECT_EQ(bits(after[0]), bits(before[0]));
}
